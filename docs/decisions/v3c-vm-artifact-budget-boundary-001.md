# V3C: exact artifact-byte boundary for verify and dump

Status: measured and completed on 2026-08-03. This is a focused capacity
coverage slice, not a new resource-policy change.

## Trigger and boundary

V3A added `compiler-design-vm verify` as the no-output parse/verify load
boundary. The existing capacity corpus covered `dump` artifact-size rejection,
but did not prove that the new command accepted an artifact exactly at its
byte limit and rejected it one byte below that limit. The long Unicode artifact
already provides a stable payload large enough to exercise both outcomes.

The test records the emitted `.cdbc` byte size dynamically, then asserts:

- `verify --max-artifact-bytes <artifact-size>` succeeds with empty stdout;
- `verify --max-artifact-bytes <artifact-size - 1>` fails with the stable
  `artifact bytes (limit N)` diagnostic; and
- `dump --max-artifact-bytes <artifact-size>` still succeeds and emits the
  canonical artifact text.

This is an exact success/rejection boundary, so it satisfies the V3C trigger
without inventing a larger default limit, a new accounting unit, or an RSS
threshold.

## Compatibility

Both commands continue to use the same `RunConfig::max_artifact_bytes` check
before parse, verification, formatting, or execution. `.cdbc 0.1`, canonical
dump bytes, resource diagnostics, and default resource limits are unchanged.

## Evidence

```sh
PYTHONDONTWRITEBYTECODE=1 python3 tests/vm_capacity_tests.py \
  ./build/compiler_design vm-rs
```

The focused capacity run passed the exact verify success/rejection cases,
canonical dump boundary, existing aggregate/array/map/struct/string/module
cases, and retained-byte observations. No generated measurement report is
committed.

## Next boundary

No broader V3C policy is admitted. A future capacity slice needs a new exact
boundary or evidence of an unbounded host cost; otherwise the roadmap moves
to a concrete V2 host consumer or remains at the current VM product boundary.
