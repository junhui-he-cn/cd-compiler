# CD VM / cdbc 0.3 — C Machine Foundation Plan

## 1. 版本目标

`cdbc 0.3` 的目标不是直接支持 LLVM 或 Clang，而是：

> 在现有 CD 动态值 VM 基础上，增加一套足以承载基础 C/LLVM machine semantics 的低层执行能力，并冻结一套后续 LLVM backend 可以稳定依赖的 VM ABI。

0.3 只修改 `cd-compiler/vm-rs`、cdbc artifact、interpreter、verifier、debug/disassembly 和测试；不修改 `cd-llvm`、LLVM SelectionDAG、Clang、libc 或 POSIX runtime。

最终结构：

```text
CD VM
├── Dynamic Layer
│   ├── Number / Bool / String
│   ├── Array / Map / Struct / Variant
│   └── existing dynamic semantics
│
└── Machine Layer
    ├── Machine integer
    ├── Machine float
    ├── Address
    ├── Linear memory
    ├── Machine stack/frame
    ├── Typed LOAD/STORE
    ├── DATA/BSS/RODATA
    ├── Scalar call ABI metadata
    └── Memory operations
```

核心原则：

1. 不推翻现有 VM。
2. 不把 C 对象映射成现有动态 `Array/Map/String/Struct`。
3. Machine layer 与 Dynamic layer 并存。
4. C object 使用 byte-addressable linear memory。
5. Pointer 使用 VM address，不使用 host pointer。
6. 精确整数不能继续依赖 `f64 Number`。
7. 0.3 先冻结 interpreter semantics，再考虑 JIT。
8. LLVM-specific 信息不能进入 cdbc 0.3 ABI。

---

## 2. 非目标

0.3 暂不实现：

```text
LLVM / Clang / libc / POSIX
完整 malloc/free allocator
function pointer / CALL_INDIRECT
varargs
atomics / threads / TLS
SIMD
exceptions
setjmp/longjmp
VLA
dynamic libraries
native object files
```

---

## 3. 复用现有 VM 基础设施

现有 VM 已经具备：

- virtual register execution model
- `Value`
- function representation
- function call / return
- call stack / resumable frames
- globals
- module/link infrastructure
- verifier
- debugger
- trace/profile
- scheduler
- JIT admission/fallback
- cdbc 0.2 serializer/deserializer

0.3 的原则是：

> 尽可能复用这些基础设施，只增加 C-compatible machine semantics。

禁止重新设计整个 frame/call/register system。

---

## 4. cdbc 版本策略

0.3 machine foundation 使用：

```text
cdbc 0.3
```

推荐兼容策略：

```text
Reader:
    cdbc 0.2
    cdbc 0.3

Writer:
    cdbc 0.3
```

这里的 `Writer: cdbc 0.3` 指 Rust VM 的显式 machine-artifact writer，不是
C++ 编译器的默认发射端。C++ 编译器、`--emit-bytecode` 和
`--emit-module-bytecode` 继续发射 `cdbc 0.2`；不得在仍标记为 `0.2` 的
artifact 中偷偷改变 wire semantics。

---

## 5. Milestone 总览

| Milestone | 内容 | 优先级 |
|---|---|---|
| VM03-00 | 0.3 spec / ABI contract | P0 |
| VM03-01 | Machine integer value | P0 |
| VM03-02 | Integer arithmetic / bitwise / compare | P0 |
| VM03-03 | Integer and float conversions | P0 |
| VM03-04 | Address type + LinearMemory | P0 |
| VM03-05 | Typed LOAD / STORE | P0 |
| VM03-06 | Machine stack/frame | P0 |
| VM03-07 | DATA / BSS / RODATA | P0 |
| VM03-08 | MEMCPY / MEMMOVE / MEMSET | P1 |
| VM03-09 | Function machine ABI metadata | P1 |
| VM03-10 | Symbols / relocations | P1 |
| VM03-11 | cdbc 0.3 serialization | P0 |
| VM03-12 | Verifier + trap model | P0 |
| VM03-13 | Debugger / disassembler machine support | P1 |
| VM03-14 | JIT fallback policy | P1 |
| VM03-15 | Machine integration suite | P0 |
| VM03-16 | ABI freeze / LLVM-ready gate | P0 |

---

## 6. VM03-00 — 0.3 Spec / ABI Contract

新增建议：

```text
docs/cdbc-0.3-machine-foundation.md
```

至少定义：

- Machine value model
- Integer representation
- Float representation
- Address model
- Linear memory
- Endianness
- Alignment
- Machine stack
- Data segments
- Machine opcodes
- Trap semantics
- Function machine metadata
- Artifact versioning
- Compatibility policy

验收：

- 所有 P0 数据模型有书面定义
- opcode semantics 无歧义
- address space policy 明确
- integer width/sign semantics 明确
- 0.2 compatibility policy 明确
- 不包含 LLVM-specific design

---

## 7. VM03-01 — Machine Integer Value

推荐扩展：

```rust
enum Value {
    // existing dynamic values
    Number(f64),
    Bool(bool),
    String(...),
    Array(...),
    Map(...),
    Struct(...),
    Variant(...),

    // cdbc 0.3 machine values
    MachineInt(u64),
    MachineFloat(f64),
    Address(u64),
}
```

Machine integer 统一保存：

```text
64-bit raw bits
```

不要创建 `I8/I16/I32/I64/U8/U16/U32/U64` 多个 Value variant。宽度和 signedness 由 opcode 语义决定。

验收：

- `MachineInt(u64)` 可 round-trip serialization
- 不影响动态 `Number`
- dynamic regression 全部通过
- debug/display 能区分 MachineInt 和 Number

---

## 8. VM03-02 — Integer Arithmetic / Bitwise / Compare

新增：

```text
ICONST

IADD
ISUB
IMUL

SDIV
UDIV
SREM
UREM

AND
OR
XOR
NOT

SHL
LSHR
ASHR

ICMP
```

需要明确 width：

```text
8 / 16 / 32 / 64
```

整数 arithmetic 必须使用 explicit wrapping semantics。

示例：

```text
IADD width=32

0xffff_ffff + 1
=
0x0000_0000
```

不得依赖 Rust debug/release overflow 行为。

`ICMP` 推荐：

```text
ICMP dst, lhs, rhs, width, predicate
```

predicate：

```text
EQ NE
SLT SLE SGT SGE
ULT ULE UGT UGE
```

必须区分 `LSHR` 与 `ASHR`。

必须冻结：

- shift amount 超范围行为
- division by zero
- signed min / -1 overflow
- remainder semantics

---

## 9. VM03-03 — Integer / Float Conversions

整数转换：

```text
TRUNC
ZEXT
SEXT
```

不能继续统一表达成 MOVE。

machine float：

```text
FCONST
FADD
FSUB
FMUL
FDIV
FNEG
FCMP
```

至少表达：

```text
F32
F64
```

整数/浮点转换：

```text
SITOFP
UITOFP
FPTOSI
FPTOUI
FPEXT
FPTRUNC
```

必须定义：

- NaN
- infinity
- out-of-range
- truncation

行为。

---

## 10. VM03-04 — Address + LinearMemory

定义：

```text
VmAddress = u64
0 = NULL
```

推荐：

```rust
Address(u64)
```

禁止使用 host pointer 作为 bytecode address。

新增：

```rust
struct LinearMemory {
    bytes: Vec<u8>,
    ...
}
```

Machine C object 全部进入 LinearMemory：

- globals
- arrays
- structs
- local stack objects
- C strings
- future heap allocations

动态对象继续由现有动态 object system 管理。

逻辑地址空间：

```text
0
│
├── NULL / invalid guard
├── RODATA
├── DATA
├── BSS
├── HEAP
├── free space
└── STACK
```

固定：

```text
little endian
```

推荐 natural alignment：

```text
i8      1
i16     2
i32     4
i64     8
f32     4
f64     8
address 8
```

第一版允许 unaligned access。

---

## 11. VM03-05 — Typed LOAD / STORE

推荐统一 opcode：

```text
LOAD  dst, address, type
STORE address, src, type
```

type：

```text
I8
I16
I32
I64
F32
F64
ADDR
```

LOAD 不负责 signed extension。

正确模型：

```text
LOAD I8
SEXT 8 -> 32
```

或：

```text
LOAD I8
ZEXT 8 -> 32
```

STORE 只保存指定宽度低位 bits。

测试必须覆盖：

- every integer width
- float load/store
- address load/store
- unaligned access
- OOB read/write
- RODATA write rejection

---

## 12. VM03-06 — Machine Stack / Frame

复用现有 call frame，不重写：

- call stack
- return handling
- register array
- scheduler frame model

在现有 frame 上增加 machine metadata，例如：

```rust
machine_frame_base: Option<VmAddress>,
machine_frame_size: u64,
```

新增：

```text
FRAME_ADDR dst, offset
```

语义：

```text
dst = current_machine_frame_base + offset
```

CALL：

```text
allocate machine frame
→ set frame base
→ execute callee
```

RETURN：

```text
release frame allocation
→ restore caller
```

Scheduler 长期推荐：

```text
shared:
    RODATA
    DATA
    BSS
    HEAP

per-task:
    machine stack
```

如果 0.3 不做 per-task machine stack，则明确限制 machine-stack 程序为 single-task execution，并让 runtime/verifier 明确拒绝不支持场景。

---

## 13. VM03-07 — DATA / BSS / RODATA

现有 dynamic globals 保留。

新增 machine globals：

```text
Dynamic global:
GlobalId -> Value

Machine global:
symbol -> LinearMemory address -> raw bytes
```

至少支持：

```text
RODATA
DATA
BSS
```

segment metadata：

```text
alignment
size
initial bytes
permissions
```

权限：

```text
RODATA: readable, not writable
DATA:   readable, writable, initialized
BSS:    readable, writable, zero initialized
```

测试：

- initialized i32 global
- zero initialized global
- constant byte array
- null-terminated byte string
- RODATA write trap
- alignment

---

## 14. VM03-08 — MEMCPY / MEMMOVE / MEMSET

新增：

```text
MEMCPY
MEMMOVE
MEMSET
```

不要依赖 libc。

要求：

- bounds checked
- zero-size 正确
- MEMMOVE overlap-safe
- MEMSET byte fill semantics
- invalid range 产生 VM trap

---

## 15. VM03-09 — Function Machine ABI Metadata

现有 Function representation 增加 machine metadata。

至少：

```text
machine_frame_size
```

可选：

```text
machine_param_count
machine_return_type
```

禁止加入 LLVM-specific：

```text
LLVM calling convention id
SelectionDAG metadata
MVT enum
```

0.3 scalar ABI 推荐：

```text
up to 8 scalar arguments
single scalar return
```

scalar：

```text
MachineInt
MachineFloat
Address
```

优先复用现有 register passing。

超过限制：

```text
verifier reject
```

0.3 不实现 stack arguments。

---

## 16. VM03-10 — Symbols / Relocations

至少支持：

```text
function symbol
global/data symbol
```

relocation 初版可只支持：

```text
ABS64
```

以及必要的 function reference。

loader 流程：

```text
load artifact
→ allocate segments
→ resolve symbols
→ patch relocations
```

无需模拟 native linker。

测试：

- global address
- pointer-to-global initializer
- data symbol reference
- function symbol reference
- undefined symbol rejection
- duplicate symbol rejection

---

## 17. VM03-11 — cdbc 0.3 Serialization

Program 逻辑增加：

```text
existing dynamic fields
+
machine constants
data segments
symbols
relocations
machine function metadata
machine opcodes
```

原则：

- dynamic constants 不被 machine integer semantics 污染
- machine integer serialization 必须精确
- Address 不保存 host address
- 0.3 reader 严格检查 version

兼容性：

```text
0.2 artifact -> 0.3 runtime: supported
0.3 artifact -> 0.2 runtime: clean version rejection
```

---

## 18. VM03-12 — Verifier + Trap Model

至少定义：

```text
MemoryOutOfBounds
NullPointerAccess
WriteToReadOnlyMemory
StackOverflow

IntegerDivisionByZero

InvalidAddress
InvalidConversion

InvalidInstruction
InvalidOperand
```

Rust panic 不能作为 VM trap semantics。

Verifier 增加：

```text
opcode validity
machine type validity
integer width validity
register validity

LOAD/STORE type validity
data segment validity
segment overlap

frame metadata validity
FRAME_ADDR validity

symbol validity
relocation validity

call ABI validity
```

例如：

```text
IADD width=17
```

必须 load-time verifier reject。

---

## 19. VM03-13 — Debugger / Disassembler

至少支持 machine value 展示：

```text
MachineInt:
    decimal
    hex

Address:
    hex

MachineFloat:
    decimal
```

推荐增加：

```text
memory examine
frame base
frame size
```

memory examine 可以不是最终 release blocker，但 machine value formatting 应完成。

---

## 20. VM03-14 — JIT Fallback Policy

0.3 不要求新 machine opcode 全部 JIT。

默认策略：

```text
function contains unsupported 0.3 machine opcode
        ↓
JIT admission fails
        ↓
interpreter fallback
```

禁止：

```text
unsupported machine opcode
→ wrong JIT codegen
```

现有 dynamic JIT regression 必须保持通过。

---

## 21. VM03-15 — Machine Integration Suite

最终验证不能依赖 LLVM。

使用 hand-written cdbc / bytecode builder / Rust fixture 构建等价程序：

```c
struct Point {
    int x;
    int y;
};

int sum(int *a, int n)
{
    int s = 0;

    for (int i = 0; i < n; ++i)
        s += a[i];

    return s;
}

int main(void)
{
    int a[4] = {1, 2, 3, 4};

    struct Point p;
    p.x = sum(a, 4);
    p.y = 2;

    return p.x + p.y;
}
```

必须仅依赖：

```text
MachineInt
Address
LinearMemory
FRAME_ADDR
LOAD
STORE
integer arithmetic
branch
CALL
RETURN
```

期望：

```text
result = 12
```

额外测试：

```text
integer_wrapping
signed_unsigned_compare
bitwise
integer_casts
float_integer_casts

stack_local
nested_frames
recursive_frame
stack_overflow

array_memory
struct_layout
global_data
bss
rodata_string

memcpy
memmove
memset

call_args
call_return
address_pass
```

---

## 22. VM03-16 — ABI Freeze / LLVM-Ready Gate

Status: accepted on 2026-09-08 for the Rust VM machine-artifact line. The
formal decision is recorded in
[`docs/decisions/cdbc-0.3-machine-abi-001.md`](decisions/cdbc-0.3-machine-abi-001.md).
This status does not switch the C++ compiler from `cdbc 0.2`.

只有满足以下条件才冻结 ABI：

```text
cdbc 0.3 round-trip PASS

0.2 compatibility PASS

dynamic-value regression PASS

machine integration suite PASS

verifier malformed-input suite PASS

no Rust panic for paths covered by spec

integer semantics frozen

address semantics frozen

memory layout contract frozen

LOAD/STORE semantics frozen

stack/frame semantics frozen

data segment semantics frozen

function machine ABI frozen

JIT fallback behavior defined

no LLVM dependency
```

最终发布标记：

```text
cdbc 0.3
Machine Foundation ABI Frozen
LLVM Ready
```

---

## 23. 主要文件改动区域

预计主要涉及：

```text
vm-rs/src/value.rs
```

负责：

- MachineInt
- MachineFloat
- Address

```text
vm-rs/src/bytecode.rs
```

负责：

- machine opcode
- operand type
- data segments
- machine function metadata
- symbols / relocations

```text
vm-rs/src/vm.rs
```

负责：

- machine opcode execution
- LinearMemory integration
- LOAD/STORE
- arithmetic
- traps
- memory ops

```text
vm-rs/src/scheduler.rs
```

负责：

- existing frame reuse
- machine frame base/size
- machine stack ownership
- task interaction policy

```text
vm-rs/src/format.rs
```

负责：

- cdbc 0.3 version
- serialization
- deserialization
- 0.2 compatibility

Verifier 相关文件负责：

- opcode verification
- data segment validation
- memory operand validation
- machine ABI validation
- relocation validation

Debugger / formatting 相关文件负责：

- MachineInt formatting
- Address formatting
- optional memory examine

JIT 相关文件负责：

- machine opcode admission rejection
- interpreter fallback

0.3 第一阶段禁止顺手实现完整 machine JIT lowering。

---

## 24. 推荐 PR 顺序

```text
PR01  cdbc 0.3 machine ABI/spec
PR02  MachineInt / MachineFloat / Address values
PR03  integer arithmetic
PR04  bitwise / signed / unsigned compare
PR05  integer conversions
PR06  float conversions
PR07  LinearMemory
PR08  typed LOAD / STORE
PR09  memory traps
PR10  machine stack/frame + FRAME_ADDR
PR11  DATA/BSS/RODATA
PR12  MEMCPY/MEMMOVE/MEMSET
PR13  function machine metadata / scalar ABI
PR14  symbol/relocation support
PR15  cdbc 0.3 serializer/deserializer
PR16  verifier integration
PR17  debugger/disassembler machine display
PR18  JIT fallback policy
PR19  machine integration suite
PR20  ABI freeze / 0.3 release
```

一个 PR 只处理一个主要语义层。

---

## 25. Agent 执行规则

每个 milestone 必须：

```text
1. Read the existing implementation.
2. Update the 0.3 spec before or with semantic changes.
3. Add failing tests first.
4. Implement the smallest semantic unit.
5. Run targeted tests.
6. Run all vm-rs regression tests.
7. Run cdbc format tests.
8. Run verifier tests.
9. Confirm dynamic VM behavior is unchanged.
10. Report files changed and architecture decisions.
11. Stop if acceptance criteria are not met.
```

禁止：

```text
change semantics without updating spec

fix tests by changing unrelated dynamic behavior

introduce LLVM-specific concepts

add host pointers into bytecode

depend on Rust panic semantics

reinterpret Number(f64) as exact C integer

map C arrays/structs to dynamic Array/Struct
```

---

## 26. Agent Milestone Report Format

```text
Milestone:
Status:

Files changed:
- ...

Spec changes:
- ...

Architecture decisions:
- ...

Opcodes added/changed:
- ...

Artifact changes:
- ...

Tests added:
- ...

Commands executed:
- ...

Tests passing:
- ...

Tests failing:
- ...

Backward compatibility:
- ...

Known limitations:
- ...

Blockers:
- ...

Next milestone:
- ...
```

若当前 milestone 未满足 acceptance criteria：

```text
STOP
```

不得继续下一阶段。

---

## 27. AI Agent Orchestrator Prompt

```text
You are implementing cdbc 0.3 Machine Foundation in:

https://github.com/junhui-he-cn/cd-compiler

Primary implementation area:
vm-rs

The goal of cdbc 0.3 is NOT to integrate LLVM or Clang.

The goal is to extend the existing dynamic CD VM with a low-level machine
execution layer capable of representing the semantics required later by a
basic C/LLVM backend.

The existing dynamic VM architecture must remain intact.

Architecture:

CD VM
├── existing dynamic layer
└── new machine layer
    ├── exact integer
    ├── machine float
    ├── VM address
    ├── linear memory
    ├── machine stack/frame
    ├── typed load/store
    ├── DATA/BSS/RODATA
    └── scalar call ABI metadata

Hard constraints:

1. Do not modify cd-llvm.
2. Do not add LLVM or Clang dependencies.
3. Do not replace the existing dynamic Value model.
4. Add machine values alongside existing dynamic values.
5. Do not represent exact C integers using Number(f64).
6. Do not use host pointers as bytecode addresses.
7. VM addresses are deterministic VM virtual addresses.
8. C objects live in byte-addressable LinearMemory.
9. Do not map C arrays, structs, or strings to dynamic Array/Struct/String.
10. Preserve existing dynamic globals; machine globals use data segments.
11. Reuse the existing call stack/frame/register infrastructure.
12. Do not rewrite scheduler/frame architecture unless strictly necessary.
13. New machine opcodes may initially be interpreter-only.
14. Unsupported machine opcodes must cause JIT admission fallback.
15. Rust panics are not valid VM trap semantics.
16. Every semantic change must be reflected in the cdbc 0.3 specification.
17. Preserve cdbc 0.2 read compatibility where practical.
18. Do not silently alter cdbc 0.2 wire semantics.
19. Stop after every milestone and report results.
20. Do not proceed when acceptance criteria fail.

Implementation order:

VM03-00 spec/ABI
VM03-01 MachineInt/MachineFloat/Address
VM03-02 integer arithmetic/bitwise/compare
VM03-03 integer/float conversions
VM03-04 LinearMemory
VM03-05 typed LOAD/STORE
VM03-06 machine stack/frame
VM03-07 DATA/BSS/RODATA
VM03-08 MEMCPY/MEMMOVE/MEMSET
VM03-09 function machine ABI metadata
VM03-10 symbols/relocations
VM03-11 cdbc 0.3 serialization
VM03-12 verifier/trap integration
VM03-13 debugger/disassembler support
VM03-14 JIT fallback
VM03-15 machine integration suite
VM03-16 ABI freeze

The final 0.3 release gate must NOT depend on LLVM.

Construct a VM-level program equivalent to:

struct Point {
    int x;
    int y;
};

int sum(int *a, int n)
{
    int s = 0;
    for (int i = 0; i < n; ++i)
        s += a[i];
    return s;
}

int main(void)
{
    int a[4] = {1, 2, 3, 4};
    struct Point p;
    p.x = sum(a, 4);
    p.y = 2;
    return p.x + p.y;
}

The hand-built cdbc 0.3 / VM test must return 12 using only:

- MachineInt
- Address
- LinearMemory
- FRAME_ADDR
- LOAD
- STORE
- integer arithmetic
- branches
- CALL
- RETURN

When this integration test passes together with the full compatibility,
verifier, serializer, and dynamic regression suites, the cdbc 0.3 Machine
Foundation ABI can be frozen and declared LLVM-ready.
```

---

## 28. 0.3 Release Definition

```text
cdbc 0.3
C Machine Foundation

Adds:
- exact machine integers
- machine floats
- VM addresses
- linear byte-addressable memory
- typed load/store
- machine stack frames
- global data segments
- RODATA/DATA/BSS
- memory bulk operations
- machine function metadata
- machine symbols/relocations
- verifier/trap support

Preserves:
- existing dynamic Value semantics
- dynamic collections
- current call model
- modules
- debugger
- scheduler
- existing 0.2 programs where compatibility is supported

Does not yet add:
- LLVM
- Clang
- libc
- heap allocator
- function pointers
- varargs
- atomics
- threads
```

0.3 的结束条件不是“能编译 C”，而是：

> VM machine ABI 足够稳定，使 LLVM/Clang 后端可以在下一阶段只做 lowering，而不需要继续修改 VM 的核心语义。
