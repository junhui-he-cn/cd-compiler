# M2B-LEX-001: aggregate recoverable lexer diagnostics

Status: resolved and implemented.

## Decision

The lexer records independent recoverable character errors and continues
scanning until the requested boundary or end of input:

- lone `&` and otherwise unexpected single characters are consumed, recorded,
  and scanning continues so later tokens and errors remain observable;
- an unterminated string records one diagnostic and stops naturally at EOF;
- a non-empty lexer error list is raised before parser or semantic stages; and
- file-backed and direct multi-file diagnostics preserve source order, original
  paths, file-local coordinates, source snippets, carets, empty stdout, and exit
  status `1`.

The import/direct-input loader still stops before parsing after any lexer error.
This slice does not add token recovery for malformed multi-character operators,
does not reinterpret invalid characters as valid syntax, and does not change
the `scanTokensUntil(import)` source-loading probe beyond propagating errors
encountered before its boundary.

## Evidence and gate

`tests/lexer_recovery_tests.py` checks two stdin character errors, two direct
multi-file errors with path remapping, and the unchanged unterminated-string
boundary. `malformed.lexer.multiple_errors` binds the aggregate behavior to the
bounded malformed corpus.

Focused verification:

```sh
cmake -S . -B build
cmake --build build --target compiler_design
ctest --test-dir build --output-on-failure -R '^lexer_recovery$'
python3 tests/run_malformed_tests.py ./build/compiler_design vm-rs
python3 tests/verification_inventory.py --write
git diff --check
```

## Deletion boundary

No lexer implementation path is deleted. Unterminated strings and later
multi-character lexical recovery remain explicit follow-up decisions if the
language needs them.
