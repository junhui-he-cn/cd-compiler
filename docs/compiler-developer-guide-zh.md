# Compiler Design 编译器端开发者指南（中文）

本文档面向需要阅读、修改或扩展编译器端代码的开发者，梳理**编译器主线的完整编译流程**与其中的关键细节。语言参考和产物契约见 `README.md`、`docs/language-grammar.ebnf`、`docs/bytecode-text-format.md` 与 `docs/decisions/`。

## 1. 仓库结构

编译器端是 C++17 项目，可执行文件为 `compiler_design`；Rust VM（`vm-rs/`）是独立的 `.cdbc` 执行引擎，不是编译器流水线的一部分。

| 目录 / 文件 | 职责 |
| --- | --- |
| `src/main.cpp` | CLI 入口：参数校验、模式分派、顶层错误处理 |
| `include/` + `src/`（Lexer/Parser/Ast） | 词法、语法分析，AST 定义与打印 |
| `include/FrontendSession.hpp` + `src/FrontendSession.cpp` | 源码加载、import 解析、模块图构建、缓存 sidecar 预载 |
| `include/DeclarationIndex.hpp` + `src/DeclarationIndex.cpp` | 声明/符号/类型表达式/作用域等快照元数据 |
| `include/TypeChecker.hpp` + `src/TypeChecker.cpp` | 类型检查、nullable flow、模块接口生成 |
| `include/ModuleInterface*` | 内存模块接口与 `cdi 0.1` sidecar 产物 |
| `include/ModuleGraph.hpp` | 模块图节点/边值类型 |
| `include/ModuleCache.hpp` + `src/ModuleCache.cpp` | 模块产物缓存 key、`cdbc-cache 0.2` manifest |
| `include/IR*` + `src/IR*` | 三地址虚拟寄存器 IR |
| `include/Optimizer.hpp` + `include/SSA.hpp` | O1 SSA 优化管线 |
| `include/Bytecode*` + `src/Bytecode*` | IR 到字节码的降低、`.cdbc` 文本产物 |
| `include/LanguageServer.hpp` + `src/LanguageServer.cpp` | 独立的 LSP stdio 模式 |
| `tests/` | golden、CTest、模块缓存、LSP、调试器、验证清单等测试 |

## 2. 主线编译流程总览

普通模式（非 LSP、非格式化）的主线如下：

```text
CLI 参数解析/校验（main.cpp）
  -> FrontendSession::loadFiles：读取源码、词法、语法、import 递归、模块图
  -> TypeChecker::check：DeclarationIndex 收集 + 模块依赖序类型检查 + 模块接口
  -> IRCompiler::compile：AST -> 线性 IR（按 entry 模块/单模块）
  -> optimizeProgram：O0 原样；O1 走 SSA 优化后重建 IR
  -> BytecodeCompiler::compile：IR -> BytecodeProgram
  -> 文本产物：writeBytecodeText（链接程序）或 writeBytecodeModuleText（模块产物）
```

`--tokens`、`--ir`、`--bytecode` 是观察性输出，但仍会继续走到类型检查；只有 `--emit-bytecode` / `--emit-module-bytecode` 会真正写产物。`--format` / `--format-check` 在加载源码后提前返回，不做类型检查与代码生成。

## 3. CLI 入口（src/main.cpp）

### 3.1 参数与模式

- 常规模式：`--tokens`、`--ir`、`--bytecode`、`--module-interface`，可组合。
- 格式化：`--format` 或 `--format-check`（可带 `--format-indent-width N`），与其它模式互斥。
- 产物：`--emit-bytecode out.cdbc`、`--emit-module-bytecode dir`。
- 模块缓存：`--module-cache dir`、`--module-interface-cache dir`、`--module-cache-strict`、`--module-cache-fallback`、`--module-rebuild-report file`。
- 优化：`--opt-level 0|1`，仅在 `--ir`、`--bytecode` 或产物发射时有效。
- LSP：`--lsp`，独立 stdio 协议，不允许携带其它参数。

**所有非 LSP 模式都至少需要一个源文件**（参数缺失时打印用法并返回 64）。每个源文件都是独立模块；多个源文件按命令行顺序作为入口模块，跨文件可见性必须通过 `import`/`export`。

### 3.2 关键校验规则

- `--module-cache-strict` 与 `--module-cache-fallback` 互斥。
- `--module-cache-fallback` 仅允许 interface-only 缓存消费者，且不能与 `--module-cache` / `--emit-module-bytecode` 同用。
- `--module-interface-cache` 不能提供字节码体：若同时要产物，必须用 `--module-cache` + `--emit-module-bytecode`。
- `--module-cache` 与 `--module-interface-cache` 若同时给出，必须指向同一目录。
- interface-only 消费者默认 strict（除非显式 `--module-cache-fallback`）。
- `--module-rebuild-report` 依赖 `--module-cache`。

### 3.3 错误出口

类型/文件诊断返回 1；用法错误返回 64；运行异常返回 1。诊断统一转换为带源码上下文的 CLI 文本（见第 8 节）。

## 4. 前端：源码加载与模块图（FrontendSession）

`FrontendSession::loadFiles(paths)` 是普通模式的前端入口。所有输入统一走单条模块路径，每个 CLI 文件都是 entry 模块：

### 4.1 入口模块加载

对每个直接输入调用 `loadFile(path, isImport=false, isEntry=true, ...)`：

1. **规范化与去重**：`normalizedExistingPath` 得到 canonical path；`canonicalToUnitId_` 命中则复用（entry 标记可叠加）。
2. **循环检测**：`loadingStack_` 出现重复 canonical path 即抛 `Import` 诊断。
3. **缓存 sidecar**：对非 entry 的 import，尝试 `loadCachedInterface` 读取 `.cdi` sidecar；strict 模式下不可信即抛 Import 诊断，非 strict 则继续解析源码。sidecar 可用时该 unit 不保留 AST body（`statements` 为空），依赖也须全部可缓存。
4. **词法/语法**：Lexer `scanTokens()` 后 `annotateSourceTokens` 把快照 source id 关联到 token；Parser `parse()` 产出 AST。
5. **递归解析 import/export**：遍历语句，`ImportStmt` 与带 `sourcePath` 的 `ExportStmt` 经 `resolveImportPath` 解析后递归 `loadFile`。
6. `rebuildModuleGraph()`：按 unit 与语句构建 `ModuleGraph`（节点：module id、canonical path、entry；边：import / re-export，带 requested path）。每次编译（文件、stdin、虚拟文件）都产生图；`Program` 始终携带 `moduleGraph` 与 `ModuleStmt` 列表。
7. `rebuildCombinedSource()` + `assembleProgram()`：模块语句带上 `moduleId`、`isEntry`、`sourceHash` 与 `resolvedModuleId`。多个入口模块的词法/语法错误按 CLI 顺序聚合后一起报告；import/循环等加载错误仍立即停止。

兼容性保留面：

- 单个入口模块（一个 CLI 文件且无 import）与 stdin 保持 pathless 诊断。
- 单个模块的 AST 文本输出保持扁平 `Program` 形状；多模块程序输出 `Module N entry` 包装。
- 单模块程序的 `.cdbc`/调试元数据不写入 `module=` 属性，保持既有产物表面；多模块图才记录模块身份。

### 4.2 import 路径解析

`resolveImportPath` 的搜索顺序：

- 绝对路径：直接作为候选。
- 相对路径：先 `importingPath.parent_path()`，再（非显式路径时）依次尝试 `--import-path`/`-I` 目录。
- 每个 base 上按 `importCandidatesForBase` 尝试候选（显式 `./x.cd` 只试该路径；非显式还会试 `x.cd` 等）。
- 虚拟 workspace 模式（LSP）中，`--import-path` 之外的文件不允许磁盘导入，除非声明 `virtualImportRoots`。

## 5. 类型检查（TypeChecker）

### 5.1 入口 `TypeChecker::check(program)`

顺序如下：

1. `DeclarationIndex::collect(program)`：构建快照本地声明/符号/类型表达式/作用域/import-export/模式绑定等索引（TypeChecker 内的 legacy 路径逐步迁移到这些记录）。
2. 重置 checker 状态，导入预载模块接口（来自前端 sidecar）。
3. 按模块依赖顺序检查（见下），每完成一个模块体即生成一份 `ModuleInterface`。

### 5.2 模块依赖顺序

`checkModulesInDependencyOrder` 对模块图做 DFS：

- 依赖模块失败或跳过时，导入方同样跳过（不重复报告）。
- 预载（sidecar）模块：不进入 body checker，仅使用其接口。
- 每个模块体 `checkModule` 在隔离的瞬时状态（作用域、struct/enum 表、方法表、flow facts 等）中检查，成功后把公开符号写入 `moduleSymbols_` 并生成接口；失败则恢复状态并记录一条带文件上下文的 Type 诊断。

### 5.3 检查内容

- 语句/表达式类型检查、重复声明、未定义变量、函数签名与调用兼容、泛型约束。
- nullable flow：`if`/`while`/`for`/字段/数组索引的 nil 收窄与失效规则（详见 `AGENTS.md` 的 Current Language Semantics）。
- struct/enum 声明、`impl` 方法、operator 重载元数据。
- import/export 名称解析与 re-export 兼容性。

## 6. IR 生成（IRCompiler）

### 6.1 入口

- `compile(program, declarationIndex)`：无独立模块 id，遍历全部语句，只编译 entry 模块（`ModuleStmt::isEntry`），import 语句递归编译其模块体。
- `compileModule(program, moduleId, declarationIndex)`：只编译一个模块体，用于 `--emit-module-bytecode`；import/export 变成 `IRModuleDependency`（携带依赖模块 id、边类型、请求路径、当前指令偏移），供链接器使用。

### 6.2 降低方式

`compileStatement` 按 AST 形状分发；核心包括：

- 语句：`let`、`print`、`if/else`、`while`、`for`、`for-in`、`break`/`continue`、`fun`、`return`、`match`、表达式语句。
- 表达式：字面量、数组/映射/区间、struct/enum 构造、索引与字段访问/赋值、调用、函数表达式、模式匹配、一元/二元/逻辑/赋值/复合赋值。
- lowering 直接消费 `DeclarationIndex` 的迁移记录（绑定、调用目标、字段/索引操作、模式、M1D 函数/返回/捕获元数据、M1E1 签名类型等）。
- 绑定通过 `registerBinding` 建立 resolved name -> BindingId 映射；`StoreVar` 用于声明/初始化，赋值类操作使用 assignment 专用指令（语义区别很重要）。

IR 是三地址虚拟寄存器形式，常量池、名字表、源信息随 `IRProgram` 传递。

## 7. 优化（Optimizer / SSA）

`optimizeProgram(ir, level)`：

- **O0（默认）**：原样返回 IR，不做任何变换。这是兼容性默认路径。
- **O1（显式）**：`optimizeIRProgram` 构建 CFG、SSA、执行复制传播/常量折叠/分支简化/块合并/跳线等 passes，验证 de-SSA 结果后 `rebuild` 回 `IRProgram`。优化仅替换 IR 流与 main 依赖锚点，常量池/名字/源信息保持一致。

`ssaOptimizationPipelineFingerprint` 为模块缓存记录 O1 管线指纹；缓存 key 包含优化级别与管线指纹。

## 8. 字节码降低与产物（BytecodeCompiler / BytecodeTextEmitter）

### 8.1 降低

`BytecodeCompiler::compile(ir)`：常量池、名字表、寄存器数直接复制，`lowerInstructions` 逐条映射 `IROp -> BytecodeOp`，`lowerFunction` 处理函数表。所有索引做 `checkedU32` 溢出校验。

### 8.2 `.cdbc 0.1` 文本产物

- `writeBytecodeText`：链接程序（无 `artifact` 声明），可直接 `run`。
- `writeBytecodeModuleText`：模块产物，开头 `artifact: module` + `module:` 元数据（identity、path、canonical_path、entry、依赖表），经 Rust VM `link` 后才能运行。
- 可选 `debug_ranges`/`debug_locations`：源局部半开字节区间，**不得序列化快照本地 id**。
- 格式契约见 `docs/bytecode-text-format.md`；改 opcode 或格式时需同步 C++ 发射器、Rust `vm-rs/src/format.rs`、文档与 `tests/bytecode_artifacts/`。

### 8.3 模块缓存

`--emit-module-bytecode dir --module-cache cache` 时：

1. 先读 `cache/module-cache.cdbc`（`cdbc-cache 0.2` manifest）。
2. 为每个图节点计算 key：identity、source hash、interface hash、优化级别与管线指纹、entry 顺序、依赖（canonical path + kind + requested path + interface hash）。
3. `planModuleCacheBuild` 决策 Reuse / Rebuild；Reused 直接复制缓存产物，否则重新编译并写 sidecar（`interfaces/`）+ manifest。
4. `--module-rebuild-report` 输出私有/公开失效与构建决策。

缓存目录必须与产物目录不同。接口 sidecar 只有在配对产物也存在时才可信（module-product 边界）。

## 9. 诊断格式

```text
<Kind> error at <line>:<column>: <message>
  <source line>
  <caret>
<Kind> error: <message>
```

- 单文件 pathless：`Type error at 3:5: ...`。
- 文件诊断（imported 文件、直接多文件输入、模块检查）：`Type error at <path>:<line>:<column>: ...`。
- Lexer 可恢复错误先聚合再进入 parser；未终止字符串 stop-at-EOF；parser 语句边界与 `impl` 方法列表可恢复并报告多条。
- 模块调度每个失败模块在依赖序中报告一条，导入方被抑制。
- 编译（IR/Bytecode）、import 加载与运行时诊断目前无位置（除非未来切片显式改变）。

## 10. 构建与验证

### 10.1 构建

```sh
cmake -S . -B build
cmake --build build
```

### 10.2 常用验证

```sh
ctest --test-dir build --output-on-failure
python3 tests/run_golden_tests.py ./build/compiler_design
python3 tests/run_golden_tests_selftest.py
python3 tests/bytecode_artifact_tests.py ./build/compiler_design vm-rs
python3 tests/bytecode_module_artifact_tests.py ./build/compiler_design vm-rs
python3 tests/bytecode_module_cache_tests.py ./build/compiler_design vm-rs
python3 tests/lsp_tests.py ./build/compiler_design
python3 tests/debugger_tests.py ./build/compiler_design vm-rs
python3 tests/run_rust_vm_tests.py ./build/compiler_design vm-rs --goldens
cargo test --manifest-path vm-rs/Cargo.toml
rm -rf tests/__pycache__
```

完整 M0A 门禁：

```sh
python3 tests/verification_inventory.py
python3 tests/run_verification.py ./build/compiler_design vm-rs --report build/verification-report.json
python3 tests/run_boundary_tests.py ./build/compiler_design
python3 tests/run_malformed_tests.py ./build/compiler_design vm-rs --report build/malformed-report.json
```

### 10.3 golden 测试约定

- 成功用例：`tests/golden/<case>/input.cd` + 可选 `ast.out`/`ir.out`/`bytecode.out`/`run.out`；无预期文件视为失败。
- 错误用例：`parse_errors`、`type_errors`、`import_errors`、`runtime_errors` 各自带 `.err` 与 `.exit`；错误用例不产生 stdout。
- 有意图的输出变化用 `--update`（只重写已存在的预期文件），`--case <substring>` 限定范围；`--update-missing` 仅在确实需要新建成功输出时使用。刷新后要人工审查 diff。
- 新增 fixture 或 CTest 后运行 `python3 tests/verification_inventory.py --write` 并审查生成的 case 元数据。

## 11. 常见修改入口

| 目标 | 入口 |
| --- | --- |
| 加 token / 词法形式 | `include/Token.hpp`、`src/Lexer.cpp` |
| 加 AST 节点与打印 | `include/Ast.hpp`、`src/Ast.cpp` |
| 加语法 | `include/Parser.hpp`、`src/Parser.cpp`；保持递归下降优先级 |
| 加类型规则 | `src/TypeChecker.cpp`、`include/TypeUtils.hpp`（迁移到 `SemanticTypes` 时同步） |
| 加 IR 指令 | `include/IR.hpp`、`src/IR.cpp`、`src/IRCompiler.cpp`（尽量复用现有 op） |
| 加字节码指令 / 改格式 | `include/Bytecode.hpp`、`src/BytecodeCompiler.cpp`、`src/BytecodeTextEmitter.cpp`、`vm-rs/src/format.rs`、`docs/bytecode-text-format.md`、`tests/bytecode_artifacts/` |
| 改运行时语义 | `vm-rs/src/vm.rs` + 对应 Rust 单测 + `tests/run_rust_vm_tests.py` |
| 改诊断格式 | 相应错误类 + 刷新对应 golden |

语言层改动必须同步 `docs/language-grammar.ebnf`，用户可见行为同步 `README.md`。改 IR/字节码/运行时行为时，先加失败 fixture（可行时），实现最小改动，再有意刷新预期输出，最后跑完整验证。

## 12. 兼容性红线

以下契约在显式决策前不得改动：

- `cdbc 0.1` 文本产物契约；
- O0 作为默认优化级别；
- 普通 CLI 必须有源文件；
- 模块产物源 fallback（冷构建/可修复产物），strict 切换属于 C4 决策门；
- C++/Rust 执行奇偶（`tests/run_rust_vm_tests.py` 与相关 parity 门）；
- VM 解释器为默认执行路径（JIT 仅测试启用、白名单、可回退）。
