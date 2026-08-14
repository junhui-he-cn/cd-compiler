# cd-compiler Bytecode 0.2 重构执行计划

> 目标仓库：`junhui-he-cn/cd-compiler`  
> 目标：将当前 `.cdbc 0.1` 的“高层、名字驱动、类型擦除 Bytecode”逐步重构为更适合解释器、Verifier、JIT 和长期演进的 `.cdbc 0.2`。  
> 执行方式：本文件按阶段交给 AI Agent 执行。每个阶段必须独立完成、独立测试、独立提交，不允许一次性跨阶段大改。

---

## 0. 总体目标

当前 `.cdbc 0.1` 更接近“文本化高层 IR + VM 契约”，仍保留大量源语言层语义，例如：

- `load_var / store_var / assign_var` 仍然使用名字索引；
- 闭包通过名字环境捕获，而不是显式 upvalue；
- struct 字段通过字段名查询；
- enum 使用类型名/variant 名；
- native 通过字符串名称调用；
- Ord witness 通过约定字符串名称寻找；
- `index / len / add` 等指令承担多种运行时动态分派；
- 控制流使用 instruction offset，而不是显式 Basic Block；
- verifier 主要是结构和索引范围校验，缺少 CFG / definite-assignment；
- module dependency 通过在 `main` 指令流中按 `at=N` 插入实现；
- 文档与 map 实际实现存在重复键语义不一致。

Bytecode 0.2 的设计目标：

```text
Source
  ↓
AST
  ↓
TypeChecker
  ↓
IR
  ↓
SSA / Optimize
  ↓
Closure Conversion
  ↓
Layout Lowering
  ↓
cdbc 0.2
  ↓
Verifier
  ↓
Interpreter / JIT
```

其中 `.cdbc 0.2` 必须满足：

1. 名字解析基本完成；
2. 闭包转换基本完成；
3. 类型/字段/variant 布局基本完成；
4. CFG 已显式形成；
5. VM 不再承担本应属于编译器 lowering 的工作；
6. 文本格式继续保留，暂不引入 binary bytecode；
7. 不改变源语言 `number = f64` 的语义。

---

# 1. 核心设计原则

AI Agent 在所有阶段必须遵守以下原则。

## 1.1 不改变源语言语义

本次重构重点是 Bytecode / VM 边界调整，不是语言设计重写。

暂时保持：

```text
number = IEEE-754 f64
```

不要在 Bytecode 0.2 阶段引入：

```text
Int64
UInt64
BigInt
```

除非未来单独设计语言版本。

---

## 1.2 不立即引入 Binary Bytecode

`.cdbc 0.2` 第一阶段仍然使用文本格式。

原因：

- 当前 dump / golden test / verify / trace 工具都依赖文本格式；
- ISA 本身还在变化；
- 同时修改 ISA + binary encoder + decoder 会显著增加调试复杂度。

Binary container 放到 Bytecode ISA 稳定之后再考虑。

---

## 1.3 VM 不应做编译器 lowering

最终 VM 不应再回答：

```text
"x" 是 local 还是 global？
"name" 是 struct 的第几个字段？
"sqrt" 对应哪个 native？
User 的 Ord witness 函数叫什么？
```

这些问题应在编译器阶段转换为：

```text
LocalId
UpvalueId
GlobalId
FieldSlot
NativeId
FuncId
```

---

## 1.4 每个阶段必须可运行

禁止一次完成所有重构。

推荐规则：

```text
Phase N 完成
    ↓
build
unit test
golden test
VM parity test
    ↓
commit
    ↓
Phase N+1
```

如果一个阶段不能保持仓库可构建，应进一步拆分。

---

# 2. Bytecode 0.2 ID 体系

新增/统一以下 ID：

```text
cN  ConstantId
sN  StringId
rN  RegId
lN  LocalId
uN  UpvalueId
gN  GlobalId
fN  FuncId
tN  TypeId
vN  VariantId
iN  NativeId / NativeImportId
bN  BlockId
mN  ModuleId
```

重要原则：

```text
StringId != Symbol Identity
```

`StringId` 只能用于：

- debug；
- source metadata；
- error text；
- 显示名称；
- native import 的外部符号名称。

不能再使用：

```text
StringId / name index
```

作为 VM 内变量、字段、variant 的核心身份。

---

# 3. 建议的 Bytecode 0.2 顶层模型

目标数据结构概念：

```text
Program
├── version
├── strings
├── constants
├── types
├── globals
├── native_imports
├── modules
├── functions
└── entry/module metadata
```

不再维护特殊的：

```text
Program.main
```

`main` / module init 统一作为普通函数存在。

建议：

```text
Module {
    id
    identity
    init_function
    is_entry
}
```

或最小版本：

```text
Program {
    entry: FuncId
    functions: Vec<Function>
}
```

后续 module redesign 再增加 `Module`。

---

# 4. Phase 0：建立重构基线

## 目标

在修改 Bytecode 前锁定当前行为，避免重构后无法判断是否破坏语义。

## Agent 任务

### 4.1 阅读并记录以下文件

至少检查：

```text
docs/bytecode-instructions-zh.md

vm-rs/src/bytecode.rs
vm-rs/src/format.rs
vm-rs/src/runtime.rs
vm-rs/src/vm.rs

compiler / emitter 中负责 bytecode 输出的代码
例如：
BytecodeTextEmitter.cpp
BytecodeCompiler 相关实现
```

Agent 必须先确认仓库实际路径，不允许凭假设修改。

### 4.2 建立行为基线测试

至少覆盖：

- local variable；
- assignment；
- shadowing；
- nested closure；
- closure mutation；
- loop + closure capture；
- global variable；
- array；
- map；
- duplicate map key；
- struct field read/write；
- enum variant；
- native call；
- arithmetic；
- string concat；
- comparison；
- function call；
- recursive call；
- module dependency；
- jump；
- invalid bytecode verification；
- debug metadata。

### 4.3 特别加入 map 重复键测试

输入逻辑：

```text
a: 1
b: 2
a: 3
```

锁定当前实际行为。

最终 Bytecode 0.2 目标语义：

```text
unique keys
stable insertion order
last write wins
updating an existing key does not move its position
```

### 4.4 double 文本 round-trip 测试

加入能够验证 15 位精度不足的 f64 case。

## 验收标准

- 当前 master 行为全部被测试覆盖；
- 所有测试通过；
- 没有改变现有 Bytecode；
- 输出一份 `BASELINE.md` 或测试说明；
- 之后所有阶段以这些测试为回归基线。

---

# 5. Phase 1：重构 Bytecode 核心 ID 和数据结构

## 目标

先重构 `bytecode.rs` 的数据模型，不立即大规模修改 VM 执行逻辑。

## 5.1 新增强类型 ID

Rust 建议使用 newtype：

```rust
struct RegId(u32);
struct LocalId(u32);
struct UpvalueId(u32);
struct GlobalId(u32);
struct FuncId(u32);
struct TypeId(u32);
struct VariantId(u32);
struct NativeId(u32);
struct BlockId(u32);
struct StringId(u32);
struct ConstId(u32);
```

如已有类似类型，复用而不是重复定义。

## 5.2 取消特殊 main

目标：

```rust
Program {
    functions: Vec<Function>,
    entry: FuncId,
    ...
}
```

不要同时保留：

```rust
main: FunctionBody
functions: Vec<Function>
```

如果一次改动太大，可先增加 `entry`，然后兼容旧 main，下一提交再彻底删除。

## 5.3 Function 预留以下元数据

建议：

```rust
Function {
    id: FuncId,
    name: Option<StringId>,

    arity: u32,

    register_count: u32,
    local_count: u32,

    upvalues: Vec<UpvalueDesc>,

    body: ...
}
```

暂时允许 `body` 仍使用线性 instruction list。

## 验收标准

- 数据结构使用强类型 ID；
- main 开始统一进入 Function 模型；
- parser/emitter/VM 能编译；
- 旧 `.cdbc 0.1` 仍可运行，或者提供明确 0.1→内部 0.2 兼容层；
- 所有 Phase 0 测试通过。

---

# 6. Phase 2：变量 lowering

这是整个重构中优先级最高的一步。

## 当前问题

当前 Bytecode：

```text
load_var nN
store_var nN
assign_var nN
```

VM 仍需通过名字决定变量作用域和 local slot。

目标：变量在进入 Bytecode 前已经被解析。

---

## 6.1 新增变量指令

推荐：

```text
rD = load_local lN
bind_local lN, rV
set_local lN, rV

rD = load_upvalue uN
set_upvalue uN, rV

rD = load_global gN
init_global gN, rV
set_global gN, rV
```

### 不要使用单一 `store_local`

必须区分：

```text
bind
```

和：

```text
set
```

原因是闭包 + shadowing + loop capture 需要区别：

```text
新 binding
```

与：

```text
修改已有 binding
```

---

## 6.2 local slot 分配必须移到编译器

编译器在生成 Bytecode 前完成：

```text
symbol
    ↓
local/upvalue/global
    ↓
numeric slot
```

VM 不再扫描：

```text
StoreVar
```

然后建立 `slot_by_name`。

---

## 6.3 参数

函数入口定义：

```text
l0 = arg0
l1 = arg1
...
```

参数槽位视为已绑定。

---

## 6.4 shadowing

必须测试：

```text
let x = 1;

{
    let x = 2;
}
```

两个 `x` 必须映射到不同 `LocalId`，而不是相同名字。

---

## 验收标准

- 新编译生成的 bytecode 不包含 `load_var/store_var/assign_var`；
- VM 热路径不需要通过变量名字查询 local；
- shadowing 行为正确；
- assignment 行为正确；
- globals 行为正确；
- Phase 0 测试通过。

---

# 7. Phase 3：显式 Closure / Upvalue

## 目标

删除基于字符串环境的闭包捕获模型。

---

## 7.1 UpvalueDesc

建议：

```rust
enum UpvalueSource {
    Local(LocalId),
    Upvalue(UpvalueId),
}

struct UpvalueDesc {
    source: UpvalueSource,
}
```

Function：

```rust
Function {
    upvalues: Vec<UpvalueDesc>,
    ...
}
```

---

## 7.2 Closure runtime

目标：

```rust
struct Closure {
    function: FuncId,
    upvalues: Vec<CellRef>,
}
```

删除或逐步淘汰：

```text
Environment<String, Cell>
```

作为 closure 的主要表示。

---

## 7.3 make_closure

替换：

```text
make_function fN
```

为：

```text
make_closure fN
```

其 capture descriptor 来自 `Function.upvalues`。

示例：

```text
function f2:
    capture u0 = local l3
    capture u1 = upvalue u0
```

`make_closure f2`：

```text
u0 ← parent.local[l3].cell
u1 ← parent.upvalues[u0]
```

---

## 7.4 captured local 的 binding 语义

Local runtime 概念上应支持：

```text
Unbound
Direct(Value)
Captured(CellRef)
```

如果 local 会被 closure 捕获：

```text
bind_local l0, r0
```

必须创建新的 Cell。

如果：

```text
set_local l0, r1
```

则修改当前 Cell。

这对 loop capture 至关重要。

---

## 7.5 必测场景

### Case A

```text
let x = 1;
let f = fn() { return x; };
x = 2;
f() == 2
```

### Case B

循环中创建多个 closure：

```text
for (...) {
    let x = ...
    push(fn(){ return x; })
}
```

不同 closure 必须捕获不同 binding。

### Case C

多层 closure：

```text
A -> B -> C
```

C 可以 capture B 的 upvalue。

### Case D

未使用变量不能被捕获。

---

## 验收标准

- Closure 不再复制当前所有可见变量；
- 不再使用变量名查 closure environment；
- 只捕获实际 free variables；
- nested closure 和 mutation 语义正确；
- loop capture 正确；
- 所有回归测试通过。

---

# 8. Phase 4：Basic Block + Terminator

## 目标

将 instruction-offset 控制流改为显式 CFG。

---

## 8.1 BasicBlock

建议：

```rust
struct BasicBlock {
    id: BlockId,
    instructions: Vec<Instruction>,
    terminator: Terminator,
}
```

---

## 8.2 Terminator

建议：

```rust
enum Terminator {
    Br {
        target: BlockId,
    },

    BrIf {
        condition: RegId,
        if_true: BlockId,
        if_false: BlockId,
    },

    Return {
        value: RegId,
    },

    ReturnNil,
}
```

---

## 8.3 替换旧跳转

```text
jump
→ br

jump_if_true
jump_if_false
→ br_if
```

不要保留两个条件跳转 opcode。

---

## 8.4 禁止隐式函数返回

旧行为如果支持：

```text
执行到 function instruction end
→ return nil
```

0.2 应取消。

编译器必须生成：

```text
return_nil
```

---

## 8.5 文本格式

推荐：

```text
block b0:
    ...
    br_if r0, b1, b2

block b1:
    ...
    return r1
```

文本 parser 直接使用 BlockId，不再以绝对 instruction offset 表示控制流。

---

## 验收标准

- 所有函数由 BasicBlock 组成；
- 每个 block 必须有且只有一个 terminator；
- branch target 必须是有效 BlockId；
- 不存在隐式 fallthrough；
- 不存在跳到 function end 的特殊语义；
- existing control-flow tests 通过。

---

# 9. Phase 5：Verifier 2.0

## 目标

Verifier 从“索引范围检查器”升级成真正的静态 bytecode verifier。

---

## 9.1 结构验证

必须检查：

```text
ConstId
StringId
FuncId
LocalId
UpvalueId
GlobalId
TypeId
VariantId
NativeId
BlockId
RegId
```

全部合法。

---

## 9.2 CFG 验证

检查：

- block ID 唯一；
- terminator 存在；
- branch target 合法；
- 不允许 terminator 后继续存在 instruction；
- entry block 合法。

---

## 9.3 Register definite-assignment

算法：

```text
IN[b] = intersection(OUT[pred])
OUT[b] = IN[b] + registers defined in b
```

扫描 block 时：

```text
read rN
```

要求：

```text
rN ∈ current_defined_set
```

否则 verifier error。

示例：

```text
r1 = add_num r0, r0
```

如果 `r0` 未定义：

```text
verification error
```

不能再因为 VM 默认把 register 初始化为 Nil 而拖到运行期。

---

## 9.4 Local definite-binding

参数 local 默认 bound。

其他 local：

```text
load_local
set_local
```

前必须证明在所有控制流路径上已有：

```text
bind_local
```

---

## 9.5 Closure 验证

检查：

```text
capture local lN
capture upvalue uN
```

索引合法；

closure parent 合法；

被 capture 的 local 存在；

make_closure 时相关 binding 已建立。

---

## 9.6 Native 验证

利用 NativeSpec：

```text
min_arity
max_arity
ABI
```

尽可能在 verify 阶段检查。

---

## 验收标准

新增 negative tests：

- undefined register；
- unbound local；
- invalid block；
- missing terminator；
- invalid upvalue；
- invalid native arity；
- invalid function ID；
- malformed capture。

所有 malformed bytecode 在执行前被拒绝。

---

# 10. Phase 6：Type/Layout Table

## 目标

将 struct / enum 的名字驱动访问降为数字布局。

---

## 10.1 Type table

建议：

```text
t0 = struct User {
    field 0 = "id"
    field 1 = "name"
    field 2 = "age"
}

t1 = enum Option {
    v0 None payload=0
    v1 Some payload=1
}
```

字段字符串仅用于：

- debug；
- dump；
- diagnostics。

---

## 10.2 Struct 指令

替换：

```text
struct
field
assign_field
```

为：

```text
make_struct tN [...]
struct_get rObj, tN, field_slot
struct_set rObj, tN, field_slot, rValue
```

运行时结构：

```rust
StructValue {
    type_id: TypeId,
    identity: ...,
    fields: Vec<Value>,
}
```

不再依赖：

```text
Vec<(String, Value)>
```

完成字段读取。

---

## 10.3 Enum 指令

替换：

```text
variant
variant_tag
variant_field
```

为：

```text
make_variant tN, vN [...]
is_variant rValue, tN, vN
variant_get rValue, tN, vN, payload_index
```

Runtime：

```rust
VariantValue {
    type_id: TypeId,
    variant_id: VariantId,
    payload: Vec<Value>,
}
```

---

## 验收标准

- struct field access 不再字符串线性搜索；
- enum variant identity 不再依赖类型名/variant 名；
- verifier 可以检查 field slot / payload index；
- 所有 struct/enum tests 通过。

---

# 11. Phase 7：Native Import Table

## 目标

将：

```text
native_call nN
```

改为：

```text
call_native iN
```

---

## 11.1 Native import metadata

建议：

```text
native_imports:
  i0 = "std.array.push" abi=1
  i1 = "std.math.sqrt" abi=1
  i2 = "std.io.print" abi=1
```

load/link 时解析：

```text
iN → NativeId
```

VM hot path 不再字符串查找。

---

## 11.2 删除核心 `print` opcode

旧：

```text
print r0
```

改成：

```text
rTmp = call_native i_print [r0]
```

`print` 的：

- output budget；
- cancellation；
- side effect policy；

由 native framework 管理。

---

## 11.3 ABI

至少定义：

```text
native symbol name
ABI version
arity
```

未来可以扩展：

```text
effects
capabilities
signature
```

---

## 验收标准

- core VM ISA 不含 `print`；
- native dispatch 热路径不使用字符串；
- native arity 尽可能在 verify 阶段完成；
- 原 native tests 通过。

---

# 12. Phase 8：Typed / Specialized Opcodes

## 目标

减少解释器中的动态类型分派。

---

## 12.1 算术

旧：

```text
add
subtract
multiply
divide
negate
```

建议：

```text
add_num
sub_num
mul_num
div_num
neg_num

concat_str
```

---

## 12.2 比较

保留通用：

```text
eq
neq
```

按语言现有 equality semantics。

有序比较拆分：

```text
lt_num
le_num
gt_num
ge_num

lt_str
le_str
gt_str
ge_str
```

---

## 12.3 Ord witness

VM 不再通过字符串：

```text
__capability_ord_User_less
```

寻找 witness。

TypeChecker / lowering 应把：

```text
a < b
```

变为：

```text
call_direct fWitness [...]
```

或：

```text
call rWitness [...]
```

VM 不需要理解：

```text
Ord
trait
capability
witness naming convention
```

---

## 验收标准

- arithmetic hot path 不再每次判断 number/string；
- struct comparison 不再通过函数命名字符串解析；
- semantic parity tests 全部通过。

---

# 13. Phase 9：集合专用指令

## 目标

删除 `index / assign_index / len` 的多类型动态分派。

---

## 13.1 Array

```text
make_array
array_get
array_set
len_array
```

---

## 13.2 Map

```text
make_map
map_get
map_set
len_map
```

---

## 13.3 Range

```text
range_get
len_range
```

不允许：

```text
range_set
```

---

## 13.4 String

```text
len_str
```

如未来语言支持字符串 index，可单独设计。

---

## 验收标准

- 不再有通用 `index`；
- 不再有通用 `assign_index`；
- 不再有通用 `len`；
- verifier 能更精确验证 opcode；
- tests 通过。

---

# 14. Phase 10：Iterator Protocol

## 目标

删除语义混乱的 `assert_array`。

当前 `assert_array` 实际支持：

```text
array
range
map
```

因此它本质是：

```text
prepare_for_iteration
```

---

## 14.1 新指令

建议：

```text
rIter = iter_init rCollection
rHas, rValue = iter_next rIter
```

如果现有 bytecode 不方便支持双结果指令，也可以设计：

```text
rValue = iter_next rIter, bEnd
```

优先选择更符合现有 VM 数据模型的形式。

---

## 14.2 保持现有 snapshot semantics

Array：

```text
snapshot length
```

Map：

```text
snapshot keys
```

Range：

```text
snapshot start/end/step
```

确保 mutation during iteration 行为与 0.1 已有契约一致。

---

## 14.3 Runtime iterator 为 VM internal value

不必直接暴露为源语言值。

---

## 验收标准

- 删除 `assert_array`；
- for-in lowering 统一走 iterator protocol；
- array/map/range iteration 语义与旧版一致；
- mutation-during-iteration tests 通过。

---

# 15. Phase 11：Map 语义正式修订

## 目标

解决文档和实际实现不一致。

Bytecode 0.2 正式定义：

```text
Map keys are unique.
Map preserves insertion order.
Last write wins.
Updating an existing key does not change its iteration position.
```

示例：

```text
a:1
b:2
a:3
```

结果：

```text
a:3
b:2
```

必须统一：

- constructor；
- map_set；
- map_get；
- remove；
- keys；
- values；
- iteration；
- merge；
- equality（若相关）；
- print/dump。

---

## 验收标准

文档、Rust VM、C++ compiler/emitter、tests 行为完全一致。

---

# 16. Phase 12：Module System 重构

## 目标

移除 dependency `at=N` + main instruction insertion。

---

## 16.1 Module metadata

建议：

```text
module m0:
    init = f0

module m1:
    init = f4
```

Runtime module state：

```text
Uninitialized
Initializing
Initialized
```

---

## 16.2 init_module

新指令：

```text
init_module mN
```

行为：

```text
if Initialized:
    no-op

if Uninitialized:
    mark Initializing
    call module.init

if Initializing:
    follow language-defined cycle policy
```

循环依赖策略必须显式定义。

---

## 16.3 Linker

Linker 只负责：

```text
table merge
symbol resolution
ID relocation
module graph
```

不再负责：

```text
split main instruction stream
insert dependency instructions
repair jump offsets
repair debug offsets
```

---

## 验收标准

- 不再出现 `dependency at=N`；
- module init 可单独调用；
- 依赖只初始化一次；
- cyclic import 行为有测试；
- debug metadata 不需要因为插指令而整体偏移修复。

---

# 17. Phase 13：Direct Call

## 目标

为已知、不捕获 closure 的函数增加直接调用。

新指令：

```text
rD = call_direct fN [args...]
```

适用：

```text
top-level function
module init
known witness
captures=0 function
```

Verifier 可静态检查：

```text
FuncId
arity
captures == 0
```

JIT 可直接生成：

```text
direct function call
```

---

## 验收标准

- direct calls 不需要构造 Function Value；
- verifier 检查 arity；
- recursive direct call 正确；
- ordinary closure call 继续使用 `call`。

---

# 18. Phase 14：f64 文本精度修复

该任务可提前单独完成，也可以在任意低风险阶段执行。

查找类似：

```cpp
std::setprecision(15)
```

改为：

```cpp
std::setprecision(
    std::numeric_limits<double>::max_digits10
)
```

确保 include：

```cpp
#include <limits>
```

必须增加 round-trip test：

```text
double
→ text bytecode
→ parser
→ double
```

bitwise 或数值严格相等。

---

# 19. 建议的最终 0.2 opcode 集

最终预计包含：

## 常量/数据移动

```text
constant
copy
```

## Closure / Binding

```text
make_closure

load_local
bind_local
set_local

load_upvalue
set_upvalue

load_global
init_global
set_global
```

## Call

```text
call
call_direct
call_native
```

## Array

```text
make_array
array_get
array_set
len_array
```

## Map

```text
make_map
map_get
map_set
len_map
```

## Range

```text
range_get
len_range
```

## Struct

```text
make_struct
struct_get
struct_set
```

## Enum

```text
make_variant
is_variant
variant_get
```

## Iterator

```text
iter_init
iter_next
```

## Type guards

```text
assert_type
```

如果静态类型保证足够强，可进一步减少 `assert_type` 使用。

## Arithmetic

```text
neg_num
add_num
sub_num
mul_num
div_num

concat_str
```

## Logic

```text
not
eq
neq
```

## Comparison

```text
lt_num
le_num
gt_num
ge_num

lt_str
le_str
gt_str
ge_str
```

## Other length

```text
len_str
```

## Module

```text
init_module
```

## Error

```text
trap
```

## Terminator

```text
br
br_if
return
return_nil
```

---

# 20. 0.1 → 0.2 指令迁移表

| cdbc 0.1 | cdbc 0.2 | 操作 |
|---|---|---|
| `constant` | `constant` | 保留 |
| `make_function` | `make_closure` | 修改 |
| `array` | `make_array` | 重命名 |
| `map` | `make_map` | 修改语义 |
| `struct` | `make_struct tN` | 修改 |
| `variant` | `make_variant tN,vN` | 修改 |
| `variant_tag` | `is_variant` | 修改 |
| `variant_field` | `variant_get` | 修改 |
| `move` | `copy` | 重命名 |
| `load_var` | `load_local/load_upvalue/load_global` | 拆分 |
| `store_var` | `bind_local/init_global` | 拆分 |
| `assign_var` | `set_local/set_upvalue/set_global` | 拆分 |
| `call` | `call` | 保留 |
| `native_call` | `call_native iN` | 修改 |
| `index` | `array_get/map_get/range_get` | 拆分 |
| `assign_index` | `array_set/map_set` | 拆分 |
| `field` | `struct_get` | 修改 |
| `assign_field` | `struct_set` | 修改 |
| `len` | `len_*` | 拆分 |
| `assert_array` | `iter_init` | 删除/替换 |
| `assert_number` | `assert_type` / 删除 | 修改 |
| `print` | native `std.io.print` | 删除 |
| `return` | terminator `return` | 保留/迁移 |
| `negate` | `neg_num` | 类型化 |
| `not` | `not` | 保留 |
| `add` | `add_num/concat_str` | 拆分 |
| `subtract` | `sub_num` | 修改 |
| `multiply` | `mul_num` | 修改 |
| `divide` | `div_num` | 修改 |
| `equal` | `eq` | 保留 |
| `not_equal` | `neq` | 保留 |
| `greater` | typed compare / witness call | 拆分 |
| `greater_equal` | typed compare / witness call | 拆分 |
| `less` | typed compare / witness call | 拆分 |
| `less_equal` | typed compare / witness call | 拆分 |
| `jump` | `br` | 修改 |
| `jump_if_false` | `br_if` | 合并 |
| `jump_if_true` | `br_if` | 合并 |

---

# 21. Agent 每阶段必须执行的标准流程

每个 Phase 必须按以下顺序执行。

## Step 1：Inspect

先阅读相关源码。

禁止在未定位真实实现前直接修改。

必须记录：

```text
涉及文件
关键结构
现有调用链
测试位置
兼容风险
```

---

## Step 2：Design

在修改代码前，先输出本阶段的最小设计：

```text
old model
new model
compatibility strategy
files to modify
tests to add
```

如果实际代码结构与本计划不同，应以实际代码为准，但必须说明差异。

---

## Step 3：Implement minimally

遵守：

```text
small patch
no unrelated refactor
no formatting sweep
no premature optimization
```

不要顺手重写整个 VM。

---

## Step 4：Tests

至少运行：

```text
unit tests
VM tests
compiler tests
bytecode parser tests
verifier negative tests
golden tests
```

如仓库有：

```text
cargo test
ctest
ninja test
lit
custom scripts
```

以仓库真实命令为准。

---

## Step 5：Parity

同一个源程序：

```text
old pipeline output
new pipeline output
```

在源语言可观察语义上必须一致。

重点比较：

```text
stdout
return/result
runtime error
closure behavior
iteration order
module init
```

---

## Step 6：Documentation

每个完成的 ISA 修改必须同步更新：

```text
docs/bytecode-instructions-zh.md
```

如有英文版，也同步修改。

禁止代码已经改变、文档仍描述旧语义。

---

## Step 7：Commit

一个 Phase 最好对应一个或少量明确 commit。

建议 commit style：

```text
bytecode: add explicit local/upvalue/global slots
vm: replace closure environment with upvalue vector
verifier: add register definite-assignment analysis
bytecode: introduce basic blocks and terminators
```

不要把所有 Phase 合并为一个巨型 commit。

---

# 22. Agent 禁止事项

AI Agent 不得：

1. 一次性重写整个编译器和 VM；
2. 在没有测试的情况下删除 0.1 行为；
3. 在 0.2 阶段增加新的源语言 number 类型；
4. 提前设计 binary bytecode；
5. 为了简化实现破坏 closure binding semantics；
6. 把 `bind_local` 和 `set_local` 简化成相同逻辑；
7. 保留 VM 中名字→slot 的隐式 lowering，同时声称完成变量 lowering；
8. 继续依赖字符串命名约定寻找 Ord witness；
9. 在 map 文档与实现语义不一致的情况下继续扩展 map；
10. 使用 `Nil` 掩盖未初始化寄存器错误；
11. 在 verifier 能处理的错误上继续拖到运行时；
12. 为追求 opcode 数量少而使用超宽泛动态 opcode；
13. 做无关代码格式化和大范围 rename；
14. 在阶段未通过全部测试前进入下一 Phase。

---

# 23. Agent 优先级

必须遵守：

```text
P0
变量 lowering
closure/upvalue
CFG
verifier

P1
type/layout
native ID
map semantics

P2
typed arithmetic
specialized collection opcodes
iterator

P3
module redesign
direct call
binary format（未来）
```

如果时间有限，只完成 P0 也比“一次性部分完成所有东西”更有价值。

---

# 24. 推荐的里程碑

## Milestone A：VM 不再按变量名执行

完成：

```text
Phase 0
Phase 1
Phase 2
Phase 3
```

结果：

- local/upvalue/global 全部 numeric；
- closure 精确捕获；
- VM 不再使用变量字符串进行热路径解析。

这是第一个重要里程碑。

---

## Milestone B：Bytecode 拥有真正 CFG 和 Verifier

完成：

```text
Phase 4
Phase 5
```

结果：

- BasicBlock；
- terminator；
- definite-assignment；
- definite-binding；
- malformed bytecode 更早失败。

这是第二个重要里程碑。

---

## Milestone C：对象和 native 不再字符串驱动

完成：

```text
Phase 6
Phase 7
Phase 11
```

结果：

- struct field slot；
- enum VariantId；
- NativeId；
- map 语义稳定。

这是第三个重要里程碑。

---

## Milestone D：解释器/JIT 友好 ISA

完成：

```text
Phase 8
Phase 9
Phase 10
Phase 13
```

结果：

- typed arithmetic；
- specialized collection access；
- iterator protocol；
- direct call。

---

## Milestone E：模块模型稳定

完成：

```text
Phase 12
```

之后再讨论：

```text
Binary .cdbc
Bytecode cache
Serialization ABI
Version compatibility
```

---

# 25. 第一条建议给 AI Agent 的执行命令

建议不要直接告诉 Agent：

> 实现全部 Bytecode 0.2。

而应从下面这个任务开始：

```text
请先执行本计划的 Phase 0。

要求：
1. 不修改 Bytecode ISA；
2. 阅读并确认 bytecode parser、VM、runtime、emitter、compiler lowering 的真实代码路径；
3. 为当前 .cdbc 0.1 建立行为基线测试；
4. 特别覆盖 closure binding、loop closure capture、map duplicate key、module dependency、invalid bytecode verifier 和 f64 round-trip；
5. 输出涉及文件、现有行为、发现的文档/实现不一致；
6. 所有当前测试必须继续通过；
7. 完成后停止，不执行 Phase 1。
```

Phase 0 完成后，再给 Agent：

```text
请执行本计划的 Phase 1。
只完成 Bytecode ID / Program / Function 基础数据模型重构。
不要提前执行变量 lowering、closure、CFG 或 typed opcode。
完成测试后停止。
```

这种方式最稳。

---

# 26. 最终完成标准

Bytecode 0.2 只有满足以下条件才视为完成：

- [ ] 新 Bytecode 不以变量名执行 local/upvalue/global；
- [ ] closure 只捕获实际 free variables；
- [ ] closure mutation / rebinding / loop capture 语义正确；
- [ ] main 与普通 Function 模型统一；
- [ ] CFG 使用 BasicBlock；
- [ ] control flow 不依赖裸 instruction offset；
- [ ] verifier 检查 register definite-assignment；
- [ ] verifier 检查 local definite-binding；
- [ ] struct field 使用 numeric slot；
- [ ] enum 使用 TypeId + VariantId；
- [ ] native call 使用 NativeId/import table；
- [ ] Ord witness 不通过字符串命名约定查询；
- [ ] map duplicate-key 语义唯一且文档/实现一致；
- [ ] `assert_array` 被 iterator protocol 替代；
- [ ] arithmetic / collection hot path 明显减少动态字符串或类型分派；
- [ ] module 不再通过 `at=N` 插入 main 指令；
- [ ] f64 文本 round-trip 可保证；
- [ ] `.cdbc 0.2` 文档与实现同步；
- [ ] interpreter tests 通过；
- [ ] JIT（如当前支持）与 interpreter 保持语义一致；
- [ ] malformed bytecode tests 通过；
- [ ] golden tests 通过；
- [ ] 原有源语言程序可观察行为无非预期变化。

---

# 27. 暂不处理的内容

以下内容明确不属于第一轮 Bytecode 0.2 重构范围：

```text
Binary bytecode file format
Int64 / BigInt language type
GC algorithm replacement
full SSA bytecode
register allocator rewrite
new object model unrelated optimization
new source-language syntax
new exception system
new async/coroutine model
```

除非某一项是完成上述 Phase 的必要依赖，否则不要扩展范围。

---

# 28. 成功后的目标架构

最终希望得到：

```text
Source
  │
  ▼
Parser / AST
  │
  ▼
TypeChecker
  │
  ▼
Typed IR
  │
  ▼
SSA / Optimization
  │
  ▼
Closure Conversion
  │
  ├── LocalId
  ├── UpvalueId
  └── GlobalId
  │
  ▼
Layout Lowering
  │
  ├── TypeId
  ├── FieldSlot
  ├── VariantId
  ├── FuncId
  └── NativeId
  │
  ▼
cdbc 0.2
  │
  ├── Functions
  ├── BasicBlocks
  ├── Registers
  ├── Explicit Upvalues
  ├── Typed/Specialized Ops
  └── Stable Metadata
  │
  ▼
Verifier
  │
  ├── structural verification
  ├── CFG verification
  ├── definite assignment
  ├── definite binding
  ├── capture validation
  └── ABI validation
  │
  ├───────────────┐
  ▼               ▼
Interpreter       JIT
```

核心原则：

> Compiler 负责理解语言，Bytecode 负责表达已 lower 的程序，VM 负责高效执行，而不是再次解释源语言名字和语义。

