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

当前基线的主要限制也要明确记录：VM 是 binary crate 而不是稳定库 API；
运行时对象主要由 `Rc<RefCell<...>>` 和手工 identity 管理；资源预算和协作式
取消已经由 VM-1B 固定，但还没有交互式 debugger、快照/回滚、二进制 artifact
或 JIT。`cdbc 0.1` 的兼容性优先级高于这些后续能力。

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
registry、cycle policy、GC 和 peak-memory measurement 仍未完成，详见
[`docs/decisions/vm-heap-facade-001.md`](decisions/vm-heap-facade-001.md)。

**状态（测量窄切片已完成，2026-07-29）：** `HeapStats` ledger 现在通过只读
`Weak` 记录报告 environment、cell、array、map 和 struct 的 allocation total、
live/dead 数量；它不持有 VM root，也不通过 `Display` 遍历递归值。Rust 单测已
覆盖 acyclic/cycle、closure cell/environment cycle、native temporary、runtime
error 和 trace root release，当前 focused 结果为 `68/68`。function、range、
variant 的 inline `Value` 生命周期、精确 peak、ledger compaction、GC、搬迁
句柄和持久 host root 仍保持 deferred；下一步需要 workload 级 peak 测量和决策，
而不是直接替换 `Rc<RefCell>`。

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

### VM-4B：profile、coverage 和运行报告

**目标：** 为优化和问题定位提供机器可读的执行证据。

**交付：** 增加 opt-in 的 instruction/function/native counters、执行时间、
  allocation/peak counters（若 VM-2 已定义 heap）和 source-range coverage；
  规定输出 schema、排序和是否影响执行。

**边界：** profiling 默认关闭，不改变 stdout、runtime error、trace sequence
  或资源预算；不把 profile 结果直接当作 JIT 输入，除非 VM-7 单独决策。

### VM-4C：错误与诊断 API 稳定化

**目标：** 把 CLI 文本之外的 runtime error、stack frame、source range、
  resource exhaustion 和 artifact rejection 暴露为稳定结构，供 LSP、测试和
  宿主程序使用。

**验收：** 每种错误至少有结构化字段、稳定 kind、最小复现 artifact 和 CLI
  渲染；path canonicalization 只在显示层处理，不污染 artifact identity。

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

### VM-5B：执行循环和 frame 优化

**目标：** 在语义和资源边界冻结后，降低 dispatch、寄存器访问、函数调用、
  native callback 和临时 `Value` clone 的成本。

**候选方向：** frame 生命周期与函数 body clone、instruction dispatch、name/
  constant lookup、调用栈构造、trace-off fast path、容器借用和 native 注册表。

**边界：** 不用 unsafe/threaded dispatch/JIT 掩盖 verifier 缺口；每一次优化都
  必须保留 C++/Rust output、error、alias、debug 和 resource-limit parity。

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

VM-1A、VM-1B、VM-1C 和 VM-2A 决策记录已完成，VM-2B 的第一窄切片也已完成。
下一步仍是 VM-2B 的 live/dead/cycle、closure、native 临时 root、错误退出、
debug 观察和 peak-memory 测量；在这些证据完成前不推进 GC、persistent VM、JIT
或新的 artifact version。

这份路线图的成功标准不是同时铺开所有 VM 研究方向，而是让每个运行时能力
都有清楚的契约、独立的证据和可回退的迁移路径；语言 roadmap 继续独立演进，
两者只在明确的 `.cdbc`/runtime 边界处交汇。
