# Language 0.2 Phase 0 规范一致性审计基线

本文档是 `docs/cd-compiler-language-0.2-execution-plan.md` Phase 0 的产物：只记录
「文档 / 实现 / 测试」之间的差异，不修改任何语言行为。审计基准提交为
`576c0de3`。

## 审计范围

- 用户手册：`USER_MANUAL.md`
- 语法参考：`docs/language-grammar.ebnf`
- 词法：`include/Token.hpp`、`src/Lexer.cpp`
- 解析：`include/Parser.hpp`、`src/Parser.cpp`
- AST：`include/Ast.hpp`、`src/Ast.cpp`
- 类型检查：`include/TypeChecker.hpp`、`src/TypeChecker*.cpp`、`include/TypeUtils.hpp`
- 中间表示与字节码：`include/IR.hpp`、`src/IRCompiler.cpp`、`src/BytecodeCompiler.cpp`
- 测试：`tests/golden/`、`tests/bytecode_artifacts/`、`vm-rs/tests/`

## Feature Matrix

图例：`✓` 一致；`⚠` 一致但属于已知技术债；`仅文档` 文档承诺但实现缺失；`仅实现`
实现存在但文档缺失；`—` 各层均不存在（计划中可选项）。

| Feature | Manual | EBNF | Lexer | Parser | AST | TypeChecker | Tests | 结论 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `let` 变量声明 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 一致 |
| 赋值 / 复合赋值 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 一致 |
| `fun` 函数 / 闭包 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 一致 |
| 泛型声明 `fun f<T>` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 一致 |
| 显式泛型调用 `f<T>(x)` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 一致，但见「已知技术债」#1 |
| `optional<T>` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 一致 |
| 类型后置 `T?` | — | — | — | — | — | — | — | 各层均无（Phase 13 可选项） |
| 表达式后缀 `?`（unwrap/early-return） | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 一致 |
| `??` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 一致 |
| `if` / `else` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 一致 |
| `else if` | — | — | — | — | — | — | — | 各层均无（Phase 8） |
| `if let` / `while let` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 一致 |
| `while` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 一致 |
| C 风格 `for` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 一致（Phase 18 标记 legacy） |
| `for-in` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 一致 |
| `match` 语句 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 一致 |
| `match` 表达式 | 仅文档 | — | — | — | 仅实现为语句 | — | — | 漂移 #1 |
| pattern guard | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 一致 |
| `struct` 声明 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 一致 |
| struct 字面量构造 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 一致，但见「已知技术债」#2 |
| struct record pattern | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 一致 |
| `enum` 声明 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 一致（位置化 payload） |
| enum 命名 payload `Ok(value: number)` | 仅文档 | — | — | — | — | — | — | 漂移 #2 |
| enum variant pattern | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 一致（按位置绑定） |
| `impl` 方法 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 一致 |
| capability 约束 `T: Eq/Ord/Hash` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 一致（仅内置约束，见 Phase 19） |
| `import` / `export` / 转导出 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 一致 |
| 多文件 CLI 语义 | 仅文档（组合程序） | — | — | 仅实现（每文件一模块） | — | — | ✓ | 漂移 #3 |
| array | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 一致 |
| map | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 一致 |
| range | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 一致 |
| 字符串 / 转义 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 一致（当前无转义，Phase 6） |
| 数字字面量 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 一致（仅整数/小数，Phase 15） |
| 注释 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 一致（仅 `//`，Phase 16） |
| `print` 语句 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 一致（Phase 7 函数化） |
| truthiness | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 一致（非严格 bool，Phase 5） |

## 重点核对项

### 1. match expression

用户手册第 586 行起承诺 `match` 可作为表达式使用（`let label = match result {...};`，
arm 用逗号分隔、返回值），但：

- EBNF 只有 `matchStmt = "match", expression, "{", { matchArm }, "}"`，
  `matchArm = pattern, [ "if", expression ], "=>", block`；
- AST 只有 `MatchStmt`（`include/Ast.hpp`），TypeChecker/IRCompiler 也只处理
  `MatchStmt`；
- 没有 `MatchExpr` 节点、类型检查或 lowering 路径。

结论：**match 表达式是文档独有**，当前实现只支持语句。

### 2. enum named payload

用户手册第 523–555 行承诺 `Ok(value: number)` 形式的命名 payload 及按名字绑定：

- EBNF `enumPayload = typeExpr` 只有位置化类型列表；
- AST `EnumVariantDecl { name, payloadTypes }` 不保存字段名；
- `EnumVariantDecl` 中已无字段名，variant pattern 按位置绑定 payload。

结论：**命名 payload 是文档独有**，当前实现为位置化 payload。

### 3. optional\<T\> / T?

- 手册、EBNF、TypeChecker 均只支持 `optional<T>`；
- 没有任何 `T?` 类型后缀（Lexer 的 `?` 只用于表达式后缀 unwrap 与 `??`）；
- 表达式后缀 `?` 与 `??` 三层一致。

结论：一致，`T?` 属于 Phase 13 可选项，当前不存在。

### 4. generic call whitespace sensitivity

`src/Parser.cpp`（`isGenericCallStart` 附近）要求 `<` 与 callee 名字同行且
`column == name.column + name.lexeme.size()`，即 `f<T>(x)` 的 `<` 必须紧邻名字。
EBNF 注释明确记录了这条限制。手册没有说明该限制。

结论：实现与 EBNF 一致，但该空白敏感规则是 Phase 11 要移除的技术债；手册缺少说明。

### 5. struct literal in condition

`Parser::conditionExpression()` 在解析 `if`/`while` 条件时把
`allowStructConstructors_` 置为 false（`src/Parser.cpp:951-958`），因此条件中不能
直接写 `Point { x: 1 }`。EBNF 和手册都没有记录这条限制。

结论：**实现独有限制**，未在 EBNF/手册文档化，是 Phase 10 要移除的 parser hack。

### 6. print statement

`print` 是关键字（`TokenType::Print`），AST 有 `PrintStmt`，IR 阶段改写为
`call_native print`。手册、EBNF、实现一致。Phase 7 计划改为普通函数调用。

### 7. truthiness

手册与 EBNF 一致：只有 `nil` 和 `false` 为假，`0`、`""` 为真；条件不做 bool 类型
限制。TypeChecker 未强制条件类型。Phase 5 将改为严格 `bool`。

### 8. C-style for / for-in

两者手册、EBNF、AST（`ForStmt`/`ForInStmt`）、IR、测试全部一致。C 风格 `for`
计划在 Phase 18 标记为 legacy。

### 9. import / export

手册第 9 章、EBNF `importDecl`/`exportDecl`、AST `ImportStmt`/`ExportStmt`、
FrontendSession 模块图语义一致（直接导入暴露导出名、别名限定访问、转导出）。
但用户手册第 126 行的 CLI 描述仍是旧的「多个文件拼接为一个组合程序」，与当前
「每文件一个模块」实现矛盾，见漂移 #3。

## 规范漂移清单

| # | 漂移 | 证据 | Phase 1 决策 |
| --- | --- | --- | --- |
| 1 | 手册承诺 `match` 表达式，实现只有语句 | `USER_MANUAL.md:586`；AST 只有 `MatchStmt`；EBNF 只有 `matchStmt` | 以文档承诺为准；已在 Phase 2 实现（`MatchExpr` 统一，语句位置为表达式语句） |
| 2 | 手册承诺 enum 命名 payload，实现位置化 | `USER_MANUAL.md:523-555`；`EnumVariantDecl` 无字段名；EBNF `enumPayload = typeExpr` | 以文档承诺为准；已在 Phase 3 实现（命名/位置化二选一，构造与 pattern 仍按位置） |
| 3 | 手册 CLI 仍是组合程序模型 | `USER_MANUAL.md:126,640` vs 每文件一模块（`FrontendSession`/AGENTS） | 以现有实现为准；已修正手册两处 |
| 4 | 条件中禁用 struct 字面量未文档化 | `src/Parser.cpp:951-958`；EBNF/手册无说明 | 以现有实现为准；已补充手册/EBNF 说明并新增 negative test，Phase 10 再移除 hack |

## 已知技术债（Phase 0 记录，不修复）

1. 显式泛型调用依赖 `<` 与名字紧邻的空白敏感规则（Phase 11 改为 `f::<T>(x)` 或等价方案）。
2. `if`/`while` 条件通过 `allowStructConstructors_` 上下文开关禁用 struct 构造（Phase 10）。
3. truthiness 允许非 bool 条件，`&&`/`||` 返回 operand 值（Phase 5 收紧）。
4. 字符串无转义、无块注释、无 `else if`、无尾逗号统一（Phase 6/8/14/16）。
5. 数字字面量仅整数/小数（Phase 15）。
6. capability 只是内置约束，非完整 trait/capability 系统（Phase 19 决策）。

## 验证

Phase 0 未修改任何源码，全部现有验证保持通过：

- `python3 tests/run_golden_tests.py ./build/compiler_design` → 787/787
- `cargo test --manifest-path vm-rs/Cargo.toml` → 全部通过
- `ctest --test-dir build --output-on-failure` → 47/47
- `python3 tests/run_verification.py ./build/compiler_design vm-rs` → 1825/1825
- `python3 tests/run_malformed_tests.py` → 107/107
- `python3 tests/run_boundary_tests.py` → 4/4
- `python3 tests/vm_compatibility_matrix.py` → 7 cells 通过

## Phase 1 处理结果

Phase 1 只处理 Phase 0 确认的漂移，未引入计划外语法：

- 漂移 #1、#2 明确选择「以文档承诺为准」：`USER_MANUAL.md` 的 match 表达式与命名
  payload 承诺保留，实现分别由 Phase 2（match expression）与 Phase 3（enum named
  payload）完成。
- 漂移 #3 选择「以现有实现为准」：`USER_MANUAL.md:126` 与 `:640` 已从「多个文件
  拼接为组合程序」改为「每文件一个模块、多个 CLI 文件按顺序作为入口模块」。
- 漂移 #4 选择「以现有实现为准」：`USER_MANUAL.md` 与 `docs/language-grammar.ebnf`
  已记录「条件/guard/for-in 迭代对象内禁用 struct 字面量构造」的限制，并新增
  `tests/golden/parse_errors/struct_literal_in_condition.*` 锁定当前诊断。

Phase 1 验证：golden 788/788（新增 1 个 parse-error 夹具），其余套件与 Phase 0
一致全部通过。

## 结论与下一步

Phase 0/1/2/3/4 完成：match 表达式已统一为 `MatchExpr`；enum 命名 payload 已实现；
绑定默认不可变，`let mut` 声明可变绑定，函数参数同样不可变，对不可变绑定的赋值与
复合赋值是类型错误，数组元素/结构体字段等对象内部可变性不受影响（含闭包捕获用例）；
`if`/`while`/`for` 条件与 `&&`/`||` 操作数强制为 `bool`，逻辑运算返回 `bool` 并保留
短路，可选值通过 `if let`/`??` 显式处理；字符串支持 `\n`/`\r`/`\t`/`\0`/`\\`/
`\"` 转义，裸换行与未知转义在词法阶段报错；`print` 已从语句/keyword 改为普通
native 函数调用 `print(value)`，AST 不再有 `PrintStmt`。两条以实现为准的漂移已修正。
`else if` 链已支持，`else` 绑定最近的未配对 `if`。Phase 9 已开始：record pattern
支持字段简写，例如 `Point { x, y }` 等价于 `Point { x: x, y: y }`；下一窄切片是
rest pattern（`..`）。
