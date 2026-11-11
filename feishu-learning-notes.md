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

## 11. UDS 下载主线：0x34 / 0x36 / 0x37 / 0x31

整个 OTA 分两段，本章讲第一段：PC 把固件包安全送进 NOR。

```text
第一段：PC → CAN → ISO-TP → UDS(0x34 / 0x36 / 0x37) → NOR 下载区
第二段：复位 → Bootloader → 备份 → 安装 → TRIAL → 确认 → IDLE
```

分两段的根本原因：App 正在内部 Flash 里运行，不能擦自己。

### 11.1 四个服务各管一段

| SID | 干什么 | 一句话记忆 |
|---|---|---|
| `0x34` | 登记 | **只宣布，不传数据**；先落盘 DOWNLOADING，再擦 NOR |
| `0x35` | 上传（未实现） | 发过去得到 `0x11` 服务不支持 |
| `0x36` | 搬字节 | 只把字节写进 NOR，**不解包、不算 CRC** |
| `0x37` | 验收 | 重读包头 + 整包校验，成功后落盘 READY |
| `0x31` | 复核 | 核对 NOR 里的包和 `pending_*` 一致，**不是健康确认** |

### 11.2 0x34：五道门，先落盘再擦

```c
if (len != 11u || req[1] != 0u || req[2] != 0x44u)  → 0x13
if (s->session != UDS_SESSION_PROGRAMMING)          → 0x22
if (!s->unlocked)                                   → 0x33
if (s->dl_active)                                   → 0x24
addr = get_be32(req+3); size = get_be32(req+7);
if (addr != APP_BASE_ADDR || size<IMAGE_HEADER_SIZE+8u || size>上限) → 0x31
```

请求固定 11 字节：

```text
34 00 44 | 08 01 00 00 | 00 00 05 DC
SID 子功能   地址(4B)      大小(4B)
    └─┬─┘
   00 44 = 不压缩、不加密
```

地址是**逻辑执行地址**，不是写入地址：数据存到 NOR 的 `DOWNLOAD_OFFSET`，复位后由 Bootloader 搬到内部 Flash 的 `0x08010000`。

顺序不能反：

```c
m.state = UPDATE_DOWNLOADING;
/* Invalidate the old staged package durably BEFORE any erase. */
if (metadata_save(store, &m) ||
    external.erase(DOWNLOAD_OFFSET, EXT_IMAGE_SLOT_SIZE)) → 0x72
```

先落盘的意义：擦到一半掉电，下次上电看到 `DOWNLOADING`，不会误以为有完整包。

正响应 `74 20 02 00`：

```text
74       = 0x34 + 0x40
20       = lengthFormatIdentifier：高 4 位 2 表示「最大块长度」占 2 字节
02 00    = 512
```

512 算的是**整个 0x36 请求的长度**，含 SID 和块号，所以纯数据最多 510。

### 11.3 0x36：重复块只答复，不重复累加

```c
counter = req[1]; n = len - 2;

if (dl_has_previous && counter == dl_previous_counter) {
    /* 疑似重传 */
    if (n != dl_last_size)                    → 0x73
    if (read(offset + dl_last_offset, previous, n)) → 0x72
    if (memcmp(previous, req+2, n))           → 0x73
    /* 一模一样：什么也不做，直接回 76 */
} else {
    if (counter != dl_next_counter)           → 0x73
    if (n > dl_size - dl_written)             → 0x31
    write(offset + dl_written, req+2, n);
    dl_last_offset = dl_written; dl_last_size = n;
    dl_previous_counter = counter; dl_has_previous = 1;
    dl_written += n;                          /* ← 只有这里累加 */
    dl_next_counter = counter + 1;            /* FF → 00 合法 */
}
```

1500 字节走一遍：

| 请求 | 写在哪 | dl_written | next |
|---|---|---|---|
| `36 01` + 510 | offset + 0 | 510 | 2 |
| `36 02` + 510 | offset + 510 | 1020 | 3 |
| `36 03` + 480 | offset + 1020 | 1500 | 4 |

重传时块号等于 `dl_previous_counter`，从 NOR 读回上一块逐字节比对，一致就回 `76`，`dl_written` 和 `dl_next_counter` 都不变。

为什么必须读 NOR 来比：

- 没法区分「重传」和「同号不同内容」，只能比内容
- 省 RAM：没有常驻 510 字节缓存，NOR 里本来就有一份
- 顺带验了上一块写得对不对

跳号一律 `0x73`：只认「我期待的下一个」和「我刚收过的那个」。

长度检查用减法不用加法，避免 uint32 溢出：

```c
if (n > dl_size - dl_written) → 0x31    /* 安全 */
/* 而不是 dl_written + n > dl_size */
```

### 11.4 长度不对的四种情况

| 情况 | 判定 | 返回 |
|---|---|---|
| 单帧太短（连块号都没有） | `len < 3` | `0x13` |
| 单帧超过协商块长 | `len > 512` | `0x13` |
| 累计超过声明总长 | `n > dl_size - dl_written` | `0x31` |
| 一直没收够 | `0x36` 不判断，到 `0x37` 才查 | `0x24` |

为什么「累计超过总长」不是 `0x13`：`0x13` 是这一帧本身的格式问题，`0x31` 是格式没问题但数值超出声明范围。

为什么 `0x36` 不判断「收完了没」：它回答不了「后面还有没有」。只有 PC 发 `0x37` 说「我传完了」，服务器才有资格判断「你传完了但我没收齐」。

### 11.5 0x37：七步验收

```c
if (len != 1u)                            → 0x13
if (session != PROGRAMMING)                → 0x22
if (!unlocked)                             → 0x33
if (!dl_active || dl_written != dl_size)   → 0x24

/* ① 重读包头 */
if (image_read_header(DOWNLOAD_OFFSET, &h) ||
    /* ② 长度自洽：payload + 头 + 签名 = 声明总长 */
    h.image_size + IMAGE_HEADER_SIZE +
        ((h.flags & IMAGE_FLAG_SIGNED) ? OTA_SIGNATURE_SIZE : 0u) != dl_size ||
    /* ③ 整包校验：从 NOR 读回来算 */
    image_validate(DOWNLOAD_OFFSET, 0, 0))  → 0x72

/* ④ 状态必须还在下载中 */
if (metadata_load(store, &m) != 1 || m.state != UPDATE_DOWNLOADING) → 0x72

/* ⑤ 版本必须严格更高 */
if (h.version <= m.min_version || h.version <= m.active_version)   → 0x31

/* ⑥ 落盘 READY */
m.state = READY; m.pending_version = h.version;
m.pending_size = h.image_size; m.pending_crc = h.payload_crc;
m.boot_attempts = 0;
if (metadata_save(store, &m))               → 0x72

/* ⑦ 清下载态，回 77 */
s->dl_active = 0u;
```

- 必须重读包头：`0x36` 全程只把字节原样写进 NOR，没解析过包头，服务器此刻不知道 NOR 里是什么
- 必须验 NOR 而不是收到的字节：`image_validate` 从 NOR 读回来算，能发现 NOR 写坏
- `77` 是安全点：READY 已进 EEPROM，之后断电重启由 Bootloader 自动安装
- `77 ≠ 装好了`：内部 Flash 里还是旧 App
- 版本必须**严格更高**，同版本重刷也拒绝

### 11.6 0x31：只复核，不确认健康

```c
if (len != 4u)                                  → 0x13
if ((req[1] & 0x7Fu) != 1u)                     → 0x12
if (req[2] != 0u || req[3] != 1u)                → 0x31
if (session != PROGRAMMING || !unlocked)         → 0x33
if (metadata_load(store,&m) != 1 || m.state != UPDATE_READY) → 0x22
if (h.version != m.pending_version ||
    h.image_size != m.pending_size ||
    h.payload_crc != m.pending_crc ||
    image_validate(DOWNLOAD_OFFSET,0,0))         → 0x72
resp = 71 01 00 01 00
```

做的事只有一件：**复核 NOR 里的包和 `pending_*` 是否仍然一致**。

```c
/* Routine 0001 verifies a READY package. Health confirmation belongs
 * to the running app; a tester cannot confirm a package before boot. */
```

`0x31` 不是健康确认。健康确认只能由运行中的 App 调用 `boot_confirm`，诊断仪没有这个权力。

### 11.7 为什么不在 0x36 里算 CRC

1. 算的是**收到的字节**，不是**写进 NOR 的字节**。NOR 写入失败时这个 CRC 照样是对的，要发现写失败只能回读
2. 传输错误 CAN 层已经管了：15 位 CRC + ACK 管单帧，ISO-TP 序号管丢帧乱序，UDS 块号管丢块重复块
3. 包头里的 `payload_crc` 是整包的，最后一块到齐前算不出来；中间某块错了也不能只补那一块

### 11.8 三个够用的 NRC

| NRC | 什么时候出现 |
|---|---|
| `0x24` | 序列错：没开下载就发 `0x36`；没传完就发 `0x37` |
| `0x73` | 块号错：既不是期待的下一块，也不是刚收过的那个 |
| `0x31` | 范围错：地址不对、大小越界、版本不够高 |

---

## 12. 会话、解锁与复位：0x10 / 0x27 / 0x11

### 12.1 0x10 会话控制

| 值 | 会话 | 能干什么 |
|---|---|---|
| `01` | 默认 | 只能读 DID。车辆正常运行时就是这个状态 |
| `02` | 编程 | 只有在这里 `0x34 / 0x36 / 0x37 / 0x31` 才被允许 |
| `03` | 扩展 | 本项目基本没用 |

```c
s->session    = sub;
s->unlocked   = 0u;      /* 重新上锁 */
s->seed_valid = 0u;
s->seed       = 0u;
s->dl_active  = 0u;      /* 下载任务作废 */
```

**每换一次会话，权限都归零，必须重新 27。**

正响应 `50 02 00 32 01 F4`：P2 = 50 ms，P2\* = 500 × 10 ms = 5000 ms。

判断通过要看「期望的正响应回来了没有」，不能只看「不是 `7F`」——`50 03`、`50 01` 也不是 `7F`，但那是别的会话。

### 12.2 0x27 安全访问

```text
27 01  →  67 01 [4 字节种子]     我给你一个随机数
            （PC 用同样的算法算出 key）
27 02 [4 字节 key] → 67 02        钥匙对，解锁
```

```c
if (s->unlocked) { 返回种子 0 }          /* 种子 0 表示已经解锁 */

s->seed = 0xC0DE0000u ^ (s->request_count * 2654435761u);
if (s->seed == 0u) s->seed = 0x12345678u;

/* 27 02 */
if (!s->seed_valid) → 0x24
if (key != uds_calc_key(s->seed)) {
    s->seed_valid = 0u;                  /* 一次机会，错了种子作废 */
    s->key_failures++;
    if (s->key_failures >= 3u) { 锁 10 秒; → 0x36 }
    → 0x35
}
```

钥匙算法：

```c
uint32_t k = seed ^ 0xA5A5A5A5u;
k = (k << 7) | (k >> 25);
k += 0x9E3779B9u;
```

说实话：种子由 `request_count` 算出，可预测，密钥也在 App 里，所以它只是协议形状，不是真正的安全边界。真实产品用硬件随机数或 HSM。

### 12.3 0x11 复位：只记账，不当场复位

```c
if (sub != 0x01 && sub != 0x03) → 0x12
s->reset_requested = 1u;
resp[0] = 0x51u; resp[1] = sub;
```

真正复位要等两件事都干净：ISO-TP 多帧响应发完、CAN 硬件发送邮箱真的空了。否则 `51 01` 会残缺或丢失，PC 会以为复位失败而重试。

### 12.4 硬件复位 vs 断电

| | 硬件复位 | 断电 |
|---|---|---|
| RAM | 清空 | 清空 |
| EEPROM / NOR / 内部 Flash | 保留 | 保留 |
| 发生时机 | 软件主动请求，时刻确定 | 随时可能，可能停在任意指令中间 |
| 正在进行的写入 | 那一刻没有写入在进行 | 可能正好赶上分页写或扇区擦，撕裂写 |
| 电源与时钟 | 稳定 | 电压跌落过程不确定 |

一句话：断电是「无序停机」，复位是「有序重启」。

### 12.5 复位之后：READY → INSTALLING → TRIAL → IDLE

```text
Bootloader 启动 → 看到 READY
  ↓
create_backup      旧 App：内部 Flash → NOR 备份区（只读内部 Flash）
  ↓
set_state(INSTALLING)               ← 备份完成之后才改状态
  ↓
install()          擦内部 Flash，把新包从 NOR 下载区搬进去
  ↓
set_state(TRIAL)   装好了，但还没被信任
  ↓
boot_attempts++ 落盘 → 跳新 App
  ↓
新 App 自检通过 → boot_confirm → IDLE
  ↓
3 次起不来 → RESTORING → 从备份区把旧版搬回来
```

三个容易记反的点：

1. **READY 是 `37` 那一刻落盘的**，不是复位后进入的
2. **备份在 INSTALLING 之前**，且在 READY 状态下完成，只读内部 Flash
3. **装完是 TRIAL 不是 IDLE**，要 App 自己调 `boot_confirm` 才变 IDLE（`boot_attempts == 0` 说明还没真跑过）

关键结论：`boot_select()` 只在 Bootloader 里调用，App 侧只调 `boot_accept_app_health()`。**App 里根本没有备份和安装的代码，所以这个包永远不会自己装上去，必须复位。**

### 12.6 dl_active：只有下载阶段才需要的开关

它是 RAM 里的一个字节，回答的只有一句话：**「现在有没有一个已经打开、但还没结算的下载单？」**

全部 7 处读写：

| 行号 | 位置 | 代码 | 作用 |
|---|---|---|---|
| 99 | `0x10` 切会话 | `dl_active = 0` | 换会话就作废 |
| 293 | `0x34` 门槛 | `if (dl_active) → 0x24` | 防重复开单 |
| 310 | `0x34` 成功 | `dl_active = 1` | 打开窗口 |
| 330 | `0x36` 门槛 | `if (!dl_active) → 0x24` | 没开单不准传 |
| 363 | `0x37` 门槛 | `if (!dl_active) → 0x24` | 没开单不准验收 |
| 378 | `0x37` 成功 | `dl_active = 0` | 结算关窗 |
| 442 | S3 超时 | `dl_active = 0` | 太久不说话就作废 |

`0x27` / `0x11` / `0x31` / Bootloader 都不碰它。

为什么只有下载需要：

1. 下载是唯一被外部请求切碎的阶段，每条 `36` 都要判断「该不该执行」
2. 下载有一堆跨请求存活的零碎状态（`dl_offset` / `dl_written` / `dl_next_counter` …），`dl_active` 是总开关
3. 下载能被打断（切会话、S3 超时），后面三个阶段是 Bootloader 顺序执行，天然不可中断

`0x37` 里两个条件不是一回事：

```text
!dl_active              → 「你压根没开单」     前提不存在
written != size         → 「你开单了但没干完」 任务未完成
```

### 12.7 六个状态值与全部合法转换

| 值 | 名称 | 谁写的 | 什么时候写 |
|---|---|---|---|
| 0 | `IDLE` | `clear_pending()` / `boot_confirm()` | 没有待装包；或新版本已确认 |
| 1 | `DOWNLOADING` | `0x34` | 开始下载 |
| 2 | `READY` | `0x37` | 验收全过 |
| 3 | `INSTALLING` | `boot_select()` | 备份完成之后 |
| 4 | `TRIAL` | `boot_select()` | 安装完成之后 |
| 5 | `RESTORING` | `boot_select()` | 新包不合格，或试运行失败 |

| 从 | 到 | 触发条件 | 代码 |
|---|---|---|---|
| `IDLE` / `DOWNLOADING` | `DOWNLOADING` | `0x34` 通过五道门 | uds.c:305 |
| `DOWNLOADING` | `READY` | `0x37` 七步全过 | uds.c:375 |
| `READY` | `IDLE` | 包读不出来 / 头部对不上 | boot.c:96 |
| `READY` | `INSTALLING` | 备份成功 | boot.c:104 |
| `INSTALLING` | `TRIAL` | `install()` 成功 | boot.c:118 |
| `INSTALLING` | `RESTORING` | 包不合格但备份可用 | boot.c:115 |
| `TRIAL` | `IDLE` | `boot_confirm()` 成功 | boot.c:157 |
| `TRIAL` | `RESTORING` | 3 次起不来或镜像校验失败 | boot.c:129 |
| `RESTORING` | `IDLE` | 回滚完成 | boot.c:141 |

掉电之后：

| 掉电时的 state | 重启后 Bootloader 怎么做 |
|---|---|
| `IDLE` | 直接跳旧 App |
| `DOWNLOADING` | 直接跳旧 App（半截包不管，下次 `0x34` 会擦） |
| `READY` | 备份 → 安装 |
| `INSTALLING` | 备份作废重做 → 安装（不续传） |
| `TRIAL` | `boot_attempts+1` → 再试；到 3 次回滚 |
| `RESTORING` | 重新回滚一遍 |

`DOWNLOADING` 掉电可以「跳旧 App 不管」，因为内部 Flash 完好；`INSTALLING` 掉电不行，因为内部 Flash 可能已被擦掉一半，旧 App 不在了，必须靠备份区重建。

### 12.8 S3 超时清哪四样

```c
s->session    = UDS_SESSION_DEFAULT;
s->unlocked   = 0u;
s->seed_valid = 0u;
s->dl_active  = 0u;
```

四层关系：

```text
session    ─ 你在哪个房间
unlocked   ─ 你有没有钥匙
seed_valid ─ 手上那张临时通行证过没过期
dl_active  ─ 柜台有没有给你开着单
```

**没有清 `state`，也不该清。** S3 超时只说明「客户端不说话了」，不能据此推断「包坏了」。

所以会出现「不同步」，而这是刻意设计的：

```text
S3 超时后：
  dl_active = 0           这次连接没戏了
  state     = DOWNLOADING 设备仍处在「可能有个半截包」的事实里
```

`state = DOWNLOADING` 本来就不承诺包完整，所以遗留它是无害且自洽的。

---

## 13. 面试可用的 10 句话

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

## 14. 待继续

- `0x36` 发送中途断电，PC 端如何判断“包可能已 READY”？
- `0x37` 应答丢失时，`UnknownCommit` 的完整恢复流程。
- `TRIAL` 回滚的边界：备份区校验失败怎么办？
- 真实板子上的 `0x78` pending 与 1 MiB NOR 擦除时间。
- 签名包与未签名包的 flags 差异。
