# Compiler Design VM Roadmap

这是一份独立的 Rust VM 路线图。它与 [`docs/roadmap.md`](roadmap.md) 并列，
不替代也不改写后者；后者继续负责语言、前端、语义分析、编译器工具和
编译器侧模块工作的排序。本文件只负责 `vm-rs/`、`.cdbc` 运行时边界、
模块链接、运行时资源、调试和 VM 产品化。

路线图的起点是 2026-07-29 的 `master`（`be73643`，版本 `0.1.0`）。状态
以代码、artifact、测试和决策记录为准，不以路线图文字作为完成证明。

## 1. 目标与边界

### 1.1 VM 的目标

Rust VM 的长期目标是成为一个可独立验证、可重复执行、可观测、可嵌入的
`.cdbc` 执行环境：

- 对合法 artifact 保持与 C++ 编译器输出一致的语言运行时语义；
- 在执行前拒绝损坏、越界、版本不兼容和超出资源预算的输入；
- 对相同 artifact 和相同运行配置给出确定性的输出、错误和调试事件；
- 提供 CLI、库 API、模块链接和调试接口，而不是只作为测试用解释器；
- 用可重复的正确性、兼容性和性能数据支持每一次运行时变更。

### 1.2 不在本路线图内的工作

下列工作仍归 [`docs/roadmap.md`](roadmap.md)，不会因为 VM 路线图的存在而
自动成为 VM 任务：

- 新的词法、语法、类型系统、泛型约束、nullable flow 和模式匹配规则；
- 前端声明索引、类型检查、IR 设计和 C++ 编译器内部重构；
- 语言标准库 API 的设计。标准库的 VM native 实现只在已有语言契约确定后
  作为配套实现进入本路线图；
- 包管理、依赖解析和源码搜索路径；
- 没有经过独立决策的 GC、任务调度、异步、JIT 或动态派发。

编译器和 VM 共享 `.cdbc`、native 名称、运行时值格式和调试元数据。任何
共享边界的修改都必须同时更新 C++ emitter、Rust parser/formatter/executor、
格式文档和跨后端测试，但不应把编译器的语义工作复制到 VM 路线图。

## 2. 当前 VM 基线

以下是已交付的起点，不是待办事项：

| 区域 | 当前状态 | 事实证据 |
| --- | --- | --- |
| `.cdbc` artifact | `cdbc 0.1` 文本格式可解析、规范化打印，并在 parser、CLI、linker 和执行入口共享结构 verifier；支持 linked program 与 module product 两种 envelope | [`docs/bytecode-text-format.md`](bytecode-text-format.md)、`vm-rs/src/format.rs` |
| 指令执行 | Rust register VM 已执行常量、变量、调用、闭包、控制流、算术、比较、数组、map、range、struct、enum、索引和字段操作 | `vm-rs/src/vm.rs`、`vm-rs/src/bytecode.rs` |
| 运行时值 | 已有 `nil`、number、bool、string、function、array、map、range、struct、variant；数组、map、struct 字段和闭包环境保留共享/别名语义 | `vm-rs/src/value.rs`、`vm-rs/src/runtime.rs` |
| native 运行时 | 已覆盖集合修改/查询、字符串、数学、`typeOf`、`hash`、`contains`、切片和 callback helpers 等已登记 native | `vm-rs/src/vm.rs`、`docs/bytecode-text-format.md` |
| 模块链接 | `artifact: module` 产品可被 `link` 按 entry order、依赖插入点和 canonical identity 确定性展开；寄存器、函数、跳转和 debug 引用会 rebasing | `vm-rs/src/link.rs`、`tests/bytecode_module_artifact_tests.py` |
| 调试与错误 | `run` 提供运行时错误和调用栈；`trace` 使用 `debug_sources`、`debug_locations`、`debug_ranges` 输出源码事件、局部变量、返回和失败 | `vm-rs/src/main.rs`、`tests/debugger_tests.py`、`docs/decisions/m5d-debug-001.md` |
| 跨后端验证 | C++ emission、Rust dump/link/run、模块 artifact、module cache、golden 和 Cargo 测试已经形成验证链 | `tests/bytecode_artifact_tests.py`、`tests/run_rust_vm_tests.py`、`tests/bytecode_module_cache_tests.py` |

当前基线的主要限制也要明确记录：库 API 已存在但仍是 `0.1` additive boundary，
不是 crates.io/跨线程稳定承诺；运行时对象主要由 `Rc<RefCell<...>>` 和手工
identity 管理；资源预算和协作式取消已经由 VM-1B 固定，profile 当前只提供
确定性 counters，不包含 wall-clock 或 allocation/peak 字段；快照/回滚、二进制
artifact 或 JIT 仍未进入默认队列。`cdbc 0.1` 的兼容性优先级高于这些后续能力。

## 3. 路线图总览

```text
当前 cdbc 0.1 / Rust VM 基线
  -> VM-1 执行安全与契约加固
  -> VM-2 运行时所有权与堆模型
  -> VM-3 可嵌入 VM 与 artifact 产品化
  -> VM-4 调试与可观测性
  -> VM-5 性能与容量工程
  -> VM-6 运行时生态与兼容性扩展
  -> VM-7 条件研究：持久会话、调度器、GC/JIT
```

推荐的近期开发顺序是 `VM-1A -> VM-1B -> VM-1C -> VM-2A -> VM-3A -> VM-4A`。
`VM-2B`、`VM-5B` 和 `VM-6` 必须以测量结果为依据；`VM-7` 不进入默认
开发队列，直到前置决策和 workload 证据满足门槛。

每个 active slice 都必须有：目标、明确 non-goal、边界决策、迁移方案、
命名的测试/benchmark case、删除旧路径的条件和可复现命令。完成后把细节
留在 `docs/decisions/` 与测试中，路线图只保留状态和下一步。

## 4. VM-1：执行安全与契约加固（P0，已完成）

### VM-1A：独立 artifact verifier（已完成，2026-07-29）

**目标：** 把“文本能解析”与“程序可以安全执行”分成清晰的验证层。

**交付：**

- 对 constants、names、functions、registers、params、instruction operands、
  jumps、debug references 和 native capability 做完整结构验证；
- 在执行前验证函数入口、返回路径和跳转目标，不让执行循环承担可静态
  判定的 malformed artifact 检查；
- 明确 parser error、verification error、link error、resource error 和
  runtime error 的 CLI 输出及退出码；
- 对 `artifact: module` 复用同一验证器，保证 module 在 `dump`、`link` 和
  `run` 前得到一致的拒绝结果。

**边界：** 不改变 `cdbc 0.1` 的合法文本，不引入新的 opcode，不把 verifier
  变成编译器类型检查器；结构上合法但语言语义错误的 artifact 仍由运行时
  按现有契约处理。

**验收：** 为每一种 operand/reference 错误建立最小 malformed corpus；所有
  输入在执行前失败且不产生 stdout；正常 artifact 的 C++ emission、Rust
  dump 和 Rust run 结果不变。对应测试应覆盖 linked、module、debug metadata
  和 native allowlist，而不是只测试算术指令。

**状态与证据（已完成，2026-07-29）：** `format::verify_artifact`、`verify_program` 和
`verify_module_artifact` 已成为 public verifier 边界；parser 返回 artifact 前、
CLI 的 `dump`/`run`/`trace`/`link` 读取后、module linker 输入和最终 linked
program 都会经过验证。当前完整证据为 Rust 单测 `59/59`、CTest `34/34`、
artifact `118/118`、module cache `11/11`、Rust VM `778/778`、canonical
verification `1884/1884`、boundary `5/5`、malformed `108/108` 和 debugger
全部通过；边界决策见
[`docs/decisions/vm-artifact-verifier-001.md`](decisions/vm-artifact-verifier-001.md)。

### VM-1B：确定性资源预算与取消

**目标：** 防止无限循环、深递归、异常大的容器、过长输出或恶意 artifact
  让 VM 无界消耗资源。

**交付：** 先固定 `RunConfig`/CLI 配置的语义，再实现至少以下预算：

- instruction steps；
- call depth；
- 活跃运行时对象或总元素数量；
- 输出字节数；
- artifact 大小和模块展开规模。

每一项都要有默认策略、禁用/覆盖方式、确定性错误和计数边界。取消检查点
应位于指令循环、native callback 和可能增长的容器操作中。

**边界：** 本阶段不提供线程调度、异步任务或暂停后恢复；资源预算不是
  GC，也不允许通过静默截断输出改变语言语义。

**验收：** 同一 artifact、配置和输入重复运行得到相同的资源错误；预算触发
  时不 panic、不死循环、不残留部分成功状态；正常 workload 的运行输出与
现有 `run_rust_vm_tests.py` 完全一致。

**状态与证据（已完成，2026-07-29）：** `vm::RunConfig` 固定默认预算、单项
覆盖和 `--unlimited` 语义；`CancellationToken` 在指令循环、native callback
和容器增长点执行协作式检查。资源错误不会产生部分 `run` stdout，artifact 和
module expansion 也在读取/链接前受预算保护。Rust 单测 `59/59`、VM resource
budget focused test、CTest `34/34`、Rust VM `778/778` 和 canonical
verification `1884/1884` 全部通过；详细边界见
[`docs/decisions/vm-resource-budget-001.md`](decisions/vm-resource-budget-001.md)。

### VM-1C：panic-free 与变异 artifact 防线

**目标：** 把用户可控制的 `.cdbc` 输入视为不可信输入，消除验证遗漏导致
  的 Rust panic 或非确定性失败。

**交付：** 使用 mutation/fuzz corpus 覆盖截断 section、重复 section、超大
  index、非法 UTF-8/escape、反向 range、模块环和依赖插入点边界；将发现的
  panic 转成稳定错误，并固定最小回归样本。

**边界：** 不承诺沙箱隔离或操作系统级安全；本阶段只保证 VM 自己不因 artifact
  结构而崩溃，并明确 native 函数的资源/失败边界。

**删除条件：** 当 parser/linker/executor 都经过统一 verifier，malformed
  corpus 在固定 seed 下无 panic 且每个拒绝原因稳定后，才可以删除重复的局部
  防御代码；不能以“测试没崩溃”作为删除依据。

**状态与证据（已完成，2026-07-29）：** malformed corpus 已覆盖 duplicate
section、非法 escape、反向 debug range、非法 UTF-8 artifact 和 module cycle
回归；parser、linker 和 executor 对这些输入均返回稳定错误而不 panic。malformed
测试 `108/108`、CTest `34/34`、canonical verification `1884/1884`、Rust
单测 `59/59` 和 module/artifact/debugger 回归全部通过。

## 5. VM-2：运行时所有权与堆模型（P0/P1）

### VM-2A：所有权、别名和生命周期决策

**目标：** 在写 GC 或替换 `Rc<RefCell>` 前，先把当前运行时语义写成可验证的
  决策：数组/map/struct 字段/闭包捕获的 alias、identity equality、copy 与
  native 返回值的关系，是否允许循环引用，以及 runtime error 后哪些对象
  仍可观察。

**交付：** `docs/decisions/vm-runtime-ownership-001.md`（必要时附
machine-readable case matrix），包含值分类、root 集合、native 临时 root、递归值、
  失败回收、调试观察和线程模型。

**边界：** 这是决策和 corpus slice，不直接替换运行时存储，也不引入用户可见
  的 copy/borrow 语法。

**验收：** alias、closure capture、嵌套 aggregate、变异、identity equality、
variant payload 和错误路径都有 C++/Rust 对照样例；没有明确决策的循环/并发
行为保持 deferred，而不是由实现细节悄悄决定。

**状态（决策已记录，2026-07-29）：** 当前 `Value`/`runtime`/`VM` 的共享
aggregate、cell/environment、identity equality、native 浅拷贝、callback 快照、
执行 root 和错误生命周期已固定为可验证契约；循环引用、持久 host root、并发
值和回收策略保持 deferred。详见
[`docs/decisions/vm-runtime-ownership-001.md`](decisions/vm-runtime-ownership-001.md)。

### VM-2B：统一 Heap/Handle 抽象

**目标：** 让 VM 的对象分配、identity、root 访问和 debug 观察经过统一运行时
  层，避免 `vm.rs`、native helpers 和 linker/loader 各自持有生命周期规则。

**交付：** 在不改变 `.cdbc 0.1` 和用户可见 alias 语义的前提下，引入清晰的
  `Heap`/handle 或等价抽象；`Value` 只依赖稳定的运行时引用接口；记录每种
  aggregate 的分配、借用和错误处理不变量。

**边界：** 不为了抽象而更换数据结构；如果基准显示当前 reference-counted
  实现足够，保留它也是合法结果，但必须由测量和循环引用决策支持。

**状态（第一窄切片已完成，2026-07-29）：** `runtime::Heap` 已集中 VM-local
identity 分配、environment/cell 创建和 function/array/map/range/struct/variant
构造；VM 的 globals、frame、capture、parameter 和 native allocation 路径已迁移，
现有 `Rc<RefCell>` alias/equality/lifetime 语义保持不变。完整存储替换、root
registry、cycle policy、GC 和 host-byte peak-memory measurement 仍未完成，详见
[`docs/decisions/vm-heap-facade-001.md`](decisions/vm-heap-facade-001.md)。

**状态（测量窄切片已完成，2026-07-29）：** `HeapStats` ledger 现在通过只读
`Weak` 记录报告 environment、cell、array、map 和 struct 的 allocation total、
live/dead 数量；它不持有 VM root，也不通过 `Display` 遍历递归值。Rust 单测已
覆盖 acyclic/cycle、closure cell/environment cycle、native temporary、runtime
error 和 trace root release，当前 focused 结果为 `69/69`。function、range、
variant 的 inline `Value` 生命周期、exact host-byte peak、ledger compaction、GC、
搬迁句柄和持久 host root 仍保持 deferred；下一步需要 workload 级 peak 测量
和决策，而不是直接替换 `Rc<RefCell>`。

**状态（peak workload 窄切片已完成，2026-07-30）：** 共享存储现在由
`TrackedStorage<T>` 保留原有 `Rc<RefCell>` alias 边界，并在最后一个 `Rc` 释放
时更新 live ledger；`HeapStatsSnapshot.peak_live` 提供本次 VM 生命周期内 tracked
environment、cell、array、map、struct 的最大同时存活数量。新增混合 aggregate、
native temporary、长数组 churn、深递归闭包和大数组/map payload workload 测试，
Rust focused 结果为 `72/72`。随后增加 opt-in 的
`estimated_live_bytes`/`estimated_peak_live_bytes`，统计 tracked storage 的
representation pressure，并以独立 decision record 固定其非全局 allocator 边界；
本轮 Rust focused 结果为 `73/73`。这不是 exact host allocator/RSS，也不覆盖
inline function/range/variant、ledger compaction、GC、搬迁句柄或持久 host root；
下一步基于这组对象与字节 workload 证据评估 backend，而不是直接替换
`Rc<RefCell>`。

### VM-2C：可选的 tracing GC 或其他回收策略

**目标：** 只有在 VM-2A/2B 证明有必要时，提供可测量的堆回收，覆盖 closures、
  shared cells、arrays、maps、structs、variants 和跨 native 调用的 roots。

**边界：** GC 不得改变 identity equality、打印顺序、aliasing、错误位置或
  `trace` 事件顺序；不会把 GC 当作性能捷径，也不会在没有 allocation/peak
  workload 基线时启动实现。

**门槛：** 至少有 live/dead/cycle、深闭包、长循环、错误退出、debug trace 和
  大容器 workload；报告峰值内存、回收次数、暂停时间和输出一致性。若 VM-2A
  结论是不需要 tracing GC，则关闭此分支并把理由留在决策记录。

## 6. VM-3：可嵌入 VM 与 artifact 产品化（P1）

### VM-3A：库 API 与 CLI 分层

**目标：** 将当前 binary-only `vm-rs` 拆出可测试、可嵌入的 Rust library
  边界，同时保留 `compiler-design-vm` CLI。

**交付：** 稳定命名的 API 至少包括 artifact parse/verify、module link、
  `RunConfig`、执行结果、runtime diagnostics 和 trace events；CLI 只负责
  参数解析、文件 IO、格式化和退出码。多个 VM 实例应能在同一进程内独立运行。

**边界：** 第一阶段不承诺 crates.io 发布、跨线程 `Send`、进程级 sandbox 或
  persistent session；库 API 的版本策略和错误类型先写决策记录。

**验收：** 添加 Rust integration tests，不再只能通过 `cargo run` 进程测试；
  CLI 与 library 对同一 artifact 得到相同 stdout/stderr/result/trace；一个
  实例失败不能污染另一个实例的 globals、identity allocator 或输出缓冲。

**状态（第一 library boundary 窄切片已完成，2026-07-30）：** 新增
`vm-rs/src/lib.rs`，公开 artifact parse/format/verify、module link、
`Program`、`RunConfig`、`VM`、runtime error、cancellation 和 trace 类型；CLI
继续负责文件 IO、参数、输出渲染和退出码。`vm-rs/tests/library_api.rs` 已覆盖
内存 artifact 的 parse/verify/run/trace、module link 以及两个 VM 实例隔离。
当前 crate 仍保持 `publish = false`、单线程 `Rc<RefCell>` 语义和现有错误类型，
persistent session 和 `Send`/`Sync` 保持 deferred；第一版 API 与错误边界见
[`docs/decisions/vm-library-api-001.md`](decisions/vm-library-api-001.md)。

**状态（typed error/version boundary 窄切片已完成，2026-07-30）：** 新增
`LIBRARY_API_VERSION`、`ARTIFACT_FORMAT_*` 常量，以及 additive 的
`parse_artifact_checked`/`verify_*_checked` 和
`link_modules*_checked` API。`ArtifactError` 区分 parse、unsupported version、
verification；`LinkError` 暴露 kind、module identity 和 dependency index，旧
`ParseError`/`String` 函数通过兼容适配器保留原文字诊断。`cdbc 0.1`、CLI、旧
linker API 和 artifact bytes 不变；integration tests 已固定 typed fields 和
legacy display。详细边界见
[`docs/decisions/vm-library-error-boundary-001.md`](decisions/vm-library-error-boundary-001.md)。

### VM-3B：模块链接器的产品级加固

**目标：** 在已有 module product/link 基础上，提升大模块图的诊断、可测试性和
  可观察性。

**交付：** 明确 dependency graph 的 cycle、duplicate identity、entry ordering、
  duplicate public linkage、debug source rebasing 和 expansion order；提供
  可选的 link report，记录输入产品、展开顺序、数量和拒绝原因。

**边界：** 编译器拥有 `cdbc-cache 0.2` 的 source/public-interface invalidation
  策略；VM 不复制 module cache，也不读取 `.cdi` 来补充可执行函数体。

**验收：** 线性、菱形、重复依赖、环、缺失模块、多个 entry、空依赖和 debug
  metadata 图都有固定测试；link 后的执行、trace stack 和 source path 与
  直接 linked artifact 一致。

**状态（第一 linker report 窄切片已完成，2026-07-30）：** 新增
`link_modules_with_report`/`LinkResult`/`LinkReport`，记录排序后的输入模块、
entry order、dependency source-order expansion、输入依赖规模和 linked
program 规模；旧 `link_modules` 保持原返回值，CLI 默认不输出报告。当前已用
diamond、cycle、invalid-input 和 library linked-run tests 固定顺序、计数和错误
兼容性；typed linker error 已作为 VM-3A-002 的 additive facade 交付，报告文件
schema、大图容量和性能仍 deferred，
决策记录见
[`docs/decisions/vm-module-link-report-001.md`](decisions/vm-module-link-report-001.md)。

### VM-3C：artifact 版本和二进制表示决策

**目标：** 在需要更快加载、更小产物或更安全校验时，决定是否增加 binary
  artifact；先定义兼容策略，再实现格式。

**交付：** 比较继续维护 `cdbc 0.1` 文本、增加 `cdbc 0.2` binary、或采用
  文本外层加 binary payload 的方案；决策必须包含 magic/version/checksum、
  endian/UTF-8、debug metadata、module products、错误恢复和 tooling。

**边界：** 在决策通过前不改变 `cdbc 0.1` header，不让 VM 默认输出不可审阅的
  二进制，也不为了性能复制一套未验证的 parser。

**门槛：** 只有加载时间、artifact 大小或完整性需求被 benchmark 证明后，才
  进入实现；旧 `.cdbc 0.1` 的 `dump`、`link`、`run` 和 metadata-free 兼容
  行为必须继续有回归测试。

## 7. VM-4：调试与可观测性（P1）

### VM-4A：交互式 breakpoint/step debugger

**目标：** 在已交付的一次性 `trace` 之上增加确定性的交互调试会话。

**交付：** 先固定 `docs/decisions/vm-debugger-001.md` 中的协议，再支持
  source/range breakpoint、`continue`、`step`、`next`、`quit`；暂停事件包含
  module/source/range、当前 frame、调用栈和可见 locals。nested call、loop、
  imported module、return 和 runtime error 必须有稳定 stop order。

**边界：** 第一阶段不支持 source expression evaluation、寄存器/变量修改、
  watch expression、热替换或新的 source mapper；所有位置都来自已有
  `debug_sources`、`debug_locations`、`debug_ranges`。

**验收：** `tests/debugger_tests.py` 扩展为重复会话、断点命中、step/next、
  import、闭包 locals、错误和 metadata-free artifact 的矩阵；`trace` 和
  普通 `run` 的输出保持兼容。

**状态（第一 interactive debugger 窄切片已完成，2026-07-30）：** 新增
`compiler-design-vm debug` 和 library `VM::debug`/`DebugHook` 边界；会话在真实
执行中的每条指令前暂停，支持 entry、source line/range breakpoint、continue、
step、next、delete、quit，并复用已有 debug source/location/range、调用栈和
locals。错误沿嵌套 frame 以稳定的 inner-to-outer `error` pause 暴露，最终
`RuntimeError` 仍保持原 stderr 诊断；`run`、`trace`、`.cdbc 0.1` 和资源预算
不变。debugger matrix 已覆盖 linked/imported module、range、闭包/函数 locals、
return、runtime error、metadata-free artifact 和重复 quit 会话，决策记录见
[`docs/decisions/vm-debugger-001.md`](decisions/vm-debugger-001.md)。

### VM-4B：profile、coverage 和运行报告

**目标：** 为优化和问题定位提供机器可读的执行证据。

**交付：** 增加 opt-in 的 instruction/function/native counters、执行时间、
  allocation/peak counters（若 VM-2 已定义 heap）和 source-range coverage；
  规定输出 schema、排序和是否影响执行。

**边界：** profiling 默认关闭，不改变 stdout、runtime error、trace sequence
  或资源预算；不把 profile 结果直接当作 JIT 输入，除非 VM-7 单独决策。

**状态（第一 deterministic counter 窄切片已完成，2026-07-30）：** library
`VM::profile`/`ProfileRun`/`ProfileReport` 和 `compiler-design-vm profile`
现在统计通过 instruction checkpoint 的 bytecode 指令、entry/函数调用与指令、
native 调用、已有 `DebugRange` 命中和成功写入的 output bytes。函数按 artifact
定义顺序输出，native 和 source range 使用稳定排序；运行时错误、资源错误和取消
仍返回已经收集的 partial report，CLI 只输出报告而不混入业务 stdout。`run`、
`trace`、`debug`、`.cdbc 0.1` 和资源语义保持不变，边界记录见
[`docs/decisions/vm-profile-001.md`](decisions/vm-profile-001.md)。wall-clock
执行时间和 allocation/peak counters 需要单独的确定性/heap 测量决策，暂不伪装
为本 slice 的完成项。

### VM-4C：错误与诊断 API 稳定化

**目标：** 把 CLI 文本之外的 runtime error、stack frame、source range、
  resource exhaustion 和 artifact rejection 暴露为稳定结构，供 LSP、测试和
  宿主程序使用。

**验收：** 每种错误至少有结构化字段、稳定 kind、最小复现 artifact 和 CLI
  渲染；path canonicalization 只在显示层处理，不污染 artifact identity。

**状态（第一 structured kind 窄切片已完成，2026-07-30）：** 现有
`ArtifactError`、`LinkError` 和 `RuntimeError` typed domains 保持分离，并为
`ArtifactErrorKind`、`LinkErrorKind`、`RuntimeErrorKind` 和 `ResourceKind` 增加
稳定 `as_str()` labels。runtime 结构继续携带 `DebugLocation`、有序调用帧和原始
`DebugSource` path/module；artifact 和 linker 继续携带 line/module/dependency
context，CLI 的旧文本和退出码不变。统一 JSON/schema、source mapper 和路径重写
仍 deferred，边界记录见
[`docs/decisions/vm-diagnostics-001.md`](decisions/vm-diagnostics-001.md)。

## 8. VM-5：性能与容量工程（P1/P2）

### VM-5A：可重复 benchmark 基线

**目标：** 先建立 VM 独立于 C++ 编译时间的性能基线。

**交付：** 扩展 [`docs/benchmarking.md`](benchmarking.md) 和现有 benchmark
  runner，至少覆盖 startup/load、算术循环、调用/闭包、数组/map mutation、
  callback native、字符串 Unicode、module link 和 runtime error；分别记录
  compiler、artifact load/link 和 VM execution 时间。

**规则：** 先记录当前版本的 machine/toolchain/workload/样本数量，再定义回归
  阈值；benchmark 不能因为波动直接阻断正确性 gate，但性能变更必须附带前后
  数据。

**状态（第一四阶段 baseline 窄切片已完成，2026-07-30）：**
`tests/run_benchmarks.py` 的 schema 2 现在分别测量 compiler emission、模块
link、artifact load/canonical dump 和 VM execution；report 记录 commit、主机、
CMake/Rust/Cargo toolchain、workload expectation digest 和每阶段的
min/median/max。manifest 已固定十一个 workload，覆盖 startup/load、算术、调用/闭包、
scaled loop/closure、数组/map、callback/native、Unicode、module link 和 runtime error；预期 runtime
error 只有在 stdout、stderr、exit code 全部匹配时才算通过。当前 load 边界复用
`dump`，所以包含 canonical formatting；不把它伪装成纯 parser 或 RSS 测量。
详细契约见 [`docs/decisions/vm-benchmark-001.md`](decisions/vm-benchmark-001.md) 和
[`docs/decisions/vm-benchmark-002-execution-scale.md`](decisions/vm-benchmark-002-execution-scale.md)。

### VM-5B：执行循环和 frame 优化

**目标：** 在语义和资源边界冻结后，降低 dispatch、寄存器访问、函数调用、
  native callback 和临时 `Value` clone 的成本。

**候选方向：** frame 生命周期与函数 body clone、instruction dispatch、name/
  constant lookup、调用栈构造、trace-off fast path、容器借用和 native 注册表。

**边界：** 不用 unsafe/threaded dispatch/JIT 掩盖 verifier 缺口；每一次优化都
  必须保留 C++/Rust output、error、alias、debug 和 resource-limit parity。

**状态（第一 trace-off instruction preamble 窄切片已完成，2026-07-30）：**
默认 `run` 在 trace/debug/profile 均关闭时跳过无效的 trace/debug/profile 调用和
每条指令的 `DebugLocation` clone；错误路径仍重新取得位置，资源 checkpoint、
trace、debugger、profile 和 `.cdbc 0.1` 行为不变。scaled loop 的 runtime
median 从 1.050626s 降至 0.866248s，scaled closure 从 0.517751s 降至
0.452582s；小 workload 仅作为正确性/启动噪声参考。详见
[`docs/decisions/vm-execution-loop-001.md`](decisions/vm-execution-loop-001.md)。

**状态（第二函数体缓存窄切片已完成，2026-07-30）：**
`VM::call_function` 按函数索引懒加载不可变的 name/params/registers/instructions/
locations body；未调用的函数不分配缓存，重复调用共享同一 `Rc`，frame 的寄存器、
参数 cell、closure 和调用深度行为保持不变。scaled closure 的 runtime median
从 0.466308s 降至 0.441467s；没有函数调用的 scaled loop 为 0.882778s 与
0.933272s，视为测量噪声而非回归阈值。缓存只在单个 VM 生命周期内保留，淘汰和
容量测量留给 VM-5C；详见
[`docs/decisions/vm-execution-loop-002-function-body-cache.md`](decisions/vm-execution-loop-002-function-body-cache.md)。

**状态（第三 trace-off frame-boundary 窄切片已完成，2026-07-30）：**
`execute_body` 现在只在 trace/debug 启用时建立和销毁 trace frame，并只在该路径
构造 output/return/error 事件的位置和返回值字符串；默认 `run` 与 profile-only
执行跳过这些 no-op 成本，trace/debug 的 frame 栈和事件顺序不变。scaled closure
的 runtime median 从 0.430038s 降至 0.401322s；没有函数调用的 scaled loop 为
0.884185s 与 0.892779s，视为测量噪声。详见
[`docs/decisions/vm-execution-loop-003-trace-off-frame-boundary.md`](decisions/vm-execution-loop-003-trace-off-frame-boundary.md)。

**状态（第四 borrowed call-site 窄切片已完成，2026-07-30）：**
内部 `call_function` 与 native callback 路径借用 caller 的 `DebugLocation`，成功
调用和 callback 迭代不再 clone；只有构造 runtime error/call stack 时才复制位置。
runtime error 的位置和 stack 顺序不变。scaled closure 的 runtime median 从
0.404358s 降至 0.393967s；collection/loop 的小或无调用 workload 只作为噪声
参考。详见
[`docs/decisions/vm-execution-loop-004-borrow-call-sites.md`](decisions/vm-execution-loop-004-borrow-call-sites.md)。

**状态（第五 borrowed name-operand 窄切片已完成，2026-07-30）：**
`LoadVar`、`AssignVar`、`VariantTag` 和 struct field access 直接借用 immutable
`Program` 的 name 字符串；需要长期持有名称的 declaration/native/constructor/error
路径仍复制，未引入 per-VM name cache。scaled closure 的 runtime median 从
0.392948s 降至 0.376456s，scaled loop 从 0.885938s 降至 0.829240s；详见
[`docs/decisions/vm-execution-loop-005-borrow-name-operands.md`](decisions/vm-execution-loop-005-borrow-name-operands.md)。

**状态（第六 main-frame global cell cache 窄切片已完成，2026-07-30）：**
主帧按 canonical name slot 缓存已解析的 global `Cell`；重复的 bytecode name
index 会归一化到同一 slot，`StoreVar` 在绑定创建或替换时刷新 slot，函数帧和
closure 仍使用原有 locals -> closure -> globals 解析顺序。scaled closure 的
runtime median 从 0.386656s 降至 0.299741s，scaled loop 从 0.864550s 降至
0.519380s；详见
[`docs/decisions/vm-execution-loop-006-global-cell-cache.md`](decisions/vm-execution-loop-006-global-cell-cache.md)。

### VM-5C：容量与大模块图

**目标：** 让 VM 在大 artifact、深调用、长字符串、大数组和多模块 link 下
  有明确的容量行为，而不是依赖偶然的 host 内存。

**验收：** 测试最大合法/预算内输入、预算外输入、深递归、长链/菱形模块图和
  大 debug table；记录 peak memory、load/link time、执行时间和错误类型。若
  VM-2 的 heap 方案改变测量结果，必须重新基线。

## 9. VM-6：运行时生态与兼容性扩展（P2）

### VM-6A：native ABI 与注册表

**目标：** 把 native 名称、arity、参数/返回值约束、callback 能力、错误和
  资源消耗从 `vm.rs` 的分支集合整理为可验证的注册表。

**边界：** 不开放任意 Rust 函数给 artifact；native ABI 不是动态插件系统，
  不允许未登记的 `.cdbc` native name 进入执行。

### VM-6B：宿主集成与 I/O 策略

**目标：** 明确 VM 是否、以及如何向宿主暴露输出 sink、诊断 sink、时间/随机数、
  文件或网络能力。默认 VM 必须保持无隐式外部副作用和可重复执行。

**门槛：** 每个外部能力必须有 capability、测试替身、资源预算和 deterministic
  policy；没有这些条件时保持纯计算 VM。

### VM-6C：兼容矩阵与发布策略

**目标：** 将 compiler version、`cdbc` version、module product、debug metadata
  和 Rust VM version 的兼容关系写成矩阵，明确旧 artifact 的保留期限和升级
  诊断。

**验收：** 至少保留 metadata-free `cdbc 0.1`、当前 debug metadata、module
  products 和未来 successor version 的拒绝/兼容样例；版本变更遵守
  [`docs/versioning.md`](versioning.md)。

## 10. VM-7：条件研究轨道（不在默认队列）

这些方向只能在前置决策、基准和兼容性证据完成后单独立项：

### VM-7A：持久会话、快照和回滚

当前 `run` 是一次性执行。持久 VM 状态会涉及 globals、closures、共享 cell、
数组/map/struct 别名、native 状态、输出和调试事件的 snapshot/rollback，不能
作为 REPL 或性能优化的顺手扩展。只有当 replay 性能或宿主场景证明一次性 VM
不足时，才先做状态模型决策，再实现 session API。

### VM-7B：任务调度与异步

需要先决定语言级任务、yield point、阻塞/取消、共享数据和 deterministic
ordering；VM 不单独发明 async 语义。默认保持单线程、同步执行。

### VM-7C：JIT 或其他 native 加速

需要稳定的 bytecode/verifier、profile schema、debug/exception mapping、
resource accounting 和 fallback interpreter。没有 hot workload、编译成本、
代码缓存和错误回退数据时，不启动 JIT 实现。

### VM-7D：tracing GC（若 VM-2C 未完成）

GC 只能作为 VM-2 的决策后续，不作为独立的“先做起来”路线；其 root、暂停、
循环、debug 和宿主 API 影响必须与 VM-2 合并评审。

## 11. 交付与验证契约

### 11.1 VM focused gate

只改 Rust VM、artifact parser、linker 或 VM tests 时，至少运行：

```sh
cargo test --manifest-path vm-rs/Cargo.toml
python3 tests/bytecode_artifact_tests.py ./build/compiler_design vm-rs
python3 tests/bytecode_module_artifact_tests.py ./build/compiler_design vm-rs
python3 tests/run_rust_vm_tests.py ./build/compiler_design vm-rs --goldens
python3 tests/debugger_tests.py ./build/compiler_design vm-rs
python3 tests/profile_tests.py ./build/compiler_design vm-rs
git diff --check
```

若变更涉及模块缓存、`.cdbc` 文本、C++ emitter、native 名称或 verification
inventory，还必须运行相应的 `bytecode_module_cache_tests.py`、golden、CTest、
canonical verification 和 malformed corpus。完整仓库 gate 仍以
[`AGENTS.md`](../AGENTS.md) 和 [`docs/roadmap.md`](roadmap.md) 的命令为准。

### 11.2 新 slice 的证据格式

每个 VM slice 在决策记录或 PR 描述中记录：

1. 基线 commit、Rust/C++/Cargo/toolchain 版本；
2. 变更的 artifact/runtime contract 和明确 non-goal；
3. focused case ID、输入 artifact/source、stdout/stderr/exit；
4. linked/module/debug metadata 的兼容结果；
5. benchmark 前后数据（如果触及性能、内存或加载）；
6. fallback、兼容路径或重复实现何时可以删除。

测试生成的 `tests/__pycache__/`、`build/`、Cargo `target/` 和 benchmark 临时
产物不是路线图证据，不应提交。

## 12. 当前下一步

VM-1A、VM-1B、VM-1C、VM-2A、VM-2B 的 tracked-object/retained-byte 测量边界、
VM-3A 的第一 library boundary、typed error/version boundary、VM-3B 的第一
linker report slice、VM-4A 的第一 interactive debugger slice、VM-4B 的第一
deterministic profile counter slice、VM-4C 的第一 structured kind slice、VM-5A
的 reproducible benchmark baseline/scale slices 和 VM-5B 的 trace-off instruction
preamble/function-body cache/frame-boundary/borrowed-call-site/name-operand/global-cell-cache slices 已完成；GC、persistent VM、
JIT 和新的 artifact version 仍未进入默认队列。VM-4B 的 wall-clock 与
allocation/peak 扩展、VM-4C 的统一 host schema，以及 VM-5B 的后续性能优化都
需要独立决策；下一步应依据十一个 workload 的 baseline 数据选择下一个明确的
VM-5B 执行循环或 frame 优化目标，同时保持现有 CLI、linked/module artifact、
trace、profile 和 typed diagnostics 兼容。

这份路线图的成功标准不是同时铺开所有 VM 研究方向，而是让每个运行时能力
都有清楚的契约、独立的证据和可回退的迁移路径；语言 roadmap 继续独立演进，
两者只在明确的 `.cdbc`/runtime 边界处交汇。
