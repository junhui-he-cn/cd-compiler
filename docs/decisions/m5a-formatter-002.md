# M5A-FORMAT-002: formatter corpus and semantic parity gate

Status: implemented against the current successful golden corpus.

## Decision

Bind the formatter baseline to every successful golden case currently
registered by the repository: 235 source cases, including the direct
multi-file `args.txt` case. Error fixtures remain owned by the existing parser,
type, import, and runtime suites until invalid-input formatting gets its own
decision.

For each source, the corpus test invokes `--format`, extracts line comments
with string-aware scanning, and requires the comment text and order to be
unchanged. It formats the result again and requires byte-for-byte idempotence.
Inputs without imports are compiled from temporary formatted files and must
produce the same AST/semantic output as the original source. Import cases are
reparsed and reformatted from a temporary sibling file so relative import
resolution keeps its original directory context; standalone stdin is not used
for those cases because the language intentionally rejects imports from stdin.

## Quantitative gate

`tests/formatter_corpus_tests.py` reports `235/235` successful cases, zero
comment changes, zero parse failures after formatting, zero idempotence
failures, and zero AST/semantic differences for non-import cases. The CTest
entry and both formatter focused suites are included in the generated
verification inventory.

## Compatibility and deletion boundary

The test is additive and does not change compiler semantics or golden output.
It intentionally does not format error fixtures, rewrite repository sources,
or compare import AST paths after relocating temporary files. No formatter
implementation path is deleted by this slice.
