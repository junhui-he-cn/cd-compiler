# Compiler Design 语言用户手册

> 文档状态：初稿。本文档描述当前仓库中已经实现的语言和工具链行为，示例源文件使用 `.cd` 扩展名。

Compiler Design 是一门用于实验编译器前端、类型检查、IR 和字节码执行的小型语言。源程序可以直接交给 C++ 编译器检查并查看 AST/IR，也可以生成 `.cdbc` 字节码，再由 Rust VM 执行。

## 1. 快速开始

### 构建

在仓库根目录执行：

```sh
cmake -S . -B build
cmake --build build
```

### 第一个程序

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

生成并运行字节码：

```sh
./build/compiler_design --emit-bytecode hello.cdbc hello.cd
cargo run --manifest-path vm-rs/Cargo.toml -- run hello.cdbc
```

如果只想使用 C++ 编译器检查语法和类型，不需要生成字节码；如果要执行程序，则使用 Rust VM 的 `run` 命令。

没有输入文件时，普通模式从标准输入读取源代码：

```sh
printf 'print 1 + 2;\n' | ./build/compiler_design
```

字节码和模块产物模式必须提供至少一个输入文件。

## 2. 源文件和基本语法

程序由声明和语句组成。语句使用分号结束，代码块使用大括号；注释使用 `//`，字符串使用双引号。

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
| `[T]` | `[number]`, `[string?]` | 数组；数组是可变的引用值 |
| `map<K, V>` | `map<string, number>` | 映射；键只能是 `nil`、`number`、`bool` 或 `string` |
| `range` | `range(0, 3)` | 不可变的有限整数范围 |
| `fun(...) : T` | `fun(number): string` | 函数值和闭包 |
| 命名结构体 | `Person`, `Box<number>` | 名义类型，字段形状由 `struct` 声明定义 |
| 枚举 | `Result`, `Result<number>` | 名义类型，由多个 variant 构成 |

### 类型注解和可空类型

`let`、函数参数、函数返回值和函数值都可以使用类型注解：

```cd
let score: number = 100;
let maybeScore: number? = nil;
let values: [number?] = [1, nil, 3];

fun parseScore(text: string): number? {
  return nil;
}

let addOne: fun(number): number = fun (value: number): number {
  return value + 1;
};
```

`T?` 表示 `T` 或 `nil`。非空值可以赋给 `T?`，但 `T?` 不能直接当作 `T` 使用。类型检查器支持对简单变量、已知结构体直接字段，以及编译期可确定的数组索引进行 nil 检查收窄：

```cd
fun printName(name: string?) {
  if (name == nil) {
    return;
  }
  // 这里 name 已经被收窄为 string
  print name;
}
```

调用结果、未知索引、map/range 元素和复杂循环出口的收窄仍保持保守行为；发生赋值、字段写入、索引写入或可能改变捕获变量的调用后，应重新检查 nil。

### 类型推断和泛型

未注解的 `let` 会尽量从初始化表达式推断类型：

```cd
let count = 3;            // number
let flags = [true, false]; // [bool]
```

函数、结构体和枚举可以声明类型参数，也可以使用具体约束：

```cd
fun identity<T>(value: T): T {
  return value;
}

fun positive<T: number>(value: T): T {
  return value;
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

类型参数通常从实参、字段值或期望类型推断；也可以在调用或构造时显式提供。泛型参数按名义类型处理，结构体和枚举的泛型实参是不变的。

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

## 5. 条件和循环

### `if`

```cd
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
match result {
  NamedResult.Ok(value) if value > 0 => { print value; }
  NamedResult.Ok(_) => { print 0; }
  NamedResult.Err(message) => { print message; }
}
```

match 表达式返回所选 arm 的表达式值；arm 使用逗号分隔，可以有尾逗号：

```cd
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

导出多个名称可以写成 `export answer, User;`。转导出可以写成 `export answer from "./lib.cd";`，但转导出的名字不会自动成为当前模块的本地名字。相同规范路径的重复导入会被去重；不能从 stdin 导入。当前不支持包清单、import map、通配符导出、导出重命名或语言级 separate compilation。

命令行直接传入多个 `.cd` 文件时，它们会按参数顺序作为一个组合程序编译；这与通过 `import` 构建模块图是两种不同的输入方式。

### 模块字节码

需要独立模块产物时使用：

```sh
./build/compiler_design --emit-module-bytecode module-products main.cd
cargo run --manifest-path vm-rs/Cargo.toml -- link module-products program.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- run program.cdbc
```

`--module-interface` 可查看类型检查后的公开接口。`--module-cache`、`--module-interface-cache` 和 `--module-cache-strict` 用于显式启用或约束模块缓存；普通用户通常先使用源文件导入和 `--emit-bytecode` 即可。

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

常用成员形式包括：

```cd
numbers.push(4);
let doubled = numbers.map(fun (x: number): number { return x * 2; });
print numbers.len();
print scores.contains("Ada");
print scores.keys();
print "你好".charAt(1);
```

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

字节码运行时错误会附带源位置；通过函数调用产生的运行时错误还会显示调用栈。

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
builtin member-call sugar，数组接收者仍使用数组 builtin。

## 13. 当前边界

当前实现仍是实验性语言，以下能力尚未提供或仍较保守：

- 没有包管理、包清单、import map、导出重命名和通配符导出；
- 没有字符串或自定义迭代器的 `for-in`；
- 没有 `Person(...)` 形式的结构体构造函数；
- 没有继承、重载、动态派发、静态方法和函数值字段调用；
- 复杂动态字段、未知索引、map/range 元素和部分循环出口不提供精确 nullable narrowing；
- 当函数签名或集合元素类型无法可靠推断时，需要补充显式类型注解。

实现级语法的完整参考见 [`docs/language-grammar.ebnf`](docs/language-grammar.ebnf)；命令行和字节码开发信息见 [`README.md`](README.md) 及 [`docs/bytecode-text-format.md`](docs/bytecode-text-format.md)。
