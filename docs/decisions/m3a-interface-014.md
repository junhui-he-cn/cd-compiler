# M3A-INTERFACE-014: complete imported inventory cache gate

Status: implemented against the `M3A-INTERFACE-009` fallback contract and the
`M3A-INTERFACE-011` strict-cache policy.

## Decision

Make the complete imported-file inventory an executable cache gate. The module
cache runner discovers and checks 42 successful import-graph entries (41
language-success `input.cd` fixtures plus the imported runtime-error graph) and
26 top-level parse/type/import diagnostic entries (6 parse, 15 type, and 5
import fixtures).

For successful graph entries the gate compares no-cache module products with a
cold cache build, requires a no-change build to reuse every module product,
checks missing and malformed sidecar fallback parity, checks deterministic
repeated fallback output, and requires strict malformed-sidecar rejection. For
diagnostic entries it compares no-cache and missing-sidecar source diagnostics
including exit code, stdout, and stderr.

The linkage-name allocator high-water mark is carried as sidecar reconstruction
metadata but is excluded from the public interface hash. This keeps unchanged
entry products reusable when dependency bodies are preloaded. Sidecars written
before this metadata existed remain readable with the conservative high-water
mark of zero.

## Compatibility and migration

The language, default source fallback, strict opt-in policy, `cdi 0.1`, and
`cdbc 0.1` remain unchanged. The canonical case is registered in
`tests/module_cache_cases.json` and runs through `tests/run_verification.py`;
the existing aggregate CTest and eight focused module-cache cases remain
active.

## Quantitative gate

The canonical inventory records 1,767 cases, including nine module-cache
cases. The complete-import case must report all 42 successful graph entries and
26 diagnostic entries with zero unexplained parity differences.

## Old-path deletion condition

The complete inventory evidence gate is now satisfied. This slice does not
promote strict rejection to the default or remove source fallback and
dependency-body checking. That next policy boundary must distinguish cold
module-product builds, which need source to create or repair products, from
interface-only cache consumers before changing CLI behavior.

## Verification

```sh
cmake --build build
ctest --test-dir build --output-on-failure -R '^(module_interface_artifact|module_cache|bytecode_module_cache)$'
python3 tests/bytecode_module_cache_tests.py ./build/compiler_design vm-rs
python3 tests/verification_inventory.py
python3 tests/run_verification.py ./build/compiler_design vm-rs --report build/verification-report.json
git diff --check
```
