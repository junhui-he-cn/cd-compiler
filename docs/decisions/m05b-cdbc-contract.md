# M0.5B `cdbc 0.1` Contract Audit and Evolution Decision

Status: implemented against reference commit `42b9da1`.

## Current contract

The existing line-oriented UTF-8 `.cdbc` artifact is the current versioned
compiler/VM contract:

```text
cdbc 0.1
```

The core envelope is ordered as `constants`, `names`, `main`, and zero or more
`function` sections. `debug_sources`, `debug_locations`, and `debug_ranges` are
optional additive sections. The Rust parser is strict about section order,
sequential references, instruction shapes, debug mappings and ranges, unknown
sections, and trailing input; its `dump` formatter is the canonical text
representation.

The audit covers every `artifact` runner case in the current verification
inventory: 116 case IDs over 58 checked-in `expected.cdbc` fixtures. Every case
records the observed header/version and envelope capabilities in the audit
report. All 58 fixtures contain debug sources, locations, and source-local
ranges; four import-aware fixtures also contain canonical source-to-module
identities; 28 contain functions and 31 contain `native_call` instructions.

## Audit results

The executable audit is `tests/cdbc_contract_audit.py`. It reuses
`tests/bytecode_artifact_tests.py`, dumps every reference artifact through the
Rust VM, and probes invalid family/version headers plus malformed bytecode
references through `dump` only. M4A hardening is recorded in
`docs/decisions/m4a-artifact-validation.md` and its JSON decision record.

| Check | Result |
| --- | ---: |
| artifact assertions | 116/116 |
| reference Rust dumps | 58/58 byte-for-byte |
| invalid family/version probes | 2/2 rejected |
| invalid bytecode reference probes | 7/7 rejected |
| VM `run` calls for invalid probes | 0 |

The checked-in machine-readable baseline is
`docs/verification/m05b-baseline.json`; the per-case capability and dynamic
evidence report is `docs/verification/m05b-artifact-audit-report.json`.
Reproduce it in a build directory with:

```sh
python3 tests/cdbc_contract_audit.py \
  ./build/compiler_design vm-rs \
  --report build/m05b-cdbc-audit-report.json
```

The reference corpus digest is
`f4b23e35573ad3ce2e14b8f4b778b263842f7d8bd5792dbe958d00d85b353596`, computed
from sorted artifact paths and their complete-text SHA-256 values.

## Evolution classification

| Candidate | Classification | Current decision |
| --- | --- | --- |
| artifact kind | resolved after audit | linked programs omit an artifact declaration; M3B module products use strict `artifact: module` metadata in the same `cdbc 0.1` family |
| debug sources/locations/ranges | already present | optional source-mapped runtime metadata is consumed by the Rust VM; M4B adds source-to-module identity and source-local half-open ranges |
| unknown-section policy | already present | strict rejection protects older readers |
| runtime/target identity | deferred | wait for a second runtime, target, or transport consumer |
| capability flags | deferred | wait for a named negotiation requirement |
| module identity | resolved by M3B/M4B | module products serialize graph canonical-path identity and dependency target identities; import-aware debug source tables also retain canonical source-to-module identity |
| explicit framing | not currently required | no streaming, binary, or embedded transport exists |
| integrity metadata | deferred | requires a trust boundary or shared artifact cache |
| successor version/negotiation | successor-version candidate | conditional M4A path only if a breaking change is selected |

Every deferred, not-currently-required, or successor candidate has a named
revisit milestone and condition in
`docs/decisions/m05b-cdbc-contract.json`. The successor candidate has a named
M4A consumer and planned compatibility fixtures, but it is not selected now.

## Successor decision

M0.5B selected no successor version. The current 0.1 envelope already covers
the present C++ compiler/Rust VM pair, including core instructions, native calls,
optional debug metadata and source ranges, strict parse/dump, and runtime
diagnostics. Adding
fields without a concrete consumer or transport would create speculative ABI
surface.

M3B-ARTIFACT-001 subsequently used that compatible envelope for independently
validated module products. It did not select a successor version: module
identity, dependency insertion markers, and the non-executable module kind are
recognized additions within `cdbc 0.1`. The module-product decision is recorded
in `docs/decisions/m3b-module-artifacts.md`.

M4A owns any later choice of supported version ranges, negotiation, migration,
rejection behavior, deprecation period, and older-version lifecycle. A
successor becomes justified only when a breaking opcode/value-layout change,
second runtime/target, module linking requirement, or named transport/security
requirement cannot be expressed as a compatible 0.1 extension.

## Migration and deletion rules

M0.5B changes no artifact reader, writer, default emission, or VM behavior. The
current 0.1 reader/writer, canonical dumps, diagnostics, and execution results
remain the M4A reference corpus. Invalid family/version input must continue to
fail in Rust parsing before VM execution.

No old artifact path is deleted in this slice. M4A may change or remove 0.1
support only after its compatibility matrix, migration/rejection policy, and
support lifecycle are checked in and verified.
