# USART1 串口台架：先用串口跑通 UDS，再接 CAN

适用版本：v0.14 工作区。状态：**代码与主机测试完成；尚未在实物板上执行**。

这一阶段只做一件事：证明 `UDS / ISO-TP / Bootloader / 存储核心` 与物理链路解耦。串口不是“另一个 OTA 实现”，而是把同一份经典 CAN 帧编码后送过 USART1；CAN 到货后只替换 link，ISO-TP、UDS、状态机和用例不变。

## 1. 为什么可以先不用 CAN

`src/core/` 中没有任何 CAN 驱动依赖：

- `uds.c` 接收的是完整 UDS 请求字节；
- `isotp.c` 接收的是 `can_frame_t`；
- `boot.c` 只通过 `flash_port_t` / `eeprom_port_t` 访问存储。

平台层新增 `ota_stm32_link_t`：

```c
typedef struct {
    ota_port_t port;                 /* can_send + now_ms */
    int (*poll)(void *user);         /* drive TX / timeouts */
    int (*pop)(void *user, can_frame_t *frame);
    int (*idle)(const void *user);   /* queue + hardware idle */
} ota_stm32_link_t;
```

`stm32_can.c` 和 `stm32_uart.c` 各自实现同一个 link。`stm32_diag.c` 只调用 link 操作，不再直接引用 CAN 队列。

## 2. 串口线格式

实现位于 `include/ota/can_uart.h` / `src/core/can_uart.c`，PC 侧镜像在 `tools/pc/transport.py`：

```
A5 5A | ver_flags | dlc | id[4 大端] | data[dlc] | crc16[2 大端]
```

- `ver_flags` 低 7 位 = 1，bit7 = 扩展帧；
- CRC-16/CCITT-FALSE，覆盖 `ver_flags` 到最后一个数据字节；
- 单帧长度 = 10 + dlc，最大 18 字节；
- 接收端在字节流中搜索 `A5 5A`，CRC 错误整帧丢弃并重新同步；
- 这是**台架协议**，不是车载总线协议，不可用于量产。

示例（标准帧 `0x7E8`，数据 `00..07`）：

```
A5 5A 01 08 00 00 07 E8 00 01 02 03 04 05 06 07 8D 70
```

## 3. 构建 UART 只读诊断目标

默认工程仍是 CAN 诊断；生成 UART 变体时替换两个 diagnostics 目标：

```bash
python tools/generate_explorer_mdk.py \
  --dfp  <STM32F4xx_DFP/2.16.0> \
  --cmsis <CMSIS/5.0.1/CMSIS/Include> \
  --uart
```

生成后的四个目标：

| 目标 | 行为 |
|---|---|
| `explorer_ZG_loopback_READONLY` / `ZE` | 静默内部 CAN 回环，保持原样 |
| `explorer_ZG_uart_READONLY` / `ZE` | Bootloader 诊断主循环，USART1 二进制传输 |

UART 目标仍然 `OTA_ENABLE_FLASHING=0`、`storage_verified=0`。因此这一阶段只能跑 `info` / `status`，`0x34/0x36/0x37/0x31` 会返回 NRC `0x22`。它验证传输层和诊断链路，不验证真实擦写。

GNU 构建（需要 arm-none-eabi 工具链与 DFP）：

```bash
python tools/build_explorer_gnu.py --gcc <arm-none-eabi-gcc> --dfp <DFP> --cmsis <CMSIS>
```

把 `explorer_ZG_uart_READONLY.bin` 下载到 `0x08000000`。先确认芯片容量与目标一致。

## 4. 接线

| 板卡 | USB-TTL |
|---|---|
| USART1 TX `PA9` | RXD |
| USART1 RX `PA10` | TXD |
| GND | GND |

- 115200、8-N-1；
- 共地；不要接 CAN；
- 板上 `USB_232` 已经连到 USART1，也可以直接用板载 CH340；
- 上电前确认 `BOOT0` 为低，否则不会从 Flash 启动。

## 5. PC 侧

```bash
pip install -r requirements-serial.txt

python tools/uds_tool.py info \
  --backend uart --serial-port COM5 --baud 115200

python tools/uds_tool.py status \
  --backend uart --serial-port COM5 --baud 115200
```

预期：

- `info` 返回 `F195` 软件版本、`F190` VIN、`F187` 零件号；
- `status` 返回 40 字节升级状态；无有效元数据时有效标志为 0；
- 启动早期的文本日志会被 PC 解码器忽略；`ota_bsp_uart_binary(1)` 之后 BSP 不再输出文本日志。

如果 `info` 超时：

1. 先交换 TX/RX 再试；
2. 确认共地、115200、8N1；
3. 确认 `BOOT0` 低、固件确实烧到 `0x08000000`；
4. 用逻辑分析仪或示波器看 `PA9` 是否有周期性帧；
5. 若串口收到可读文本而不是二进制，说明烧的是 CAN 诊断目标或 `OTA_DIAG_UART` 没打开。

## 6. 这一阶段证明了什么

| 项 | 状态 |
|---|---|
| UART 编解码与 CRC | 主机 C 回归：帧/噪声/坏 CRC/扩展帧/非法长度 |
| UART link 与 diag 状态机 | STM32 适配器主机模型：请求 → ISO-TP → UDS → 应答 |
| PC 串口后端 | Python 单元测试：与 C 侧同一字节向量 |
| 真实 USART1 收发 | **未做** |
| 真实擦写 / 跳转 / 断电 | **未做** |

## 7. 下一步：写使能 bench

当前 `explorer_reference` 是只读 BSP，`ota_user_nor_*` 返回 `-1`，`storage_verified=0`。要跑真实 V1→V2，需要新增一个明确的 bench 阶段，不能直接放宽只读目标：

1. 链接 `platform/stm32f407/examples/w25q128_hal_user.c`，在 BSP 初始化时执行 `ota_hal_nor_probe()`，确认 JEDEC `EF 40 18` 与容量；
2. `ota_user_config` 只在探测通过时设置 `storage_verified=1`，并填 `nor_page_bytes=256`、`nor_erase_bytes=4096`、`eeprom_page_bytes=8`；
3. 打开 `OTA_ENABLE_FLASHING=1`、`OTA_ENABLE_BOOT_HANDOFF=1`；
4. 构建固定在 `0x08010000` 的 App，启动时把 `VTOR` 设为 `0x08010000`（当前 `mdk/app.uvprojx` 还是 UNCONFIGURED，不能直接用于 bench）；
5. App 健康检查必须是真实检查，不能用一个“永远返回 1”的钩子冒充；至少要有独立于 Bootloader 的运行证据；
6. 先跑单次 V1→V2，再跑未确认回滚，最后做断电矩阵。

在这些完成前，不要把“串口 info 通了”写成“实机 OTA 通了”。

## 8. 断电矩阵（写使能后执行）

每个点至少 2 次，记录 `state / active_version / pending_version / boot_attempts` 和启动结果：

| # | 拔电点 | 预期 | 实际 | 是否变砖 |
|---|---|---|---|---|
| 1 | `0x37` 提交 READY 之后、复位之前 | 复位后 READY，可重新安装或重新下载 | | |
| 2 | 元数据 body 写入中途 | 旧副本仍有效，状态回到上一步 | | |
| 3 | 元数据 `commit` 写入前 | 目标记录无效，旧副本被选中 | | |
| 4 | Bootloader 备份旧 App 中途 | 内部旧 App 完整，下次重做备份 | | |
| 5 | 擦除内部 App 中途 / 安装新 App 中途 | INSTALLING 重做整段安装 | | |
| 6 | 跳转前一瞬 | 仍在 INSTALLING/TRIAL 语义内，不跳损坏 App | | |
| 7 | 新 App 首次运行、确认前 | TRIAL 计数已持久化；连续 3 次后回滚 | | |
| 8 | RESTORING 搬回旧 App 中途 | 复位后重做整段恢复 | | |

记录模板建议：

```text
日期 / 固件版本 / 拔电点 / 拔电时刻（串口日志最后一行）
拔电前 state / active / pending / attempts
复位后 state / active / pending / attempts
是否进入 Bootloader / 是否运行 App / 是否可再次升级
结论：预期是否成立；不成立时保留日志与存储镜像
```

## 9. 与 CAN 的复用

USB-CAN 到货后：

- 保持 `src/core/`、`tests/`、PC `IsoTpClient`/`UdsClient` 不变；
- 把 `ota_stm32_uart_link` 换回 `ota_stm32_can_link`；
- 用同一组用例跑 `info`、`status`、`flash`、未确认回滚、断电矩阵；
- 串口台架保留为无 CAN 适配器时的回归入口。
