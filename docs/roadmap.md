# Compiler Design Roadmap

Current-state record for the compiler, language, and compiler tools. Language
0.2 follows the staged execution plan and its accepted decision records. The
Rust runtime has a separate current-state record in
[`docs/vm-roadmap.md`](vm-roadmap.md).

## Compatibility contracts

`cdbc 0.2` emission and execution only, per-file module compilation with CLI
entry order, O0 as the default optimizer level, source fallback for cold or
repairable module-product builds, C++/Rust parity, and interpreter-default VM
execution remain unchanged until an explicit decision changes them.

## Current shipped baseline

| Area | Shipped baseline |
| --- | --- |
| Verification | Versioned inventory, canonical runner, boundary and malformed corpora |
| Front end | Typed source identities, lossless source, declaration/semantic indexes, import-aware module graph |
| Language | Functions, closures, generics and constraints, `optional<T>`/`T?`, trailing commas, numeric separators/exponents, line/block comments, half-open `..<` ranges, legacy C-style `for`, enums/patterns, named and recursive structs, collection semantics, `Eq`/`Ord`/`Hash` capabilities |
| Modules | Public interfaces, `.cdi`, independent module products, linker inputs, `cdbc-cache 0.2`, strict and fallback modes |
| IR and optimization | Linear register IR plus verified CFG/SSA/de-SSA and explicit `--opt-level 1` |
| Tools | Formatter, open/closed-workspace LSP definition and references, trace and interactive VM debugger |
| Artifact boundary | Compiler emits validated linked and module `cdbc 0.2` products with debug metadata; the VM accepts `cdbc 0.2` only |

The authoritative implementation contracts remain in `README.md`,
`docs/language-grammar.ebnf`, and `docs/bytecode-text-format.md`; the full
repository verification gate lives in `AGENTS.md`.

## Active language decisions

C-style `for` remains accepted as a legacy compatibility form. New code should
prefer `for-in` over arrays, maps, and ranges. The compiler does not emit an
implicit deprecation warning because it has no warning diagnostic channel;
removal or warning behavior requires a separate decision.

Phase 19 is decided: `Eq`, `Ord`, and `Hash` remain built-in compile-time
generic constraints. They are erased before runtime, user-defined capability
implementations and trait objects remain out of scope, and structs do not
satisfy `Ord` after struct ordering operators were removed. See
[`language-capability-001`](decisions/language-capability-001.md).
