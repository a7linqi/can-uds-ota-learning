# 从规范到代码：把 shall 翻译成 if

> 这份文档记录的是「怎么从一份协议规范推导出代码检查」的方法，不是某个服务的说明书。
> 目的：以后接手任何新服务、新协议，都能用同一套流程落地，而不是靠背。
> 也给未来的自己（或 AI 上下文丢失后）留一条可接续的线：**结论可以忘，"怎么得出的"不能丢。**

---

## 0. 为什么需要这份文档

学习协议最容易走进的误区：

```text
读了一堆"34 要检查长度、会话、解锁……"
→ 记住了这几个检查项
→ 换一个服务（比如 0x31）就不会了
```

因为记住的是**结论**，不是**方法**。

这份文档要固定下来的是方法：

```text
规范里每一条 shall  →  代码里一个 if
每条 shall 不满足时的要求  →  那个 if 的 return 值（NRC）
```

一旦这个映射建起来，"该检查什么"就不再需要背，直接读规范就能推。

---

## 1. 核心原则：规范规定行为，不规定代码结构

```text
规范说的是黑盒行为：
  给定这个请求，必须返回这个结果
  （shall 语句 + NRC）

规范不说的是：
  用什么语言、什么函数、什么数据结构
  用 if 还是 switch、状态放全局还是结构体
```

同一份规范，C、C++、AUTOSAR 配置、甚至 Python 都能实现。

**所以规范里绝不会出现「你要写六个 if」。** 六道门是从 shall 翻译出来的结果，不是规范原文的条款编号。

---

## 2. 翻译流程：四步

```text
① 读规范的 shall 列表
      把"必须/禁止/应"这类句子摘出来，一条一行

② 每条翻译成一个 if
      shall: "服务只在编程会话可用"
      →     if (s->session != SESSION_PROGRAMMING)

③ 查规范附录的 NRC 表，确定这条不满足该回哪个码
      →     return NRC_CONDITIONS_NOT_CORRECT;   /* 0x22 */

④ 排顺序：先便宜的检查，后贵的
      →     只读 RAM 的在前面，要读 EEPROM / NOR 的放最后
```

**第 ④ 步是工程判断，规范不会告诉你。** 这是需要自己拿捏的地方。

排序的实际收益：

```text
0x34 的前五道门全部只读请求字节和 RAM 标志
→ 前五道失败时，一次 EEPROM 都没碰
→ 恶意或错误请求不会造成 I2C 开销
```

---

## 3. 完整示例：`0x34` 的六道门

### 3.1 对照表（shall ↔ if ↔ NRC）

| 规范里的 shall | 代码里的判断 | 不满足返回 |
|---|---|---|
| 请求长度必须是 11 字节 | `len != 11u` | `0x13` |
| `dataFormatIdentifier` 必须是支持的值 | `req[2] != 0x44u` | `0x13` |
| 只允许在编程会话使用 | `session != PROGRAMMING` | `0x22` |
| 必须已完成安全访问 | `!unlocked` | `0x33` |
| 不允许存在进行中的下载 | `dl_active` | `0x24` |
| `memoryAddress` 必须在支持范围内 | `addr != APP_BASE_ADDR` | `0x31` |
| `memorySize` 必须在支持范围内 | `size < 40 \|\| size > 上限` | `0x31` |
| 当前状态必须允许开始下载 | `state != IDLE && state != DOWNLOADING` | `0x22` |

**代码里那几行 `if` 不是凭空发明的，是规范的几条要求。**

### 3.2 哪些是标准定的，哪些是项目定的

这一点必须分清，否则面试被追问会答不上来。

| 类别 | 内容 |
|---|---|
| **标准强制** | 服务格式、必须检查会话、必须检查安全、NRC 的语义 |
| **标准留白，项目填值** | 地址范围的具体值（`0x08010000`）、大小上下限、支持哪些 `dataFormatIdentifier` |
| **完全项目自定** | `dl_active`（防重复登记）、状态机门限（哪些状态允许新下载） |

```text
骨架（要做哪几类检查）  →  标准定的
血肉（具体数值、状态规则） →  项目定的
```

### 3.3 顺序为什么是这个顺序

```text
格式 → 会话 → 解锁 → 重复登记 → 地址范围 → 元数据状态
①      ②      ③      ④           ⑤           ⑥
只看字节/RAM，极快                 算术     读 EEPROM，最慢
```

---

## 4. 第二个例子：`0x36`（验证方法可复用）

| 检查 | 代码 | NRC |
|---|---|---|
| 长度 ≥ 3 且 ≤ 最大块长 | `len < 3u \|\| len > UDS_MAX_DOWNLOAD_BLOCK` | `0x13` |
| 编程会话 | `session != PROGRAMMING` | `0x22` |
| 已解锁 | `!unlocked` | `0x33` |
| 已经发过 `0x34` | `!dl_active` | `0x24` |
| 块号连续 | `counter != dl_next_counter` | `0x73` |
| 数据量不超声明大小 | `n > dl_size - dl_written` | `0x31` |
| 写存储成功 | `p.write(...)` | `0x72` |

**同一套流程，换一个服务照样能用。**

### 4.1 一个容易被忽略的设计点：块号是身份，内容只是载荷

```text
块号 = 这是第几块（身份）
内容 = 这一块装的是什么（载荷）
```

判断"是否重复"只看块号，不看内容。因为合法数据可能真的重复出现（固件里未使用的 Flash 区域全是 `0xFF`）。如果靠内容判断重复，会把合法数据误判成重传，导致漏写。

```text
块号相同 + 内容相同  →  确认是重传 → 只回应答，不重复写
块号相同 + 内容不同  →  状态不一致 → 0x73
块号不同 + 块号 == 期待值  →  正常写入（内容相同也照写）
块号不同 + 块号 != 期待值  →  0x73 跳号
```

`memcmp` 只在"块号相同"时才执行——先便宜后贵，和第 2 节的排序原则一致。

### 4.2 为什么内容不同时不自动重来

ECU 只报错，不擅自恢复。

```text
状态不一致时：
  任何一方单独"修复"，都可能让不一致更严重
  → 暴露问题，让掌握完整信息的一方（tester）决定
```

这和 `boot_select` 里"IDLE 下 App 损坏不自动回滚"是同一条设计原则。

---

## 5. 状态机在哪：就在结构体里

以 UDS 服务端为例，所谓"状态机"不是一张转移表，而是**一堆字段的当前组合**：

```c
typedef struct {
    /* 会话 / 安全 */
    uint8_t  session;
    uint8_t  unlocked;
    uint32_t seed;
    uint8_t  seed_valid;          /* 种子一次性有效 */
    uint32_t key_failures;
    uint32_t lockout_started_ms;
    uint8_t  lockout_active;

    /* 下载进度 */
    uint8_t  dl_active;
    uint8_t  dl_has_previous;
    uint8_t  dl_previous_counter;
    uint32_t dl_last_offset;
    uint16_t dl_last_size;
    uint32_t dl_offset;
    uint32_t dl_size;
    uint32_t dl_written;
    uint8_t  dl_next_counter;

    /* 其他 */
    uint8_t  reset_requested;
    uint32_t request_count;
    uint32_t last_request_ms;     /* S3 保活 */
} uds_server_t;
```

```text
状态  =  这些字段的当前组合
判断  =  if 里的条件（"这个组合允不允许这个操作"）
转移  =  if 里的赋值（s->unlocked = 1u; s->dl_written += n; ...）
```

### 5.1 一个值得记住的坑：默认值恰好是合法值

```c
uint8_t dl_previous_counter;   /* 初值 0 */
uint8_t dl_has_previous;       /* 辅助标志 */
```

块号可以是 `0`（`0xFF` 之后回绕）。如果只用 `counter == dl_previous_counter` 判断重复，那么**第一次收到块号 0 时会被误判成重复块**。

所以需要一个 `dl_has_previous` 来区分"从没写过"和"写过块号 0"。

```text
通用规律：
  当一个"上次值"的默认值本身可能是合法值时，必须额外加一个 has_xxx 标志
```

---

## 6. 三层结构

```text
【第 1 层】入口分发 —— switch (SID)
  uds_handle()
    ├─ 0x10 → handle_session_control()
    ├─ 0x22 → handle_read_data()
    ├─ 0x27 → handle_security()
    ├─ 0x34 → handle_request_download()
    ├─ 0x36 → handle_transfer_data()
    ├─ 0x37 → handle_transfer_exit()
    ├─ 0x3E → handle_tester_present()
    └─ 其他 → 0x11

【第 2 层】每个服务内部 —— 一串 if（第 2 节的方法）
【第 3 层】状态存储 —— uds_server_t 的字段（第 5 节）
```

每一层都很薄，所以整个 `uds.c` 只有 500 行左右，却能覆盖一个完整的 UDS 子集。

---

## 7. 分发方式怎么选：`switch` 还是多个 `if`

同一个问题（分发），两处代码用了相反的做法，判据是：

> **一次调用该做几件事**

| 场景 | 选择 | 理由 |
|---|---|---|
| `uds_handle` | `switch (SID)` | 一条 UDS 请求只可能是一个服务 |
| `boot_select` | 多个顺序 `if` | 一次上电可能连走好几个状态 |

```text
switch     →  进入时锁定分支，中途改状态也不会跳到别的 case → 一次只做一件
多个 if    →  每次重新读 state，改了就落到下一个分支       → 一次做多件
```

**多个 `if` 的代价是"位置即逻辑"**：新增的状态跳转代码必须放在目标状态的 `if` 之前，否则同一轮接不上，要多一次复位。

---

## 8. 常用 NRC 速查

| NRC | 名称 | 含义 | 典型场景 |
|---|---|---|---|
| `0x11` | serviceNotSupported | 服务不支持 | 发 `0x35` 给只实现 34/36/37 的 ECU |
| `0x12` | subFunctionNotSupported | 子功能不支持 | `10 04` |
| `0x13` | incorrectMessageLengthOrInvalidFormat | 长度或格式不对 | `34` 只有 10 字节 |
| `0x22` | conditionsNotCorrect | **条件不满足** | 默认会话下发 `34` |
| `0x24` | requestSequenceError | **请求序列错误** | 没要种子就 `27 02`；没 `34` 就 `36` |
| `0x31` | requestOutOfRange | 请求超范围 | `34` 的地址不在支持范围 |
| `0x33` | securityAccessDenied | **安全拒绝** | 编程会话但没解锁 |
| `0x35` | invalidKey | 密钥无效 | `27 02` 的 key 算错 |
| `0x36` | exceededNumberOfAttempts | 尝试次数超限 | key 错 3 次被锁 |
| `0x72` | generalProgrammingFailure | **编程失败** | 擦/写 NOR 出错 |
| `0x73` | wrongBlockSequenceCounter | **块号错误** | `0x36` 跳号，或同块号但内容不同 |
| `0x78` | responsePending | 响应挂起 | 处理时间长，先回这个 |
| `0x7F` | serviceNotSupportedInActiveSession | 当前会话不支持 | `10 01` 之后发 `34` |

### 8.1 最容易混的两个

```text
0x22  →  现在这个"状态"下，这个服务就不该用（去换会话）
0x33  →  状态对了，但这个"人"没权限（去解锁）

0x73  →  你的"块号"不对（协议层，tester 的错）
0x72  →  我这边"写存储"失败（硬件层，ECU 的错）
```

**用错 NRC 会被审核挑出来**，因为诊断仪要靠它判断下一步做什么。

---

## 9. 判断"必须有"的技巧

标准文档用词强度不同：

```text
shall        →  强制，不做就是不合规
should       →  建议，不做要说明理由
may / can    →  可选
```

**看到 `shall` 的地方就是代码里必须有的检查。** 这比通读全文高效得多。

---

## 10. 参考资料怎么选

| 目的 | 看什么 | 说明 |
|---|---|---|
| 服务流程与交互顺序 | AUTOSAR Dcm 规范 | 免费、表格化、和量产一致，比 ISO 好读 |
| 正负响应字节布局 | ISO 14229-1 | 权威来源 |
| 分帧与定时 | ISO 15765-2 | SF/FF/CF/FC、BS/STmin、`N_xx` |
| 边界与异常处理 | 两三个开源实现 | Python 的 `udsoncan`、C 的 `linux net/can/isotp.c` |
| 项目特有规则 | 公司 / OEM 诊断规范 | 真正的验收依据 |

**别从 ISO 原文开始**，先用 Dcm + 开源实现把流程跑通，遇到"为什么必须这样"再回头查 ISO。

### 10.1 读 OEM 规范时的重点

90% 内容与标准相同，只看差异：

```text
DID 列表与安全等级
安全访问的等级数、种子长度、算法
定时参数（P2 / P2* / S3）
最大块长（影响 36 的分包）
会话跳转路径（有的要求 01 → 03 → 02）
量产前置流程（85 01 / 28 03 / 2E 写指纹 / 31 01 擦除例程）
NRC 的取舍（有的不允许回 0x78）
```

**通用判断法**：读的时候如果觉得"这个我懂"，说明是标准部分；一旦遇到不认识的东西，那就是项目特有的要求。

---

## 11. 待补充

- [ ] `0x27` 的翻译示例（种子的"一次性有效"对应哪条 shall）
- [ ] `0x37` 的翻译示例（七步验收各自的依据）
- [ ] "块号是身份、内容只是载荷"这一条在其他协议里的类似体现
- [ ] 什么时候该用 `has_xxx` 标志位的通用判据
- [ ] 从 OEM 规范里读到"加严要求"时，怎么判断能不能落地
