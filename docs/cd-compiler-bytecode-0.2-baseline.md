# `.cdbc 0.1 → 0.2` 重构基线（Phase 0）

本文件是 `docs/cd-compiler-bytecode-0.2-execution-plan.md` 的 Phase 0 交付物：在
修改 Bytecode ISA 之前，锁定当前 `.cdbc 0.1` 的行为，记录真实代码路径、文档与实现
的不一致，并给出后续 Phase 的回归基线与版本协调清单。

## 状态

- 基准分支：`feat/bytecode-0.2-phase0`，起点 `master` = `origin/master` = `e502b4ea`。
- 本阶段**没有修改 Bytecode ISA**，`VERSION` 保持 `0.1.0`，`.cdbc 0.1` 契约不变。
- 新增内容：5 个 golden fixture、1 个 C++ 发射端测试、1 个 Rust VM 集成测试、本
  文档。后续所有 Phase 以这些测试为回归基线。

## 1. 实际代码路径清单

| 文件 | 职责与关键事实 |
| --- | --- |
| `include/IR.hpp` / `src/IR.cpp` | `IROp` 枚举（36 个 opcode）、`IRInstruction`/`IRFunction`/`IRProgram`；IR 打印器用 `Value` 的 ostream 格式化数字 |
| `src/IRCompiler.cpp`（1666 行） | AST → IR 唯一 lowering 入口；变量名、`make_function`、`assert_array`、`native_call`、跳转偏移都产生于此处 |
| `include/Bytecode.hpp` / `src/Bytecode.cpp` | `BytecodeProgram`（`constants_`/`names_`/`instructions_`/`registerCount_`/`functions_`）；`--bytecode` 调试打印（`bN` 寄存器、`#N` 常量、`@N` 名字） |
| `src/BytecodeCompiler.cpp` | IR → `BytecodeProgram` 的 1:1 映射，不改语义 |
| `src/BytecodeTextEmitter.cpp` | `.cdbc 0.1` 文本、module envelope、debug 三节；`numberText` 用 `std::setprecision(15)`（Phase 14 目标） |
| `vm-rs/src/bytecode.rs` | `Program { constants, names, main, functions, debug_sources }`：`main` 仍是特殊 `FunctionBody`；`Function` 是线性指令表 + 绝对指令偏移跳转 |
| `vm-rs/src/format.rs` | 文本 parser/formatter/verifier；`ARTIFACT_HEADER = "cdbc 0.1"`；`SUPPORTED_NATIVE_FUNCTIONS` 固定 29 名单；verifier 只做结构/索引/常量有限性/名字/函数表/native 名单/debug 元数据检查 |
| `vm-rs/src/runtime.rs` | 运行时值：`StructValue` 字段是 `Vec<(String, Value)>`、`VariantValue` 是 `enum_name`+`variant_name` 字符串；`normalize_map_entries` 是 map 去重语义的唯一实现 |
| `vm-rs/src/vm.rs`（10516 行） | 执行主循环；`prepare_function` 在 VM 侧扫描 `store_var` 建立 `slot_by_name`；`capture_environment` 复制整个名字环境；main 帧 `is_main=true`、`variable_plan=None`、closure 为空；globals 是名字键 `SharedEnvironment` + name-index cache；Ord witness 按 `__capability_ord_{Type}_{cmp}` 全局名查找 |
| `vm-rs/src/link.rs` | module linker；`ModuleDependency.instruction_offset`（`at=N`）决定依赖 `main` 指令插入点，并整体重映射跳转/debug 偏移 |
| `vm-rs/src/jit.rs` | 测试专用 x86-64 JIT，默认禁用、白名单、VM 本地、interpreter fallback；不参与 `.cdbc` 产物 |

## 2. 当前 0.1 可观察行为契约（与 0.2 目标对照）

| 维度 | 0.1 现状（本基线锁定） | 0.2 目标 |
| --- | --- | --- |
| 变量 | `load_var/store_var/assign_var nN`；VM `prepare_function` 扫描 `store_var` 建名字→slot；非 main 帧先 local slot、再名字键 closure、再 globals；main 帧全走 globals + name-index cache | `load_local/bind_local/set_local/load_upvalue/set_upvalue/load_global/init_global/set_global`，slot 全部由编译器分配（Phase 2） |
| 闭包 | `capture_environment` 复制帧内所有名字 cell；顶层闭包捕获空环境、调用时回退读会话 globals。循环内 `let x` 的三个闭包都读同一顶层 cell → **20 20 20** | 显式 `upvalues`，只捕获实际 free variable，循环每次迭代独立 binding → 0 10 20（Phase 3） |
| 返回 | 执行到指令末尾隐式返回 `nil`；`jump` 目标允许等于指令数（落到末尾） | 显式 `return_nil`，无隐式 fallthrough（Phase 4） |
| 控制流 | 线性指令表 + 绝对指令偏移 `jump/jump_if_false/jump_if_true` | `BasicBlock` + `br/br_if` + terminator（Phase 4） |
| 寄存器 | 未验证路径寄存器初始为 `Nil`，读未初始化寄存器不报错 | verifier definite-assignment，读未定义寄存器在执行前拒绝（Phase 5） |
| 变量绑定 | 未定义名字在运行时报 `undefined variable`，verifier 不检查 | verifier definite-binding（Phase 5） |
| struct | 字段 `Vec<(String, Value)>` 线性查找；`type_name: Option<String>` | `TypeId` + `FieldSlot`，`make_struct tN` / `struct_get tN, slot`（Phase 6） |
| enum | 身份 = `enum_name` + `variant_name` 字符串 | `TypeId` + `VariantId`（Phase 6） |
| native | `native_call nN`；VM 构造时按 name-index 解析固定 29 名 `native_specs`，热路径已索引化但 artifact 仍带名字 | `call_native iN` + import 表，`print` 移出核心 ISA（Phase 7） |
| Ord | struct 比较在 VM 内按 `__capability_ord_{Type}_{cmp}`（含局部名 fallback）全局名查找 witness | TypeChecker/lowering 直接生成 witness call（Phase 12.3/8） |
| 集合 | 通用 `index/assign_index/len/assert_array` 按 array/map/range 动态分派；`assert_array` 实际支持 array/range/map（名字误导）；`print` 是核心 opcode | `array_get/map_get/range_get`、`iter_init/iter_next`（Phase 9/10） |
| map | `normalize_map_entries` 去重：**保留首次位置、后写覆盖**；`map[key]=value` 原位替换或追加。与 0.2 目标语义已一致 | 唯一键、稳定插入序、last write wins、更新不移动位置（Phase 11 只做文档/测试统一） |
| f64 | `src/Value.cpp:349` 与 `src/BytecodeTextEmitter.cpp:43` 都用 `std::setprecision(15)`：`0.30000000000000004` → `0.3`、`1.0000000000000002` → `1` | `max_digits10`，文本 round-trip bitwise 相等（Phase 14） |
| 入口 | `Program.main` 是特殊字段 | `Program { functions, entry: FuncId }`（Phase 1） |
| module | `ModuleDependency.instruction_offset`（`at=N`）+ linker 插入依赖指令、修复偏移 | `module` 表 + `init_module mN` + 运行时初始化状态（Phase 12） |

## 3. 发现的文档/实现不一致

1. **map 重复键**：`docs/bytecode-instructions-zh.md:213-214` 写“重复键允许并存，
   身份相等时后写键对应条目在后”；实现（`vm-rs/src/runtime.rs::normalize_map_entries`
   、`execute_assign_index`）和 `USER_MANUAL.md:705` 都是“去重、保留首次位置、后写
   覆盖、更新不移动位置”，且 `tests/golden/maps` 已锁定实现行为。计划 §15（Phase 11）
   应统一文档并转正这一语义。
2. **f64 文本精度**：发射端 15 位有效数字无法表达 17 位有效数字的 double；本阶段用
   `tests/bytecode_emitter_f64_roundtrip_tests.cpp` 锁定“当前有损”，Phase 14 改为
   `max_digits10` 后需同步刷新该测试与 `tests/golden/f64_roundtrip`。
3. **`assert_array` 命名误导**：实际接受 array/range/map，本质是 `prepare_for_iteration`
   （计划 §14 已确认，Phase 10 替换为 iterator protocol）。
4. **Ord witness 字符串约定**：VM 仍在运行时按命名约定查找 struct 比较 witness，
   属于“VM 承担 compiler lowering”的遗留，与 `USER_MANUAL.md:301` 的“struct 不参与
   内置顺序比较”表层契约并存；计划 §12.3 要求移除。

## 4. 基线测试覆盖映射

计划 §4.2 清单逐项映射到测试位置（“新增”为本阶段产物）：

| 计划项 | 覆盖位置 |
| --- | --- |
| local variable / assignment | `tests/golden/bytecode_variables`、`assignment` |
| shadowing | `block_assignment_inner_shadow`、`closure_shadowing` |
| nested closure | `closure_nested_recursion`、新增 `closure_nested_upvalue`（A→B→C + upvalue 修改） |
| closure mutation | `closure_counter`、`closure_shared_cell`、新增 `closure_mutation`（Case A：`x=2` 后 `f()==2`） |
| loop + closure capture | 新增 `closure_loop_capture`（锁定 0.1 的 `20 20 20`） |
| global variable | `function_scope_global` |
| array / map | `array_*`、`maps` |
| duplicate map key | `maps`、新增 `map_duplicate_keys`、`vm-rs/tests/bytecode_0_2_baseline.rs` |
| struct field read/write | `structs`、`struct_field_assignment` |
| enum variant | `adt_pattern_matching`、`generic_enums` |
| native call | `native_stdlib_*` |
| arithmetic / string concat / comparison | `arithmetic`、`strings`、`member_calls_strings`、`comparison`、artifact `string_ordering` |
| function call / recursive call | `function_call_add`、`function_recursion` |
| module dependency | `adt_module_import`、`module_exports`、`tests/bytecode_module_artifact_tests.py` |
| jump | `bytecode_control_flow`、artifact `control_flow` |
| invalid bytecode verification | `tests/run_malformed_tests.py` + `malformed_cases.json`、`vm-rs/src/format.rs` 单测、新增 `bytecode_0_2_baseline.rs`（未定义变量/寄存器当前推迟到运行期） |
| debug metadata | `tests/module_debug_metadata_tests.py`、`tests/debugger_tests.py` |
| f64 round-trip | 新增 `f64_roundtrip` golden、`tests/bytecode_emitter_f64_roundtrip_tests.cpp`、`bytecode_0_2_baseline.rs`（parser 无损侧） |

## 5. VERSION 匹配与 0.2 版本协调清单

当前各版本面（Phase 0 全部保持不动）：

| 版本面 | 当前值 | 位置 |
| --- | --- | --- |
| 项目版本 | `0.1.0` | `VERSION`（`master`/`0.1` 分支）；另有 `0.1.1` 发布分支 + 不可变 tag `v0.1.1`（`182edab8`，未并入 master） |
| crate 版本 | `0.1.0` | `vm-rs/Cargo.toml` |
| 库 API 版本 | `0.1` | `vm-rs/src/lib.rs::LIBRARY_API_VERSION` |
| artifact 版本/头 | `0.1` / `cdbc 0.1` | `vm-rs/src/format.rs` |
| CLI 版本串 | `compiler-design-vm 0.1.0` | `vm-rs/src/main.rs::HELP` |
| 兼容矩阵契约 | compiler `0.1.0`、artifact `0.1`、crate `0.1.0`、api `0.1`、`cdbc-cache 0.2` schema 4、固定 native 名单 | `docs/decisions/x1-compiler-vm-compatibility-001.json` |
| 矩阵校验器 | 硬编码断言 artifact/api 必须保持 `0.1`，并断言矩阵 compiler 版本 == `VERSION` | `tests/vm_compatibility_matrix.py` |
| 库测试断言 | `LIBRARY_API_VERSION == "0.1"`、`ARTIFACT_FORMAT_VERSION == "0.1"`、错误文本含 `cdbc 0.1` | `vm-rs/tests/library_api.rs` |

`.cdbc 0.2` 真正落地时，以下内容必须在同一个 commit 内联动更新，否则
`tests/vm_compatibility_matrix.py`、`tests/cdbc_contract_audit.py` 与
`vm-rs/tests/library_api.rs` 会立刻红：

1. `VERSION` → `0.2.0`（同时按 `docs/versioning.md` 打不可变 tag `v0.2.0`）；
2. `vm-rs/src/format.rs`：`ARTIFACT_FORMAT_VERSION="0.2"`、`ARTIFACT_HEADER="cdbc 0.2"`；
3. `vm-rs/src/lib.rs`：`LIBRARY_API_VERSION`（若门面变化）；
4. `vm-rs/Cargo.toml` → `0.2.0`；
5. `vm-rs/src/main.rs` HELP 版本串；
6. `docs/decisions/x1-compiler-vm-compatibility-001.json` 的 `version_contract` 与新
   决策记录（X1 规则明确：“successor artifact version … requires a separate
   decision record”）；
7. `tests/vm_compatibility_matrix.py` 中的硬编码 `0.1` 断言；
8. `vm-rs/tests/library_api.rs` 中的版本断言与 header 错误文本；
9. 所有 `cdbc 0.1` 文档与测试 fixture 头（`docs/bytecode-*`、`tests/bytecode_artifacts/*`）；
10. `docs/versioning.md` 的 0.2 发布行说明。

Phase 0 不改任何版本号：本阶段不改变 artifact 契约，提前改 `VERSION` 会与 0.1
artifact 版本、X1 矩阵校验器及不可变 tag 语义冲突。

## 6. 后续

按计划 §25，Phase 0 完成后停止。下一阶段是 Phase 1（强类型 ID、`Program { functions,
entry }`、`Function` 元数据骨架），不提前做变量 lowering、closure、CFG 或 typed
opcode。
