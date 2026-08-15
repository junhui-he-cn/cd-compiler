# Compiler Design 字节码（.cdbc 0.2）指令参考

本文档描述 Compiler Design 编译器产出的字节码文本工件 `.cdbc` 的 0.2 版定义，
并逐一说明全部指令的语法、语义、操作数约束和运行时错误。文档面向需要阅读、
编写或调试工件的开发者和实现第三方解析器/执行器的人。

权威英文契约是 [`docs/bytecode-text-format.md`](bytecode-text-format.md)；
C++ 发射端在 `src/BytecodeTextEmitter.cpp`，Rust 解析与执行端在
`vm-rs/src/format.rs` 与 `vm-rs/src/vm.rs`。若本文档与实现冲突，以实现和英文契约为准。

## 1. 定位与工件种类

`.cdbc`（Compiler Design ByteCode）是编译器与 Rust VM 之间稳定、版本化、可验证的
文本工件契约。它与 `--bytecode` 的人类可读调试打印不是同一种东西：调试打印供人检查，
`.cdbc` 是机器间契约，必须可被 Rust VM 严格解析、验证并执行。

每个文件以版本头开头：

```text
cdbc 0.2
```

VM 同时接受 `cdbc 0.1` 旧工件作为构造期兼容输入：旧工件使用名字驱动的
`load_var/store_var/assign_var`、`native_call` 与 `print`，由 VM 在构造期映射到
旧的按名解析路径；0.2 工件使用数值 slot（`lN/uN/gN`）与 native 导入索引
（`iN`），VM 热路径不再按名字解析或按名字查找 native。

工件有两种严格种类：

- **链接程序（linked program）**：没有 `artifact` 声明，是 `--emit-bytecode` 的默认
  产物，可以直接交给 VM 的 `run` 执行。
- **独立模块产品（module product）**：以 `artifact: module` 开头并带 `module:` 元数据
  封套，由 `--emit-module-bytecode <目录>` 为模块图的每个节点生成一个
  `module-<图节点ID>.cdbc`。模块产品可被 `dump`，但不是最终可执行程序，必须经
  `link` 确定性地展开依赖、重定位引用后成为链接程序才能 `run`。

模块封套形如：

```text
artifact: module

module:
  identity = "/workspace/lib.cd"
  path = "lib.cd"
  canonical_path = "/workspace/lib.cd"
  entry = false
  dependencies:
    d0 target="/workspace/shared.cd" kind=import at=2 requested="./shared.cd"
```

- `identity` 是图的规范化路径，也是产品集合的键。
- `path` 是源文件显示路径；入口模块额外带零基的 `entry_order`。
- `dependencies` 按源中出现顺序排列；`kind` 为 `import` 或 `re_export`；`at` 是本地
  `main` 指令流中在该偏移**之前**展开该依赖的位置，可以等于本地指令总数。

## 2. 文件结构

一个 `.cdbc` 文件由显式区段组成，顺序固定：

```text
cdbc 0.2

constants:
  c0 = number 1
  c1 = string "hello"

names:
  n0 = "x"

globals:
  g0 = n0

native_imports:
  i0 = "print" abi=1

main registers=4:
block b0:
  r0 = constant c0
  init_global g0, r0
  r1 = load_global g0
  r3 = call_native i0 [r1]
  return_nil

function f0 name="add_one" arity=1 registers=4:
  param 0 = "value"
  r1 = constant c0
  r2 = add r0, r1
  return r2

debug_sources:
  s0 path="examples/hello.cd" text="print 1;\n"

debug_locations:
  main 0 = s0:1:7

debug_ranges:
  main 0 = s0:0:7
```

### 2.1 引用前缀

所有引用使用稳定前缀，索引为零基十进制整数：

| 前缀 | 含义 |
| --- | --- |
| `cN` | 常量池索引 |
| `nN` | 名字表索引 |
| `rN` | 当前函数体的寄存器索引 |
| `fN` | 函数表索引 |
| `iN` | native 导入表索引 |

### 2.2 常量编码

常量带显式类型标签：`nil`、`number <浮点文本>`、`bool true|false`、
`string "..."`。字符串使用双引号，至少支持 `\\`、`\"`、`\n`、`\r`、`\t` 转义。
常量池只允许标量（nil / number / bool / string）；数组、map、结构体、变体、函数
等聚合值都在运行时由构造指令创建，不能进常量池。

`number` 是 IEEE 754 双精度浮点数。验证期拒绝无法解析或非有限的数字文本。
实现备注：C++ 发射端目前以 15 位有效数字输出 `number` 文本（
`BytecodeTextEmitter::numberText`），低于双精度精确往返所需的 17 位；极端值存在
静默漂移风险。

### 2.3 名字表

`names` 区段是字符串表，用于变量名、结构体字段名、结构体类型名、枚举与变体名、
断言消息等。注意名字表**不去重**：多个索引可以保存相同文本（例如
`n1`、`n4`、`n5` 都等于 `"add#2"`），它们是不同的名字索引，运行时语义依赖这种
索引级别名，第三方实现不得按字符串合并。

`native_imports` 区段（位于 `types:` 之后、`main` 之前）单独保存 native 导入：
`i0 = "print" abi=1`。名字必须来自固定的注册集合，`abi` 当前固定为 1；指令用
`call_native iN` 按索引调用，模块链接时按名字去重并重映射导入索引。

### 2.4 函数区段

`main registers=N:` 是顶层函数体，`function fN name="..." arity=A registers=R:` 是
普通函数。函数的 `param` 行（若存在）出现在指令之前。`registers` 是函数体可用的
寄存器个数（即最高合法寄存器索引加一）。

### 2.5 调试元数据

`debug_sources`、`debug_locations`、`debug_ranges` 是可选附加区段。源条目按零基
`sN` 索引，内嵌显示路径和原始源文本，导入感知的条目还可带稳定的
`module="<规范化路径>"`。`debug_locations` 把指令映射到 `sN:行:列`（行列为 1 基），
`debug_ranges` 映射到源内半开字节区间 `sN:start:end`。range 必须与同指令的 location
存在且源索引一致，区间必须落在 UTF-8 源文本内。无调试元数据的工件仍然合法，运行时
错误退化为无位置的旧式单行信息。

### 2.6 执行前验证

Rust 解析器接受 `cdbc 0.1` 与 `cdbc 0.2` 头。执行前验证：数字常量有限；
常量/名字/函数/寄存器/native 导入引用不越界；跳转目标合法；调试位置表形状正确；
native 名字在支持集合内、`abi=1`、导入名不重复、调用元数在范围内；模块封套的身份、
入口元数据、依赖目标与插入偏移合法。无效工件在执行前被拒绝，错误分类为
`parse` / `unsupported_version` / `verification`。

## 3. 执行模型与通用语义

VM 是寄存器机。每个函数体拥有一块预分配寄存器数组和一个指令指针（`ip`，零基）。
调用函数时创建新帧：实参写入该函数的局部槽，闭包环境指向创建闭包时捕获的外层环境；
顶层 `main` 的变量解析到全局环境。函数体内对名字的解析在加载/准备期完成：局部变量
映射到槽位下标，自由变量回退到闭包环境再回退到全局，执行热路径不做字符串哈希。

运行时值共十种：`nil`、`number`（f64）、`bool`、`string`（UTF-8）、`function`、
`array`、`map`、`range`、`struct`、`variant`。

通用约定：

- **真值**：只有 `nil` 和 `false` 为假；`0`、`""` 等一律为真。
- **相等**：`equal` / `not_equal` 使用运行时相等——number/bool/string 按值，
  function/array/map/struct 按引用身份，range 按 start/stop/step 分量，variant 按
  枚举名 + 变体名 + 各 payload 递归比较。
- **别名**：array/map/struct 是引用值；`move`、寄存器复制、索引读出都是浅复制，
  共享同一底层对象。
- **字符串**：长度与 `substr`/`charAt` 按 Unicode 标量（scalar value）计数和切片，
  不会切开一个标量的 UTF-8 编码；grapheme 与归一化不属于该格式版本。
- **确定性**：map 保插入序；数组/map/range 的 for-in 迭代使用长度或键列表快照；
  原生回调按左到右顺序执行。
- **资源与取消**：指令步数、调用深度、运行时元素数、输出字节量都有预算；超限产生
  `resource` 类运行时错误。协作调度器在指令/原生调用边界检查取消与 GC 安全点。
- **JIT**：默认禁用，是可选执行优化，不改 `.cdbc` 语义；解释器始终是默认与回退路径。

## 4. 指令详解

符号约定：`rD` 目标寄存器，`rS` / `rL` / `rR` 源寄存器，`cN` 常量索引，`nN` 名字
索引，`fN` 函数索引，`N` 十进制整数（跳转目标或 payload 下标）。所有索引零基。
0.2 正文由 `block bN:` 区段组成，每个 block 以 terminator 结束：
`br bN`（无条件）、`br_if rC, bT, bF`（条件）、`return rV`、`return_nil`；
0.2 不再有隐式 fallthrough。无目标寄存器的指令（`bind_local`、`set_local`、
`set_upvalue`、`init_global`、`set_global`、`return`、`br`、`br_if`、
`return_nil` 与三条旧跳转）没有 `rD`。旧 `cdbc 0.1` 的线性 `jump/jump_if_*` 仍在
兼容读取路径中保留；旧 `print rV` 同样只在兼容读取路径中保留，0.2 把它降为
`call_native` 到 `print` 导入。

执行前验证（0.2 块体）包括：block ID 顺序且分支目标合法、每个 block 以
terminator 结束、寄存器 definite-assignment（未定义读取直接拒绝）、local
definite-binding（参数默认已绑定，`load_local/set_local` 需要前置
`bind_local`）、`upvalue uN = global gM` 来源越界、native 调用 arity、以及
block 体内出现旧 `jump/jump_if_*` 的混用。

`types:` 区段（在 `names:`/`globals:` 之后、`main` 之前）给出运行时类型布局：
`tN = struct "Name" field0="f" field1="g"` 或
`tN = enum "Name" v0="A" payload=1 v1="B" payload=0`。字段/变体名仅用于 debug、
dump 与值显示；身份与访问全部使用数值 slot：
`make_struct tN` / `struct_get rO, tN, slot` / `struct_set rO, tN, slot, rV` /
`make_variant tN, vN` / `is_variant rV, tN, vN` / `variant_get rV, tN, vN, idx`。
verifier 校验 type/variant id、field slot 与 payload 下标范围。

### 4.1 常量与构造

#### constant

```text
rD = constant cN
```

把常量池第 `cN` 项复制到 `rD`。常量只可能是 nil / number / bool / string。聚合值
必须由后续构造指令创建。未验证工件中常量索引越界报
`constant index out of range`。

#### make_function

```text
rD = make_function fN
```

创建指向函数表第 `fN` 项的**函数值（闭包）**，按该函数的 `upvalue` 声明逐项捕获
父帧的局部 cell 或上级 upvalue cell，不执行函数体。函数值是普通运行时值：可存入寄存器、
变量、数组元素，可作为实参或返回值，并被 `call` 调用。函数索引越界报
`function index out of range`。

#### array

```text
rD = array [rA, rB, ...]
```

创建新数组，元素按括号内寄存器顺序排列，值为各寄存器当前值的浅复制（聚合元素与
源共享引用）。允许零元素。创建受运行时元素预算约束。

#### map

```text
rD = map [rK0: rV0, rK1: rV1, ...]
```

创建新的有序 map，键/值成对出现。键只允许 `nil` / `number` / `bool` / `string`，
否则运行时错误 `map key must be nil, number, bool, or string`。插入顺序即迭代顺序；
重复键允许并存，身份相等时后写键对应条目在后。map 是引用值，按身份相等。

#### struct

```text
rD = struct {nF: rV, ...}
rD = struct nT {nF: rV, ...}
```

创建结构体。字段名来自名字表（`nF`），字段值取自各寄存器。可选的 `nT` 记录该结构体
的运行时类型名（供 `typeOf` 与 Ord 见证使用）；省略时是匿名结构体，`typeOf` 返回
`"struct"`。字段顺序按书写顺序保留。字段名或寄存器越界、超出元素预算时失败。

#### variant

```text
rD = variant nEnum.nVariant [rP0, rP1, ...]
```

创建枚举变体值。`nEnum` 是枚举名，`nVariant` 是变体名，方括号内是位置化的 payload
寄存器列表（可空）。泛型枚举的类型参数是编译期元数据，在运行时指令中擦除。

#### variant_tag

```text
rD = variant_tag rV nEnum.nVariant
```

测试 `rV` 是否属于枚举 `nEnum` 的变体 `nVariant`，把布尔结果写入 `rD`。非变体值
返回 `false` 而不是报错；不读取 payload。

#### variant_field

```text
rD = variant_field rV N
```

读取 `rV` 的第 `N` 个位置 payload 写入 `rD`。对非变体值报
`can only access fields on enum variants`；payload 下标越界报
`enum variant field index out of bounds`。

### 4.2 数据移动与变量

#### move

```text
rD = move rS
```

把 `rS` 的值复制到 `rD`。对聚合值是引用复制（两者别名同一对象），不深拷贝。对应
IR 的 `Copy` 操作。

#### load_local / bind_local / set_local

```text
rD = load_local lN
bind_local lN, rV
set_local lN, rV
```

`lN` 是编译器在发射前分配的**数值局部槽**：参数占 `l0..l(arity-1)`，函数体内声明
的局部变量按首次声明顺序接续。`bind_local` 新建 cell（对应 `let` 声明），
`set_local` 更新已有 cell（对应赋值），`load_local` 读取。遮蔽会产生不同槽。
slot 未绑定时报 `unbound local lN`。

#### load_upvalue / set_upvalue

```text
rD = load_upvalue uN
set_upvalue uN, rV
```

`uN` 是当前函数的 upvalue 槽，按函数头中的 `upvalue uN = local lM` /
`upvalue uN = upvalue uM` / `upvalue uN = global gM` 声明在 `make_function` 时
捕获对应 cell：`local` 取父帧局部、`upvalue` 取父帧上级捕获、`global` 取创建点
当前的全局 cell。因此循环体里每次 `bind_local`/`init_global` 产生的 cell 会被当次
创建的闭包单独捕获（不同迭代捕获不同 cell）。

#### load_global / init_global / set_global

```text
rD = load_global gN
init_global gN, rV
set_global gN, rV
```

`gN` 是编译器分配的**数值全局槽**。`init_global` 新建 cell（对应顶层 `let`），
`set_global` 更新，`load_global` 读取。模块产品在 `names:` 后附带
`globals:` 区段（`gN = nK`），链接器按名字去重并重定位跨模块引用。

### 4.3 调用

#### call

```text
rD = call rF [rA0, rA1, ...]
```

`rF` 必须是函数值，否则报 `can only call functions`。实参个数必须等于被调函数的
`arity`，否则报 `expected N arguments but got M`。创建新帧执行函数体；函数 `return`
的值写入 `rD`，函数体正常结束而无 `return` 时 `rD` 为 `nil`。递归经由闭包环境支持。
调用受调用深度预算约束；启用 JIT 时该调用可能被编译路径接管（默认禁用，语义不变）。

#### call_direct

```text
rD = call_direct fN [rA0, rA1, ...]
```

按函数表 `fN` 直接调用编译器证明**无捕获自由变量**的函数，跳过 `make_function`/
`call` 的函数值间接层：直接调用不构造函数值、不复制调用者名字环境，也不计函数值
运行时元素预算；global 来源的 upvalue 仍经全局 cell 解析。验证期拒绝越界目标、
元数不匹配以及带 local/upvalue 捕获的目标；递归直接调用正确。普通闭包调用继续走
`call`。

#### call_native

```text
rD = call_native iN [rA0, rA1, ...]
```

按 `native_imports` 表的 `iN` 调用注册的 VM 原生标准库函数。VM 在构造期把导入索引
解析为内部 native ID，执行热路径不再做字符串查找。导入名在验证期必须属于支持集合、
`abi=1` 且不重复，索引必须不越界；未验证工件运行时报
``unknown native stdlib function `<名字>` ``。实参个数不满足该函数的元数时报其固定
错误；各函数自身再做类型/取值校验。完整注册表见第 5 节。

旧 `cdbc 0.1` 的 `native_call nN [rA0, ...]` 按名字表调用，仍在兼容读取路径中
保留，但 0.2 工件永不发射该形式。

### 4.4 集合与字段

当编译器静态知道集合类型时，索引、赋值与长度使用集合专用指令，解释器热路径不再按
运行时值标签分派：

```text
rD = array_get rC, rI    rD = array_set rC, rI, rV
rD = map_get rC, rI      rD = map_set rC, rI, rV
rD = range_get rC, rI

rD = len_array rV        rD = len_map rV
rD = len_range rV        rD = len_str rV
```

#### array_get / array_set

数组读取/更新：下标必须是整数 number 且不越界（`array index must be number` /
`array index must be integer` / `array index out of range`），`array_set` 替换既有
元素并返回赋入值，不扩容。

#### map_get / map_set

map 读取/更新：键限 nil/number/bool/string 并按运行时相等查找，缺失读报
`map key not found`；`map_set` 更新或插入（新增条目计入元素预算）并返回赋入值。

#### range_get

range 读取：下标必须是整数 number 且不越界，按 `start + step * 下标` 计算；空 range
或越界报 `range index out of bounds`。range 不可写，不存在 `range_set`。

#### len_array / len_map / len_range / len_str

`len_array`/`len_map` 为元素数，`len_range` 为区间长度，`len_str` 为 Unicode 标量数
（不是 UTF-8 字节数）。

#### 旧式动态 index / assign_index / len（兼容读取）

旧 `cdbc 0.1` 的 `index`、`assign_index` 与 `len` 仍在兼容读取路径中保留，按运行时
类型在数组/map/range/string 之间分派；0.2 工件对静态已知集合类型永远发射专用指令。

#### field

```text
rD = field rO, nN
```

读取结构体 `rO` 中名字为 `nN` 的字段。非结构体报
`can only access fields on structs`；字段不存在报 ``undefined field `<名字>` ``。
字段按名字线性查找（当前表示没有字段偏移表）。

#### assign_field

```text
rD = assign_field rO, nN, rV
```

更新结构体 `rO` 的既有字段 `nN` 为 `rV`，`rD` 得到赋入值。字段必须已存在，否则报
``undefined field `<名字>` ``；非结构体报 `can only assign fields on structs`。

#### iter_init / iter_has / iter_next

```text
rIter = iter_init rCollection
rHas = iter_has rIter
rValue = iter_next rIter
```

for-in 通过 VM 内部迭代器协议降级（迭代器是 VM 内部值，不暴露为源语言值）：

- `iter_init`：数组在进入时快照**长度**（迭代期间读活数组元素）、map 快照成按插入序
  排列的键数组、range 保持不可变；非数组/map/range 报
  `for-in expects array, range, or map`。
- `iter_has`：纯查询，返回是否还有元素，不推进位置。
- `iter_next`：返回当前元素并推进一位；越界报 `iterator exhausted`。

编译器把 `iter_has`/`iter_next` 排进循环块，`break`/`continue` 与迭代期间的变更
语义和旧 `assert_array` + `len` + `index` 降级完全一致。旧 `cdbc 0.1` 的
`assert_array` 仍在兼容读取路径中保留。

#### assert_number

```text
rD = assert_number rV, nN
```

`rV` 是 number 时原样写入 `rD`；否则以名字表 `nN` 的文本作为运行时错误消息。
编译器用它固化静态已知的数值断言，消息由前端选择。

### 4.5 算术、逻辑与比较

当编译器静态知道操作数类型时，算术与有序比较使用单一类型的专用指令，解释器热路径
不再按运行时值标签分支：

```text
rD = add_num rL, rR     rD = concat_str rL, rR
rD = sub_num rL, rR     rD = mul_num rL, rR
rD = div_num rL, rR     rD = neg_num rV

rD = lt_num rL, rR      rD = lt_str rL, rR
rD = le_num rL, rR      rD = le_str rL, rR
rD = gt_num rL, rR      rD = gt_str rL, rR
rD = ge_num rL, rR      rD = ge_str rL, rR
```

#### add_num / sub_num / mul_num / div_num / neg_num

仅接受 number：`add_num`/`sub_num`/`mul_num`/`div_num` 做 IEEE 754 浮点运算，
非 number 报 ``<op> expects numbers``；`div_num` 除数为零报 `division by zero`；
`neg_num` 取负，非 number 报 `neg_num expects number, got <类型>`。

#### concat_str

把两个 string 按原文拼接；非 string 报 `concat_str expects two strings`。

#### lt_num / le_num / gt_num / ge_num

两个 number 的数值有序比较，结果 bool；非 number 报 ``<op> expects numbers``。

#### lt_str / le_str / gt_str / ge_str

两个 string 按 Unicode 标量序列的字典序比较，结果 bool；非 string 报
``<op> expects two strings``。

#### not

```text
rD = not rV
```

真值取反：只有 `nil` 与 `false` 得到 `true`；`0`、`""` 等得到 `false`。

#### equal / not_equal

```text
rD = equal rL, rR
rD = not_equal rL, rR
```

运行时相等比较，`not_equal` 是其取反。聚合引用值按身份相等（见第 3 节），variant
按枚举名/变体名/payload 递归比较，range 按分量比较。

#### 旧式动态算术/比较（兼容读取）

旧 `cdbc 0.1` 的 `negate`、`add`、`subtract`、`multiply`、`divide`、
`greater`、`greater_equal`、`less`、`less_equal` 仍在兼容读取路径中保留：`add`
同时支持 number 加法与 string 拼接，其余有序比较在运行时区分 number/string。
0.2 工件对已知类型永远发射专用指令；只有泛型 `T: Ord` 函数体（形参类型未实例化）
仍使用动态有序比较。VM 不再按全局名字查找结构体 Ord 见证——结构体不可有序比较，
编译器在类型检查阶段已拒绝这类表达式。

### 4.6 控制流

#### jump

```text
jump N
```

无条件把 `ip` 设为 `N`。`N` 合法范围是 `0..=指令数`；等于指令数时函数体直接结束
（相当于无返回值返回）。

#### jump_if_false / jump_if_true

```text
jump_if_false rC, N
jump_if_true rC, N
```

按 `rC` 的真值（见第 3 节）条件跳转：`jump_if_false` 在假时跳转，
`jump_if_true` 在真时跳转；不满足条件时顺序执行下一条。目标范围与 `jump` 相同。

### 4.7 输出与返回

#### print

```text
print rV
```

旧式兼容读取指令：把 `rV` 的运行时文本表示加一个换行写入程序输出。0.2 工件不发射
该形式；编译器把 `print` 语句降为 ``rD = call_native i_print [rV]``，由 native
框架统一管理输出字节预算、取消、副作用与 trace 归因，行为与旧指令完全一致。
`print` 是程序输出的唯一来源；`main` 的返回值不会被打印。

#### return

```text
return rV
```

结束当前函数体（或 `main`），返回 `rV`。函数调用者通过 `call` 的目标寄存器取得该值；
`main` 的返回值只表示程序结束，不产生输出。

## 5. native 导入注册表

`native_imports` 的名字必须是下表之一（验证期强制）。元数列为固定的最小/最大实参个数；
标「回调」的会通过 VM 的普通函数调用机制回调传入的函数值，因此遵守同样的预算、取消与
错误栈规则。

| 名字 | 元数 | 回调 | 说明 |
| --- | --- | --- | --- |
| `push` | 2 | 否 | 向数组尾部追加，返回 nil（原地修改） |
| `pop` | 1 | 否 | 移除并返回数组尾元素，空数组报错 |
| `remove` | 2 | 否 | 从 map 移除键并返回被移除值 |
| `clear` | 1 | 否 | 清空 map |
| `merge` | 2 | 否 | 浅合并为新的有序 map（条目顺序：左图全部 + 右图全部） |
| `keys` | 1 | 否 | 返回 map 的键数组（插入序） |
| `values` | 1 | 否 | 返回 map 的值数组（插入序） |
| `floor` / `ceil` / `sqrt` | 1 | 否 | 数值舍入/平方根 |
| `str` | 1 | 否 | 转成字符串表示 |
| `substr` | 3 | 否 | 按 Unicode 标量切片（起始、长度） |
| `charAt` | 2 | 否 | 按 Unicode 标量取字符 |
| `typeOf` | 1 | 否 | 返回运行时类型名 |
| `hash` | 1 | 否 | 确定性 32 位 FNV-1a 哈希（number） |
| `contains` | 2 | 否 | 数组元素/map 键/range 成员测试（按运行时相等） |
| `slice` | 3 | 否 | 数组浅切片 |
| `copy` | 1 | 否 | 数组浅拷贝 |
| `concat` | 2 | 否 | 两个数组浅拼接，返回新数组 |
| `map` | 2 | 是 | 一参回调逐个映射，返回新数组 |
| `filter` | 2 | 是 | 一参布尔谓词过滤，返回新数组 |
| `flatMap` | 2 | 是 | 一参回调返回数组，展平一层 |
| `any` | 2 | 是 | 布尔谓词短路「存在」，空数组为 false |
| `all` | 2 | 是 | 布尔谓词短路「全真」，空数组为 true |
| `count` | 2 | 是 | 谓词为真的元素个数 |
| `find` | 2 | 是 | 第一个谓词为真的元素或 nil |
| `findIndex` | 2 | 是 | 第一个谓词为真的零基下标或 -1 |
| `reduce` | 3 | 是 | `(累加器, 元素)` 二参回调左折叠，须显式初值 |
| `range` | 1..3 | 否 | `range(start[, stop[, step]])`，生成不可变整数区间 |
| `print` | 1 | 否 | 输出一个值并换行，返回 nil（由 native 框架管理预算） |

这些函数同时以可遮蔽的普通函数形式和不可遮蔽的成员调用糖暴露给语言层；字节码层面
只关心 `call_native` 的导入名字与元数契约。

## 6. 兼容性与非目标

- `cdbc 0.1` 是强兼容契约：新 opcode、新 section、语义变化都必须同步更新 C++ 发射端、
  Rust 解析/格式化/执行端、本参考、英文契约与黄金工件。
- 本格式当前**不是**二进制编码，不定义二进制布局、压缩、校验器内部结构、GC 布局、
  任务调度器协议或 JIT 元数据格式。这些是后续阶段或决策门的范围。
- 字符串不做 grapheme 分段与归一化；`number` 无整数/浮点之分；字符串不支持 `index`。
- 指令集是类型擦除的高层字节码：运行时做动态类型检查，指令本身不带静态类型标注。
