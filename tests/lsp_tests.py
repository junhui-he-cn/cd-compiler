#!/usr/bin/env python3

"""Exercise the stdio JSON-RPC language-server boundaries."""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def frame(message: dict) -> bytes:
    body = json.dumps(message, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    return b"Content-Length: " + str(len(body)).encode("ascii") + b"\r\n\r\n" + body


def send(process: subprocess.Popen[bytes], message: dict) -> None:
    assert process.stdin is not None
    process.stdin.write(frame(message))
    process.stdin.flush()


def receive(process: subprocess.Popen[bytes]) -> dict:
    assert process.stdout is not None
    content_length: int | None = None
    while True:
        line = process.stdout.readline()
        if not line:
            raise AssertionError("language server closed stdout before a response")
        line = line.rstrip(b"\r\n")
        if not line:
            break
        name, separator, value = line.partition(b":")
        if separator and name.lower() == b"content-length":
            content_length = int(value.strip())
    if content_length is None:
        raise AssertionError("language server response omitted Content-Length")
    body = process.stdout.read(content_length)
    if len(body) != content_length:
        raise AssertionError("language server response was truncated")
    return json.loads(body.decode("utf-8"))


def assert_publish(message: dict, uri: str, expected_count: int) -> list[dict]:
    if message.get("jsonrpc") != "2.0" or message.get("method") != "textDocument/publishDiagnostics":
        raise AssertionError(f"unexpected notification: {message!r}")
    params = message.get("params")
    if not isinstance(params, dict) or params.get("uri") != uri:
        raise AssertionError(f"notification URI mismatch: {message!r}")
    diagnostics = params.get("diagnostics")
    if not isinstance(diagnostics, list) or len(diagnostics) != expected_count:
        raise AssertionError(f"diagnostic count mismatch: {message!r}")
    return diagnostics


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: lsp_tests.py COMPILER", file=sys.stderr)
        return 2

    workspace_directory = tempfile.TemporaryDirectory(prefix="compiler-design-lsp-workspace-")
    outside_directory = tempfile.TemporaryDirectory(prefix="compiler-design-lsp-outside-")
    workspace_path = Path(workspace_directory.name)
    outside_path = Path(outside_directory.name)
    main_path = workspace_path / "main.cd"
    closed_module_path = workspace_path / "closed-disk.cd"
    closed_importer_path = workspace_path / "closed-importer.cd"
    outside_module_path = outside_path / "outside.cd"
    closed_module_path.write_text(
        "fun closedHelper(value: number): number { return value; }\n"
        "export closedHelper;\n",
        encoding="utf-8",
    )
    closed_importer_path.write_text(
        'import "./missing-from-closed.cd";\n',
        encoding="utf-8",
    )
    outside_module_path.write_text(
        "fun outsideHelper(value: number): number { return value; }\n"
        "export outsideHelper;\n",
        encoding="utf-8",
    )

    uri = main_path.as_uri()
    other_uri = (workspace_path / "other.cd").as_uri()
    module_uri = (workspace_path / "compiler-design-lsp-module.cd").as_uri()
    api_uri = (workspace_path / "compiler-design-lsp-api.cd").as_uri()
    incomplete_uri = (workspace_path / "incomplete.cd").as_uri()
    process = subprocess.Popen(
        [sys.argv[1], "--lsp"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        send(
            process,
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {"capabilities": {}, "rootUri": workspace_path.as_uri()},
            },
        )
        initialize = receive(process)
        result = initialize.get("result", {})
        capabilities = result.get("capabilities", {}) if isinstance(result, dict) else {}
        if (
            initialize.get("id") != 1
            or capabilities.get("textDocumentSync") != 1
            or capabilities.get("documentFormattingProvider") is not True
            or capabilities.get("definitionProvider") is not True
            or capabilities.get("documentSymbolProvider") is not True
            or capabilities.get("referencesProvider") is not True
            or capabilities.get("hoverProvider") is not True
            or capabilities.get("renameProvider") is not True
            or capabilities.get("completionProvider") != {"triggerCharacters": ["."]}
            or capabilities.get("workspaceSymbolProvider") is not True
        ):
            raise AssertionError(f"initialize response mismatch: {initialize!r}")

        send(process, {"jsonrpc": "2.0", "method": "initialized", "params": {}})
        send(
            process,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": uri,
                        "languageId": "compiler-design",
                        "version": 1,
                        "text": "let value=1;\n",
                    }
                },
            },
        )
        assert_publish(receive(process), uri, 0)

        send(
            process,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": other_uri,
                        "languageId": "compiler-design",
                        "version": 1,
                        "text": (
                            "fun helper(value: number): number { return value; }\n"
                            "export helper;\n"
                        ),
                    }
                },
            },
        )
        assert_publish(receive(process), other_uri, 0)

        send(
            process,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": incomplete_uri,
                        "languageId": "compiler-design",
                        "version": 1,
                        "text": "pri",
                    }
                },
            },
        )
        assert_publish(receive(process), incomplete_uri, 1)

        send(
            process,
            {
                "jsonrpc": "2.0",
                "id": 16,
                "method": "textDocument/completion",
                "params": {
                    "textDocument": {"uri": incomplete_uri},
                    "position": {"line": 0, "character": 3},
                },
            },
        )
        incomplete_completion = receive(process)
        if incomplete_completion.get("result") != {
            "isIncomplete": False,
            "items": [
                {
                    "label": "print",
                    "kind": 14,
                    "detail": "keyword",
                    "textEdit": {
                        "range": {
                            "start": {"line": 0, "character": 0},
                            "end": {"line": 0, "character": 3},
                        },
                        "newText": "print",
                    },
                },
                {
                    "label": "private",
                    "kind": 14,
                    "detail": "keyword",
                    "textEdit": {
                        "range": {
                            "start": {"line": 0, "character": 0},
                            "end": {"line": 0, "character": 3},
                        },
                        "newText": "private",
                    },
                },
            ],
        }:
            raise AssertionError(
                f"incomplete document completion response mismatch: {incomplete_completion!r}"
            )

        send(
            process,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didClose",
                "params": {"textDocument": {"uri": incomplete_uri}},
            },
        )
        assert_publish(receive(process), incomplete_uri, 0)

        send(
            process,
            {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "textDocument/formatting",
                "params": {
                    "textDocument": {"uri": uri},
                    "options": {"tabSize": 2, "insertSpaces": True},
                },
            },
        )
        formatting = receive(process)
        edits = formatting.get("result")
        if (
            formatting.get("id") != 2
            or not isinstance(edits, list)
            or len(edits) != 1
            or edits[0].get("newText") != "let value = 1;\n"
            or edits[0].get("range") != {
                "start": {"line": 0, "character": 0},
                "end": {"line": 1, "character": 0},
            }
        ):
            raise AssertionError(f"formatting response mismatch: {formatting!r}")

        valid_source = (
            "fun add(value: number): number { return value; }\n"
            "let result = add(1);\n"
            "print(result);\n"
        )
        send(
            process,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 2},
                    "contentChanges": [{"text": valid_source}],
                },
            },
        )
        assert_publish(receive(process), uri, 0)

        send(
            process,
            {
                "jsonrpc": "2.0",
                "id": 3,
                "method": "textDocument/documentSymbol",
                "params": {"textDocument": {"uri": uri}},
            },
        )
        symbols = receive(process)
        symbol_values = symbols.get("result")
        if (
            symbols.get("id") != 3
            or not isinstance(symbol_values, list)
            or [symbol.get("name") for symbol in symbol_values]
            != [str(main_path), "add", "value", "result"]
        ):
            raise AssertionError(f"document symbol response mismatch: {symbols!r}")

        send(
            process,
            {
                "jsonrpc": "2.0",
                "id": 4,
                "method": "textDocument/definition",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 1, "character": 13},
                },
            },
        )
        add_definition = receive(process)
        if add_definition.get("result") != {
            "uri": uri,
            "range": {
                "start": {"line": 0, "character": 4},
                "end": {"line": 0, "character": 7},
            },
        }:
            raise AssertionError(f"function definition response mismatch: {add_definition!r}")

        send(
            process,
            {
                "jsonrpc": "2.0",
                "id": 5,
                "method": "textDocument/definition",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 2, "character": 6},
                },
            },
        )
        result_definition = receive(process)
        if result_definition.get("result") != {
            "uri": uri,
            "range": {
                "start": {"line": 1, "character": 4},
                "end": {"line": 1, "character": 10},
            },
        }:
            raise AssertionError(f"variable definition response mismatch: {result_definition!r}")

        send(
            process,
            {
                "jsonrpc": "2.0",
                "id": 6,
                "method": "textDocument/references",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 1, "character": 13},
                    "context": {"includeDeclaration": False},
                },
            },
        )
        add_references = receive(process)
        if add_references.get("result") != [
            {
                "uri": uri,
                "range": {
                    "start": {"line": 1, "character": 13},
                    "end": {"line": 1, "character": 16},
                },
            }
        ]:
            raise AssertionError(f"reference response mismatch: {add_references!r}")

        send(
            process,
            {
                "jsonrpc": "2.0",
                "id": 7,
                "method": "textDocument/references",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 1, "character": 13},
                    "context": {"includeDeclaration": True},
                },
            },
        )
        add_references_with_declaration = receive(process)
        if add_references_with_declaration.get("result") != [
            {
                "uri": uri,
                "range": {
                    "start": {"line": 0, "character": 4},
                    "end": {"line": 0, "character": 7},
                },
            },
            {
                "uri": uri,
                "range": {
                    "start": {"line": 1, "character": 13},
                    "end": {"line": 1, "character": 16},
                },
            },
        ]:
            raise AssertionError(
                "reference-with-declaration response mismatch: "
                f"{add_references_with_declaration!r}"
            )

        send(
            process,
            {
                "jsonrpc": "2.0",
                "id": 8,
                "method": "textDocument/hover",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 0, "character": 4},
                },
            },
        )
        add_hover = receive(process)
        if add_hover.get("result") != {
            "contents": {"kind": "plaintext", "value": "fun(number): number"},
            "range": {
                "start": {"line": 0, "character": 4},
                "end": {"line": 0, "character": 7},
            },
        }:
            raise AssertionError(f"function hover response mismatch: {add_hover!r}")

        send(
            process,
            {
                "jsonrpc": "2.0",
                "id": 9,
                "method": "textDocument/hover",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 2, "character": 6},
                },
            },
        )
        result_hover = receive(process)
        if result_hover.get("result") != {
            "contents": {"kind": "plaintext", "value": "number"},
            "range": {
                "start": {"line": 2, "character": 6},
                "end": {"line": 2, "character": 12},
            },
        }:
            raise AssertionError(f"variable hover response mismatch: {result_hover!r}")

        send(
            process,
            {
                "jsonrpc": "2.0",
                "id": 10,
                "method": "textDocument/rename",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 1, "character": 13},
                    "newName": "sum",
                },
            },
        )
        rename = receive(process)
        if rename.get("result") != {
            "changes": {
                uri: [
                    {
                        "range": {
                            "start": {"line": 0, "character": 4},
                            "end": {"line": 0, "character": 7},
                        },
                        "newText": "sum",
                    },
                    {
                        "range": {
                            "start": {"line": 1, "character": 13},
                            "end": {"line": 1, "character": 16},
                        },
                        "newText": "sum",
                    },
                ]
            }
        }:
            raise AssertionError(f"rename response mismatch: {rename!r}")

        send(
            process,
            {
                "jsonrpc": "2.0",
                "id": 11,
                "method": "textDocument/rename",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 1, "character": 13},
                    "newName": "1invalid",
                },
            },
        )
        invalid_rename = receive(process)
        if invalid_rename.get("result") is not None:
            raise AssertionError(f"invalid rename response mismatch: {invalid_rename!r}")

        send(
            process,
            {
                "jsonrpc": "2.0",
                "id": 13,
                "method": "textDocument/completion",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 1, "character": 15},
                },
            },
        )
        completion = receive(process)
        if completion.get("result") != {
            "isIncomplete": False,
            "items": [
                {
                    "label": "add",
                    "kind": 3,
                    "detail": "fun(number): number",
                    "textEdit": {
                        "range": {
                            "start": {"line": 1, "character": 13},
                            "end": {"line": 1, "character": 15},
                        },
                        "newText": "add",
                    },
                }
            ],
        }:
            raise AssertionError(f"completion response mismatch: {completion!r}")

        send(
            process,
            {
                "jsonrpc": "2.0",
                "id": 14,
                "method": "textDocument/completion",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 2, "character": 5},
                },
            },
        )
        print_completion = receive(process)
        if print_completion.get("result") != {
            "isIncomplete": False,
            "items": [
                {
                    "label": "print",
                    "kind": 14,
                    "detail": "keyword",
                    "textEdit": {
                        "range": {
                            "start": {"line": 2, "character": 0},
                            "end": {"line": 2, "character": 5},
                        },
                        "newText": "print",
                    },
                }
            ],
        }:
            raise AssertionError(f"builtin completion response mismatch: {print_completion!r}")

        send(
            process,
            {
                "jsonrpc": "2.0",
                "id": 15,
                "method": "workspace/symbol",
                "params": {"query": "helper"},
            },
        )
        workspace_symbols = receive(process)
        if workspace_symbols.get("result") != [
            {
                "name": "helper",
                "kind": 12,
                "location": {
                    "uri": other_uri,
                    "range": {
                        "start": {"line": 0, "character": 4},
                        "end": {"line": 0, "character": 10},
                    },
                },
            }
        ]:
            raise AssertionError(f"workspace symbol response mismatch: {workspace_symbols!r}")

        send(
            process,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": module_uri,
                        "languageId": "compiler-design",
                        "version": 1,
                        "text": (
                            "let value = 42;\n"
                            "export value;\n"
                            "export fun answer(): number { return 42; }\n"
                            "struct Box { value: number }\n"
                            "impl Box { fun get(): number { return this.value; } }\n"
                            "enum Result { Ok(number), Empty }\n"
                            "export Box, Result;\n"
                        ),
                    }
                },
            },
        )
        assert_publish(receive(process), module_uri, 0)

        send(
            process,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": api_uri,
                        "languageId": "compiler-design",
                        "version": 1,
                        "text": 'export Box, Result from "./compiler-design-lsp-module.cd";\n',
                    }
                },
            },
        )
        assert_publish(receive(process), api_uri, 0)

        imported_source = (
            'import "./compiler-design-lsp-module.cd";\n'
            "print(value);\n"
            "print(answer());\n"
        )
        send(
            process,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 3},
                    "contentChanges": [{"text": imported_source}],
                },
            },
        )
        assert_publish(receive(process), uri, 0)

        send(
            process,
            {
                "jsonrpc": "2.0",
                "id": 17,
                "method": "textDocument/definition",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 1, "character": 7},
                },
            },
        )
        imported_definition = receive(process)
        if imported_definition.get("result") != {
            "uri": module_uri,
            "range": {
                "start": {"line": 0, "character": 4},
                "end": {"line": 0, "character": 9},
            },
        }:
            raise AssertionError(
                f"cross-module definition response mismatch: {imported_definition!r}"
            )

        send(
            process,
            {
                "jsonrpc": "2.0",
                "id": 18,
                "method": "textDocument/definition",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 2, "character": 8},
                },
            },
        )
        declarative_definition = receive(process)
        if declarative_definition.get("result") != {
            "uri": module_uri,
            "range": {
                "start": {"line": 2, "character": 11},
                "end": {"line": 2, "character": 17},
            },
        }:
            raise AssertionError(
                f"declarative export definition response mismatch: {declarative_definition!r}"
            )

        closed_source = (
            f'import "./{closed_module_path.name}";\n'
            "print(closedHelper(1));\n"
            "print(closedHelper(2));\n"
        )
        send(
            process,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 4},
                    "contentChanges": [{"text": closed_source}],
                },
            },
        )
        assert_publish(receive(process), uri, 0)

        send(
            process,
            {
                "jsonrpc": "2.0",
                "id": 29,
                "method": "textDocument/definition",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 1, "character": 10},
                },
            },
        )
        closed_definition = receive(process)
        if closed_definition.get("result") != {
            "uri": closed_module_path.as_uri(),
            "range": {
                "start": {"line": 0, "character": 4},
                "end": {"line": 0, "character": 16},
            },
        }:
            raise AssertionError(
                f"closed-module definition response mismatch: {closed_definition!r}"
            )

        send(
            process,
            {
                "jsonrpc": "2.0",
                "id": 30,
                "method": "textDocument/references",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 1, "character": 10},
                    "context": {"includeDeclaration": False},
                },
            },
        )
        closed_references = receive(process)
        if closed_references.get("result") != [
            {
                "uri": uri,
                "range": {
                    "start": {"line": 1, "character": 6},
                    "end": {"line": 1, "character": 18},
                },
            },
            {
                "uri": uri,
                "range": {
                    "start": {"line": 2, "character": 6},
                    "end": {"line": 2, "character": 18},
                },
            },
        ]:
            raise AssertionError(
                f"closed-module references response mismatch: {closed_references!r}"
            )

        send(
            process,
            {
                "jsonrpc": "2.0",
                "id": 31,
                "method": "textDocument/references",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 1, "character": 10},
                    "context": {"includeDeclaration": True},
                },
            },
        )
        closed_references_with_declaration = receive(process)
        if closed_references_with_declaration.get("result") != [
            {
                "uri": closed_module_path.as_uri(),
                "range": {
                    "start": {"line": 0, "character": 4},
                    "end": {"line": 0, "character": 16},
                },
            },
            {
                "uri": uri,
                "range": {
                    "start": {"line": 1, "character": 6},
                    "end": {"line": 1, "character": 18},
                },
            },
            {
                "uri": uri,
                "range": {
                    "start": {"line": 2, "character": 6},
                    "end": {"line": 2, "character": 18},
                },
            },
        ]:
            raise AssertionError(
                "closed-module references-with-declaration response mismatch: "
                f"{closed_references_with_declaration!r}"
            )

        closed_module_path.write_text("let =;\n", encoding="utf-8")
        send(
            process,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": closed_module_path.as_uri(),
                        "languageId": "compiler-design",
                        "version": 1,
                        "text": (
                            "fun closedHelper(value: number): number { return value + 1; }\n"
                            "export closedHelper;\n"
                        ),
                    }
                },
            },
        )
        assert_publish(receive(process), closed_module_path.as_uri(), 0)

        outside_import = os.path.relpath(outside_module_path, main_path.parent).replace(os.sep, "/")
        send(
            process,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 5},
                    "contentChanges": [
                        {
                            "text": (
                                f'import "{outside_import}";\n'
                                "print(outsideHelper(1));\n"
                            )
                        }
                    ],
                },
            },
        )
        outside_diagnostics = assert_publish(receive(process), uri, 1)
        if "failed to open import" not in outside_diagnostics[0].get("message", ""):
            raise AssertionError(f"workspace-boundary diagnostic mismatch: {outside_diagnostics!r}")

        send(
            process,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 6},
                    "contentChanges": [
                        {"text": 'import "./missing-lsp-module.cd";\nprint(missingHelper(1));\n'}
                    ],
                },
            },
        )
        missing_diagnostics = assert_publish(receive(process), uri, 1)
        if "failed to open import" not in missing_diagnostics[0].get("message", ""):
            raise AssertionError(f"missing-module diagnostic mismatch: {missing_diagnostics!r}")

        send(
            process,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didClose",
                "params": {"textDocument": {"uri": closed_module_path.as_uri()}},
            },
        )
        assert_publish(receive(process), closed_module_path.as_uri(), 0)

        send(
            process,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 7},
                    "contentChanges": [
                        {"text": f'import "./{closed_importer_path.name}";\n'}
                    ],
                },
            },
        )
        nested_missing_diagnostics = assert_publish(receive(process), uri, 1)
        if "failed to open import" not in nested_missing_diagnostics[0].get("message", ""):
            raise AssertionError(
                f"nested missing-module diagnostic mismatch: {nested_missing_diagnostics!r}"
            )

        namespace_source = (
            'import "./compiler-design-lsp-module.cd" as lib;\n'
            "print(lib.value);\n"
            'import "./compiler-design-lsp-api.cd" as api;\n'
            "let box: api.Box = api.Box { value: 1 };\n"
            "let result: api.Result = api.Result.Ok(2);\n"
            "print(box.value);\n"
            "print(box.get());\n"
            "let hel = 1;\n"
        )
        send(
            process,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 8},
                    "contentChanges": [{"text": namespace_source}],
                },
            },
        )
        assert_publish(receive(process), uri, 0)

        send(
            process,
            {
                "jsonrpc": "2.0",
                "id": 18,
                "method": "textDocument/definition",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 1, "character": 11},
                },
            },
        )
        namespace_definition = receive(process)
        if namespace_definition.get("result") != {
            "uri": module_uri,
            "range": {
                "start": {"line": 0, "character": 4},
                "end": {"line": 0, "character": 9},
            },
        }:
            raise AssertionError(
                f"namespace definition response mismatch: {namespace_definition!r}"
            )

        send(
            process,
            {
                "jsonrpc": "2.0",
                "id": 19,
                "method": "textDocument/references",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 1, "character": 11},
                    "context": {"includeDeclaration": False},
                },
            },
        )
        namespace_references = receive(process)
        if namespace_references.get("result") != [
            {
                "uri": uri,
                "range": {
                    "start": {"line": 1, "character": 10},
                    "end": {"line": 1, "character": 15},
                },
            }
        ]:
            raise AssertionError(
                f"cross-module references response mismatch: {namespace_references!r}"
            )

        send(
            process,
            {
                "jsonrpc": "2.0",
                "id": 20,
                "method": "textDocument/references",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 1, "character": 11},
                    "context": {"includeDeclaration": True},
                },
            },
        )
        namespace_references_with_declaration = receive(process)
        if namespace_references_with_declaration.get("result") != [
            {
                "uri": module_uri,
                "range": {
                    "start": {"line": 0, "character": 4},
                    "end": {"line": 0, "character": 9},
                },
            },
            {
                "uri": uri,
                "range": {
                    "start": {"line": 1, "character": 10},
                    "end": {"line": 1, "character": 15},
                },
            },
        ]:
            raise AssertionError(
                "cross-module references-with-declaration response mismatch: "
                f"{namespace_references_with_declaration!r}"
            )

        send(
            process,
            {
                "jsonrpc": "2.0",
                "id": 21,
                "method": "textDocument/rename",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 1, "character": 11},
                    "newName": "amount",
                },
            },
        )
        namespace_rename = receive(process)
        if namespace_rename.get("result") != {
            "changes": {
                module_uri: [
                    {
                        "range": {
                            "start": {"line": 0, "character": 4},
                            "end": {"line": 0, "character": 9},
                        },
                        "newText": "amount",
                    },
                    {
                        "range": {
                            "start": {"line": 1, "character": 7},
                            "end": {"line": 1, "character": 12},
                        },
                        "newText": "amount",
                    },
                ],
                uri: [
                    {
                        "range": {
                            "start": {"line": 1, "character": 10},
                            "end": {"line": 1, "character": 15},
                        },
                        "newText": "amount",
                    }
                ],
            }
        }:
            raise AssertionError(
                f"cross-module rename response mismatch: {namespace_rename!r}"
            )

        send(
            process,
            {
                "jsonrpc": "2.0",
                "id": 22,
                "method": "textDocument/completion",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 1, "character": 12},
                },
            },
        )
        namespace_completion = receive(process)
        if namespace_completion.get("result") != {
            "isIncomplete": False,
            "items": [
                {
                    "label": "value",
                    "kind": 6,
                    "detail": "variable",
                    "textEdit": {
                        "range": {
                            "start": {"line": 1, "character": 10},
                            "end": {"line": 1, "character": 12},
                        },
                        "newText": "value",
                    },
                }
            ],
        }:
            raise AssertionError(
                f"namespace completion response mismatch: {namespace_completion!r}"
            )

        send(
            process,
            {
                "jsonrpc": "2.0",
                "id": 23,
                "method": "textDocument/definition",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 3, "character": 15},
                },
            },
        )
        qualified_type_definition = receive(process)
        if qualified_type_definition.get("result") != {
            "uri": module_uri,
            "range": {
                "start": {"line": 3, "character": 7},
                "end": {"line": 3, "character": 10},
            },
        }:
            raise AssertionError(
                "namespace-qualified type definition response mismatch: "
                f"{qualified_type_definition!r}"
            )

        send(
            process,
            {
                "jsonrpc": "2.0",
                "id": 24,
                "method": "textDocument/definition",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 4, "character": 37},
                },
            },
        )
        qualified_variant_definition = receive(process)
        if qualified_variant_definition.get("result") != {
            "uri": module_uri,
            "range": {
                "start": {"line": 5, "character": 14},
                "end": {"line": 5, "character": 16},
            },
        }:
            raise AssertionError(
                "namespace-qualified enum variant definition response mismatch: "
                f"{qualified_variant_definition!r}"
            )

        send(
            process,
            {
                "jsonrpc": "2.0",
                "id": 25,
                "method": "textDocument/completion",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 4, "character": 37},
                },
            },
        )
        qualified_variant_completion = receive(process)
        if qualified_variant_completion.get("result") != {
            "isIncomplete": False,
            "items": [
                {
                    "label": "Ok",
                    "kind": 20,
                    "detail": "variant",
                    "textEdit": {
                        "range": {
                            "start": {"line": 4, "character": 36},
                            "end": {"line": 4, "character": 37},
                        },
                        "newText": "Ok",
                    },
                }
            ],
        }:
            raise AssertionError(
                "namespace-qualified enum variant completion response mismatch: "
                f"{qualified_variant_completion!r}"
            )

        send(
            process,
            {
                "jsonrpc": "2.0",
                "id": 26,
                "method": "textDocument/completion",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 5, "character": 12},
                },
            },
        )
        struct_field_completion = receive(process)
        if struct_field_completion.get("result") != {
            "isIncomplete": False,
            "items": [
                {
                    "label": "value",
                    "kind": 5,
                    "detail": "field",
                    "textEdit": {
                        "range": {
                            "start": {"line": 5, "character": 10},
                            "end": {"line": 5, "character": 12},
                        },
                        "newText": "value",
                    },
                }
            ],
        }:
            raise AssertionError(
                "typed struct field completion response mismatch: "
                f"{struct_field_completion!r}"
            )

        send(
            process,
            {
                "jsonrpc": "2.0",
                "id": 27,
                "method": "textDocument/completion",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 6, "character": 11},
                },
            },
        )
        struct_method_completion = receive(process)
        if struct_method_completion.get("result") != {
            "isIncomplete": False,
            "items": [
                {
                    "label": "get",
                    "kind": 2,
                    "detail": "fun(Box): number",
                    "textEdit": {
                        "range": {
                            "start": {"line": 6, "character": 10},
                            "end": {"line": 6, "character": 11},
                        },
                        "newText": "get",
                    },
                }
            ],
        }:
            raise AssertionError(
                "typed struct method completion response mismatch: "
                f"{struct_method_completion!r}"
            )

        send(
            process,
            {
                "jsonrpc": "2.0",
                "id": 28,
                "method": "textDocument/completion",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 7, "character": 7},
                },
            },
        )
        workspace_completion = receive(process)
        if workspace_completion.get("result") != {
            "isIncomplete": False,
            "items": [
                {
                    "label": "hel",
                    "kind": 6,
                    "detail": "variable",
                    "textEdit": {
                        "range": {
                            "start": {"line": 7, "character": 4},
                            "end": {"line": 7, "character": 7},
                        },
                        "newText": "hel",
                    },
                },
                {
                    "label": "helper",
                    "kind": 3,
                    "detail": "fun(number): number",
                    "textEdit": {
                        "range": {
                            "start": {"line": 7, "character": 4},
                            "end": {"line": 7, "character": 7},
                        },
                        "newText": "helper",
                    },
                },
            ],
        }:
            raise AssertionError(
                "opened workspace completion response mismatch: "
                f"{workspace_completion!r}"
            )

        send(
            process,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didClose",
                "params": {"textDocument": {"uri": module_uri}},
            },
        )
        assert_publish(receive(process), module_uri, 0)

        send(
            process,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didClose",
                "params": {"textDocument": {"uri": other_uri}},
            },
        )
        assert_publish(receive(process), other_uri, 0)

        send(
            process,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didClose",
                "params": {"textDocument": {"uri": api_uri}},
            },
        )
        assert_publish(receive(process), api_uri, 0)

        send(
            process,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 9},
                    "contentChanges": [{"text": "let =;\n"}],
                },
            },
        )
        parse_diagnostics = assert_publish(receive(process), uri, 1)
        if parse_diagnostics[0].get("source") != "compiler_design":
            raise AssertionError(f"parse diagnostic source mismatch: {parse_diagnostics!r}")

        send(
            process,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 10},
                    "contentChanges": [{"text": "print(missing);\n"}],
                },
            },
        )
        type_diagnostics = assert_publish(receive(process), uri, 1)
        if type_diagnostics[0].get("source") != "compiler_design":
            raise AssertionError(f"type diagnostic source mismatch: {type_diagnostics!r}")

        send(
            process,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didClose",
                "params": {"textDocument": {"uri": uri}},
            },
        )
        assert_publish(receive(process), uri, 0)

        send(process, {"jsonrpc": "2.0", "id": 16, "method": "shutdown", "params": None})
        shutdown = receive(process)
        if shutdown.get("id") != 16 or shutdown.get("result") is not None:
            raise AssertionError(f"shutdown response mismatch: {shutdown!r}")
        send(process, {"jsonrpc": "2.0", "method": "exit"})
        process.stdin.close()
        if process.wait(timeout=5) != 0:
            raise AssertionError(f"language server exited with {process.returncode}")
        stderr = process.stderr.read().decode("utf-8") if process.stderr is not None else ""
        if stderr:
            raise AssertionError(f"language server wrote stderr: {stderr!r}")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        workspace_directory.cleanup()
        outside_directory.cleanup()

    print("language server: initialize, queries, diagnostics, formatting, shutdown passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
