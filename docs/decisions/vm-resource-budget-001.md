# VM-RESOURCE-BUDGET-001：确定性资源预算与取消边界

Status: implemented and verified on 2026-07-29.

## Decision

Rust VM 为每次执行创建一个 `RunConfig`。默认配置保护现有 `.cdbc 0.1`
workload，同时允许宿主或 CLI 显式覆盖；单项配置为 `0` 时表示关闭该项，
`--unlimited` 显式关闭所有预算。预算触发时返回稳定的 resource error，VM
不 panic、不返回部分成功输出。

默认值如下：

| 预算 | 默认值 | 计数边界 |
| --- | ---: | --- |
| instruction steps | 10,000,000 | 每条 bytecode 指令和每个可能长时间运行的 native 迭代检查点各计一次；达到上限后拒绝下一步 |
| call depth | 1,024 | 只计算活跃用户函数/callback frame，main 不计入；达到上限后拒绝下一次调用 |
| runtime elements | 1,000,000 | 累计分配单位：aggregate/function object 加 immediate slots；当前 VM 无回收，因此不回退计数 |
| output bytes | 16 MiB | `print` 写入的 UTF-8 bytes，包含换行；写入会使总数超过上限时失败 |
| artifact bytes | 64 MiB | 每个输入 `.cdbc` 文件和最终 linked text artifact |
| module count | 1,024 | `link` 读取的 module products 数量 |
| module instructions | 1,000,000 | `link` 展开前所有 module body 的 main/function instruction 总数，以及最终 linked program |

`RunConfig` 的 cancellation token 是 cooperative 的：指令循环、native
callback 迭代和容器增长点检查 token，已取消时返回 `execution cancelled`。
CLI 不捕获操作系统信号；宿主调用者负责持有和触发 token。

## Implementation boundary

- `vm::RunConfig`、`vm::CancellationToken` 和 `RuntimeErrorKind` 是 Rust VM
  的执行边界；CLI 的 `dump`、`run`、`trace` 和 `link` 都使用同一套配置解析
  和输入大小检查。
- 指令步数、调用深度、runtime elements 和输出 bytes 在执行状态中累计；native
  callback、字符串/集合增长、map 插入、variant/range/function/aggregate
  创建都在可能增长前检查预算。
- artifact bytes、module count 和 module instructions 在 parser/linker 输入
  和最终 linked program 写出前检查。预算触发返回稳定错误，不截断成功输出。
- 取消仍是 cooperative API；CLI 不安装信号处理器，也不承诺暂停/恢复或跨线程
  调度。

## Error contract

- 资源错误使用 `RuntimeErrorKind::Resource`，显示为
  `Runtime error: resource limit exceeded: <kind> (limit <n>)`；带有源码位置
  时沿用现有位置和调用栈渲染。
- 取消使用 `RuntimeErrorKind::Cancelled`，显示为
  `Runtime error: execution cancelled`。
- artifact/module 输入预算属于 CLI 输入错误路径，使用同样的
  `resource limit exceeded: ...` 文本并返回非零结果；拒绝前不产生 stdout。
- `run` 失败时不打印此前已经积累的 VM output；`trace` 可以保留此前已经
  产生的 trace events，再以稳定 runtime error 结束。

## Non-goals

本切片不实现实时 wall-clock timeout、操作系统信号处理、线程调度、GC、精确
heap byte accounting 或 persistent session。累计 runtime element 预算是
增长防线，不是所有权模型；所有权、alias 和回收策略留给 VM-2A/VM-2B。

## Verification cases

- `VM1B-STEPS-001`: loop/callback 在固定 step limit 下确定性失败；limit 为
  0 时关闭该预算。
- `VM1B-CALL-001`: nested function/callback 超过 call-depth limit 时失败，
  main frame 不占深度。
- `VM1B-ELEMENTS-001`: array/map/function/push/map-insert/variant/range 的
  累计 allocation units 在增长前失败，不留下越界对象。
- `VM1B-OUTPUT-001`: 多次 print 在 UTF-8 byte boundary 上稳定失败，失败时
  CLI stdout 为空。
- `VM1B-ARTIFACT-001`: dump/run/trace/link 在读取过大 artifact 前拒绝。
- `VM1B-MODULE-001`: link 在 module count/instruction expansion 超限前拒绝。
- `VM1B-CANCEL-001`: 预取消和执行检查点返回同一 cancellation kind，不污染
  后续 VM 实例。

Focused Rust and CLI tests must retain existing C++ emission, linked/module,
debug metadata, native callback, and Rust VM golden parity.

## Verification record

2026-07-29：Rust `59/59`、CTest `34/34`、VM resource budget focused test、
artifact `118/118`、module cache `11/11`、Rust VM `778/778`、malformed `108/108`
和 canonical verification `1884/1884` 全部通过。`git diff --check` 也必须在
提交前保持通过；测试生成的 `tests/__pycache__/` 不属于交付物。
