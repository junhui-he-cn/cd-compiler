#!/usr/bin/env python3

"""Exercise the first stdio JSON-RPC language-server boundary."""

from __future__ import annotations

import json
import subprocess
import sys


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

    uri = "file:///tmp/compiler-design-lsp.cd"
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
                "params": {"capabilities": {}},
            },
        )
        initialize = receive(process)
        result = initialize.get("result", {})
        capabilities = result.get("capabilities", {}) if isinstance(result, dict) else {}
        if (
            initialize.get("id") != 1
            or capabilities.get("textDocumentSync") != 1
            or capabilities.get("documentFormattingProvider") is not True
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

        send(
            process,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 2},
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
                    "textDocument": {"uri": uri, "version": 3},
                    "contentChanges": [{"text": "print missing;\n"}],
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

        send(process, {"jsonrpc": "2.0", "id": 3, "method": "shutdown", "params": None})
        shutdown = receive(process)
        if shutdown.get("id") != 3 or shutdown.get("result") is not None:
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

    print("language server: initialize, diagnostics, formatting, shutdown passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
