# 可选看门狗与 App 健康确认

`OTA_ENABLE_WATCHDOG` 默认是 0。开启后，Bootloader 在 BSP 初始化成功后启动 IWDG；App 初始化后再次配置相同参数。HAL 参考配置为分频 256、重装值 4095。按标称 LSI 32 kHz 计算约 32.768 秒，实际时间随 LSI 偏差变化，必须测量后确定内部擦除的余量。

这部分遵循本机 ST STM32F4xx_DFP 2.16.0 的 `stm32f4xx_hal_iwdg.c`：IWDG 独立于主时钟，启动后不能通过普通代码停止；硬件调试冻结位会改变调试时行为。工程不自动修改选项字节或调试冻结配置。

## 喂狗规则

- Bootloader 活动期间最多每 250 ms 决定一次刷新。NOR/EEPROM 服务点以及内部 Flash 扇区、编程和 CRC 读取之间提供机会，防止整个长流程占用主循环而无人刷新。
- 一个已经卡死的底层调用无法提供服务点，最终由独立看门狗复位；恢复依赖原有 INSTALLING/RESTORING 事务状态。
- App 的后台存储服务和中断不喂狗。初始健康检查应在 10 秒内完成，健康确认过程本身也不喂狗。
- App 自检通过且 `boot_accept_app_health()` 成功后才允许刷新。TRIAL 必须先把确认持久化；确认失败不能因为主循环还在转而继续喂狗。
- 已确认 App 运行期间也必须持续通过健康钩子，健康检查失败则停止刷新。

10 秒限制针对健康检查结果及开始确认的时机。确认提交仍受存储端口的独立期限约束；如果提交卡死，不能靠提交过程的回调继续喂狗。

`boot_accept_app_health()` 在 TRIAL 调用原有确认事务；在 IDLE/DOWNLOADING 验证当前旧 App 的 CRC，接受健康结果但不改下载状态。后者防止下载中断后旧 App 因 DOWNLOADING 标志而永远不能喂狗。READY/INSTALLING/RESTORING 不接受 App 健康确认。

## 验证内容

C 核心测试用虚拟硬件超时驱动三次未确认的启动复位，再由实际 Bootloader 恢复旧镜像，也检查正常确认和确认写失败。

`run_firmware_checks.py` 直接运行真实 `firmware/app/main.c` 入口，使用有限时长 BSP/看门狗模型结束无限主循环。ZG/ZE 分别检查：不健康、健康确认、旧 App 的 DOWNLOADING 状态、确认写被忽略、健康检查过晚、当前 App CRC 不符、刷新失败以及确认后丢失健康。模型还检查后台服务不能偷偷喂狗。

模型用较短的虚拟硬件超时加快验证，没有模拟真实 LSI、电源、CPU 停顿或硬件复位引脚。模型中的自动复位与回滚已于 2026-11-05 在真实板子上复现（见下文）。

## 板上启用顺序

先保持门控关闭，完成 LED、CAN、存储和手动复位恢复。再测量 LSI/看门狗实际超时、最长单扇区擦除、BSP 初始化及 App 自检时间，确认最短看门狗周期能覆盖合法的最长无服务窗口。最后单独启用看门狗门控，测试 App 死循环、健康失败、确认 EEPROM 写失败及安装中卡死。

## 实机测量（2026-11-05，STM32F407ZGT6 探索者板）

按上一节顺序做完了，数字如下（证据 `stages/board-lab/WATCHDOG_EVIDENCE.md`）：

- **真实超时 32.341 s**（三次 32.341 / 32.381 / 32.337 s 的中位数），反推 LSI ≈ 32.4 kHz；
  寄存器回读 `PR=6`（256 分频）、`RLR=0x0FFF`、`SR=0`。
- **正常升级不误咬**：`34` 擦除 14.04 s + 90 帧 `36` + `37` 验签 + 安装跳转，全程零复位。
- **App 健康时持续喂狗**：连续 70 s 无复位，状态与 seq 不变。
- **App 不健康时必咬**：sick 镜像三次复位（32.62 / 32.61 / 32.38 s）后，
  loader 用 8.71 s 完成 RESTORING 并回滚到上一可用镜像。

启用方式（`explorer_reference.h`）：`OTA_ENABLE_WATCHDOG=1` **并**显式定义
`OTA_EXPLORER_WATCHDOG_CAPABLE=1`，否则编译期仍然拒绝。产品 Keil 目标保持 0；
板上验证用的是 `build-temp/build_stage_d.py` 的 `loader_wd` / `app_wd` 变体。

`DBGMCU` 的 IWDG 冻结位没有设置：调试器 halt 住内核时看门狗仍在计数，
超过 32 s 会被复位。需要长时间 halt 调试时请自行设置 `DBG_IWDG_STOP`，本工程不改调试配置。
