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
| `include/TypeChecker.hpp` + `src/TypeChecker.cpp` | TypeChecker 入口、作用域/声明、语句分发（核心） |
| `include/TypeCheckerInternal.hpp` | TypeChecker 各实现文件共享的内部辅助函数（非公共 API） |
| `src/TypeCheckerModules.cpp` | 模块图调度、模块接口生成/校验、import/export 与命名空间处理 |
| `src/TypeCheckerTypes.cpp` | struct/enum/impl/method 声明、签名解析与类型声明检查 |
| `src/TypeCheckerFunctions.cpp` | 函数/泛型/调用、能力约束、调用副作用失效 |
| `src/TypeCheckerExpressions.cpp` | 表达式检查、字面量/构造/match/pattern、native 与成员调用、索引/字段/一元/二元 |
| `src/TypeCheckerFlow.cpp` | nullable flow 收窄与事实命名 |
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

`FrontendSession` 是普通模式（非 LSP、非格式化单独路径）唯一的源码加载前端。它负责文件/标准输入/虚拟文档的读取、import 递归发现、模块身份去重、模块图构建、缓存 sidecar 预载，以及文件感知诊断的包装。**所有输入统一走单条模块路径**：每个文件（含 stdin、虚拟文件）都是一个模块，CLI 文件按命令行顺序成为 entry 模块，跨文件可见性必须通过 `import`/`export`。

### 4.0 生命周期与配置

每次加载入口（`loadFiles` / `loadStdin` / `loadVirtualFiles`）都会先调用 `reset()`（`src/FrontendSession.cpp:313`），清空：

- `units_`（已加载模块记录）、`canonicalToUnitId_`（canonical path -> unit id 映射）、`loadingStack_`（循环检测栈）；
- `sourceFiles_`、`directEntryCanonicalPaths_`、`moduleGraph_`、`preloadedModuleInterfaces_`；
- `moduleProductCacheLoad_`（manifest 惰性缓存）、`virtualSources_`、`virtualSourceMode_`。

配置必须在加载之前通过 setter 设置：

| setter | 作用 |
| --- | --- |
| `setImportSearchPaths(paths)` | 设置 `-I`/`--import-path` 搜索目录（按 CLI 顺序保存，路径做 `lexically_normal`） |
| `setVirtualImportRoots(paths)` | 虚拟 workspace 模式允许磁盘导入的根目录集合（canonical 化保存） |
| `setModuleInterfaceCacheDirectory(path)` | 设置缓存根目录，同时使 `moduleProductCacheLoad_` 失效 |
| `setModuleInterfaceCacheStrict(bool)` | strict 模式：sidecar 不可信时直接抛 Import 诊断而不是源回退 |
| `setModuleProductCacheMode(bool)` | 模块产物边界：要求 `cdbc-cache 0.2` manifest 与配对产物同时可信 |

### 4.1 核心数据模型

`ParsedUnit`（`include/FrontendSession.hpp`）表示一个已加载模块：

```cpp
struct ParsedUnit {
    std::size_t id = 0;                    // 快照本地模块 id（依赖先序分配）
    std::size_t sourceId = 0;              // 对应 sourceFiles_ 下标
    std::string path;                      // display path（用户拼写，诊断用）
    std::string canonicalPath;             // 归一化身份，去重/缓存/图身份用
    std::string source;                    // 原始源码
    std::vector<Token> tokens;             // 词法结果（已关联 source id）
    std::vector<StmtPtr> statements;       // AST 顶层语句；sidecar 预载时为空
    bool isEntry = false;                  // 是否为 CLI/stdin/虚拟入口
    std::optional<ModuleInterfaceArtifact> interfaceArtifact; // 预载 .cdi 时存在
};
```

`SourceFile`（`include/SourceIdentity.hpp`）是 `Program::sources` 中的源码元数据：`path`（可能为相对 cwd 的显示路径）、`text`、快照本地 `SourceFileId`，以及可选的 `moduleIdentity`（仅“模块感知”图才填充，见 4.5）。

`ModuleGraph`（`include/ModuleGraph.hpp`）是图的纯值类型：`nodes`（module id、source id、display/canonical path、isEntry）与 `edges`（importing/imported module id、`Import`/`ReExport` 类型、requested path 原文）。

### 4.2 三个加载入口

**`loadFiles(paths)`（`src/FrontendSession.cpp:574`）—— 普通 CLI 模式**

1. 把所有 CLI 路径先做 `normalizedExistingPath` 并写入 `directEntryCanonicalPaths_`：这个集合让“CLI 文件同时又被 import”的模块永远走源码解析，不会使用 sidecar。
2. 按 CLI 顺序逐个 `loadFile(path, isImport=false, isEntry=true)`。
3. **入口错误聚合**：每个 entry 的 `FileDiagnosticErrorList` / `FileDiagnosticError`（lex/parse 错误）被收集到 `entryErrors`；全部入口处理完后再一次性抛出。这样 `compiler_design a.cd b.cd` 中两个文件各自的词法/语法错误都会报告。import/循环等 locationless 加载错误不属于这两个类型，仍然立即抛出（stop-first）。
4. 全部成功后 `rebuildModuleGraph()` -> `assembleProgram()`。

**`loadStdin(input)`（`src/FrontendSession.cpp:537`）—— LSP 单文档与内部测试**

1. 源码作为 `<stdin>`、source id 0 的 entry 模块。
2. `parseSource("<stdin>", source, pathless=true, 0)`：lex + parse 一步完成并包装为 pathless `FileDiagnosticError(List)`。
3. `hasImportToken(tokens)` 或顶层带 `from` 的 `ExportStmt` 命中即抛 `Import error: import is not supported from stdin`。
4. 构造单 unit -> `rebuildModuleGraph()` -> `assembleProgram()`。

**`loadVirtualFiles(files)`（`src/FrontendSession.cpp:603`）—— LSP workspace**

1. `virtualSourceMode_ = true`；每个虚拟文件按 canonical path 写入 `virtualSources_`（open 文档优先于磁盘同名文件）。
2. 每个文件作为 entry 调用 `loadFile`（不聚合错误，保持 LSP 现有 stop-first 行为）。
3. 组装模块图程序。

### 4.3 `loadFile` 单模块加载流水线（`src/FrontendSession.cpp:613`）

`loadFile(path, isImport, isEntry)` 是唯一递归加载函数：

1. **路径身份**：`normalizedExistingPath`（`weakly_canonical`，失败回退 `absolute`）得到 canonical path；`displayPath` 保留用户拼写用于诊断。
2. **循环检测**：canonical path 已在 `loadingStack_` 中 -> 抛 `Import error: import cycle detected: <a> -> <b> -> <a>`（`displayCycle` 只打印环段）。
3. **去重**：`canonicalToUnitId_` 命中 -> 若本次是 entry 则把已有 unit 标记 `isEntry = true` 并返回既有 id（同一文件既被 CLI 指定又被 import 时复用）。
4. **虚拟/磁盘边界**：虚拟模式下，不在 `virtualSources_` 且 import 不在任何 `virtualImportRoots_` 内 -> 拒绝；否则 `canOpenFile` 检查。entry 失败抛 `failed to open input file: <path>`（`std::runtime_error`），import 失败抛 Import 诊断。
5. `loadingStack_.push_back(canonicalPath)` 后读源码（虚拟源优先）。
6. **缓存 sidecar 预载**（仅 `isImport` 且 canonical path 不在 `directEntryCanonicalPaths_`）：见 4.4。成功则直接构造“无 body”unit 并返回；失败按 strict/fallback 决定抛错或继续源码解析。
7. **词法/语法**：`parseSource(displayPath, source, pathless=false, sourceId)` 一步完成 lex + token 标注 + parse，并把 `LexErrorList`/`ParseErrorList`/`DiagnosticError` 统一包装为带文件的诊断；parser recovery 可能一次性带出多条 parse 诊断。
9. **import/export 发现**：遍历顶层语句，`ImportStmt` 和带 `sourcePath` 的 `ExportStmt` 经 `resolveImportPath` 得到候选路径，递归 `loadFile(resolution.path, true, false)` 并把返回的 id 写回 `resolvedModuleId`。**依赖先于导入方入列**，因此 `units_`/图节点天然是依赖先序。
10. **注册**：`unit.id = units_.size()`，压入 `units_`，写入 `canonicalToUnitId_`，`loadingStack_.pop_back()`，返回 id。

异常路径统一在三个 catch 中 `loadingStack_.pop_back()` 后重抛；虚拟模式下 locationless 的 Import 错误会被重包装为带当前虚拟文件上下文的 `FileDiagnosticError`。

### 4.4 import 路径解析（`resolveImportPath`，`src/FrontendSession.cpp:486`）

1. `importPath(pathToken)` 解码字符串字面量；`isExplicitImportPath` 判定绝对路径或 `./`/`../` 前缀。
2. **base 列表**：
   - 绝对路径：单个空 base（直接使用请求路径）；
   - 相对路径：先 `importingPath.parent_path()`（import 声明所在文件目录）；
   - 非显式路径再追加 `importSearchPaths_`（`-I`/`--import-path`，按 CLI 顺序）。
3. **每个 base 的候选**（`importCandidatesForBase`）：`base / requested` 的 `lexically_normal` 形式；若请求路径无扩展名，再追加 `+ ".cd"` 候选。显式路径只产生自身候选。
4. **可用性**：候选命中 `virtualSources_`（按 canonical 比较）或磁盘可读（虚拟模式下还要求 `pathWithinRoot(candidate, root)`）即返回；`resolveImportPath` 返回的是未规范化的候选路径，真正的 canonical 化在 `loadFile` 内完成。
5. **失败**：显式路径抛 `failed to open import: <display>`；非显式抛 `failed to resolve import `<x>`; tried: <候选列表>`。

### 4.5 缓存 sidecar 与模块产物边界

`loadCachedInterface(canonicalPath, source)`（`src/FrontendSession.cpp:368`）：

1. 未设置缓存目录 -> 直接返回空结果（走源解析）。
2. 读取 `<cache>/interfaces/<canonical>.cdi`；按错误文本区分 `invalid identity metadata` / `malformed sidecar` / `missing sidecar` 作为拒绝原因。
3. 校验 sidecar `identity` 与 `canonicalPath` 字段等于当前 canonical path，且 `sourceHash == moduleCacheHash(source)`。
4. `moduleProductCacheRejection`（仅 `--module-cache` + `--emit-module-bytecode` 的 module-product 模式）：惰性读取并缓存 `<cache>/module-cache.cdbc`（`cdbc-cache 0.2`），按 identity 找到记录，比较 `cacheKey`、`artifactPath`、`interfaceArtifactPath` 三者与 sidecar 派生的期望值。
5. 配对产物存在性：`is_regular_file(<cache>/<moduleCacheArtifactPath>)`。
6. 全部通过才返回可信 artifact。

`loadFile` 中的消费逻辑：

- **strict**（`setModuleInterfaceCacheStrict(true)`）：任何拒绝原因 -> `Import error: module interface cache rejected for <canonical>: <reason>`；
- **非 strict**：拒绝 -> 源回退（保留已有加载结果）；
- **成功预载**：递归加载 `artifact.dependencies`（依赖也须为 sidecar 且 `interfaceHash` 匹配）；依赖不齐时 strict 抛 `dependency interface hash mismatch`，非 strict 继续源解析（已加载的依赖 unit 保留并去重）。
- 预载 unit 的 `statements` 为空、`interfaceArtifact` 有值；`ModuleStmt::bodySourceBacked = false`，TypeChecker 只消费其接口，IR 阶段拒绝降级。

### 4.6 程序组装（`assembleProgram`，`src/FrontendSession.cpp:857`）

1. `program.sources = sourceFiles_`，`program.moduleGraph = moduleGraph_`。
2. **`moduleAware` 兼容面**：只有图满足 `nodes.size() > 1 || !edges.empty()` 时才把 `SourceFile.moduleIdentity` 填成节点 canonical path。单模块程序保持旧产物/调试表面（`.cdbc` `debug_sources` 不写 `module=` 属性、调试器 `module=none`）。
3. `rebuildPreloadedModuleInterfaces()`：把所有带 `interfaceArtifact` 的 unit 转成 `ModuleInterface`（快照 id/sourceId/path/canonicalPath/isEntry 写回，依赖边按 canonical 映射为快照 moduleId）。
4. 每个 unit 构造 `ModuleStmt(unit.id, path, source, statements, isEntry, sourceId)`：`sourceHash = moduleCacheHash(source)`，`bodySourceBacked = !interfaceArtifact.has_value()`；`program.statements` 顺序 = `units_` 顺序（依赖先序）。
5. `finalizeSyntaxMetadata(program)`：`populateSyntaxRanges` + `assignSyntaxNodeIds`，给 AST 节点补齐快照本地 range 与语法节点 id。

`rebuildModuleGraph()`（`src/FrontendSession.cpp:893`）：

- 节点按 `units_` 顺序生成；
- 带 artifact 且无语句的 unit：从 `artifact.dependencies` 生成边（identity 必须能在 `canonicalToUnitId_` 中找到）；
- 普通 unit：遍历语句，`ImportStmt` -> `Import` 边，带 `sourcePath` 的 `ExportStmt` -> `ReExport` 边（requested path 保持源码原文）。

### 4.7 观察与诊断接口

- `displayTokens()`（`src/FrontendSession.cpp:986`）：按 `units_` 顺序输出每个模块 token（跳过各 unit 的 EOF），行号叠加到累计偏移，最后合成一个 EOF；供 `--tokens`。
- `losslessSourceView()`（`src/FrontendSession.cpp:1018`）：按 source id 收集 token，`buildLosslessSourceFileView` 重建注释/空白；供 `--format` / `--format-check`。
- `moduleCount()` / `moduleGraph()` / `preloadedModuleInterfaces()`：供 `main.cpp`、TypeChecker 与测试查询。

**诊断路径规则**：

- 所有文件模块（单文件、多入口、被 import 的文件）一律 pathful：首行形如 `<Kind> error at <path>:<line>:<column>: ...`，路径保留用户拼写（相对调用保持相对，绝对调用保持绝对）。
- 多个 entry 文件的 lex/parse 错误按 CLI 顺序聚合后一次报告。
- 只有 stdin 保持 pathless（`loadStdin` 走 `parseSource(..., pathless=true)`，且拒绝 import）。

### 4.8 与下游的交接

- `main.cpp`：`frontend.setImportSearchPaths(importSearchPaths)`；`--module-interface-cache`/`--module-cache` 映射到 `setModuleInterfaceCacheDirectory`；`--module-cache` 额外启用 `setModuleProductCacheMode(true)`；interface-only 消费者默认 `setModuleInterfaceCacheStrict(true)`（除非 `--module-cache-fallback`）。格式化模式从 `program.moduleGraph` 收集 entry source id 逐文件输出。
- TypeChecker：`setPreloadedModuleInterfaces(frontend.preloadedModuleInterfaces())` 后 `check(program)` 按图做依赖序检查。
- LSP：`analyzeDocument` 走 `loadStdin`；`analyzeVirtualWorkspace` 走 `loadVirtualFiles` + `setVirtualImportRoots(workspaceRoots)`。

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
- struct/enum 声明、`impl` 方法元数据（operator 重载已移除）。
- import/export 名称解析与 re-export 兼容性。

变量读取、赋值与复合赋值的解析优先消费 `DeclarationIndex` 的结构化引用
（`variableReference` / `assignmentReference` / `compoundAssignmentReference`）：
checker 维护 `DeclarationId -> Binding` 映射（`bindingsById_`），index 命中时按
符号身份解析；import 注入、无记录合成绑定等 index 未覆盖的名字回退到按名
`findVariable`。`validateMetadata` 保证两路解析在快照内一致，imported 引用元数据
因不携带快照身份而被校验跳过。

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

- 文件诊断（单文件、多文件、imported 文件、模块检查）：`Type error at <path>:<line>:<column>: ...`，路径保留用户拼写。
- stdin 保持 pathless：`Type error at 3:5: ...`。
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
