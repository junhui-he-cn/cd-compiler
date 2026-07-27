# M5A-FORMAT-004: formatter canonical-check mode

Status: implemented.

Add `--format-check` as a read-only consumer of the existing formatter. It
uses the same production lex/parse and lossless source view as `--format`,
compares each directly requested entry source with the selected formatted
output, prints one `format check failed: <path>` line per mismatch, and exits
1 if any source is not canonical. A canonical source exits 0 with no output.
The mode supports the same stdin, direct multi-file, import-entry, and
`--format-indent-width` boundaries; `--format` and `--format-check` are
mutually exclusive.

This slice changes no default formatting, language semantics, diagnostics for
invalid syntax, or backend behavior.
