# V6C: ordinary VM::run parity gate

Status: implemented on 2026-08-09 as a test-only execution slice.

## Scope

The focused Rust VM tests now enable the existing private JIT state and run a
real main body that constructs and calls an eligible ordinary function. The
tests compare that path with the interpreter for:

- successful output and instruction-step counts across repeated runs;
- VM-local compiled-entry cache reuse; and
- a runtime divide-by-zero error, including kind, message, location, stack,
  resource fields, and instruction-step count.

## Compatibility boundary

`JitState::enabled_for_tests` remains the only activation point. `VM::new`,
public run configuration, CLI behavior, observable modes, cooperative sessions,
native/callback boundaries, unsupported instructions, and `.cdbc 0.1` remain
interpreter-controlled and unchanged. This slice does not make a speedup claim
or create a public JIT option.

## Verification

The tests are restricted to x86-64 Unix hosts because they execute finalized
machine code. Other hosts retain the existing disabled/fallback behavior. The
focused tests must pass with the full Rust VM test suite; affected CTest VM and
Rust artifact parity gates remain required before any later opt-in decision.
