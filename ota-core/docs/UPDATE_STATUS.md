# 只读升级状态 DID 0x0200

这是本工程自定义数据格式，使用 `ReadDataByIdentifier`，请求为 `22 02 00`。不要求解锁；查询不改变版本、升级状态、启动次数或元数据序号。不要把该 DID 当作通用 ECU 的标准诊断标识。

正应答为 `62 02 00` 加 44 字节状态记录，多字节字段均为大端。PC 工具可以直接解析：

~~~powershell
python tools/uds_tool.py status --cc "你的 C 编译器完整路径"
python tools/uds_tool.py status --backend can --interface gs_usb --channel "你的实际通道" --index 0
~~~

| 记录偏移 | 长度 | 含义 |
|---|---|---|
| 0 | 1 | 格式版本，当前为 1 |
| 1 | 1 | 元数据有效标志，0 或 1 |
| 2 | 1 | 状态：0 IDLE、1 DOWNLOADING、2 READY、3 INSTALLING、4 TRIAL、5 RESTORING |
| 3 | 1 | 已持久化的试启动次数 |
| 4 | 4 | active_version |
| 8 | 4 | pending_version |
| 12 | 4 | min_version |
| 16 | 4 | active_size，payload 字节数 |
| 20 | 4 | pending_size，payload 字节数 |
| 24 | 4 | active_crc32 |
| 28 | 4 | pending_crc32 |
| 32 | 4 | metadata_sequence |
| 36 | 4 | 编译配置的内部 Flash 总字节数 |
| 40 | 1 | `programming_allowed`：此刻写服务是否放行 |
| 41 | 1 | `job_error`：上一个长作业是否失败收尾 |
| 42 | 1 | `pending_in_flight`：总线上还有一个 `7F xx 78` 未发完 |
| 43 | 1 | bit7 = 本镜像是 App（0 表示 Bootloader）；bit0-6 = 服务间隔超限次数 |

容量字段来自固件配置；STM32 存储端口还必须独立验证实际芯片容量。状态查询不会代替这一步。

## 为什么末尾多了 4 个字节

写服务被拒时 `0x34/0x36/0x37/0x31` 一律回 NRC `0x22`，而"条件不满足"有两个完全不同的原因：

1. **按构建关闭**：App 编译时 `OTA_ENABLE_FLASHING=0`，它只负责把下一次复位交给 Bootloader。此时 `0x22` 是正确答复，不是缺陷。
2. **按故障关闭**：Bootloader 的长作业错过服务间隔或链路失败，门控被关掉一次。

这两种情况在总线上一个字节都不差。第 40 字节和 43 字节的 bit7 就是用来在不猜的前提下分开它们：`programming_allowed=0` 且 `role=application` 属于第 1 种；`role=loader` 却 `programming_allowed=0` 才是第 2 种。PC 工具把两者分别输出为 `programming_allowed` 与 `role`。

40 字节的老记录仍能解析：PC 侧把这一版回答记为 `record_bytes: 40`、`role: null`，用于识别还没更新的板子。

两份元数据都没有有效记录时返回有效标志 0、其他元数据字段为 0，仍提供容量。读设备发生错误时返回 NRC `0x72`；不会把读失败伪装成空白元数据。

该记录只反映通过元数据格式、CRC 和语义校验的数据，不重新遍历 App 或外部镜像做 payload CRC 校验，也不能证明 App 正常运行。PC 输出 `metadata_only: true` 明确这一点。真正安装与跳转前仍由 Bootloader 校验镜像。

## 提交应答丢失后

如果 `0x37` 超时，包可能已持久化成 READY。先读取状态，并与本地 `inspect` 得到的版本、payload 长度和 CRC 对比：

- READY 且 pending 字段一致：设备记录了该待安装包；此时不能当作“下载失败”而重新擦写。后续仍应验证镜像，再决定是否复位安装。
- TRIAL：已经进入试运行阶段，只有运行中的 App 可以确认健康。
- IDLE：需要看 active 字段判断当前确认版本，不能只看状态名。
- DOWNLOADING、无有效元数据或读失败：保留现场记录，结合恢复流程判断。

工具不会自动把状态查询结果转换成复位、擦除或健康确认。仿真器每次 CLI 启动都重新创建 RAM 存储；跨升级场景请使用 `demo_roundtrip.py`，其中已通过 CAN 查询这些状态。

STM32 调度器在存储已就绪时也为这类 EEPROM 读取提供 `0x78`，即使写入门控关闭，也可查询状态。BSP 的服务间隔约束仍适用。
