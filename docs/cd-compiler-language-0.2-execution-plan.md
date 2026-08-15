# cd-compiler Language 0.2 语法与语言设计重构执行计划

> 目标仓库：`junhui-he-cn/cd-compiler`  
> 目标：系统修复当前语言规范、Parser、类型规则和语言风格之间的不一致，并逐步形成稳定、统一、适合 Bytecode 0.2 / JIT / LSP 演进的 Language 0.2。  
> 执行原则：每个 Phase 独立实施、测试、提交；禁止一次性跨阶段重写整个前端。

---

## 0. 总体目标

当前语言已经包含泛型、`optional<T>`、后缀 `?`、`??`、`if let`、`match`、struct、enum、impl、capability constraints、闭包、模块、数组/map/range 等功能。现在最需要解决的不是继续增加 feature，而是：

- 文档、EBNF、Parser 和实际语义存在漂移；
- 默认可变、truthiness 等规则与静态类型路线不够一致；
- 字符串、`else if`、pattern、模块等基础语法仍不够完整；
- Parser 已出现依赖上下文开关、空格邻接判断等技术债；
- 一些高层语义应在编译器前端完成，而不是继续泄漏到 Bytecode / VM。

Language 0.2 推荐方向：

```text
静态类型
+ 默认不可变
+ 显式可变性
+ 严格 bool 条件
+ match expression
+ pattern matching
+ optional
+ 结构化模块
+ 编译期泛型 / capability
```

---

# 1. 总体设计原则

1. **规范优先**：`USER_MANUAL.md`、EBNF、Lexer、Parser、AST、TypeChecker、IR、tests 必须描述同一门语言。
2. **先修漂移，再加语法糖**：规范不一致优先于 range operator、`T?` 等便利语法。
3. **每项 breaking change 单独实施**：必须包含兼容策略、diagnostic、测试和文档。
4. **Source 与 Bytecode 分层**：源语言负责类型、pattern、mutability、module、generic；Bytecode 负责 `LocalId/UpvalueId/TypeId/...`。
5. **不要为了 VM 简单而破坏源语言设计**。
6. **不要一次性实施全部 Language 0.2**。

---

# 2. Phase 0：语言规范一致性审计

## 目标

建立 `Documented Language / Implemented Language / Tested Language` 差异清单，不修改任何语言行为。

## 必查文件

至少定位：

```text
USER_MANUAL.md
docs/language-grammar.ebnf
include/Token.hpp
src/Lexer.cpp
src/Parser.cpp
AST 定义
TypeChecker
IR / lowering
BytecodeCompiler
tests/
examples/
```

实际路径以仓库为准。

## 建立 Feature Matrix

逐项检查：

```text
变量声明
赋值
函数
闭包
泛型声明
显式泛型调用
optional<T>
T?
postfix ?
??
if / else / else if
if let
while
C-style for
for-in
match statement
match expression
pattern guard
struct
struct literal
struct pattern
enum
enum named payload
enum pattern
impl
capability constraint
import
export
array
map
range
string
数字字面量
comments
print
```

对每项记录：

```text
Manual
EBNF
Lexer
Parser
AST
TypeChecker
Tests
```

## 已知重点

必须确认：

- `match` 文档是否支持 expression，但 Parser 是否仅实现 statement；
- enum 文档是否支持 `Ok(value: number)`，但 Parser 是否只解析类型列表；
- `optional<T>` 与 `T?` 当前真实支持状态；
- 显式泛型调用是否依赖 `<` 与标识符是否紧邻；
- struct literal 在 condition 中是否依赖类似 `allowStructConstructors_` 的开关；
- `print` 是否为独立 token / statement；
- truthiness 的真实语义；
- import/export 的真实作用域行为。

## 输出

生成：

```text
LANGUAGE_BASELINE.md
```

## 验收

- 不修改语法；
- 当前测试全部通过；
- 列出全部规范漂移；
- 完成后停止。

---

# 3. Phase 1：修复规范漂移

只处理 Phase 0 已确认的“文档 / EBNF / Parser / AST 不一致”。

每个不一致必须明确选择：

```text
以现有实现为准
或
以文档承诺为准
```

不能同时保留两个互相矛盾的版本。

要求：

- 更新正向测试；
- 更新错误测试；
- 同步用户手册和 EBNF；
- 不顺手增加计划外新 feature。

---

# 4. Phase 2：统一 `match` 为 Expression

## 目标语法

```cd
let label = match result {
    Result.Ok(value) => "ok: " + str(value),
    Result.Err(message) => "error: " + message,
};
```

并支持：

```cd
return match result {
    Result.Ok(value) => value,
    Result.Err(_) => 0,
};
```

副作用用法继续允许：

```cd
match result {
    Result.Ok(value) => print(value),
    Result.Err(error) => print(error),
};
```

## AST

优先统一为：

```text
MatchExpr
```

statement 场景表示为：

```text
ExpressionStmt(MatchExpr)
```

不要长期同时维护高度重复的 `MatchStmt` 和 `MatchExpr`。

## TypeChecker

所有 arm 的结果类型必须兼容。

## 测试

覆盖：

- assignment 中的 match；
- return match；
- nested match；
- guard；
- 副作用 match；
- arm 类型不一致错误。

---

# 5. Phase 3：实现 Enum Named Payload

## 目标

正式支持：

```cd
enum Result {
    Ok(value: number),
    Err(message: string),
}
```

Variant metadata 至少保存：

```text
field name
field type
position
```

第一阶段 constructor 仍可：

```cd
Result.Ok(123)
```

暂不强制增加 named arguments。

Pattern：

```cd
Result.Ok(value)
```

继续工作。

## 测试

- 0/1/多 payload；
- duplicate payload field；
- debug dump；
- pattern binding。

---

# 6. Phase 4：默认不可变 + `let mut`

## 新规则

```cd
let x = 1;
```

不可重新赋值。

```cd
let mut x = 1;
x = 2;
```

合法。

Lexer 新增：

```text
mut
```

AST binding 保存：

```text
mutable: bool
```

TypeChecker 必须拒绝：

```cd
let x = 1;
x = 2;
```

并给出：

```text
cannot assign to immutable binding `x`
help: declare it as `let mut x = ...`
```

## 注意

这一阶段只定义 **binding mutability**，不要未经设计把：

```cd
let arr = [1,2];
arr[0] = 3;
```

自动解释为非法。对象内部可变性是独立问题。

## Closure 必测

```cd
let mut x = 0;
let f = fun() {
    x += 1;
};
```

必须正确。

---

# 7. Phase 5：严格 Bool 条件

## 新规则

`if`、`while` 条件必须为：

```text
bool
```

以下非法：

```cd
if 1 {}
if "" {}
if [] {}
```

逻辑运算：

```cd
a && b
a || b
```

要求：

```text
a: bool
b: bool
result: bool
```

不再采用 JS/Python 风格返回 operand 的 truthiness 语义。

Optional 检测通过：

```cd
if let value = maybeValue {
}
```

`??` 继续承担 optional fallback。

## 测试

- bool condition；
- non-bool errors；
- `&&` / `||` short-circuit；
- optional use cases。

---

# 8. Phase 6：字符串系统升级

## 普通字符串

支持：

```cd
"hello\nworld"
"\"quoted\""
"C:\\Users\\test"
"\t"
"\r"
"\0"
```

普通字符串禁止裸换行。

非法：

```cd
"hello
world"
```

非法 escape：

```cd
"\q"
```

必须有明确诊断。

## 后续可选

单独 Phase 再增加：

```cd
r"raw string"
```

或：

```cd
"""multiline"""
```

不要在第一版 escape implementation 中同时引入复杂 raw-string grammar。

---

# 9. Phase 7：`print` 普通函数化

旧：

```cd
print value;
```

新：

```cd
print(value);
```

最终删除：

```text
Print token
PrintStmt
```

统一为：

```text
CallExpr
ExpressionStmt
```

Runtime 中将 `print` 作为 builtin/native/stdlib function。

如果 Bytecode 0.2 同步推进，则最终不需要特殊 `print` opcode。

---

# 10. Phase 8：支持 `else if`

目标：

```cd
if score >= 90 {
} else if score >= 60 {
} else {
}
```

推荐 grammar：

```text
if =
  "if" condition block
  [ "else" (if | block) ]
```

AST 可保留 nested-if 表达。

测试：

- 多级 else-if；
- final else；
- nested if；
- dangling else。

---

# 11. Phase 9：Pattern Ergonomics

## Field shorthand

允许：

```cd
Point { x, y }
```

等价：

```cd
Point { x: x, y: y }
```

## Rest pattern

增加：

```cd
Point {
    x,
    ..
}
```

表示显式忽略其他字段。

注意：如果当前 struct pattern 允许省略字段，不能直接改变旧语义。必须先确定兼容策略。

测试：

- shorthand；
- rest；
- duplicate fields；
- unknown fields；
- nested patterns。

---

# 12. Phase 10：修复 Struct Literal / Condition Parser Hack

如果当前 Parser 为了解析：

```cd
if condition {
```

而在条件中关闭：

```text
Point { ... }
```

这种 struct constructor，则应移除这种脆弱 context hack。

推荐规则：

```cd
let p = Point { x: 1 };

foo(Point { x: 1 });

if (Point { x: 1 } == target) {
}
```

即：存在歧义时要求 grouping，而不是继续增加：

```text
allowXXX_
insideCondition_
```

这样的 Parser 状态开关。

---

# 13. Phase 11：显式泛型调用去除空格敏感

如果当前：

```cd
foo<T>(x)
```

的解释依赖：

```text
foo 与 < 是否紧邻
```

则必须移除这一规则。

推荐显式调用采用：

```cd
foo::<T>(x)
```

普通调用依赖推断：

```cd
foo(x)
```

泛型声明仍可：

```cd
fun foo<T>(x: T): T
```

Parser 不应再通过 token column / adjacency 猜泛型调用。

---

# 14. Phase 12：模块语法增强

保留：

```cd
import "./lib.cd" as lib;
```

增加：

```cd
import { answer, User } from "./lib.cd";
```

alias：

```cd
import { answer as getAnswer } from "./lib.cd";
```

允许声明式 export：

```cd
export fun answer(): number {
    return 42;
}
```

```cd
export struct User {
}
```

可选支持：

```cd
export { answer, User };
```

## Bare import

长期推荐：

```cd
import "./lib.cd";
```

表示：

```text
仅模块初始化 / side effect
```

而不是把所有 export 隐式注入当前 scope。

如果要改变现有行为，必须单独做 breaking migration。

---

# 15. Phase 13：`T?` Optional shorthand（可选）

可重新评估支持：

```cd
number?
User?
Result<T>?
```

等价：

```cd
optional<number>
optional<User>
optional<Result<T>>
```

表达式：

```cd
value?
```

继续保持现有 unwrap / early-return 语义。

两者分别位于 type grammar / expression grammar，不必因符号相同就禁止。

这是可选项，不是 Language 0.2 核心阻塞项。

---

# 16. Phase 14：Trailing Comma 统一

所有列表式语法尽量统一支持：

```cd
foo(
    a,
    b,
);
```

```cd
[
    1,
    2,
]
```

```cd
User {
    name: "Ada",
    age: 36,
}
```

```cd
enum Result {
    Ok(number),
    Err(string),
}
```

重点是规则统一，不要“有的列表支持、有的不支持”。

---

# 17. Phase 15：数字字面量增强

保持：

```text
number = f64
```

不增加新的整数类型。

支持：

```cd
1_000_000
3.141_592
1e10
1e-6
2.5E10
```

必须拒绝：

```text
1_
1__2
1e
1e+
```

并提供明确 lexer diagnostic。

---

# 18. Phase 16：Block / Doc Comments

增加：

```cd
/*
multi-line
comment
*/
```

可为未来预留：

```cd
/// documentation
```

如果当前没有 docs AST，`///` 可先作为普通 comment 处理，不要顺手实现完整文档系统。

---

# 19. Phase 17：Range Operator（可选）

可增加半开区间：

```cd
0..<10
```

可选闭区间：

```cd
0..=10
```

现有：

```cd
range(0, 10, 2)
```

继续保留 step 能力。

不要在此阶段顺手实现 slicing。

---

# 20. Phase 18：弱化 C-style For

当前：

```cd
for let i = 0; i < 10; i += 1 {
}
```

Language 0.2 先保留，但标记：

```text
legacy / discouraged
```

等：

```text
for-in
range
range operator
```

稳定后，再决定未来版本是否删除。

---

# 21. Phase 19：Capability System 决策

当前如果只有：

```cd
fun same<T: Eq>(...)
fun before<T: Ord>(...)
```

但用户不能自定义 capability impl，则必须二选一：

## A. 明确只是 built-in generic constraints

文档不要包装成完整 trait/capability system。

## B. 长期实现完整 capability

例如：

```cd
capability Eq {
    fun equals(other: Self): bool;
}
```

```cd
impl Eq for Person {
    fun equals(other: Person): bool {
        return this.id == other.id;
    }
}
```

推荐长期走 B，但不属于 Language 0.2 核心 P0。

无论哪种方案：

```text
VM 不应理解 capability 名字
```

应在编译器 lowering 成 witness / direct function call。

---

# 22. Phase 20：Iterable / Iterator 语言协议

长期让：

```cd
for item in value {
}
```

不再硬编码：

```text
array
map
range
```

未来可引入：

```text
Iterable<T>
Iterator<T>
```

或 capability 等价物。

与 Bytecode 0.2：

```text
iter_init
iter_next
```

协调。

此项为 P3，不要早于核心语法稳定实施。

---

# 23. 明确暂时不改的语法

以下不值得为了“现代感”制造 breaking churn：

```cd
fun foo()
```

无需急着改成：

```cd
fn foo()
```

返回类型：

```cd
fun foo(): number
```

无需急着改：

```cd
fun foo() -> number
```

分号继续保留，不引入 ASI。

---

# 24. 优先级

## P0

```text
Phase 0 规范审计
Phase 1 修复漂移
Phase 2 match expression
Phase 3 enum named payload
Phase 4 let mut
Phase 5 strict bool
Phase 6 string escapes
```

## P1

```text
Phase 7 print 函数化
Phase 8 else if
Phase 9 pattern shorthand/rest
Phase 10 struct literal grammar
Phase 11 generic call syntax
Phase 12 module syntax
```

## P2

```text
Phase 13 T?
Phase 14 trailing comma
Phase 15 numeric literals
Phase 16 comments
Phase 17 range operator
Phase 18 C-style for legacy
```

## P3

```text
Phase 19 capability
Phase 20 iterable
```

---

# 25. 每个 Phase 的 Agent 标准工作流

## Step 1：Inspect

先定位相关：

```text
Lexer
Parser
AST
TypeChecker
IR
Bytecode
Tests
Docs
```

## Step 2：描述当前行为

必须先输出：

```text
当前语法
当前 AST
当前类型规则
当前测试
已知歧义
兼容风险
```

## Step 3：最小设计

明确：

```text
old syntax
new syntax
AST impact
type impact
compatibility
diagnostic
```

## Step 4：Implement minimally

原则：

```text
small patch
no unrelated rename
no formatting sweep
no massive refactor
```

## Step 5：Tests

至少运行：

```text
lexer tests
parser tests
AST tests
typechecker tests
positive tests
negative diagnostic tests
IR/bytecode integration tests
runtime tests
```

## Step 6：Documentation

同步：

```text
USER_MANUAL.md
language-grammar.ebnf
examples
```

## Step 7：Stop

当前 Phase 完成后停止，不自动进入下一 Phase。

---

# 26. Agent 禁止事项

Agent 不得：

1. 一次性实现全部 Language 0.2；
2. 未读真实 Parser 就按本计划猜代码；
3. 文档与实现不同步；
4. 修改语法却不增加 negative tests；
5. 修改 mutability 却不测试 closure；
6. 修改 truthiness 却不测试 `&&` / `||` short-circuit；
7. 修改 string lexer 却不测试错误位置；
8. 为了解决 struct literal 继续堆 Parser context flags；
9. 保留 generic call 的 whitespace-sensitive 解析；
10. 把 binding immutable 错误理解为对象深度 immutable；
11. 加 `T?` 时改变表达式 postfix `?` 语义；
12. 在本计划中引入新 integer runtime type；
13. 顺手实现 binary bytecode；
14. 因语法调整大范围重写 VM；
15. 一个 commit 混合多个 breaking feature；
16. 当前 Phase 测试未通过就进入下一 Phase。

---

# 27. Milestones

## Milestone A：规范可信

```text
Phase 0
Phase 1
Phase 2
Phase 3
```

结果：Manual / EBNF / Parser / AST 基本一致。

## Milestone B：核心语义统一

```text
Phase 4
Phase 5
Phase 6
```

结果：

```text
default immutable
strict bool
usable string syntax
```

## Milestone C：Parser 简化

```text
Phase 7
Phase 8
Phase 9
Phase 10
Phase 11
```

结果：

```text
print 不特殊
else-if 正常
pattern 更自然
struct literal hack 减少
generic call 不依赖 whitespace
```

## Milestone D：模块与可读性

```text
Phase 12
Phase 14
Phase 15
Phase 16
```

## Milestone E：长期能力

```text
Phase 17
Phase 18
Phase 19
Phase 20
```

---

# 28. 第一条交给 AI Agent 的执行命令

```text
请执行 cd-compiler-language-0.2-execution-plan.md 的 Phase 0。

要求：

1. 不修改任何语言语义或语法；
2. 阅读 USER_MANUAL、language-grammar.ebnf、Lexer、Parser、AST、TypeChecker、IR/Bytecode lowering 和 tests；
3. 建立 Feature Matrix，对每个 feature 标记 Manual / EBNF / Lexer / Parser / AST / TypeChecker / Tests 状态；
4. 重点核对：
   - match expression
   - enum named payload
   - optional<T> / T?
   - generic call whitespace sensitivity
   - struct literal in condition
   - print statement
   - truthiness
   - C-style for / for-in
   - import/export
5. 输出 LANGUAGE_BASELINE.md；
6. 发现不一致时只记录，不修复；
7. 运行当前全部相关测试；
8. 完成 Phase 0 后停止，不执行 Phase 1。
```

---

# 29. Phase 1 的 Agent 指令示例

```text
请执行 cd-compiler-language-0.2-execution-plan.md 的 Phase 1。

只修复 LANGUAGE_BASELINE.md 已确认的文档 / EBNF / Parser 规范漂移。

要求：

1. 不引入计划外新语法；
2. 每个不一致必须明确选择以实现为准还是以文档承诺为准；
3. 每个改动增加 positive / negative tests；
4. 同步 USER_MANUAL 和 EBNF；
5. 全部测试通过后停止。
```

---

# 30. Language 0.2 目标示例

```cd
import { Result, parse } from "./parser.cd";
import "./logger.cd" as log;

export struct User {
    name: string,
    private password: string,
}

export enum LoginResult {
    Ok(user: User),
    InvalidPassword,
    Error(message: string),
}

export fun login(name: string): LoginResult {
    let user: optional<User> = findUser(name);

    if let found = user {
        return LoginResult.Ok(found);
    } else if name == "" {
        return LoginResult.Error("empty name");
    } else {
        return LoginResult.Error("user not found");
    }
}

fun main() {
    let mut attempts = 0;

    for i in range(0, 3) {
        attempts += 1;

        let result = login("Ada");

        let message = match result {
            LoginResult.Ok(user) => "welcome " + user.name,
            LoginResult.InvalidPassword => "invalid password",
            LoginResult.Error(message) => "error: " + message,
        };

        print(message);
    }
}
```

如果未来启用 `T?` 和 range operator：

```cd
let user: User? = findUser(name);

for i in 0..<3 {
}
```

---

# 31. Language 0.2 核心完成标准

- [ ] USER_MANUAL / EBNF / Parser / AST 无重大漂移；
- [ ] `match` 可作为 expression；
- [ ] enum named payload 可用；
- [ ] `let` 默认 immutable；
- [ ] `let mut` 可显式修改；
- [ ] closure mutation 语义正确；
- [ ] if / while 条件严格 bool；
- [ ] `&&` / `||` 是 bool 运算；
- [ ] 普通字符串支持基本 escape；
- [ ] `print` 不需要特殊 statement AST；
- [ ] 支持 `else if`；
- [ ] pattern shorthand 可用；
- [ ] struct literal grammar 不依赖脆弱 context hack；
- [ ] explicit generic invocation 不依赖 whitespace adjacency；
- [ ] module import/export 边界更明确；
- [ ] trailing comma 规则一致；
- [ ] number literal 支持 separator / exponent；
- [ ] block comment 可用；
- [ ] breaking change 有清晰 diagnostic；
- [ ] positive tests 通过；
- [ ] negative parser/type tests 通过；
- [ ] compiler → IR → bytecode → VM integration tests 通过；
- [ ] 文档示例均可真实编译。

---

# 32. 暂不属于本轮核心范围

未经单独批准，不要顺手加入：

```text
async / await
exceptions
classes / inheritance
macros
operator overloading
full reflection
new integer runtime types
ownership / borrow checker
effect system
coroutines
binary bytecode
GC 重构
package manager
LSP architecture rewrite
```

---

# 33. 与 Bytecode 0.2 的协调

推荐交错推进，而不是先全部完成一边。

例如：

```text
Language Phase 0-3
        ↓
Bytecode binding / closure
        ↓
Language let mut / strict bool
        ↓
Bytecode CFG / verifier
        ↓
Language modules / capability
        ↓
Bytecode module / witness lowering
```

核心原则：

> Source Language 决定用户语义；Compiler 负责 lowering；Bytecode 不应再次通过字符串和动态规则重新理解源语言。
