# 一条命令验证当前源码

2026-11-05 门控遥测这一轮（`--gcc` 全量重跑）：每容量 3421 个 C 断言（ZG/ZE 一致）、420 个模型断电场景、PC 47 项、SPI NOR 740 项、Python 工具 28 项、固件入口 35+35+51 项、STM32 适配器主机模型 258（门控关闭）/325（门控打开）项；四套 GNU 只读镜像完整链接（ZG 回环 16176 B、ZG 串口台架 44224 B、ZE 回环 16172 B、ZE 串口台架 44224 B），参考 BSP 每目标编译 48 个 ARM C 对象。快照 136 个源码哈希，`status=passed`，206.4 s。`python-can virtual` 本轮未执行（`performed_this_run=false`）。`keil_target_linked=false`、`hardware_verified=false`：以上都不是实机结果。

2026-11-05 当前源码矩阵（`--gcc` 全量重跑）：每容量 3385 个 C 断言（ZG/ZE 一致）、420 个模型断电场景、PC 45 项、SPI NOR 740 项（31 个传输注入点，含 5 个跨页拆分用例）、python-can virtual 7 项、Python 工具 28 项、固件入口 35+35+51 项、STM32 适配器主机模型 232/298 项；四套 GNU 只读镜像完整链接（ZG 回环 16176 B、ZG 串口台架 43984 B、ZE 回环 16172 B、ZE 串口台架 43984 B），参考 BSP 每目标编译 46 个 ARM C 对象。NOR 写路径与更新闸门共 8 个编译变体：写钩子关闭、自检指向槽外扇区、双开关同时打开必须成功；自检指向镜像槽、自检缺地址、只开 `OTA_ENABLE_FLASHING`、只开 `OTA_EXPLORER_UPDATE_CAPABLE`、开了更新但关掉写钩子必须被 `#error` 拒绝。快照 132 个源码哈希，`status=passed`，153.4 s。`keil_target_linked=false`、`hardware_verified=false`：以上都不是实机结果；实机进度另见 [参考板文档](EXPLORER_REFERENCE.md)。

v0.13 的完整矩阵已通过，详见 [核心复核记录](CORE_REVIEW_20261102.md)：每容量 3138 个 C 断言、420 个模型断电场景，Python 工具 22 项、实际 python-can virtual 后端 7 项、Python 包到 C 校验 8 组。四套参考目标各编译 43 个 ARM C 对象，GNU 各 45 个对象并完成 ELF/BIN/MAP 链接检查。实物 SWD 识别已单独记录，项目固件尚未上板运行。

2026-11-04 UART 台架阶段：新增 26 条 CAN-over-UART 编解码断言，主机 C 基线为 3164 条/容量；STM32 适配器主机模型 232（门控关闭）/298（门控打开）条断言；Python 工具 28 项。真实串口、擦写、跳转和断电仍未执行，详见 [UART 台架说明](UART_BENCH.md)。

v0.12 增加探索者只读 BSP 验证：实际回环入口与接收队列的主机模型为 51 项断言、14 个场景；这不是物理 CAN 实验。给出 ARM/ST/CMSIS 参数时，另检查四个参考目标，每目标编译 35 个 C 对象。

追加 `--gcc "arm-none-eabi-gcc的完整路径"`、`--dfp` 和 `--cmsis`，会用 GNU Arm 工具链完成四个只读目标的汇编、链接及 ELF/BIN 地址检查。先运行 `tools/generate_explorer_mdk.py` 为本机 pack 路径生成源文件计划。主报告中的 `target_linked=true` 仅表示 GNU 只读目标完整链接；`keil_target_linked=false`、`hardware_verified=false` 仍分别表示 Keil 未完成链接、实物未验证。

带固件交付时用 `python tools/package_project.py --include-firmware`。完整链接证据位于 `evidence/explorer-gnu-results.json`，工程说明及产物位置见 [探索者只读入口](EXPLORER_REFERENCE.md)。

额外检查实际 python-can 库的内存虚拟后端：先安装 `requirements-virtual.txt`，再给验证命令追加 `--python-can-virtual`。如果用 pip 的 `--target` 隔离安装，可用 `--python-can-path` 指向该目录。该选项只用 virtual 接口，不探测 USB、不安装驱动、不访问真实总线；结果独立于 45 项基础 PC 检查。没有指定时，总报告明确记为本轮未执行。

在根目录执行，使用自己已安装的 C99 编译器：

~~~powershell
python verify_project.py --cc "你的编译器完整路径"
~~~

如果本机有 ARM Compiler 6 和 STM32F4xx_DFP，可追加 `--arm-bin`、`--dfp`、`--cmsis`，分别指向 ARM 编译器 bin、ST 器件包根目录和 CMSIS Core Include。这会额外检查 Cortex-M4 C 对象与 HAL 参考绑定；仍不会执行 ARMASM/armlink、连接调试器或给板卡烧录。

脚本依次检查两种容量的 C 核心、PC→C 协议、SPI 命令驱动、App 真实入口看门狗模型、STM32 端口/Keil 配置、Python 打包器及 Python 包→C 校验器的互操作，再跑升级/回滚演示。测试输入为合成数据，真实 CAN 后端不会打开。

结果写入 `evidence/verification.json`，其余原始日志也在 `evidence/`。验证开始时标记 running，失败则标记 failed；只有全部请求的检查完成后才标记 passed。

验证记录包含源码与构建输入的 SHA256；运行期间源码改变会使本次验证失败。`tools/package_project.py` 也会检查源码是否与成功验证时一致，再生成逐文件清单并核对压缩包 CRC/文件 SHA256，避免把旧的通过记录配给新的代码。文档修改不要求重复运行代码测试。

打包：

~~~powershell
python tools/package_project.py
~~~

默认输出到工程上级的 `deliverables/ota-core-L1.zip`。文件名保留了早期版本名称，README 中的 v0.x 表示当前工程里程碑。

计数口径不同：C 检查数是断言数，Python 用例数是 unittest 方法数，SPI 注入点数是底层调用失败位置，不能混称为同一种测试用例数。物理断电次数、刷写速度、总线负载、完整固件大小和硬件看门狗恢复仍需要实测。
