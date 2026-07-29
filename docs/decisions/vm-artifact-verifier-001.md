# VM-ARTIFACT-VERIFIER-001：执行前 artifact verifier 边界

Status: implemented as the first VM roadmap slice on 2026-07-29.

## Decision

保留 `cdbc 0.1` 的现有解析和拒绝行为，同时把已存在的结构检查暴露为可复用
的 `format::verify_artifact`、`format::verify_program` 和
`format::verify_module_artifact` 边界。解析路径继续在返回 artifact 前验证，
CLI 的 `dump`、`run`、`trace`、`link` 读取路径显式复验，Rust linker 对直接传入
内存的 module products 复验每个模块，并对最终 linked program 再验证一次。

这保证了 verifier 不依赖“artifact 一定来自 parser”的假设：任何进入执行、
link 或调试路径的 `Program`/`ModuleArtifact` 都必须经过同一套 cross-reference、
debug metadata、native capability 和 module-envelope 检查。

## 保持不变的契约

- 只接受 `cdbc 0.1`；不增加 opcode、value layout、版本或新的 wire section；
- 合法 C++ emitter 输出、Rust canonical dump、module link、run 和 trace 输出
  保持不变；
- parser/verification error 继续使用当前 `ParseError` 文本和非零 CLI 结果；
- `cdbc 0.1` metadata-free artifact 和现有 debug-source 兼容路径保留。

## 本切片覆盖

- constants、names、functions、registers、jump targets 和 native names 的
  cross-reference；
- function index/arity、debug location/range 与 source table 的一致性；
- module identity/path、entry order、dependency identity、插入点和顺序；
- linker 输入 module 与 linker 输出 program 的执行前复验；
- 直接构造的非法内存 artifact 在 link/verify API 处被拒绝，而不是静默生成
  一个可执行程序。

## 明确后续项

本切片不引入寄存器 definite-initialization/dataflow 规则、资源预算、取消、
fuzzing 框架、错误 kind 重设计或 successor artifact version。这些分别进入
VM-1B、VM-1C 或独立决策；加入新拒绝条件前必须先补 compatibility corpus。

## 验证

- Rust 单测覆盖 public verifier 的 invalid in-memory program；
- Rust linker 单测覆盖空 module identity 的 direct input rejection；
- 保持现有 `cdbc 0.1` artifact、module artifact、module cache、debugger 和
  Rust VM golden parity；
- 完整 gate 使用 `cargo test`、CTest、canonical verification、artifact/module
  tests、malformed corpus、debugger tests 和 `git diff --check`。

## 删除条件

不删除 parser 内的兼容验证，也不允许 CLI 绕过 verifier。只有在下一阶段有
独立的 verifier module、稳定错误分类和完整 malformed/fuzz 证据后，才可以
合并重复的局部防御检查。
