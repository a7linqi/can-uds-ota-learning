# CAN / UDS / OTA 学习笔记

> 项目：`ota-core`（STM32F407 + W25Q128 + AT24C02）  
> 整理日期：2026-11-05  
> 说明：从一条真实刷写报文出发，理解 UDS 下载、校验、存储与掉电恢复。

## 0. 一句话总览

一次 OTA 不是“把文件发过去”，而是：

```text
进编程会话 → 解锁 → 声明下载 → 分块搬运 → 整包验收 → 写 READY
→ 复位 → Bootloader 备份/安装 → TRIAL 试运行 → 健康确认
```

UDS 负责“说”，ISO-TP 负责“拆包”，CAN 负责“送”；NOR 存包，EEPROM 存状态，内部 Flash 存可执行代码，SRAM 存临时变量。

---

## 1. 分层：UDS / ISO-TP / CAN

```text
UDS        ：22 F1 95                  “读 F195 这个数据”
ISO-TP     ：03 22 F1 95               “单帧，3 字节”
CAN        ：ID=0x7E0, DLC=4, 上面 4 字节
UART 台架  ：A5 5A ...                 “把这一帧 CAN 编码后走串口”
```

- `0x7E0` 是测试仪发给 ECU 的 CAN ID，`0x7E8` 是 ECU 回应 ID；它们是项目/诊断规范约定，不是 UDS 标准。
- 正响应：`请求 SID + 0x40`。
- 负响应：`7F 请求SID NRC`。

---

## 2. UDS 速查

### 2.1 本项目实现的 11 个服务

| SID | 名称 | 请求示例 | 正响应示例 | 备注 |
|---|---|---|---|---|
| `0x10` | DiagnosticSessionControl | `10 03` | `50 03 00 32 01 F4` | 01 默认 / 02 编程 / 03 扩展 |
| `0x11` | ECUReset | `11 01` | `51 01` | 复位后可能丢响应 |
| `0x14` | ClearDTCInformation | `14 FF FF FF` | `54` | 只支持全部组 |
| `0x19` | ReadDTCInformation | `19 01 FF` / `19 02 FF` | `59 01 ...` / `59 02 ...` | 01 数量，02 读列表 |
| `0x22` | ReadDataByIdentifier | `22 F1 95` | `62 F1 95 30 2E 32 2E 30` | 后两字节是 DID |
| `0x27` | SecurityAccess | `27 01` / `27 02 key` | `67 01 seed` / `67 02` | 01 要种子，02 送密钥 |
| `0x31` | RoutineControl | `31 01 00 01` | `71 01 00 01 00` | 校验 READY 包 |
| `0x34` | RequestDownload | `34 00 44 <4B地址> <4B总长>` | `74 20 02 00` | 最大块长 512 |
| `0x36` | TransferData | `36 <BSC> <数据>` | `76 <BSC>` | 每块最多 510 数据字节 |
| `0x37` | RequestTransferExit | `37` | `77` | 校验后置 READY；安装发生在复位后 |
| `0x3E` | TesterPresent | `3E 00` | `7E 00` | 保持会话 |

未实现：`0x2E` 写 DID、`0x35` 上传、`0x28` 通信控制、`0x85` DTC 设置。收到会回 NRC `0x11`。

### 2.2 本项目的 DID

| DID | 含义 | 返回 |
|---|---|---|
| `F190` | VIN | 17 字节 ASCII |
| `F187` | 零件号 | `OTA-ECU-0001` |
| `F186` | 当前会话 | 1 字节：1/2/3 |
| `F195` | 软件版本 | `0.2.0` |
| `0200` | 升级状态（项目自定义） | 44 字节记录 |

`F190/F187/F186/F195` 是行业常用 DID；`0200` 是项目自定义，格式见 `docs/UPDATE_STATUS.md`。

### 2.3 先记这 13 个 NRC

| NRC | 含义 | 典型原因 |
|---|---|---|
| `0x11` | 服务不支持 | SID 没实现，例如 `2E`、`35` |
| `0x12` | 子功能不支持 | `10 04`、`31 02` |
| `0x13` | 长度不对 | `10`、`34`、`36`、`37` 字节数错 |
| `0x14` | 响应太长 | 0x22/0x19 的应答超过缓冲 |
| `0x22` | 条件不满足 | 没进编程会话、没解锁、状态不对 |
| `0x24` | 顺序错误 | 没 0x34 就发 0x36；没 seed 就发 key |
| `0x31` | 参数超范围 | 地址不是 `0x08010000`、长度/版本不合法 |
| `0x33` | 安全访问被拒绝 | 没解锁就发 0x34/0x36/0x37/0x31 |
| `0x35` | 密钥错误 | `27 02` 的 key 算错 |
| `0x36` | 尝试次数超限 | 连续 3 次错误 key，进入锁定 |
| `0x72` | 编程失败 | 元数据/擦除/CRC/签名/镜像校验失败 |
| `0x73` | 块序号错误 | 0x36 跳号，或重复块内容变了 |
| `0x78` | 还在处理，不是失败 | 长操作中，继续等，不要重发 |

注意：`0x22` 既是服务号，也是 NRC 编号；看它出现在哪个位置。

### 2.4 ISO-TP 四种帧

| 类型 | 首字节 | 例子 | 含义 |
|---|---|---|---|
| SF 单帧 | `0x0L`，L=1..7 | `03 22 F1 95` | 整条消息 3 字节 |
| FF 首帧 | `0x1H` + 低 8 位长度 | `10 08 62 F1 95 30 2E 32` | 总长 8，首帧带 6 字节 |
| CF 后续帧 | `0x2S`，S=1..F | `21 2E 30` | 序号 1，后面 2 字节 |
| FC 流控帧 | `0x3S` + BS + STmin | `30 00 00` | 状态 0 继续，BS=0，STmin=0 |

- ISO-TP 的 SN 是 4 位，从 1 开始，F 后回 0。
- UDS 0x36 的 BSC 是 8 位，从 1 开始，FF 后允许 00。
- 单帧最多 7 字节 payload，所以 8 字节应答必须 FF + FC + CF。

### 2.5 看到一串数字，按这个顺序读

1. 第一字节是 `7F`？→ 负响应，第二字节是 SID，第三字节是 NRC。
2. 第一字节高 4 位是 0/1/2/3？→ 先按 ISO-TP 去掉帧头，拿到 UDS 消息。
3. UDS 第一字节 → 查服务表。
4. `22` → 后两字节是 DID。
5. `27` → 后一字节是子功能 01/02。
6. `34` → `00 44` 后面是 4 字节地址 + 4 字节总长。
7. `36` → 后一字节是 BSC，再后面是数据。
8. `31` → 后一字节子功能，再后两字节是例程 ID。
9. `19` → 后一字节子功能，再后一字节是状态掩码。
10. 查不到 → 看 `src/core/uds.c` 的 `switch`，再看 `docs/UPDATE_STATUS.md`；仍然没有就问 ECU 诊断规范，不要猜。

---

## 3. 一次刷写的真实报文

下面只保留每条 UDS 消息的第一帧，所以不吓人：

```text
PC  -> 0x7E0  02 10 02
ECU -> 0x7E8  06 50 02 00 32 01 F4

PC  -> 0x7E0  02 27 01
ECU -> 0x7E8  06 67 01 5E E9 79 B1

PC  -> 0x7E0  06 27 02 44 A5 84 36
ECU -> 0x7E8  02 67 02

PC  -> 0x7E0  10 0B 34 00 44 08 01 00
ECU -> 0x7E8  04 74 20 02 00

PC  -> 0x7E0  10 A2 36 01 49 4D 47 32
ECU -> 0x7E8  02 76 01

PC  -> 0x7E0  01 37
ECU -> 0x7E8  01 77

PC  -> 0x7E0  04 31 01 00 01
ECU -> 0x7E8  05 71 01 00 01 00
```

### 3.1 进会话、解锁

```text
02 10 02   ->   06 50 02 00 32 01 F4
```

- `02` = SF 单帧，长度 2；UDS 是 `10 02`。
- `10` = 会话控制，`02` = 编程会话。
- `50 = 10 + 0x40`，正响应。
- `00 32` = P2 = 50 ms；`01 F4` = P2* = 5000 ms。

```text
02 27 01   ->   06 67 01 5E E9 79 B1
06 27 02 44 A5 84 36   ->   02 67 02
```

- `27 01` 请求种子，ECU 回 `67 01` + 4 字节种子。
- `27 02` 送密钥，key 是 4 字节；对了回 `67 02`。
- 这个 key 是演示算法，不是生产密钥。

### 3.2 0x34：我要下载

```text
10 0B 34 00 44 08 01 00
```

FF 首帧，完整请求 11 字节：

```text
34 00 44 08 01 00 00 00 00 00 A0
```

| 字节 | 含义 |
|---|---|
| `34` | RequestDownload |
| `00` | 子功能/保留 |
| `44` | 地址和长度都是 4 字节 |
| `08 01 00 00` | 下载地址 `0x08010000` |
| `00 00 00 A0` | 总长度 160 字节 |

ECU 回应：

```text
04 74 20 02 00
```

- `74 = 34 + 0x40`，正响应。
- `20` = 后面最大块长用 2 字节表示。
- `02 00` = 最大块长 512 字节。

`0x34` 就已经把元数据写成 `DOWNLOADING`，并擦除外部 NOR 的下载区；旧 App 完全没动。

### 3.3 0x36：一块一块传

```text
10 A2 36 01 49 4D 47 32
```

- `10` = FF 首帧。
- `A2` = 162 字节。
- 总长 162 = `2`（SID + BSC）+ `160`（数据）。
- `36` = TransferData。
- `01` = BSC，块号 1。
- `49 4D 47 32` 是 `"IMG2"`，镜像头的 magic。

ECU 回：

```text
02 76 01
```

- `76 = 36 + 0x40`，正响应。
- `01` 回显 BSC，表示“第 1 块收到了”。

请求是 FF，因为 162 字节远大于 7 字节；后面还有一串 CF，把 160 字节数据传完。

### 3.4 0x37：结束并验收

```text
01 37   ->   01 77
```

`77` 只代表“包完整、校验通过、状态变成 READY”，不代表新 App 已经在运行。

### 3.5 0x31：可选校验例程

```text
04 31 01 00 01   ->   05 71 01 00 01 00
```

- `31` = RoutineControl。
- `01` = 启动例程。
- `00 01` = 例程 ID。
- `71 01 00 01 00` = 正响应，例程执行成功。

然后 PC 发 `11 01` 复位，Bootloader 才备份旧 App、擦内部 App、安装、进入 TRIAL、跳转。

---

## 4. 0x36：块号、重复块、幂等

### 4.1 请求格式

```text
36 BSC data...
```

- `BSC` 是块号，8 位，从 1 开始，FF 后允许 00。
- 数据最多 510 字节。
- 正响应 `76 BSC`，负响应 `7F 36 NRC`。

### 4.2 为什么要块号

CAN/ISO-TP 不可靠，`76 01` 这个应答可能丢。PC 不知道 ECU 收没收到，只能重发 `36 01`。

如果没有块号，ECU 分不清：

- 这是新的一车？
- 还是刚才那车又送了一遍？

有了 BSC，ECU 就能判断：

- 块号等于上一块，内容也相同 → 重复，只回 `76`，不再写；
- 块号等于上一块，内容不同 → 同一个块号不能有两种内容，回 `73`；
- 块号是下一块 → 正常新块，写入；
- 块号跳号 → 顺序错了，回 `73`。

### 4.3 代码逻辑

`src/core/uds.c` 的 `handle_transfer_data`：

```c
if (s->dl_has_previous && counter == s->dl_previous_counter) {
    /* 重复块：和 NOR 里已写的内容比较 */
    if (n != s->dl_last_size) return NRC_0x73;
    read back previous bytes;
    if (memcmp(previous, req+2, n) != 0) return NRC_0x73;
    /* 相同：什么都不写，直接回 76 counter */
} else {
    /* 新块：块号必须等于 dl_next_counter */
    if (counter != s->dl_next_counter) return NRC_0x73;
    if (n > remaining) return NRC_0x31;
    write to NOR;
    s->dl_last_offset = s->dl_written;
    s->dl_last_size = n;
    s->dl_previous_counter = counter;
    s->dl_has_previous = 1;
    s->dl_written += n;          /* 只有新块才累加 */
    s->dl_next_counter = counter + 1;
}

/* 统一出口：相同块和新块都走到这里 */
resp[0] = 0x76;
resp[1] = counter;
return 2;
```

关键点：

- **重复块从 NOR 读回比较**，不是从 RAM 猜。NOR 才是真正写进去的东西。
- **`dl_written += n` 只在新块分支里执行**。重复块走不到这里，所以长度不会翻倍。
- **内容不同坚决拒绝**，因为覆盖会破坏镜像，还会让 PC 以为“重试成功”。

### 4.4 实际跑出来的结果

```text
第一次 36 01 数据A          -> 76 01
重复 36 01 数据A            -> 76 01
重复 36 01 数据B            -> 7F 36 73
重复 36 01 只有15字节       -> 7F 36 73
下一块 36 02 数据C          -> 76 02
重复 36 02 数据C            -> 76 02
跳号 36 04                  -> 7F 36 73
```

### 4.5 幂等

同一个请求执行一次和执行多次，结果一样，这叫**幂等（idempotent）**。网络重试、丢 ACK、重复请求，都靠它。`0x36` 的重复块处理就是幂等设计。

---

## 5. 0x37：验收，不是安装

### 5.1 0x37 检查什么

`src/core/uds.c` 的 `handle_transfer_exit`：

1. 当前是不是还在下载状态？
2. `dl_written == dl_size`？所有块都到齐了。
3. 包头长度 + payload + 签名长度，是否等于 `0x34` 声明的总长？
4. `image_validate`：
   - IMG2 magic
   - 版本非 0
   - `image_size` 在 App 区内
   - `load_address == 0x08010000`
   - flags 合法
   - header CRC
   - payload CRC32
   - 向量表：初始 SP、复位入口、Thumb 位、地址范围
5. 如果是签名包：Ed25519 验签。
6. 新版本必须高于 `active_version` 和 `min_version`。
7. 全部通过，才把元数据写成：

```text
state = READY
pending_version = 新版本
pending_size = payload 长度
pending_crc = payload CRC
```

任何一步失败，都不会进 READY，可能回 `NRC 0x72` 或 `NRC 0x31`。

### 5.2 77 不等于新 App 在跑

`77` 只代表：

> 包完整、校验通过、元数据已经持久化 `READY`。

它不代表：

- 新 App 已经装进内部 Flash；
- 新 App 已经跳转运行；
- 新 App 健康。

真正的安装发生在**复位之后**：

```text
0x37 -> 77        状态 READY
0x11 01 -> 复位
Bootloader 读元数据：READY
  -> 备份旧 App
  -> 元数据写 INSTALLING
  -> 擦内部 App
  -> 从 NOR 搬 payload 到 0x08010000
  -> 元数据写 TRIAL
  -> boot_attempts + 1 落盘
  -> 跳转新 App
新 App 运行，健康检查通过
  -> boot_confirm
  -> active = 新版本，min_version = 新版本
  -> state = IDLE
```

所以完整顺序是：

```text
下载完成 ≠ 安装完成 ≠ 运行成功 ≠ 健康确认
```

### 5.3 如果 77 这个应答丢了

PC 没收到 `77`，它**不知道** ECU 到底有没有把包提交成 READY。

这时不能直接重刷，因为包可能已经是 READY，重刷会擦掉一个本来已经好的待安装包。

正确做法：读 `0x0200` 升级状态，对比 `pending_version / pending_size / pending_crc` 和本地镜像。

- 一致且 `state=READY` → 用 `install` 或复位，不重新下载；
- 不一致 → 再决定重新下载。

`tools/pc/uds_client.py` 在 `0x37` 超时时抛 `UnknownCommit`，就是防止“以为失败又重刷”。

---

## 6. 存储：NOR / EEPROM / 内部 Flash / SRAM

### 6.1 四个岗位

| 存储 | 比喻 | 容量 | 掉电 | 主要用途 |
|---|---|---|---|---|
| AT24C02 EEPROM | 小本子 | 256 B | 保留 | 升级状态、版本、CRC、尝试次数 |
| W25Q128 NOR | 大仓库 | 16 MB（项目只用 2 MB） | 保留 | 下载包、旧 App 备份 |
| STM32 内部 Flash | 车间 | 1 MB | 保留 | Bootloader + 当前运行的 App |
| SRAM | 工作台 | 约 128 KB | 丢失 | 会话、`dl_written`、栈、临时变量 |

### 6.2 为什么不能合并成一块

| 特性 | EEPROM | NOR | 内部 Flash |
|---|---|---|---|
| 最小写入 | 字节 / 8 字节页 | 256 字节页 | 扇区 |
| 擦除 | 不需要 | 必须先擦 4 KB 扇区 | 必须先擦 16/64/128 KB 扇区 |
| 寿命 | 约 100 万次 | 约 10 万次 | 约 1 万次 |
| 容量 | 256 B | 16 MB | 1 MB |
| 速度 | 慢（I2C） | 中（SPI） | 快（CPU 直接取指） |

所以：

- **固件包大**：只能放 NOR，EEPROM 256 字节连一帧都放不下。
- **元数据小、改得频繁**：放 EEPROM。每次状态变化只写 64 字节；如果放 NOR，为了 64 字节要擦 4 KB，很快磨坏。
- **App 要执行**：必须放内部 Flash，CPU 才能直接取指令。
- **运行变量**：放 SRAM，掉电丢失也没关系。

### 6.3 最关键的原因：运行中的 App 不能擦自己

App 正在内部 Flash 里跑，PC 这时发来新固件。如果直接往内部 Flash 写：

- 擦除会把自己正在执行的代码擦掉；
- CPU 当场取指失败，程序跑飞；
- 旧 App 也没了，变砖。

所以项目选择：

```text
新固件先写到外部 NOR 的下载区
旧 App 继续在内部 Flash 里跑，完全不受影响
0x37 校验通过，EEPROM 写 READY
复位后，Bootloader 才把 NOR 里的新固件搬进内部 Flash
```

这就是 **staging（暂存）**：先把货放仓库，验收通过再进车间。

### 6.4 外部 NOR 的两个槽

`include/ota/flash_layout.h`：

```text
DOWNLOAD_OFFSET = 0x000000   1 MiB 下载区
BACKUP_OFFSET   = 0x100000   1 MiB 备份区
```

- 下载区：`0x36` 一块块写进去的新包。
- 备份区：安装前把旧 App 复制过来；新 App 试运行失败时，从这里回滚。

板子上的 W25Q128 实际有 16 MB，但项目只规划了前 2 MB 做两个 1 MB 槽。不是芯片只有 2 MB，是项目只用 2 MB。

### 6.5 EEPROM 的两份记录

```text
META_A_OFFSET = 0     64 字节
META_B_OFFSET = 128   64 字节
```

一张卡存：

```text
state、active_version、active_size、active_crc
pending_version、pending_size、pending_crc
boot_attempts、min_version
```

存两份 A/B，加 CRC 和序号。写到一半拔电，至少还有一份旧记录是完整的。这就是为什么 `0x37` 回 `77` 后立刻拔电也不丢 READY。

### 6.6 SRAM 丢了没关系

`dl_written`、`dl_active`、`unlocked`、当前会话都在 SRAM。拔电就没了，但没关系：

- 下载没完成 → 重新走 `0x34`；
- 旧 App 还在 → 不影响开车；
- 真正不能丢的状态，早就写进 EEPROM 了。

### 6.7 0x35 是什么

`0x35` 是 **RequestUpload**，和 `0x34` 方向相反：

| 服务 | 方向 | 用途 |
|---|---|---|
| `0x34` | 测试仪 → ECU | 下载：我要把固件传给你 |
| `0x35` | ECU → 测试仪 | 上传：我要从你这里读数据 |
| `0x36` | 两个方向都用 | 传输数据；方向由前面的 `0x34` 还是 `0x35` 决定 |
| `0x37` | 两个方向都用 | 传输结束 |

这个 OTA 项目只做“下载”，不做“上传”，所以 `0x35` 没实现。实测：

```text
0x35 -> 7F 35 11 (NRC 11)
```

---

## 7. 元数据 A/B：原子提交

### 7.1 一份记录长什么样

每份 64 字节：

```text
magic, seq, state,
active_version, active_size, active_crc,
pending_version, pending_size, pending_crc,
boot_attempts, min_version,
reserved[3], crc, commit
```

- `magic`：格式标记，必须是 `MAT2`。
- `seq`：记录序号，越大越新。
- `crc`：对前 56 字节算的 CRC32。
- `commit`：**生效章**，必须是 `0xA55A5AA5`，写在最后。

`read_copy` 判定一份有效，要同时满足：

1. `magic == MAT2`
2. `commit == 0xA55A5AA5`
3. `crc` 对得上
4. `sane()` 语义检查通过

少一个都算无效。

### 7.2 读的时候选哪份

| 情况 | 结果 |
|---|---|
| A 有效，B 无效 | 用 A |
| A 无效，B 有效 | 用 B |
| 两份都有效，`seq` A 大 | 用 A |
| 两份都有效，`seq` B 大 | 用 B |
| 两份都有效，`seq` 相同且内容完全相同 | 用 A |
| 两份都有效，`seq` 相同但内容不同 | 报错，不猜 |
| 两份都无效 | 返回 0：没有有效记录，进 Loader |
| 任意一份读失败 | 报错，不写 |

最后一条很重要：**只要有一份读不出来，就不写**。因为你不知道那份是不是更新的，贸然覆盖可能把唯一的新记录毁掉。

### 7.3 `seq` 回绕怎么比

`seq` 是 32 位，会从 `0xFFFFFFFF` 回到 `0`。不能直接比大小。

代码用无符号差值：

```c
delta = a.seq - b.seq;                  // uint32_t，自动模 2^32
if (delta == 0x80000000u) return -1;     // 正好半圈，无法判断
index = (delta < 0x80000000u) ? 0 : 1;   // 小于半圈选 A，否则选 B
```

- `delta < 0x80000000` → A 新；
- `delta > 0x80000000` → B 新；
- `delta == 0x80000000` → 报错，不猜。

例子：

| A 的 seq | B 的 seq | 谁新 | 原因 |
|---|---|---|---|
| `5` | `6` | B | 差 1，B 在后面 |
| `0xFFFFFFFF` | `0` | B | A 再加 1 就回绕到 0 |
| `0` | `0xFFFFFFFF` | A | B 再加 1 就是 A |
| `0x7FFFFFFF` | `0x80000000` | B | 差 1，B 在后面 |
| `0x80000000` | `0` | 报错 | 正好半圈，无法判断 |

一句话：`seq` 是环形序号，比的是“谁更靠近现在”，不是“谁的数值更大”。

### 7.4 写的时候为什么不会两边都坏

`metadata_save` 的顺序：

```text
1. 读 A/B，找到当前有效、seq 最大的那份
2. 目标 = 另一份（永远不写当前有效的那份）
3. 把目标的 commit 写 0，先作废
4. 写目标的前 60 字节：正文 + CRC
5. 写目标的 commit = 0xA55A5AA5，最后生效
6. 读回目标 64 字节，和要写的内容完全一致，才算成功
```

时间线：

```text
源记录 A：一直不动，始终有效
目标记录 B：
  commit = 0        <- 先作废
  写 body + CRC
  commit = 生效章   <- 最后一步
  读回校验
```

拔电位置与结果：

| 拔电位置 | 目标 B | 源 A | 下次上电 |
|---|---|---|---|
| 写 commit=0 之前 | 有效 | 有效 | 选 seq 大的 |
| 写 commit=0 之后 | 无效 | 有效 | 用 A |
| 写 body 中途 | 无效 | 有效 | 用 A |
| 写 commit 中途 | 可能无效/有效 | 有效 | 用 A，或 commit 完整则用 B |
| 写完全部、读回前 | 有效 | 有效 | 选 seq 大的 B |

结论：**电断在任何一步，至少有一份是完整的。**

### 7.5 为什么 commit 要单独写在最后

因为 CRC 只能说明“正文没坏”，不能说明“这条记录已经写完了”。

- 正文写了一半，CRC 字段可能还是旧的，整条记录可能看起来有 CRC；
- `commit` 是一个独立的 4 字节“生效章”，只在正文和 CRC 都写完后才写。

所以一条记录有效 = **magic 对 + commit 对 + CRC 对 + sane 通过**。这是四道门，不是只看 CRC。

### 7.6 `sane()` 还在防什么

CRC 对了，不代表内容合理。`sane()` 会检查：

- `state` 在 0..RESTORING 范围内；
- `active_size`、`pending_size` 不超过 App 区；
- `boot_attempts` 不超过 3；
- `reserved` 必须为 0；
- Active 为空时，版本/CRC/min_version 必须为 0；
- IDLE/DOWNLOADING 时，pending 字段必须为 0；
- 其他状态，`pending_version` 必须大于 `min_version` 和 `active_version`；
- RESTORING 时必须有 Active App。

这是防止“CRC 正确但语义荒唐”的记录，比如状态是 READY 但 `pending_size=0`。

---

## 8. 状态机：READY → INSTALLING → TRIAL → RESTORING

| 状态 | 含义 | 谁会写 | 下一步 |
|---|---|---|---|
| `IDLE` | 正常，无待安装包 | Bootloader / `boot_confirm` | 正常运行 |
| `DOWNLOADING` | 正在下载，包未完整 | `0x34` | `0x37` 成功 → READY |
| `READY` | 包完整、已验收，等待复位 | `0x37` | 复位后 Bootloader → INSTALLING |
| `INSTALLING` | 正在备份旧 App / 安装新包 | Bootloader | 成功 → TRIAL；失败/断电 → 重做 |
| `TRIAL` | 新 App 试运行 | Bootloader | 健康确认 → IDLE；失败/超次数 → RESTORING |
| `RESTORING` | 从备份区恢复旧 App | Bootloader | 恢复完成 → IDLE |

关键点：

- `READY` 在 EEPROM 里，不在 SRAM；拔电不丢。
- `INSTALLING` 在 EEPROM 里；安装到一半拔电，下次上电重做。
- `TRIAL` 的 `boot_attempts` 在跳转前写入；拔电也算一次尝试。
- `boot_confirm` 只有在新 App 有效、`state=TRIAL`、`boot_attempts != 0` 时才把 `active` 更新为新版本，然后 `state=IDLE`。
- `boot_select` 如果发现 `READY` 记录和 NOR 包头对不上，会清 pending，回到 IDLE，启动旧 App；没有旧 App 就停在 Loader。

---

## 9. 面试可用的 10 句话

1. UDS 服务号来自 ISO 14229；项目只实现一个子集，`uds.c` 的 `switch` 是“支持清单”。
2. `0x22` 是 ReadDataByIdentifier；后面两字节是 DID，`F195` 是软件版本。
3. `0x34` 声明下载地址和总长，返回最大块长；本项目是 512 字节。
4. `0x36` 一次搬一块，BSC 是块号；重复块相同只回 `76`，不同回 `73`。
5. `0x36` 的重复块处理是幂等设计，防止丢 ACK 后重发导致重复写入。
6. `0x37` 不是“收到”，是“整包验收”；`77` 只代表 READY，不代表安装完成。
7. 安装发生在复位后的 Bootloader；`READY → INSTALLING → TRIAL → RESTORING` 由 EEPROM 状态驱动。
8. NOR 存固件包和备份，EEPROM 存 64 字节元数据，内部 Flash 存可执行 App，SRAM 存临时变量。
9. 元数据 A/B 两份轮流写：先作废目标 commit，再写正文+CRC，最后写 commit，读回校验。
10. `seq` 是环形序号，用无符号差值和半圈判断；`0xFFFFFFFF` 之后是 `0`，不是数值越大越新。

---

## 10. 待继续

- `0x36` 发送中途断电，PC 端如何判断“包可能已 READY”？
- `0x37` 应答丢失时，`UnknownCommit` 的完整恢复流程。
- `TRIAL` 回滚的边界：备份区校验失败怎么办？
- 真实板子上的 `0x78` pending 与 1 MiB NOR 擦除时间。
- 签名包与未签名包的 flags 差异。
