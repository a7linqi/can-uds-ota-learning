"""UDS bench GUI: single-step service table plus one-click firmware update.

Two learning modes in one window:

  * Service table  - one row per UDS service, click to send exactly that
                     request and see the raw request/response bytes.
  * One-click flash - runs 0x10 -> 0x27 -> 0x34 -> 0x36 -> 0x37 -> 0x11
                     with a progress bar and an event log.

Backends: sim (host C core), uart (USART1 bench), can (python-can).

    python tools/uds_gui.py
    python tools/uds_gui.py --backend uart --serial-port COM5

Nothing here is hardware-specific; the same window drives the simulator today
and the board once the cable is plugged in.
"""

import argparse
import queue
import threading
import time
from datetime import datetime
from pathlib import Path
import sys
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

from pc.isotp_client import IsoTpClient, ProtocolError  # noqa: E402
from pc.transport import (  # noqa: E402
    PythonCanTransport, ROOT, SerialCanTransport, SimTransport, TransportError)
from pc.uds_client import (  # noqa: E402
    NegativeResponse, UdsClient, UnknownCommit, UnknownReset, inspect_package)

# (label, request bytes, note) -- every entry is a legal standalone request.
SERVICES = [
    ("0x10 01  默认会话",      b"\x10\x01",           "回到默认会话"),
    ("0x10 02  编程会话",      b"\x10\x02",           "刷写前提，会返回 P2/P2*"),
    ("0x10 03  扩展会话",      b"\x10\x03",           "诊断扩展会话"),
    ("0x11 01  ECU 复位",      b"\x11\x01",           "硬复位，可能丢响应"),
    ("0x27 01  请求种子",      b"\x27\x01",           "安全访问第一步"),
    ("0x3E 00  TesterPresent", b"\x3E\x00",           "保持会话不超时"),
    ("0x22 F190 读 VIN",       b"\x22\xF1\x90",       "17 字节 ASCII"),
    ("0x22 F195 读版本",       b"\x22\xF1\x95",       "Bootloader 版本串"),
    ("0x22 F186 读当前会话",   b"\x22\xF1\x86",       "1 字节：1/2/3"),
    ("0x22 0200 读升级状态",   b"\x22\x02\x00",       "40 字节状态记录"),
    ("0x31 01 0001 校验暂存",  b"\x31\x01\x00\x01",   "校验已下载镜像"),
]

STAGE_NAMES = {
    "request_started": "发送请求",
    "programming_unlocked": "编程会话 + 安全访问通过",
    "package_preflight": "本地镜像预检通过",
    "block_accepted": "传输数据块",
    "download_committed": "0x37 提交完成",
    "reset_requested": "已请求复位",
    "pending": "ECU 回 0x78 等待中",
    "block_retry": "块重传",
}


def now():
    return datetime.now().strftime("%H:%M:%S.%f")[:-3]


def hexs(data):
    return " ".join("%02X" % b for b in data)


class BenchApp:
    def __init__(self, root, args):
        self.root = root
        self.args = args
        self.events = queue.Queue()
        self.transport = None
        self.client = None
        self.busy = False
        self.link_ready = False
        self.port_map = {}

        root.title("UDS Bench - CAN/UDS OTA 学习台架")
        root.geometry("1180x760")

        self._build_top()
        self._build_middle()
        self._build_log()
        self.refresh_ports()
        self._set_controls(False)

        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        self.root.after(60, self._drain)
        # auto-connect on start; a failure here must not pop a modal dialog
        # before the user has even seen the window
        self.root.after(200, lambda: self.connect(silent=True))

    # ---------- layout ----------
    def _build_top(self):
        bar = ttk.Frame(self.root, padding=(10, 8))
        bar.pack(fill="x")

        ttk.Label(bar, text="后端").pack(side="left")
        self.backend = ttk.Combobox(bar, values=("sim", "uart", "can"),
                                    width=6, state="readonly")
        self.backend.set(self.args.backend)
        self.backend.pack(side="left", padx=(4, 12))

        ttk.Label(bar, text="串口").pack(side="left")
        # Detected ports only: typing a port name by hand is how you end up on a
        # Bluetooth COM port at the wrong moment.
        self.port_combo = ttk.Combobox(bar, width=30, state="readonly", values=())
        self.port_combo.pack(side="left", padx=(4, 4))
        self.refresh_btn = ttk.Button(bar, text="刷新", width=5,
                                      command=self.refresh_ports)
        self.refresh_btn.pack(side="left", padx=(0, 6))
        self.port_status = ttk.Label(bar, text="", foreground="#555")
        self.port_status.pack(side="left", padx=(0, 12))

        ttk.Label(bar, text="波特率").pack(side="left")
        rates = ("9600", "19200", "38400", "57600", "115200",
                 "230400", "460800", "921600")
        baud = str(self.args.baud)
        if baud not in rates:
            rates = (baud,) + rates
        self.baud_combo = ttk.Combobox(bar, width=8, state="readonly", values=rates)
        self.baud_combo.set(baud)
        self.baud_combo.pack(side="left", padx=(4, 12))

        self.backend.bind("<<ComboboxSelected>>", self._on_backend_change)

        # One toggle rather than separate connect/disconnect buttons: the label
        # says what the click will do, and the backend can only be switched
        # after the current link is torn down.
        self.connect_btn = ttk.Button(bar, text="连接", width=8,
                                      command=self.toggle_connect)
        self.connect_btn.pack(side="left")
        self.state_lbl = ttk.Label(bar, text="未连接", foreground="#a33")
        self.state_lbl.pack(side="left", padx=12)

    # ---------- serial port discovery ----------
    def refresh_ports(self):
        """Fill the port list from what the OS reports right now."""
        try:
            import serial.tools.list_ports as lp
        except ImportError:
            self.port_map = {}
            self.port_combo["values"] = ()
            self.port_combo.set("")
            self.port_status.configure(text="未安装 pyserial", foreground="#a33")
            return
        ports = list(lp.comports())

        def rank(p):
            # real USB bridges first, Bluetooth/virtual last
            return (0 if p.vid is not None else 1,
                    1 if "BTHENUM" in (p.hwid or "") else 0, p.device)
        ports.sort(key=rank)
        self.port_map = {}
        labels = []
        for port in ports:
            desc = (port.description or "").strip()
            # Many drivers already include the COM name in the description;
            # don't print it twice.
            if desc and port.device.upper() in desc.upper():
                label = desc
            elif desc:
                label = "%s  %s" % (port.device, desc)
            else:
                label = port.device
            self.port_map[label] = port.device
            labels.append(label)
        self.port_combo["values"] = labels
        if not labels:
            self.port_combo.set("")
            self.port_status.configure(text="未检测到串口", foreground="#a33")
            return
        want = (self.args.serial_port or "").strip().upper()
        if self.port_combo.get() in self.port_map:
            chosen = self.port_combo.get()
        elif want:
            chosen = next((l for l in labels if self.port_map[l].upper() == want), labels[0])
        else:
            chosen = labels[0]
        self.port_combo.set(chosen)
        self.port_status.configure(text="%d 个" % len(labels), foreground="#555")
        self.append_log("串口检测：%s" % self.port_map[chosen])

    def selected_port(self):
        return self.port_map.get(self.port_combo.get(), "")

    def _on_backend_change(self, _event=None):
        if self.link_ready:
            return
        if self.backend.get() == "uart":
            self.refresh_ports()

    def _build_middle(self):
        mid = ttk.Frame(self.root, padding=(10, 0))
        mid.pack(fill="both", expand=True)

        # left: service table
        left = ttk.LabelFrame(mid, text="服务表（单步发送）", padding=8)
        left.pack(side="left", fill="both", expand=True, padx=(0, 8))

        cols = ("service", "req", "resp", "note")
        self.tree = ttk.Treeview(left, columns=cols, show="headings", height=13)
        for c, w, t in (("service", 205, "服务"), ("req", 125, "请求"),
                        ("resp", 265, "响应"), ("note", 175, "说明")):
            self.tree.heading(c, text=t)
            self.tree.column(c, width=w, anchor="w")
        self.tree.pack(fill="both", expand=True)

        for label, req, note in SERVICES:
            self.tree.insert("", "end", iid=label, values=(label, hexs(req), "", note))
        self.tree.bind("<Double-1>", self.on_row_send)

        ttk.Button(left, text="发送选中服务（或双击行）",
                   command=self.send_selected).pack(fill="x", pady=(6, 0))

        custom = ttk.Frame(left)
        custom.pack(fill="x", pady=(8, 0))
        ttk.Label(custom, text="自定义请求(hex)").pack(side="left")
        self.custom_entry = ttk.Entry(custom)
        self.custom_entry.insert(0, "22 F1 90")
        self.custom_entry.pack(side="left", fill="x", expand=True, padx=6)
        ttk.Button(custom, text="发送", command=self.send_custom).pack(side="left")

        detail = ttk.LabelFrame(left, text="最近一次收发", padding=6)
        detail.pack(fill="x", pady=(8, 0))
        self.last_tx = ttk.Label(detail, text="TX: -", font=("Consolas", 9))
        self.last_tx.pack(anchor="w")
        self.last_rx = ttk.Label(detail, text="RX: -", font=("Consolas", 9))
        self.last_rx.pack(anchor="w")

        # right: one-click flash
        right = ttk.LabelFrame(mid, text="一键升级", padding=8)
        right.pack(side="right", fill="y")

        row = ttk.Frame(right)
        row.pack(fill="x")
        ttk.Label(row, text="镜像").pack(side="left")
        self.image_entry = ttk.Entry(row)
        self.image_entry.pack(side="left", fill="x", expand=True, padx=6)
        ttk.Button(row, text="浏览", width=6, command=self.pick_image).pack(side="left")

        self.image_info = ttk.Label(right, text="未选择镜像", foreground="#555",
                                    wraplength=380, justify="left")
        self.image_info.pack(fill="x", pady=(6, 0))

        self.flash_btn = ttk.Button(right, text="开始升级", command=self.start_flash)
        self.flash_btn.pack(fill="x", pady=(10, 4))

        self.progress = ttk.Progressbar(right, mode="determinate", maximum=100)
        self.progress.pack(fill="x")
        self.stage_lbl = ttk.Label(right, text="空闲", foreground="#555")
        self.stage_lbl.pack(fill="x", pady=(4, 0))

        self.result = tk.Text(right, height=11, width=52, font=("Consolas", 9),
                              wrap="word", state="disabled")
        self.result.pack(fill="x", pady=(8, 0))

        # One-click reads for the DIDs people actually ask for in a demo.
        dids = ttk.LabelFrame(right, text="常用 DID（点一下就读）", padding=6)
        dids.pack(fill="x", pady=(8, 0))
        quick = (("VIN  F190", b"\xF1\x90"),
                 ("版本  F195", b"\xF1\x95"),
                 ("会话  F186", b"\xF1\x86"),
                 ("零件号  F187", b"\xF1\x87"),
                 ("升级状态  0200", b"\x02\x00"),
                 ("TesterPresent", None))
        for i, (name, did) in enumerate(quick):
            ttk.Button(dids, text=name,
                       command=lambda n=name, d=did: self.do_quick(n, d)
                       ).grid(row=i // 3, column=i % 3, padx=2, pady=2, sticky="ew")
        for col in range(3):
            dids.columnconfigure(col, weight=1)

        row2 = ttk.Frame(right)
        row2.pack(fill="x", pady=(8, 0))
        ttk.Button(row2, text="读 info", command=self.do_info).pack(side="left")
        ttk.Button(row2, text="读 status", command=self.do_status).pack(side="left", padx=6)

        self.set_result(
            "就绪。\n\n"
            "左侧：双击服务表任意一行，单步发送\n"
            "      并查看原始请求/响应字节。\n\n"
            "右中：常用 DID 一点即读。\n\n"
            "右侧：选择镜像后点「开始升级」，\n"
            "      走完 0x10 → 0x27 → 0x34 →\n"
            "      0x36 → 0x37 → 0x11 全流程。")

    def _build_log(self):
        frame = ttk.LabelFrame(self.root, text="事件日志", padding=6)
        frame.pack(fill="both", expand=False, padx=10, pady=(0, 10))
        self.log = tk.Text(frame, height=9, font=("Consolas", 9), wrap="none",
                           state="disabled", background="#101418", foreground="#d6e2ea")
        scroll = ttk.Scrollbar(frame, command=self.log.yview)
        self.log.configure(yscrollcommand=scroll.set)
        self.log.pack(side="left", fill="both", expand=True)
        scroll.pack(side="right", fill="y")

    # ---------- helpers ----------
    def append_log(self, text, tag=None):
        self.log.configure(state="normal")
        self.log.insert("end", "%s  %s\n" % (now(), text))
        self.log.see("end")
        self.log.configure(state="disabled")

    def set_result(self, text):
        self.result.configure(state="normal")
        self.result.delete("1.0", "end")
        self.result.insert("1.0", text)
        self.result.configure(state="disabled")

    def set_state(self, text, color):
        self.state_lbl.configure(text=text, foreground=color)

    # ---------- connection ----------
    def _set_controls(self, connected):
        """One link at a time. While it is up the backend/port/baud must not
        change, so switching between sim and uart always goes through
        "断开" first -- otherwise the widgets would describe a link that is
        not the one actually carrying traffic."""
        state = "disabled" if connected else "readonly"
        self.backend.configure(state=state)
        self.port_combo.configure(state=state)
        self.baud_combo.configure(state=state)
        self.refresh_btn.configure(state="disabled" if connected else "normal")
        self.connect_btn.configure(text="断开" if connected else "连接")

    def toggle_connect(self):
        if self.link_ready:
            self.disconnect()
        else:
            self.connect()

    def disconnect(self):
        if self.busy:
            messagebox.showinfo("正在传输", "这一步还没结束，等它完成再断开。")
            return
        was = self.state_lbl.cget("text").replace("已连接 ", "")
        if self.transport is not None:
            try:
                self.transport.close()
            except Exception as exc:          # closing must never block exiting
                self.append_log("关闭连接时出错（忽略）: %s" % exc)
        self.transport = None
        self.client = None
        self.link_ready = False
        self._set_controls(False)
        self.progress.configure(value=0)
        self.stage_lbl.configure(text="空闲", foreground="#555")
        self.set_state("未连接", "#a33")
        self.append_log("已断开（%s），现在可以切换后端" % was)

    def _on_close(self):
        if self.transport is not None:
            try:
                self.transport.close()
            except Exception:
                pass
            self.transport = None
        self.root.destroy()

    def connect(self, silent=False):
        if self.busy or self.link_ready:
            return
        backend = self.backend.get()
        try:
            if backend == "sim":
                cc = self.args.cc
                self.append_log("启动模拟 ECU（编译 C 核心）...")
                transport = SimTransport(cc, self.args.flash_kib, self.args.ecu_bin)
                desc = "sim (C core)"
            elif backend == "uart":
                port = self.selected_port()
                if not port:
                    raise ValueError('没有可用串口：先插上 USB 转串口，再点“刷新”')
                baud = int(self.baud_combo.get())
                transport = SerialCanTransport(port, baud)
                desc = "uart %s @%d" % (port, baud)
            else:
                if not self.args.interface or self.args.channel is None:
                    raise ValueError("can 后端需要 --interface 和 --channel")
                transport = PythonCanTransport(self.args.interface, self.args.channel,
                                               self.args.bitrate, self.args.index)
                desc = "can %s/%s" % (self.args.interface, self.args.channel)
        except (TransportError, ValueError, OSError) as exc:
            self.link_ready = False
            self._set_controls(False)
            self.set_state("未连接" if silent else "连接失败", "#a33")
            self.append_log("连接失败: %s" % exc)
            if not silent:
                messagebox.showerror("连接失败", str(exc))
            return

        self.transport = transport
        self.client = UdsClient(IsoTpClient(transport), self.on_uds_event,
                                total_timeout=self.args.request_timeout)
        self.link_ready = True
        self._set_controls(True)
        self.progress.configure(value=0)
        self.stage_lbl.configure(text="空闲", foreground="#555")
        self.set_state("已连接 %s" % desc, "#2a7")
        self.append_log("已连接: %s" % desc)

    def on_uds_event(self, **event):
        """Called from the worker thread; must not touch widgets."""
        self.events.put(("event", event))

    # ---------- worker plumbing ----------
    def run_async(self, fn, done=None):
        if self.busy or not self.link_ready:
            if not self.link_ready:
                messagebox.showwarning("未连接", "请先连接后端")
            return
        self.busy = True

        def worker():
            try:
                result = fn()
                self.events.put(("done", (result, done, None)))
            except (TransportError, ProtocolError, NegativeResponse, ValueError,
                    UnknownCommit, UnknownReset, OSError, TimeoutError) as exc:
                self.events.put(("done", (None, done, exc)))

        threading.Thread(target=worker, daemon=True).start()

    def _drain(self):
        try:
            while True:
                kind, payload = self.events.get_nowait()
                if kind == "event":
                    self._on_event(payload)
                elif kind == "done":
                    result, done, exc = payload
                    self.busy = False
                    if exc is not None:
                        if isinstance(exc, NegativeResponse):
                            self.append_log("NRC 0x%02X（服务 0x%02X 被拒）" % (exc.nrc, exc.sid))
                            self.set_result("失败：NRC 0x%02X\n服务 0x%02X\n\n这是 ECU 主动拒绝，"
                                            "不是通信错误。" % (exc.nrc, exc.sid))
                        else:
                            self.append_log("失败: %s" % exc)
                            self.set_result("失败：%s" % exc)
                    elif done is not None:
                        done(result)
        except queue.Empty:
            pass
        self.root.after(60, self._drain)

    def _on_event(self, event):
        name = event.get("event", "?")
        label = STAGE_NAMES.get(name, name)
        if name == "block_accepted":
            total = getattr(self, "_total_bytes", 0) or 1
            sent = event.get("offset", 0) + event.get("bytes", 0)
            pct = min(100.0, 100.0 * sent / total)
            self.progress["value"] = pct
            self.stage_lbl.configure(text="0x36 传输中  %d / %d 字节（%.1f%%）"
                                     % (sent, total, pct))
            if event.get("counter", 0) % 32 == 0 or pct >= 100.0:
                self.append_log("块 %d 已确认，累计 %d 字节" % (event["counter"], sent))
        elif name == "request_started":
            self.append_log("→ 请求 0x%s（%d 字节）" % (event.get("sid"), event.get("bytes")))
        else:
            self.append_log("%s %s" % (label, {k: v for k, v in event.items() if k != "event"}))
        if name in ("programming_unlocked", "download_committed", "reset_requested"):
            self.stage_lbl.configure(text=label)

    # ---------- single step ----------
    def send_selected(self):
        sel = self.tree.selection()
        if not sel:
            messagebox.showinfo("提示", "先在服务表里选一行")
            return
        label = sel[0]
        row = next(s for s in SERVICES if s[0] == label)
        self._send_bytes(row[1], label)

    def on_row_send(self, _event):
        self.send_selected()

    def send_custom(self):
        text = self.custom_entry.get().replace(",", " ").split()
        try:
            data = bytes(int(t, 16) for t in text)
        except ValueError:
            messagebox.showerror("格式错误", "请输入十六进制字节，例如: 22 F1 90")
            return
        if not data:
            return
        self._send_bytes(data, "自定义")

    def _send_bytes(self, request, label):
        def work():
            return self.client.request_retry(request)

        def done(response):
            self.last_tx.configure(text="TX: %s" % hexs(request))
            self.last_rx.configure(text="RX: %s" % hexs(response))
            self.tree.set(label, "req", hexs(request))
            self.tree.set(label, "resp", hexs(response))
            self.append_log("← 响应 %s" % hexs(response))
            self.set_result("%s\n\n请求: %s\n响应: %s\n（%d 字节）"
                            % (label, hexs(request), hexs(response), len(response)))

        self.run_async(work, done)

    def do_quick(self, name, did):
        """Read one DID (or send TesterPresent) without touching the table."""
        request = b"\x3E\x00" if did is None else b"\x22" + did
        row = next((label for label, req, _ in SERVICES if req == request), None)

        def work():
            return self.client.request_retry(request)

        def done(response):
            self.last_tx.configure(text="TX: %s" % hexs(request))
            self.last_rx.configure(text="RX: %s" % hexs(response))
            if row:
                self.tree.set(row, "req", hexs(request))
                self.tree.set(row, "resp", hexs(response))
            self.append_log("%s → %s" % (name, hexs(response)))
            self.set_result("%s\n\n请求: %s\n响应: %s\n（%d 字节）"
                            % (name, hexs(request), hexs(response), len(response)))

        self.run_async(work, done)

    def do_info(self):
        def work():
            return self.client.info()

        def done(result):
            text = ("VIN        : %s\nBoot 版本  : %s\n当前会话   : %d"
                    % (result["vin"], result["bootloader_demo_version"], result["session"]))
            self.set_result(text)
            self.append_log("info 读取成功")

        self.run_async(work, done)

    def do_status(self):
        def work():
            return self.client.status()

        def done(result):
            text = "\n".join([
                "状态机    : %s" % result["state_name"],
                "活动版本  : %d" % result["active_version"],
                "待装版本  : %d" % result["pending_version"],
                "最低版本  : %d" % result["min_version"],
                "活动大小  : %d 字节" % result["active_size"],
                "试运行次数: %d" % result["boot_attempts"],
                "元数据有效: %s" % result["metadata_valid"],
                "序列号    : %d" % result["metadata_sequence"],
            ])
            self.set_result(text)
            self.append_log("status 读取成功：%s" % result["state_name"])

        self.run_async(work, done)

    # ---------- one-click flash ----------
    def pick_image(self):
        path = filedialog.askopenfilename(
            title="选择 IMG2 镜像",
            initialdir=str(ROOT),
            filetypes=[("IMG2 image", "*.bin"), ("All files", "*.*")])
        if path:
            self.image_entry.delete(0, "end")
            self.image_entry.insert(0, path)
            self._inspect_image(Path(path))

    def _inspect_image(self, path):
        try:
            data = path.read_bytes()
            meta = inspect_package(data, self.args.flash_kib)
        except (OSError, ValueError) as exc:
            self.image_info.configure(text="镜像无效：%s" % exc, foreground="#a33")
            self._package = None
            return
        self.image_info.configure(
            text="版本 %d | payload %d 字节 | 签名 %s | sha256 %s…"
                 % (meta["version"], meta["payload_bytes"],
                    "有" if meta.get("signed") else "无",
                    meta["package_sha256"][:16]),
            foreground="#2a7")
        self._package = data

    def start_flash(self):
        text = self.image_entry.get().strip()
        if not text:
            messagebox.showinfo("提示", "先选择要升级的镜像")
            return
        path = Path(text)
        if not hasattr(self, "_package") or self._package is None:
            self._inspect_image(path)
        if getattr(self, "_package", None) is None:
            return
        package = self._package
        try:
            meta = inspect_package(package, self.args.flash_kib)
        except ValueError as exc:
            messagebox.showerror("镜像无效", str(exc))
            return

        self._total_bytes = len(package)
        self.progress["value"] = 0
        self.stage_lbl.configure(text="开始升级…")
        self.append_log("=== 一键升级：版本 %d，%d 字节 ===" % (meta["version"], len(package)))

        def work():
            return self.client.flash(package, self.args.flash_kib,
                                     reset=True, retries=1)

        def done(result):
            self.progress["value"] = 100
            self.stage_lbl.configure(text="升级完成，已请求复位")
            self.append_log("升级完成：%d 块，已复位" % result["blocks"])
            self.set_result("\n".join([
                "升级成功",
                "版本      : %d" % result["version"],
                "payload   : %d 字节" % result["payload_bytes"],
                "传输块数  : %d" % result["blocks"],
                "已请求复位: %s" % result["reset_requested"],
                "应用自检  : %s" % result["application_health_confirmed"],
                "",
                "注意：应用健康确认由 App 自己完成，",
                "PC 无法代替它宣称健康。",
            ]))

        self.run_async(work, done)


def main():
    parser = argparse.ArgumentParser(description="UDS bench GUI")
    parser.add_argument("--backend", choices=("sim", "uart", "can"), default="sim")
    parser.add_argument("--serial-port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--interface")
    parser.add_argument("--channel")
    parser.add_argument("--index", type=int)
    parser.add_argument("--bitrate", type=int, default=500000)
    parser.add_argument("--flash-kib", type=int, choices=(512, 1024), default=1024)
    parser.add_argument("--cc", help="host C compiler for the simulator")
    parser.add_argument("--ecu-bin", type=Path)
    parser.add_argument("--request-timeout", type=float, default=120)
    args = parser.parse_args()

    if args.backend == "sim" and not args.cc:
        for candidate in (
            Path(r"D:/Project/Can/toolchain/tccbox-2025.10.27.data/purelib/tccbox/tcc_dist/tcc.exe"),
            Path("tcc"),
        ):
            if candidate.exists():
                args.cc = str(candidate)
                break

    root = tk.Tk()
    try:
        style = ttk.Style()
        if "vista" in style.theme_names():
            style.theme_use("vista")
    except tk.TclError:
        pass
    BenchApp(root, args)
    root.mainloop()


if __name__ == "__main__":
    main()
