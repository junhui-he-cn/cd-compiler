# Compiler Design 0.2 到 0.3 升级指南

本文说明 Compiler Design 0.3.0 相对于 0.2.0 的变化，以及现有程序、
`.cdbc` 工件和 VM 集成需要如何处理。

## 先看结论

0.3 是一次 **VM machine foundation** 发布，不是把 C++ 编译器默认输出
切换到新工件格式的发布：

| 项目 | 0.2 | 0.3 |
| --- | --- | --- |
| 仓库/VM crate 版本 | `0.2.0` | `0.3.0` |
| C++ 编译器默认 `.cdbc` | `cdbc 0.2` | 仍为 `cdbc 0.2` |
| Rust VM 可读取 | `cdbc 0.2` | `cdbc 0.2` 和显式 `cdbc 0.3` |
| 动态语言语义 | 0.2 语义 | 保持不变 |
| VM library API facade | `0.2` | 仍为 `0.2`，增加能力为 additive |
| 旧版基线 | 无专用备份分支 | `0.2` 分支保留 0.3 发布前基线 |

因此，普通源程序不需要因为 0.3 发布而修改。只有要生成或执行 machine
artifact 的调用方，才需要采用下面新增的 0.3 API 和数据模型。

## 1. 新增的 machine 能力

0.3 在现有动态 VM 旁边增加 machine layer；它不替换动态 `Value`，也不
把 C 对象伪装成动态数组、结构体或字符串。

### 1.1 精确 machine 值

- `MachineInt`：保存 64 位原始 bit，操作时明确指定 `8/16/32/64` 位宽和
  signedness。
- `MachineFloat`：支持 F32/F64，遵循 IEEE 表示、舍入、NaN、无穷大和有符号零
 规则。
- `Address`：VM 自己的确定性虚拟地址，序列化时绝不使用 host pointer。
- machine 值与动态 `Number`、`Bool` 等之间没有隐式转换，必须通过明确的
  machine conversion 指令完成。

### 1.2 整数与浮点运算

新增并验证了 machine 整数的加减乘、带符号/无符号除法和余数、位运算、
移位、比较与按位取反。每条操作都按显式位宽执行掩码和溢出语义。

同时支持：

- 整数位宽扩展、截断和符号解释；
- 整数与 F32/F64 的转换；
- F32/F64 运算、比较和宽度转换；
- NaN、无穷大、越界、非法转换的 typed trap。

这些操作不会改变动态语言中基于 `f64` 的 `number` 语义。

### 1.3 线性内存与类型化访问

新增 VM-owned、byte-addressable 的 `LinearMemory`：

- 小端布局；
- 明确的 region 地址和权限；
- null guard、越界、地址溢出和跨 region 访问检查；
- 允许经过验证的非对齐访问；
- typed `LOAD`/`STORE` 覆盖整数、浮点和地址域；
- 内存错误统一转换为 VM 的 typed runtime error。

machine 对象存放在线性内存中，不使用动态集合的别名或 GC 表示。

### 1.4 machine frame 与栈

machine 函数现在可以声明 frame 元数据：

- 每个 cooperative task 拥有独立的 machine stack；
- frame 向上增长并按 8 字节对齐；
- `FRAME_ADDR` 返回当前活动 frame 内的 VM 地址；
- 正常返回、runtime trap 和取消都会释放 frame；
- 递归调用获得不同的 frame 地址，已释放地址不可再次解引用。

### 1.5 静态数据段

支持 `RODATA`、`DATA`、`BSS` 三种 machine segment：

- 按确定性顺序分配并遵守 alignment；
- `RODATA` 只读；
- `DATA` 使用明确的字节 payload；
- `BSS` 在装载时零填充；
- 段权限和大小在执行前验证。

### 1.6 批量内存操作

新增 `MEMCPY`、`MEMMOVE`、`MEMSET`：

- 所有范围和权限先整体验证，再执行写入；
- `MEMMOVE` 支持重叠区域；
- 非法范围不会留下部分写入；
- 零长度操作不解引用地址，并保留规定的 null 行为。

### 1.7 machine 调用 ABI

0.3 冻结了 VM 级标量调用 ABI：

- 最多 8 个 machine 参数；
- 最多一个可选的 machine 标量返回值；
- 参数和返回值必须属于 `MachineInt`、`MachineFloat` 或 `Address` 域；
- 不支持栈参数、间接 machine call 或 function pointer。

调用边界、返回域、参数数量和类型错误都会在验证或运行时被拒绝。

### 1.8 符号与重定位

machine artifact 可以携带 VM 级函数/数据符号和重定位：

- `ABS64` 将数据符号解析为 VM 地址；
- `FUNC_INDEX` 将函数符号解析为 VM function-table index；
- 模块链接时会重定位函数索引和数据段基址；
- 未定义符号、重复符号、非法 addend 和越界目标在执行前拒绝。

### 1.9 工件读写、验证和诊断

Rust VM 新增显式 `cdbc 0.3` machine reader/writer，并扩展了：

- machine opcode、segment、ABI、symbol、relocation 的文本 round-trip；
- load-time verifier；
- typed arithmetic、memory、conversion、operand 和 linkage trap；
- machine-aware `dump`/disassembly；
- debugger 中的 machine registers 和 frame base/size 显示。

解析、验证、静态分配、符号解析和重定位全部完成后才会执行 machine
代码，Rust panic 不属于 VM 的错误语义。

### 1.10 JIT 行为

现有 JIT 仍只接纳原有白名单路径。包含 machine 指令或 machine ABI 函数的
程序会明确回退到解释器，不会生成猜测性的 host code。0.3 不包含 machine
JIT lowering。

## 2. 兼容性变化

### 2.1 `.cdbc` 版本边界

- 现有 `cdbc 0.2` 工件格式、opcode、section 和动态语义保持不变。
- Rust VM reader 同时接受有效的 `cdbc 0.2` 和显式 `cdbc 0.3`。
- 只支持 0.2 的 reader 遇到 `cdbc 0.3` 时，会在解析正文前报告
  `unsupported version`，不会部分解释工件。
- 不能把 0.3 machine 字段、opcode 或语义塞进 `cdbc 0.2` header。
- 默认 formatter 仍为 0.2；machine 调用方应使用
  `format_program_v03` 或 `format_artifact_v03`。

### 2.2 C++ 编译器边界

以下路径在 0.3 中仍然输出 `cdbc 0.2`：

```sh
./build/compiler_design --emit-bytecode build/program.cdbc examples/hello.cd
./build/compiler_design --emit-module-bytecode build/modules examples/main.cd
```

0.3 不会自动把普通源代码编译成 machine artifact，也不改变模块缓存的
`cdbc-cache 0.2` schema 4。未来要做 C++ machine lowering 或默认产物切换，
需要单独的 compiler-side 方案、类型映射、产物选择、C++/Rust parity 和迁移
决策。

### 2.3 library 与 crate 版本

Rust crate `compiler-design-vm` 的 package version 是 `0.3.0`。公开的
`LIBRARY_API_VERSION` 仍是 `"0.2"`，表示既有 library facade 的兼容边界，
不等同于仓库发布版本或 artifact header 版本。

## 3. 使用者迁移路径

### 路径 A：继续使用普通编译器和动态 VM

无需代码迁移。继续生成和运行 0.2 工件即可：

```sh
cmake -S . -B build
cmake --build build
./build/compiler_design --emit-bytecode build/program.cdbc examples/hello.cd
cargo run --manifest-path vm-rs/Cargo.toml -- verify build/program.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- run build/program.cdbc
```

这是当前默认、兼容性验证最完整的路径。

### 路径 B：读取已有 0.2 工件

不需要重编译工件。0.3 VM 会继续验证、`dump`、`run`、`trace`、`debug` 和
`profile` 现有 0.2 文件：

```sh
cargo run --manifest-path vm-rs/Cargo.toml -- dump old-0.2.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- run old-0.2.cdbc
```

### 路径 C：使用 0.3 machine artifact

machine artifact 目前由 Rust machine-aware API 或 hand-built integration
输入产生，而不是由 C++ compiler 默认发射：

```rust
use compiler_design_vm::{format_artifact_v03_checked, parse_artifact_checked};

// 通过 Program/Artifact 结构构造 machine fields 后：
let text = format_artifact_v03_checked(&artifact)?;
let parsed = parse_artifact_checked(&text)?;
```

生成文件后可用 VM CLI 验证和查看：

```sh
cargo run --manifest-path vm-rs/Cargo.toml -- verify machine-0.3.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- dump machine-0.3.cdbc
cargo run --manifest-path vm-rs/Cargo.toml -- run machine-0.3.cdbc
```

machine artifact 必须使用 0.3 writer；默认的 0.2 formatter 会拒绝 machine
opcode、segment、ABI、symbol 或 relocation 字段。

## 4. 不属于本次升级的内容

以下功能明确不在 0.3 Machine Foundation 范围内：

- C++ compiler machine lowering 和默认 artifact cutover；
- LLVM、Clang、SelectionDAG、libc、POSIX 和 native object file；
- `malloc`/`free` 等完整 heap allocator；
- function pointer、间接调用、varargs 和栈参数；
- atomics、threads、TLS、SIMD、exceptions、`setjmp`/`longjmp`、VLA；
- machine JIT lowering；
- binary artifact encoding 或 host ABI 承诺。

这些限制不是隐藏的兼容行为；需要扩大范围时，应新增版本/ABI 决策和对应
验证矩阵。

## 5. 0.3 发布验证

0.3 发布前的 Machine Foundation gate 已覆盖：

- Rust VM：240 个 library tests、4 个 CLI tests、2 个 legacy artifact tests、
  2 个 workload tests、7 个 lifetime tests、13 个 library API tests；
- CTest：`47/47`；
- golden tests：`875/875`；
- bytecode artifact tests：`124/124`；
- Rust VM golden tests：`776/776`；
- canonical verification：`1956/1956`；
- malformed corpus：`116/116`；
- boundary tests：`4/4`。

手工 machine integration program 会经过 0.3 formatter、parser、verifier、
frame-backed memory 和 cooperative execution，返回 `MachineInt(12)`。

## 6. 相关文档

- [0.3 Machine Foundation ABI 决策](decisions/cdbc-0.3-machine-abi-001.md)
- [0.3 Machine Foundation 规范](cdbc-0.3-machine-foundation.md)
- [VM 0.3 路线图](vm-roadmap.md)
- [字节码指令参考（中文）](bytecode-instructions-zh.md)
- [版本与分支策略](versioning.md)
- [0.2 备份分支](https://github.com/junhui-he-cn/cd-compiler/tree/0.2)

