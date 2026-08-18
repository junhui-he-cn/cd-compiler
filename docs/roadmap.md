# Compiler Design Roadmap

Current-state record for the compiler, language, and compiler tools. All
previously planned slices are void as of 2026-08-15: completed work lives in
decision records and Git history, and future work starts from a fresh
explicit decision. The Rust runtime has a separate current-state record in
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
| Language | Functions, closures, generics and constraints, `optional<T>`/`T?`, trailing commas, numeric separators/exponents, line/block comments, enums/patterns, named and recursive structs, collection semantics, `Eq`/`Ord`/`Hash` capabilities |
| Modules | Public interfaces, `.cdi`, independent module products, linker inputs, `cdbc-cache 0.2`, strict and fallback modes |
| IR and optimization | Linear register IR plus verified CFG/SSA/de-SSA and explicit `--opt-level 1` |
| Tools | Formatter, open/closed-workspace LSP definition and references, trace and interactive VM debugger |
| Artifact boundary | Compiler emits validated linked and module `cdbc 0.2` products with debug metadata; the VM accepts `cdbc 0.2` only |

The authoritative implementation contracts remain in `README.md`,
`docs/language-grammar.ebnf`, and `docs/bytecode-text-format.md`; the full
repository verification gate lives in `AGENTS.md`.
