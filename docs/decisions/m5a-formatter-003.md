# M5A-FORMAT-003: configurable formatter indentation

Status: implemented.

Expose the existing `FormatterOptions::indentWidth` through
`--format-indent-width N`. The option is formatter-only, requires a positive
integer, defaults to two spaces, and is rejected when `--format` is absent or
when it is combined with another output/backend mode. The default formatted
output and all compiler semantics remain unchanged.

The focused formatter tests cover four-space indentation, zero-width rejection,
missing formatter mode, and the unchanged two-space corpus gate.
