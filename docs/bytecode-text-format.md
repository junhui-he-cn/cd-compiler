# Compiler Design ByteCode Text Format

This document describes the stable text artifact format for Compiler Design bytecode files.

The file extension is `.cdbc`, short for Compiler Design ByteCode.

This format is not the same as the current `--bytecode` debug print. The debug print is for humans inspecting compiler output. The `.cdbc` format is the compiler/VM contract: stable, versioned, and parseable by the Rust VM.

## Phase Status

This format is the text artifact contract at the compiler/VM boundary. The C++ compiler can emit `.cdbc` files with `--emit-bytecode` or independent module products with `--emit-module-bytecode`; the Rust VM can parse, canonicalize, link, and execute them with `dump`, `link`, and `run`.

```sh
compiler_design --emit-bytecode output.cdbc input.cd
compiler_design --emit-module-bytecode module-products input.cd
compiler_design --emit-module-bytecode module-products --module-cache module-cache --module-rebuild-report rebuild.json input.cd
compiler-design-vm dump output.cdbc
compiler-design-vm link module-products output.cdbc
compiler-design-vm run output.cdbc
```

## Header

Every file starts with a format identifier and version:

```text
cdbc 0.1
```

Future format changes must either remain backward-compatible with `0.1` or use a new version number.

## Artifact kinds

The `cdbc 0.1` envelope has two strict artifact kinds:

- A linked program has no `artifact` declaration and is the existing output of
  `--emit-bytecode`. It may be passed to the VM `run` command.
- An independently compiled module starts with `artifact: module` and a
  `module:` metadata section. It is emitted by
  `--emit-module-bytecode <directory>` as one `module-<graph-id>.cdbc` file per
  graph node. Module products are valid inputs to `dump`, but are not final
  executable programs; the VM rejects them until `link` produces a linked
  program artifact.

A module artifact has this envelope before the ordinary bytecode sections:

```text
artifact: module

module:
  identity = "/workspace/lib.cd"
  path = "lib.cd"
  canonical_path = "/workspace/lib.cd"
  entry = false
  dependencies:
    d0 target="/workspace/shared.cd" kind=import at=2 requested="./shared.cd"
```

`identity` is the graph canonical path and is the product-set key. `path` is
the source display path. Entry modules additionally carry a zero-based
`entry_order`; non-entry modules omit it. Dependency records are ordered by
source occurrence. `kind` is `import` or `re_export`, and `at` is the local
`main` instruction offset before which a linker expands that dependency. The
offset may equal the local instruction count. The module `main` and function
sections contain only the module's own statements; import and re-export bodies
are represented by these markers rather than recursively lowered into the
product.

The link/load rule is deterministic: `compiler-design-vm link <directory>
<output.cdbc>` selects entry products by `entry_order`, walks dependency
markers in their recorded order, expands each module identity at most once,
and preserves each marker's local insertion offset while rebasing register,
constant, name, function, jump, and debug references into the final linked
program. Missing identities, duplicate identities, non-contiguous entry order,
cycles, and invalid offsets are rejected before writing the linked artifact.
The linker does not introduce a new artifact version. The optional module
product cache is separate from VM artifacts: `--module-cache <directory>`
stores a `cdbc-cache 0.2` manifest and content-addressed product files, while
`--module-rebuild-report <report.json>` records per-module reuse/rebuild
reasons. Keys include the canonical module identity, exact source bytes,
canonical public interface shape, entry metadata, and source-ordered dependency
interface inputs; snapshot-local IDs are never cache keys. A private source
change rebuilds only that module, while a public-interface change propagates
through transitive dependents. Valid paired interface sidecars can preload
dependency public shape before the current importer check; invalid or missing
sidecars/products fall back to source parsing. Linked programs built from a
single file or direct multi-file input retain their existing metadata; an
import-aware graph additionally records each source's canonical module identity
in its optional debug-source entry.

Module products keep `debug_sources` and `debug_locations` local to the module
before linking. The Rust linker appends each expanded module's source table in
deterministic expansion order and rebases every main/function location through
that table, so runtime diagnostics from an imported function retain the
original module path and call-stack order in the final linked program. The
source table also carries the optional canonical module identity described
below; the linker preserves it while rebasing only artifact-local source
indexes.

## Module interface sidecars

The module cache also stores one `cdi 0.1` sidecar under `interfaces/` for each
module. A sidecar contains the canonical module identity, exact source hash,
complete public type shape (including generic constraints, struct fields and
methods, enum variants, and linkage names), dependency identities and public
interface hashes, entry metadata, and the linkage-name allocator high-water
mark used to reconstruct unchanged snapshots. The high-water mark is cache
reconstruction metadata and is not part of the public interface hash.
Snapshot-local IDs are not serialized.

`--module-interface-cache <directory>` reads these sidecars. A dependency is
preloaded only when its sidecar identity, canonical path, source hash, recursive
dependency sidecars/interface hashes, and paired cached `.cdbc` product all
match. The resulting module node keeps source bytes for diagnostics but has no
parsed dependency body; its preloaded interface supplies semantic import
visibility. An interface-only consumer rejects a missing, malformed, stale, or
unpaired sidecar by default with an `Import` diagnostic. Pass
`--module-cache-fallback` to opt back into the normal source parser for those
cases. `--module-cache` uses the same directory while emitting module products;
that product-building path retains source fallback by default and can opt into
rejection with `--module-cache-strict`.

The cache manifest is separate from VM artifacts and uses `cdbc-cache 0.2`; its
records include the relative `.cdi` sidecar path. The `.cdi` sidecar is not a
Rust VM input and has no effect on the linked `cdbc 0.1` wire format.

## Sections

A `.cdbc` file is organized into explicit sections:

```text
cdbc 0.1

constants:
  c0 = number 1
  c1 = string "hello"

names:
  n0 = "x"

main registers=3:
  r0 = constant c0
  store_var n0, r0
  r1 = load_var n0
  print r1

function f0 name="add_one" arity=1 registers=4:
  r1 = constant c0
  r2 = add r0, r1
  return r2

debug_sources:
  s0 path="examples/hello.cd" text="print 1;\n"

debug_locations:
  main 0 = s0:1:7
  main 1 = s0:1:1
```

The section names and reference prefixes are part of the canonical text format. Function `param` lines, when present, appear before instructions in a function section.

## Debug metadata

`debug_sources` and `debug_locations` are optional additive sections. The C++
compiler emits them for source-backed instructions, and the Rust VM uses them
to report runtime source locations, source lines, carets, and call stacks. Each
`debug_sources` entry is ordered by zero-based `sN` index and embeds the
display path plus original source text. Import-aware source entries may add a
stable canonical module identity before `path`:

```text
debug_sources:
  s0 module="/workspace/lib.cd" path="lib.cd" text="fun fail() { return 1 / 0; }\n"

debug_locations:
  main 3 = s0:2:1
  function f0 2 = s0:1:21
```

`module` is optional for compatibility with older metadata and is omitted for
source entries without a module graph identity. When present it must be
non-empty and is the graph's canonical module path; it is not a snapshot-local
numeric ID. `path` remains the display path used by current runtime
diagnostics. The Rust parser and formatter accept both the old
`sN path=... text=...` form and the module-aware form, and preserve the field
through module linking.

`main` identifies the top-level body; `function fN` identifies a function
section. Locations are sparse, but every referenced source, function, and
instruction must exist. Source, function, and instruction references are
zero-based; line and column values are one-based and must be positive. Duplicate
mappings and out-of-range references are Rust parser errors. A metadata-free
`cdbc 0.1` artifact remains valid and executes with
legacy one-line runtime errors.

## Value Encoding

Constants use explicit value tags:

```text
c0 = nil
c1 = number 1.25
c2 = bool true
c3 = string "escaped string"
```

Strings use double quotes and backslash escapes for at least `\\`, `\"`, `\n`, `\r`, and `\t`.

String constants are UTF-8 text. The Rust VM's `len`, `substr`, and `charAt`
operations interpret string offsets as Unicode scalar-value positions and
never split a scalar's UTF-8 encoding. Grapheme segmentation and normalization
are language-level non-goals for this format version.

## References

References use stable prefixes:

- `cN`: constant index.
- `nN`: name index.
- `rN`: register index.
- `fN`: function index.

Indexes are zero-based decimal integers.

## Opcode Names

The opcode names are stable snake-case names:

```text
constant
make_function
array
map
struct
variant
variant_tag
variant_field
move
load_var
store_var
assign_var
call
native_call
index
assign_index
field
assign_field
len
print
return
negate
not
add
subtract
multiply
divide
equal
not_equal
greater
greater_equal
less
less_equal
jump
jump_if_false
jump_if_true
```

Map construction preserves source order and uses explicit key/value register
pairs:

```text
rD = map [rKey0: rValue0, rKey1: rValue1, ...]
```

The Rust parser rejects malformed entries that do not contain a `: ` pair
separator. Map lookup and assignment reuse the existing `index` and
`assign_index` instructions.

Struct and field instructions use name-table references for field names:

```text
rD = struct {nName: rValue, ...}
rD = struct nType {nName: rValue, ...}
rD = field rObject, nName
rD = assign_field rObject, nName, rValue
```

The optional `nType` name-table reference records a named struct runtime type name for `typeOf`. Anonymous bytecode struct instructions omit it and continue to report `"struct"` when executed by the VM.

`assign_field` mutates an existing struct field and stores the assigned value in `rD`; assigning to a missing field is a runtime error.

Enum variants use two name-table references and an ordered payload register list:

```text
rD = variant nEnum.nVariant [rPayload0, rPayload1, ...]
rD = variant_tag rValue nEnum.nVariant
rD = variant_field rValue payloadIndex
```

`variant_tag` returns a boolean and is false for non-matching values.
`variant_field` reads a positional payload and raises a runtime error for
non-variant values or an out-of-range payload index.

Generic enum type arguments are compile-time metadata and are erased from
these runtime instructions; the emitted enum name and payload layout remain
the same as for non-generic enums.

Native stdlib calls use a name-table reference for the function name:

```text
rD = native_call nName [rArg0, rArg1, ...]
```

`native_call` invokes a registered VM native stdlib function by name-table reference; in this version `push`, `pop`, `remove`, `clear`, `merge`, `keys`, `values`, `floor`, `ceil`, `sqrt`, `str`, `substr`, `charAt`, `typeOf`, `contains`, `slice`, `copy`, `concat`, `map`, `filter`, `flatMap`, `any`, `all`, `count`, `find`, `findIndex`, and `reduce` are supported.

The `range` native is also supported with one to three numeric arguments. Its
result is consumed by the existing `len`, `index`, and `assert_array`
instructions. `assert_array` accepts arrays and ranges unchanged; when given a
map for `for-in`, it produces an array snapshot of the map's insertion-ordered
keys before the existing length/index loop lowering runs.

New opcodes must be added by updating this document, the C++ bytecode artifact emitter, and the Rust VM parser/formatter and executor together.

## Compatibility validation

The Rust parser accepts exactly the `cdbc 0.1` header. Before `dump`, `link`, or
`run` receives an artifact, it validates finite number constants, constant/name/
function/register references, jump targets, debug-location table shape, and
the supported native-call capability set. Module identities, entry metadata,
dependency targets, and insertion offsets are validated by the module envelope
and linker path. Invalid artifacts are rejected before VM execution; valid
linked programs and module products retain the canonical text described above.

The version and compatibility matrix is recorded in
`docs/decisions/m4a-artifact-validation.md` and
`docs/decisions/m4a-artifact-validation.json`. No successor version is selected
for this validation-only extension.

## Non-Goals for This Phase

This format does not define binary encoding, verifier internals, GC layout, task scheduler, or JIT metadata format. Those belong to later Rust VM phases.
