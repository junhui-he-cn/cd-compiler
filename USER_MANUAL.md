# Compiler Design 语言用户手册

> 适用版本：`0.1.0`（发布标签：`v0.1.0`）。本文档描述当前仓库中已经实现的语言和工具链行为，示例源文件使用 `.cd` 扩展名。
>
> Compiler Design 仍处于 `0.x` 实验阶段：语言、CLI、`.cdbc` 字节码格式和 Rust VM 的公开行为可能在后续 minor 版本中调整。发布前请以本手册、[`docs/language-grammar.ebnf`](docs/language-grammar.ebnf) 和仓库版本号为准。

Compiler Design 是一门用于实验编译器前端、类型检查、IR 和字节码执行的小型语言。源程序可以直接交给 C++ 编译器检查并查看 AST/IR，也可以生成 `.cdbc` 字节码，再由 Rust VM 执行。

本文手册面向语言使用者；实现决策、字节码文本格式和完整验证流程分别见 [`README.md`](README.md)、[`docs/bytecode-text-format.md`](docs/bytecode-text-format.md) 以及 `tests/`。手册中的“编译器”指 `compiler_design`，“VM”指 `compiler-design-vm`。

## 目录

- [快速开始](#1-快速开始)
- [源文件和基本语法](#2-源文件和基本语法)
- [值和类型](#3-值和类型)
- [运算符和赋值](#4-运算符和赋值)
- [条件和循环](#5-条件和循环)
- [函数和闭包](#6-函数和闭包)
- [数组、map 和 range](#7-数组map-和-range)
- [结构体、枚举和模式匹配](#8-结构体枚举和模式匹配)
- [模块和导入](#9-模块和导入)
- [内置函数](#10-内置函数)
- [错误和诊断](#11-错误和诊断)
- [数据结构库示例](#12-数据结构库示例)
- [当前边界](#13-当前边界)

## 1. 快速开始

### 1.1 环境要求

从源码构建需要：

- 支持 C++17 的编译器；
- CMake 3.16 或更高版本；
- Python 3.9 或更高版本（构建测试目标需要）；
- Rust toolchain 和 Cargo（只有生成或运行 `.cdbc` 时需要）。

仓库当前不提供系统级安装器。以下命令均从仓库根目录执行，生成的编译器位于 `build/compiler_design`，Rust VM 使用 Cargo 运行。

### 1.2 构建

在仓库根目录执行：

```sh
cmake -S . -B build
cmake --build build
cargo build --manifest-path vm-rs/Cargo.toml
```

如果只使用 AST、类型检查、格式化或 IR 功能，可以不构建 Rust VM。若要运行字节码，需先完成 C++ 编译器和 Rust VM 构建。

### 1.3 第一个程序

创建 `hello.cd`：

```cd
let answer = 40 + 2;
print "answer:";
print answer;
```

编译器默认模式输出 AST：

```sh
./build/compiler_design hello.cd
```

查看不同编译阶段的结果：

```sh
./build/compiler_design --tokens hello.cd
./build/compiler_design --ir hello.cd
./build/compiler_design --bytecode hello.cd
```

默认模式和 `--tokens`、`--ir`、`--bytecode` 都是检查/查看编译阶段的模式，不会执行程序。

生成并运行字节码：

```sh
./build/compiler_design --emit-bytecode hello.cdbc hello.cd
cargo run --manifest-path vm-rs/Cargo.toml -- run hello.cdbc
```

`--emit-bytecode` 只负责生成链接后的 `.cdbc` 文件；执行由 Rust VM 的 `run` 子命令完成。VM 还提供 `dump`（规范化打印 artifact）、`trace`（输出带源码位置的确定性执行事件）、`debug`（交互式断点/单步会话）和 `profile`（机器可读的确定性执行统计）：

```sh
cargo run --manifest-path vm-rs/Cargo.toml -- dump hello.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- trace hello.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- debug hello.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- profile hello.cdbc
```

`profile` 不打印程序本身的 stdout，只输出指令总数、函数调用/指令数、
native 调用数、已有源码 range 命中数和执行期间成功写入的 output 字节数。
函数记录按 artifact 定义顺序排列，native 和 range 记录稳定排序。程序运行
失败时仍会输出已经收集的 partial report，正常运行时错误或资源错误继续写入
stderr 并返回非零状态。wall-clock 时间和 allocation/peak 统计尚未纳入这个
确定性报告。

如果只想使用 C++ 编译器检查语法和类型，不需要生成字节码。

没有输入文件时，普通模式从标准输入读取源代码：

```sh
printf 'print 1 + 2;\n' | ./build/compiler_design
```

字节码和模块产物模式必须提供至少一个输入文件。

### 1.4 编译器模式速查

| 命令 | 用途 |
| --- | --- |
| `compiler_design file.cd` | 解析并打印 AST |
| `--tokens file.cd` | 打印 lexer token |
| `--ir file.cd` | 类型检查后打印三地址 IR |
| `--bytecode file.cd` | 打印 C++ 后端生成的 bytecode |
| `--module-interface file.cd` | 打印已加载模块的公开接口元数据 |
| `--format file.cd` | 输出规范化源码 |
| `--format-check file.cd` | 检查源码是否已规范化；不输出改写后的源码 |
| `--emit-bytecode out.cdbc file.cd` | 生成可由 Rust VM 运行的链接 artifact |
| `--emit-module-bytecode out-dir file.cd` | 为 import 图生成独立模块 artifact |
| `--lsp` | 启动 stdio JSON-RPC 语言服务器 |

所有普通模式都接受一个或多个输入文件；多个文件按命令行顺序拼接为一个组合程序。`--emit-module-bytecode` 是例外：它要求输入能够建立 import-aware module graph，并为图中的模块生成独立产品。

### 1.5 格式化和编辑器支持

格式化器从生产 lexer/parser 的无损 token/trivia 视图生成稳定源码：默认使用两格缩进，保留字符串和行注释文本，最多保留一个空白行，并对过宽的逗号列表使用固定规则换行。它不会把字符串或注释拆开，也不会自动插入或删除尾逗号。

```sh
./build/compiler_design --format hello.cd
./build/compiler_design --format-check hello.cd
./build/compiler_design --format-indent-width 4 --format hello.cd
```

`--format-check` 在发现非规范源码时以非零状态退出；非法或不完整源码仍按普通 lexer/parser 诊断处理。`--format` 和 `--format-check` 不能与 AST、IR、bytecode、artifact 或模块缓存模式组合。

`--lsp` 启动 stdio JSON-RPC 服务，适合由编辑器或语言客户端管理。当前服务覆盖文档同步、诊断、格式化编辑、定义跳转、符号、引用、hover、单文档/已打开工作区重命名、声明补全和工作区符号。文件必须通过 LSP 的 open/change 消息进入虚拟工作区；未打开的 import 文件不会被当作完整工作区索引。

VS Code 客户端位于 [`vscode-extension/`](vscode-extension/)。构建客户端并安装本地 `.vsix`：

```sh
cd vscode-extension
npm install
npm run package
code --install-extension *.vsix
```

客户端默认寻找第一个工作区下的 `build/compiler_design`；可通过 `compilerDesign.serverPath` 指向其他路径。更完整的客户端说明见 [`vscode-extension/README.md`](vscode-extension/README.md)。

## 2. 源文件和基本语法

程序由声明和语句组成。语句使用分号结束，代码块使用大括号；注释使用 `//`，字符串使用双引号。当前没有自动分号插入，也没有块注释。

词法规则的实用摘要：标识符由 ASCII 字母、数字和下划线组成，但不能以数字开头；数字字面量支持整数和带小数部分的十进制数；负数是对数字使用一元 `-` 运算符。字符串在一对双引号之间，当前不提供转义序列，反斜线会作为普通字符保留。关键字包括 `let`、`fun`、`struct`、`enum`、`impl`、`match`、`if`、`else`、`while`、`for`、`break`、`continue`、`return`、`print`、`import`、`export`、`private`、`operator`、`true`、`false` 和 `nil`。

表达式位置的大括号表示 map 字面量，例如 `{ "name": "Ada" }`；语句位置的大括号表示 block。匿名 struct 字面量不是独立语法，命名结构体必须使用 `Name { field: value }` 构造。

```cd
// 变量声明、类型注解和条件语句
let name: string = "Ada";
let age = 36;

if (age >= 18) {
  print name;
} else {
  print "minor";
}
```

顶层语句会成为程序入口。变量必须先使用 `let` 声明；同一作用域不能重复声明同名变量，内层代码块可以遮蔽外层变量。代码块、函数体和循环体都会形成词法作用域。

## 3. 值和类型

语言提供以下主要类型：

| 类型 | 示例 | 说明 |
| --- | --- | --- |
| `nil` | `nil` | 空值 |
| `number` | `42`, `3.14` | 数值；数组索引和 range 参数需要整数值 |
| `bool` | `true`, `false` | 布尔值 |
| `string` | `"hello"` | 字符串；字符串操作按 Unicode scalar value 计算位置 |
| `[T]` | `[number]`, `[optional<string>]` | 数组；数组是可变的引用值 |
| `map<K, V>` | `map<string, number>` | 映射；键只能是 `nil`、`number`、`bool` 或 `string` |
| `range` | `range(0, 3)` | 不可变的有限整数范围 |
| `fun(...) : T` | `fun(number): string` | 函数值和闭包 |
| 命名结构体 | `Person`, `Box<number>` | 名义类型，字段形状由 `struct` 声明定义 |
| 枚举 | `Result`, `Result<number>` | 名义类型，由多个 variant 构成 |

### 类型注解和可空类型

`let`、函数参数、函数返回值和函数值都可以使用类型注解：

```cd
let score: number = 100;
let maybeScore: optional<number> = nil;
let values: [optional<number>] = [1, nil, 3];

fun parseScore(text: string): optional<number> {
  return nil;
}

let addOne: fun(number): number = fun (value: number): number {
  return value + 1;
};
```

`optional<T>` 表示 `T` 或 `nil`。非空值可以赋给 `optional<T>`，但 `optional<T>` 不能直接当作 `T` 使用。nil 检查不会收窄类型，`optional<T>` 必须显式解包：

```cd
fun printName(name: optional<string>) {
  if let n = name {
    // 这里 n 是 string
    print n;
  }
}
```

显式解包形式包括语句级 `if let` / `while let`（在作用域内绑定一个全新的非 nil 值）、表达式后缀 `?`（值为 nil 时从当前函数提前返回 nil，仅当函数返回 `optional<U>` 时可用）、`??`（为 nil 时取右侧值），以及 `match` 的绑定臂。nil 比较和 truthiness 条件都只是普通布尔测试，不改变类型；只有 `nil` 和 `false` 为假，`0` 和空字符串为真。

### 类型推断和泛型

未注解的 `let` 会尽量从初始化表达式推断类型：

```cd
let count = 3;            // number
let flags = [true, false]; // [bool]
```

函数、结构体和枚举可以声明类型参数，也可以使用具体约束或静态能力约束：

```cd
fun identity<T>(value: T): T {
  return value;
}

fun positive<T: number>(value: T): T {
  return value;
}

fun same<T: Eq>(left: T, right: T): bool {
  return left == right;
}

fun before<T: Ord>(left: T, right: T): bool {
  return left < right;
}

fun sameHash<T: Eq + Hash>(left: T, right: T): bool {
  return left == right && hash(left) == hash(right);
}

struct Box<T> {
  value: T
}

enum Result<T> {
  Ok(T),
  Err(string),
}

print identity<number>(42);
let box: Box<number> = Box { value: 42 };
let result: Result<number> = Result.Ok(42);
```

类型参数通常从实参、字段值或期望类型推断；也可以在调用或构造时显式提供。泛型参数按名义类型处理，结构体和枚举的泛型实参是不变的。`Eq`、`Ord` 和 `Hash` 只在编译期检查；多个 capability 可以使用 `+` 组合，例如 `T: Eq + Hash`。`Ord` 的内置满足关系包括 `number` 和 `string`，并且 `Ord` 同时满足 `Eq`；`Hash` 可通过 `hash(value)` 取得确定性的 32 位 FNV-1a 数值。公共库提供要求 `Eq + Hash` 的 `HashSet` 和 `HashMap`，数组、map、函数及命名结构体键使用稳定 identity，别名修改不会使键失效；内置 map 仍只允许基础键。当前仍没有用户自定义 capability 实现或深度冻结 key 快照。

## 4. 运算符和赋值

从高到低的主要优先级如下：

1. 调用、泛型调用、索引和成员访问：`f(x)`、`f<number>(x)`、`a[i]`、`x.field`
2. 一元运算：`!x`、`-x`
3. 乘除：`*`、`/`
4. 加减：`+`、`-`
5. 比较：`<`、`<=`、`>`、`>=`
6. 相等：`==`、`!=`
7. 逻辑与：`&&`
8. 逻辑或：`||`
9. 赋值：`=`、`+=`、`-=`、`*=`、`/=`

`&&` 和 `||` 会短路，并返回被选中的操作数，而不是强制返回 `bool`。只有 `nil` 和 `false` 为假，`0` 和空字符串仍为真。

普通赋值是右结合的，并且表达式结果就是被赋的值：

```cd
let a = 0;
let b = 0;
a = b = 7;
print a;
print b;
```

普通赋值支持变量、数组元素、map 元素和结构体字段。数值复合赋值支持变量、数组元素和结构体字段；map 元素不支持复合赋值。

相等比较按运行时值的种类处理：`nil`、数字、布尔值和字符串按值比较；数组、map、结构体和函数按引用/身份比较；range 按 `(start, stop, step)` 比较；枚举按枚举名、variant 名和 payload 递归比较。比较不同种类的值结果为不相等。数值和字符串支持内置顺序比较；自定义结构体只有在其定义模块的 `impl` 中声明对应 operator 后才能使用顺序比较。

命名结构体可以声明 `<`、`<=`、`>`、`>=` operator。每个 operator 必须接受一个与接收者相同名义类型的参数，并返回 `bool`：

```cd
struct Person {
  age: number
}

impl Person {
  operator <(other: Person): bool {
    return this.age < other.age;
  }
}

let younger = Person { age: 18 } < Person { age: 36 };
print younger;
```

operator 是静态解析的普通调用路径，不提供动态派发，也不会使自定义结构体自动满足泛型 `T: Ord`。导出的结构体会把 operator 元数据带入公开接口、sidecar 和模块产品，导入方可以继续使用已导出的实现。

## 5. 条件和循环

### `if`

```cd
let condition = true;
if (condition) {
  print "yes";
} else {
  print "no";
}
```

条件表达式使用与 `!`、`&&`、`||` 相同的 truthiness 规则。

### `while`

```cd
let n = 0;
while (n < 3) {
  print n;
  n += 1;
}
```

### C 风格 `for`

初始化、条件和递增子句都可以省略；初始化中的 `let` 只在循环条件、循环体和递增子句中可见：

```cd
for let i = 0; i < 3; i += 1 {
  print i;
}
```

`continue` 在 C 风格循环中会先执行递增子句，再进行下一次条件检查。

### `for-in`

数组按索引顺序遍历，range 按范围值遍历，map 按插入顺序遍历键：

```cd
for item in [1, 2, 3] {
  print item;
}

for value in range(1, 4) {
  print value;
}

let prices = { "tea": 8, "coffee": 12 };
for key in prices {
  print key;
}
```

数组迭代会在开始时记录长度，map 迭代会在开始时记录键列表。`break` 和 `continue` 作用于最近的循环；它们在循环外或嵌套函数中不能跳出外层循环。

## 6. 函数和闭包

命名函数的参数和返回值可以加类型注解：

```cd
fun add(left: number, right: number): number {
  return left + right;
}

print add(2, 3);
```

匿名函数是表达式，可以保存到变量或作为回调传入：

```cd
let double = fun (value: number): number {
  return value * 2;
};

print double(4);
```

命名函数、方法和匿名函数表达式都可以声明泛型参数。类型参数通常从调用实参推断，也可以显式提供；带约束的参数必须满足每个约束：

```cd
fun identity<T>(value: T): T {
  return value;
}

let text = identity<string>("hello");
let keepNumber = fun<T: number>(value: T): T {
  return value;
};
print keepNumber<number>(41);
```

函数类型写作 `fun(parameterType, ...): returnType`，可以用于 `let`、函数参数和函数返回值。未注解的函数参数、返回值和回调在缺少上下文时不能完全推断，遇到兼容性诊断时应补充注解。泛型函数值不会自动转换成单态函数类型；传给 `map`、`filter` 等回调 helper 时，所有泛型参数都必须能从已知输入和签名推断。

函数体执行 `return expression;` 返回指定值；`return;` 或执行到函数末尾时返回 `nil`。函数是值，可以作为参数、返回值或数组元素。

嵌套函数和匿名函数是闭包，会通过共享运行时单元捕获外层变量：

```cd
fun counter(): fun(): number {
  let current = 0;
  return fun (): number {
    current += 1;
    return current;
  };
}

let next = counter();
print next();
print next();
```

结构体方法在 `impl` 块中定义，接收者通过 `this` 访问：

```cd
struct Person {
  name: string
}

impl Person {
  fun greeting(): string {
    return "Hello, " + this.name;
  }
}

let person = Person { name: "Ada" };
print person.greeting();
```

结构体字段也可以声明为 `private`。私有字段在定义它的源模块内可读写和初始化，
但不会出现在导入模块的公开字段形状中；外部代码不能直接读取、赋值、用于 record
pattern 或直接构造带有私有表示的结构体。库通常通过模块内 factory 函数返回这类值，
再通过公开方法提供操作。

方法可以使用 `impl Box<T>` 这样的泛型接收者。对已知结构体接收者，用户定义的
同名方法会优先于 builtin member-call sugar；数组、map、字符串和 range 接收者
仍使用对应的 builtin fallback。当前不支持继承、重载、动态派发或静态方法。

## 7. 数组、map 和 range

### 数组

数组使用方括号创建，元素可以通过整数索引读写：

```cd
let numbers = [10, 20, 30];
numbers[1] = 25;
print numbers[1];
push(numbers, 40);
print pop(numbers);
```

数组是引用值，别名会观察到同一数组的修改。索引必须是有限整数且不能越界；`pop([])` 是运行时错误。`[]` 的元素类型未知时，后续直接 `push` 或索引赋值可以在部分场景中完善类型；需要稳定接口时建议显式写 `[T]`。

### map

map 使用 `{ key: value }` 创建：

```cd
let scores: map<string, number> = {
  "Ada": 10,
  "Grace": 12
};

scores["Ada"] = 11;
print scores["Ada"];
```

map 的键只能是 `nil`、`number`、`bool` 或 `string`。读取不存在的键会产生运行时错误；赋值会更新已有键或按插入顺序新增键。map 是引用值，`==` 比较的是身份而不是内容。

### range

`range` 创建半开区间：

```cd
let ascending = range(1, 5);          // 1, 2, 3, 4
let descending = range(5, 0, -1);     // 5, 4, 3, 2, 1
```

形式可以是 `range(stop)`、`range(start, stop)` 或 `range(start, stop, step)`。步长不能为零，参数必须是有限整数。range 支持索引、`len`、`contains` 和 `for-in`，本身不可变。

## 8. 结构体、枚举和模式匹配

### 命名结构体

结构体构造器使用类型名和命名字段；字段可以按任意顺序提供，但必须匹配声明：

```cd
struct User {
  name: string,
  age: number
}

let user = User { age: 36, name: "Ada" };
user.age = 37;
print user.name;
```

结构体是引用值，字段修改会对所有别名可见。匿名的 `{ name: "Ada" }` 在表达式位置是 map，不是匿名结构体；必须写成 `User { name: "Ada" }`。

### 枚举

枚举 variant 的构造参数按位置传入：

```cd
enum Result {
  Ok(number),
  Err(string),
  Empty,
}

let result = Result.Ok(42);
```

也可以声明带名字的 payload，名字主要用于模式匹配：

```cd
enum NamedResult {
  Ok(value: number),
  Err(message: string),
}

let result = NamedResult.Ok(42);
```

### `match`

match 语句的每个 arm 都是代码块，并且必须穷尽覆盖 scrutinee：

```cd
enum NamedResult {
  Ok(value: number),
  Err(message: string),
}

let result = NamedResult.Ok(42);
match result {
  NamedResult.Ok(value) => {
    print value;
  }
  NamedResult.Err(message) => {
    print message;
  }
}
```

支持 `_`、绑定模式、`nil`、布尔/数字/字符串字面量、variant 模式、嵌套模式、命名 payload、命名结构体 record 模式和 OR 模式：

```cd
struct Point {
  x: number,
  y: number
}

let point = Point { x: 1, y: 2 };
match point {
  Point { x: 1 } => { print "on x=1"; }
  Point { x: x, y: y } => { print x + y; }
}
```

record 模式可以省略字段或调整顺序，例如 `Point { y: 2 }`。OR 模式的所有分支必须绑定相同的名字并具有兼容类型。guard 写在模式和 `=>` 之间，但 guard 不参与穷尽性覆盖：

```cd
enum NamedResult {
  Ok(value: number),
  Err(message: string),
}

let result = NamedResult.Ok(42);
match result {
  NamedResult.Ok(value) if value > 0 => { print value; }
  NamedResult.Ok(_) => { print 0; }
  NamedResult.Err(message) => { print message; }
}
```

match 表达式返回所选 arm 的表达式值；arm 使用逗号分隔，可以有尾逗号：

```cd
enum NamedResult {
  Ok(value: number),
  Err(message: string),
}

let result = NamedResult.Ok(42);
let label = match result {
  NamedResult.Ok(value) => "ok:" + str(value),
  NamedResult.Err(message) => "err:" + message,
};
```

布尔值在覆盖 `true` 和 `false` 后穷尽；数字和字符串的取值域开放，通常需要 `_` 或绑定模式。可空枚举或结构体还必须覆盖 `nil`。

## 9. 模块和导入

模块导入必须位于顶层。导入路径相对于当前文件解析，也可以使用 `-I`/`--import-path` 增加搜索目录。

`lib.cd`：

```cd
let hidden = 40;

fun answer(): number {
  return hidden + 2;
}

export answer;
```

`main.cd`：

```cd
import "./lib.cd";

print answer();
```

未导出的顶层声明只在模块内部可见。使用别名导入时，通过限定名访问：

```cd
import "./lib.cd" as lib;
print lib.answer();
```

导出多个名称可以写成 `export answer, User;`。转导出可以写成 `export answer from "./lib.cd";`，但转导出的名字不会自动成为当前模块的本地名字；如果转导出模块也需要使用该名称，应另外写 `import`。相同规范路径的重复导入会被去重；不能从 stdin 导入。

显式路径（以 `./`、`../` 或绝对路径开头）只相对导入文件所在目录解析，并尝试原路径和补上 `.cd` 的路径。非显式路径先在导入文件所在目录尝试，再按命令行中 `-I`/`--import-path` 的顺序尝试各搜索目录；每个目录同样尝试原路径和 `.cd`。搜索路径不会覆盖显式相对路径。当前不支持包清单、import map、通配符导出或导出重命名。

模块顶层默认是私有的。`private field: type` 只在定义模块内可读写和用于初始化，导出的接口只暴露公开字段；外部模块不能读取、赋值、复合赋值、record pattern 匹配或直接构造含私有字段的结构体。推荐用模块内 factory 函数创建值，再通过公开方法提供不变量安全的 API。

命令行直接传入多个 `.cd` 文件时，它们会按参数顺序作为一个组合程序编译；这与通过 `import` 构建模块图是两种不同的输入方式。

### 模块字节码、接口和缓存

普通的 `--emit-bytecode` 生成一个包含完整调用体的链接 artifact，适合单次构建和运行。需要独立模块产物或增量缓存时，使用 `--emit-module-bytecode`：

```sh
./build/compiler_design --emit-module-bytecode module-products main.cd
cargo run --manifest-path vm-rs/Cargo.toml -- link module-products program.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- run program.cdbc
```

输出目录中的每个 `module-*.cdbc` 都是 `artifact: module` 产品，不能直接交给 VM 的 `run`；必须先由 `compiler-design-vm link` 合并成链接 program。模块编译会保留依赖插入点、公开接口和源码调试元数据。

`--module-interface` 只打印类型检查得到的公开接口（导出值、结构体公开字段、私有字段存在标记、方法/operator、枚举 variant 等），它是 introspection 模式，不会写 `.cdi` 或 `.cdbc`。

模块缓存是显式 opt-in：

```sh
./build/compiler_design \
  --emit-module-bytecode module-products \
  --module-cache module-cache \
  --module-rebuild-report rebuild.json \
  main.cd
```

缓存目录包含经过校验的模块产品、`cdi 0.1` 公开接口 sidecar 和 `cdbc-cache 0.2` manifest。编译器会同时校验源 hash、公开接口 hash、依赖接口 hash、配对产品、manifest 和产品内容摘要；正常的源码/接口/依赖漂移会重建对应模块，损坏的 manifest 默认报错并要求显式修复，被篡改的产品会按摘要校验后重建而不是复制。`rebuild.json` 记录每个模块的复用或重建原因，可用于 CI 和构建诊断。

`--module-interface-cache cache-dir` 只预加载公开接口，适用于普通源模式和 `--module-interface` 等不需要依赖 bytecode body 的消费者。接口-only 消费者默认 strict：缺失或无效 sidecar 会产生 `Import` 错误；加 `--module-cache-fallback` 才允许回退到依赖源文件。`--module-cache-strict` 和 `--module-cache-fallback` 互斥。

接口-only cache 不能为默认的单文件 `--emit-bytecode` 提供依赖函数体；要使用缓存生成可运行产物，应采用 `--emit-module-bytecode --module-cache`，然后执行 Rust VM 的 `link`。模块产物模式同样默认 strict：冷构建（尚无 manifest）会自举并写入新 manifest；manifest 损坏或不一致会报错并要求显式修复（删除缓存目录或改用 `--module-cache-fallback` 重建）；源码/接口/依赖变化等正常漂移会自动从源重建。离线构建仍需依赖源文件存在，入口模块始终从源码编译。

## 10. 内置函数

函数形式的内置函数可以被同名的词法绑定遮蔽；表中列出的成员形式是编译器提供的内置 sugar，
不会被同名普通绑定遮蔽。若已知结构体接收者声明了同名方法，则优先调用该用户方法。

| 函数 | 作用 |
| --- | --- |
| `len(value)` | 返回数组、map、range 或字符串长度 |
| `push(array, value)` / `pop(array)` | 原地追加或移除末尾元素；`push` 返回 `nil` |
| `contains(collection, value)` | 查询数组元素、map 键或 range 值 |
| `slice(array, start, length)` | 创建浅拷贝的数组切片 |
| `copy(array)` | 创建数组的一层浅拷贝 |
| `concat(left, right)` | 创建拼接后的新数组 |
| `map(array, callback)` | 对快照中的每个元素映射，返回新数组 |
| `filter(array, predicate)` | 保留谓词为真的元素，返回新数组 |
| `flatMap(array, callback)` | 映射后展开一层数组 |
| `any` / `all` / `count` | 对数组执行布尔谓词查询或计数 |
| `find` / `findIndex` | 查找第一个匹配元素或索引；找不到分别返回 `nil` 和 `-1` |
| `reduce(array, initial, callback)` | 按 `(accumulator, element)` 顺序归约 |
| `remove(map, key)` / `clear(map)` | 原地删除键或清空 map |
| `merge(left, right)` | 创建浅拷贝 map；右侧值覆盖同键值 |
| `keys(map)` / `values(map)` | 按插入顺序返回新数组 |
| `range(...)` | 创建半开整数范围 |
| `floor` / `ceil` / `sqrt` | 数值运算；负数 `sqrt` 会产生运行时错误 |
| `str(value)` | 将值转换为与 `print` 相同的文本表示 |
| `substr(string, start, length)` / `charAt(string, index)` | 按 Unicode scalar value 进行字符串切片和字符访问 |
| `typeOf(value)` | 返回运行时类型名，例如 `number`、`array`、`Person` |
| `hash(value)` | 返回满足 `Hash` capability 的值的确定性 32 位哈希数 |

函数形式的内置函数可以被同名词法绑定遮蔽；例如声明 `let map = ...` 后，当前作用域的 `map(...)` 不再指向 native helper。成员形式是编译器提供的 unshadowed sugar，不受同名普通绑定影响；已知命名结构体接收者上的用户方法优先于 builtin fallback。

数组 helper 的语义如下：`push` 原地追加并返回 `nil`，`pop` 原地移除末项并返回它，空数组 `pop` 是运行时错误；`slice`、`copy`、`concat` 返回新的顶层数组但只做 shallow copy。`map`、`filter`、`flatMap`、`any`、`all`、`count`、`find`、`findIndex` 和 `reduce` 都先对源数组取快照，再从左到右调用回调。`flatMap` 只展开一层；`any`、`all`、`find` 和 `findIndex` 在结果确定后短路；`reduce` 必须显式提供 initial，空数组直接返回 initial。

map 是有插入顺序的共享引用值。`map[key] = value` 会插入或更新，更新不会移动既有 key；缺失 key 的读取和 `remove` 会产生 `map key not found`。`clear` 原地清空并返回 `nil`；`keys`、`values` 返回按插入顺序排列的新数组；`merge(left, right)` 返回新 map，左侧顺序不变，右侧重复 key 只替换值，新 key 追加，输入 map 不被修改。内置 map 的 key 仍只允许 `nil`、`number`、`bool` 和 `string`；泛型哈希容器使用库级 `Eq + Hash` 契约。

`range(stop)`、`range(start, stop)` 和 `range(start, stop, step)` 生成不可变半开整数范围；step 不能为零，边界和 step 必须是有限整数。range 支持索引、`len`、`contains`、`for-in` 和按 `(start, stop, step)` 的结构比较。

`substr` 和 `charAt` 使用 Unicode scalar value 偏移，而不是 UTF-8 字节偏移；组合字符仍按多个 scalar value 计算，不提供 grapheme 分割或 Unicode normalization。`str` 与 `print` 使用相同的文本表示。`typeOf` 返回 `nil`、`number`、`bool`、`string`、`function`、`array`、`map`、`range`、枚举名或命名结构体名。`hash` 是确定性的 32 位 FNV-1a 结果，受 `Hash` 编译期约束保护；公共库提供 `HashSet<T: Eq + Hash>` 和 `HashMap<K: Eq + Hash, V>`，但不会放宽内置 map key 类型。

常用成员形式包括：

```cd
let numbers = [1, 2, 3];
let scores = { "Ada": 10 };
numbers.push(4);
let doubled = numbers.map(fun (x: number): number { return x * 2; });
print numbers.len();
print scores.contains("Ada");
print scores.keys();
print "你好".charAt(1);
```

可用的成员形式包括 `array.push`、`array.pop`、`array.len`、`array.contains`、`array.slice`、`array.copy`、`array.concat`、`array.map`、`array.filter`、`array.flatMap`、`array.any`、`array.all`、`array.count`、`array.find`、`array.findIndex`、`array.reduce`，map 的 `len`、`contains`、`remove`、`clear`、`merge`、`keys`、`values`，字符串的 `len`、`substr`、`charAt`，以及 range 的 `contains`。这些成员名不受词法绑定遮蔽；命名结构体方法仍按静态接收者优先规则解析。

例如，结构体可以声明 `push` 或 `pop` 方法；只有数组等 builtin receiver
才会使用对应的内置 member-call sugar。

回调型函数按从左到右的顺序处理输入快照；`map`、`filter`、`flatMap`、`slice`、`copy`、`concat` 和 `keys`/`values` 返回新的一层容器，内部结构体、数组和闭包仍可能与原值共享。`any`、`all`、`find` 和 `findIndex` 会在可以确定结果时短路。

## 11. 错误和诊断

错误种类包括 `Lex`、`Parse`、`Type`、`Compile`、`Import` 和 `Runtime`。前端错误通常包含行列号、源代码行和 caret：

```text
Type error at 1:7: undefined variable `missing`
  print missing;
        ^
```

导入文件和直接多文件输入的诊断会包含原始文件路径和文件内位置。常见错误包括：

- 使用未声明变量或在同一作用域重复声明；
- 把可空值直接当作非空值使用；
- 数组、range 索引不是有限整数或超出范围；
- 读取不存在的 map 键或删除不存在的 map 键；
- map 键不是 `nil`、`number`、`bool`、`string`；
- `pop` 空数组、`range` 使用零步长、`sqrt` 传入负数；
- match 没有穷尽覆盖，或不同 arm 的返回类型不兼容。

字节码运行时错误会附带 artifact 中保存的源码路径、行列位置和 caret；通过函数调用产生的运行时错误还会显示从内层到外层的调用栈。`trace` 的事件写到 stdout，`debug` 在执行中的暂停记录也写到 stdout，运行时失败诊断仍写到 stderr；错误程序不应依赖 stdout 中存在完整业务输出。

普通编译器在成功时退出 `0`，语法、类型、导入和编译失败退出 `1`，命令行参数组合错误退出 `64`。Rust VM 的 `dump`、`run`、`trace`、`debug`、`profile` 和 `link` 在 artifact 或执行失败时退出非零；参数数量错误同样使用 `64`。发布脚本应同时检查退出状态和 stderr，而不要只检查是否生成了输出文件。

调试 artifact 的常用路径如下：

```sh
./build/compiler_design --emit-bytecode program.cdbc main.cd
cargo run --manifest-path vm-rs/Cargo.toml -- dump program.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- trace program.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- debug program.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- profile program.cdbc
```

手写或缺少调试元数据的旧 `.cdbc` 只会得到兼容的一行运行时错误；由当前编译器生成的 artifact 会携带源码、函数、调用位置和可选的源码字节范围信息。

## 12. 数据结构库示例

仓库中的 [`library/data_structures.cd`](library/data_structures.cd) 是一个
完全使用公开语言能力编写的示例库，当前提供泛型 `Stack<T>` 和
`Queue<T>`。示例程序见
[`examples/data_structures.cd`](examples/data_structures.cd)：

```cd
import "../library/data_structures.cd" as ds;

let stack = ds.newStack<number>();
stack.push(10);
stack.push(20);
print stack.top();
print stack.pop();

let queue = ds.newQueue<string>();
queue.enqueue("first");
print queue.dequeue();
```

栈提供 `push`、`pop`、`top`、`size`、`isEmpty` 和 `snapshot`；队列提供
`enqueue`、`dequeue`、`front`、`size`、`isEmpty` 和 `snapshot`。当前库使用
传统的 `push`/`pop` API；已知结构体接收者上的用户方法会优先于同名的
builtin member-call sugar，数组接收者仍使用数组 builtin。栈和队列的内部数组及
队列游标是模块私有字段，只能通过 `newStack` 和 `newQueue` 初始化。

## 13. 当前边界

当前实现仍是实验性语言，以下能力尚未提供或仍较保守：

- 没有包管理、包清单、import map、导出重命名和通配符导出；
- 没有字符串或自定义迭代器的 `for-in`；
- 没有 `Person(...)` 形式的结构体构造函数；
- 递归枚举 payload 可以使用，递归命名结构体字段（例如 `struct Node { next: optional<Node> }`）仍被拒绝；
- 没有继承、重载、动态派发、静态方法和函数值字段调用；
- `Eq`/`Ord` 目前只能作为编译期泛型约束使用，尚无用户自定义 capability
  实现；`Hash` 只提供编译期约束和 `hash(value)` 入口，公共库的哈希容器使用
  稳定 identity key，但不会放宽内置 map key 或提供深度冻结快照；
- 没有自动 nullable 收窄，`optional<T>` 必须显式解包（`if let`/`while let`/`?`/`??` 或 `match` 绑定臂）；
- 当函数签名或集合元素类型无法可靠推断时，需要补充显式类型注解。

交互式 REPL、动态派发、包管理和自定义 iterator 不属于当前 `master` 发布面；`trace`、`debug` 和 `profile` 是可用的源码级确定性执行/调试/观测工具。profile 的 wall-clock 与 allocation/peak 扩展仍需独立的 VM 决策和 workload 证据。

### 发布维护者检查

建立发布分支或发布标签前，至少验证编译器、Rust VM 和代表性用户路径：

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
python3 tests/run_verification.py ./build/compiler_design vm-rs --report build/verification-report.json
python3 tests/run_boundary_tests.py ./build/compiler_design
python3 tests/run_malformed_tests.py ./build/compiler_design vm-rs --report build/malformed-report.json
cargo test --manifest-path vm-rs/Cargo.toml
git diff --check
```

发布版本以根目录 `VERSION` 为准；`0.1.0` 对应标签 `v0.1.0`。版本号、手册和用户可见 CLI/语言行为应在同一发布变更中保持一致。完整的版本和分支约定见 [`docs/versioning.md`](docs/versioning.md)。

实现级语法的完整参考见 [`docs/language-grammar.ebnf`](docs/language-grammar.ebnf)；命令行和字节码开发信息见 [`README.md`](README.md) 及 [`docs/bytecode-text-format.md`](docs/bytecode-text-format.md)。
