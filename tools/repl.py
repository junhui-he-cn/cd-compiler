#!/usr/bin/env python3

"""Run the source-backed Compiler Design incremental session prototype.

Each blank-line-delimited form is appended to the accepted transcript, then
the production compiler and Rust VM replay that transcript. A failed form is
never committed to the transcript, and only the newly produced stdout suffix
is exposed to the user.
    """

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
try:
    import readline
except ImportError:  # pragma: no cover - platform fallback
    readline = None
from pathlib import Path
from typing import Optional, TextIO


HELP_TEXT = """Compiler Design REPL prototype

Submit one source form at a time. A blank line commits a multi-line form.
Forms are compiled by the production compiler and evaluated by the Rust VM.

The optional --json-lines mode accepts one JSON request per input line and
returns one JSON response per output line for machine clients.

Commands at a form boundary:
  :help   show this help
  :eval EXPR  evaluate an expression and show its result
  :reset  clear the accepted transcript and runtime output baseline
  :quit   leave the session
"""


class TranscriptSession:
    def __init__(
        self,
        compiler: Path,
        vm_manifest: Path,
        root: Path,
        import_paths: list[Path],
        source_path: Optional[Path] = None,
    ) -> None:
        self.compiler = compiler
        self.vm_manifest = vm_manifest
        self.root = root
        self.import_paths = import_paths
        self.source_path = source_path or root / "session.cd"
        self.artifact_path = root / "session.cdbc"
        self.accepted_source = ""
        self.accepted_output = ""

    def _normalize_diagnostics(self, text: str) -> str:
        normalized = text.replace(str(self.source_path), "<repl>")
        normalized = normalized.replace(str(self.root), "<repl>")
        if self.source_path.parent != self.root:
            normalized = normalized.replace(str(self.source_path.parent), "<repl-root>")
        return normalized

    def _candidate_source(self, form: str) -> str:
        if not self.accepted_source:
            return form
        return self.accepted_source.rstrip("\n") + "\n" + form

    def _compile(self, source: str) -> subprocess.CompletedProcess[str]:
        self.source_path.write_text(source, encoding="utf-8")
        command = [
            str(self.compiler),
            "--emit-bytecode",
            str(self.artifact_path),
        ]
        for import_path in self.import_paths:
            command.extend(["--import-path", str(import_path)])
        command.append(str(self.source_path))
        return subprocess.run(
            command,
            text=True,
            capture_output=True,
            check=False,
        )

    def _run_vm(self) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                "cargo",
                "run",
                "--quiet",
                "--manifest-path",
                str(self.vm_manifest),
                "--",
                "run",
                str(self.artifact_path),
            ],
            text=True,
            capture_output=True,
            check=False,
        )

    def submit(self, form: str) -> tuple[bool, str, str]:
        source = self._candidate_source(form)
        compiled = self._compile(source)
        if compiled.returncode != 0:
            return False, "", self._normalize_diagnostics(compiled.stderr)

        executed = self._run_vm()
        if executed.returncode != 0:
            return False, "", self._normalize_diagnostics(executed.stderr)

        if not executed.stdout.startswith(self.accepted_output):
            return (
                False,
                "",
                "REPL error: transcript replay did not preserve prior output\n",
            )

        new_output = executed.stdout[len(self.accepted_output):]
        self.accepted_source = source
        self.accepted_output = executed.stdout
        return True, new_output, ""

    def submit_expression(self, expression: str) -> tuple[bool, str, str]:
        expression = expression.strip()
        if expression.endswith(";"):
            expression = expression[:-1].rstrip()
        return self.submit(f"print ({expression});")

    def reset(self) -> None:
        self.accepted_source = ""
        self.accepted_output = ""
        self.source_path.write_text("", encoding="utf-8")
        if self.artifact_path.exists():
            self.artifact_path.unlink()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="run the source-backed Compiler Design REPL prototype"
    )
    parser.add_argument("compiler", type=Path, help="path to compiler_design")
    parser.add_argument("vm_manifest", type=Path, help="path to vm-rs/Cargo.toml")
    parser.add_argument(
        "--import-path",
        action="append",
        type=Path,
        default=[],
        help="search path for non-explicit source imports (repeatable)",
    )
    parser.add_argument(
        "--session-root",
        type=Path,
        help="directory used as the base for explicit relative imports",
    )
    parser.add_argument(
        "--json-lines",
        action="store_true",
        help="use one JSON request and response per input line",
    )
    parser.add_argument(
        "--history-file",
        type=Path,
        help="read and write interactive readline history at this path",
    )
    return parser.parse_args()


def emit_submission(
    result: tuple[bool, str, str],
    output_stream: TextIO,
    error_stream: TextIO,
) -> None:
    _, stdout, error = result
    if stdout:
        output_stream.write(stdout)
        output_stream.flush()
    if error:
        error_stream.write(error)
        error_stream.flush()


def write_json_response(
    output_stream: TextIO,
    result: tuple[bool, str, str],
) -> None:
    ok, stdout, error = result
    response = {"ok": ok, "stdout": stdout}
    if not ok:
        response["error"] = error
    output_stream.write(
        json.dumps(response, ensure_ascii=False, separators=(",", ":")) + "\n"
    )
    output_stream.flush()


def protocol_error(message: str) -> tuple[bool, str, str]:
    return False, "", f"REPL error: {message}"


def run_json_lines(
    session: TranscriptSession,
    input_stream: TextIO,
    output_stream: TextIO,
) -> int:
    for raw_line in input_stream:
        line = raw_line.rstrip("\r\n")
        if not line.strip():
            continue
        try:
            request = json.loads(line)
        except json.JSONDecodeError as error:
            write_json_response(
                output_stream,
                protocol_error(f"invalid JSON: {error.msg}"),
            )
            continue

        if not isinstance(request, dict):
            write_json_response(
                output_stream,
                protocol_error("request must be a JSON object"),
            )
            continue

        if "source" in request:
            if set(request) != {"source"}:
                write_json_response(
                    output_stream,
                    protocol_error("source requests must contain only `source`"),
                )
                continue
            source = request["source"]
            if not isinstance(source, str):
                write_json_response(
                    output_stream,
                    protocol_error("`source` must be a string"),
                )
                continue
            write_json_response(output_stream, session.submit(source))
            continue

        if "expression" in request:
            if set(request) != {"expression"}:
                write_json_response(
                    output_stream,
                    protocol_error("expression requests must contain only `expression`"),
                )
                continue
            expression = request["expression"]
            if not isinstance(expression, str):
                write_json_response(
                    output_stream,
                    protocol_error("`expression` must be a string"),
                )
                continue
            write_json_response(output_stream, session.submit_expression(expression))
            continue

        if "command" in request:
            if set(request) != {"command"}:
                write_json_response(
                    output_stream,
                    protocol_error("command requests must contain only `command`"),
                )
                continue
            command = request["command"]
            if not isinstance(command, str):
                write_json_response(
                    output_stream,
                    protocol_error("`command` must be a string"),
                )
                continue
            if command == "reset":
                session.reset()
                write_json_response(output_stream, (True, "", ""))
                continue
            if command == "help":
                write_json_response(output_stream, (True, HELP_TEXT, ""))
                continue
            if command == "quit":
                write_json_response(output_stream, (True, "", ""))
                return 0
            write_json_response(
                output_stream,
                protocol_error(f"unknown command `{command}`"),
            )
            continue

        write_json_response(
            output_stream,
            protocol_error(
                "request must contain exactly one of `source`, `expression`, or `command`"
            ),
        )
    return 0


def run_session(
    session: TranscriptSession,
    input_stream: TextIO,
    output_stream: TextIO,
    error_stream: TextIO,
    history_file: Optional[Path] = None,
) -> int:
    pending: list[str] = []
    interactive = input_stream.isatty() and error_stream.isatty()

    def show_prompt() -> None:
        if interactive:
            error_stream.write("... " if pending else ">>> ")
            error_stream.flush()

    def consume_line(raw_line: str) -> bool:
        line = raw_line.rstrip("\r\n")
        if not pending and line.startswith(":"):
            if line == ":quit":
                return True
            if line == ":reset":
                session.reset()
            elif line == ":help":
                error_stream.write(HELP_TEXT)
            elif line == ":eval" or line.startswith(":eval "):
                expression = line[len(":eval"):].strip()
                if not expression:
                    error_stream.write("REPL error: :eval requires an expression\n")
                else:
                    emit_submission(
                        session.submit_expression(expression),
                        output_stream,
                        error_stream,
                    )
            else:
                error_stream.write(f"REPL error: unknown command `{line}`\n")
            return False

        if not line.strip():
            if pending:
                emit_submission(
                    session.submit("\n".join(pending)),
                    output_stream,
                    error_stream,
                )
                pending.clear()
            return False

        pending.append(line)
        return False

    def finish_pending() -> None:
        if pending:
            emit_submission(session.submit("\n".join(pending)), output_stream, error_stream)

    def load_history() -> None:
        if not history_file or readline is None:
            return
        try:
            readline.read_history_file(str(history_file))
        except FileNotFoundError:
            pass
        except OSError as error:
            error_stream.write(f"REPL error: failed to read history file: {error}\n")
            error_stream.flush()

    def save_history() -> None:
        if not history_file or readline is None:
            return
        try:
            readline.write_history_file(str(history_file))
        except OSError as error:
            error_stream.write(f"REPL error: failed to write history file: {error}\n")
            error_stream.flush()

    if interactive and readline is not None:
        load_history()
        try:
            while True:
                show_prompt()
                try:
                    raw_line = input() + "\n"
                except EOFError:
                    break
                line = raw_line.rstrip("\r\n")
                if line.strip():
                    readline.add_history(line)
                if consume_line(raw_line):
                    return 0
            finish_pending()
        finally:
            save_history()
        return 0

    show_prompt()
    for raw_line in input_stream:
        if consume_line(raw_line):
            return 0
        show_prompt()
    finish_pending()
    return 0


def main() -> int:
    args = parse_args()
    compiler = args.compiler.resolve()
    vm_manifest = args.vm_manifest.resolve()
    if not compiler.is_file():
        print(f"compiler not found: {compiler}", file=sys.stderr)
        return 2
    if not vm_manifest.is_file():
        print(f"VM manifest not found: {vm_manifest}", file=sys.stderr)
        return 2

    session_root = args.session_root.resolve() if args.session_root else None
    if session_root and not session_root.is_dir():
        print(f"session root not found: {session_root}", file=sys.stderr)
        return 2

    session_source_path: Optional[Path] = None
    try:
        with tempfile.TemporaryDirectory(prefix="compiler-repl-") as directory:
            if session_root:
                with tempfile.NamedTemporaryFile(
                    prefix=".compiler-repl-",
                    suffix=".cd",
                    dir=session_root,
                    delete=False,
                ) as source_file:
                    session_source_path = Path(source_file.name)
            session = TranscriptSession(
                compiler,
                vm_manifest,
                Path(directory),
                [path.resolve() for path in args.import_path],
                session_source_path,
            )
            if args.json_lines:
                return run_json_lines(session, sys.stdin, sys.stdout)
            history_file = args.history_file.resolve() if args.history_file else None
            return run_session(session, sys.stdin, sys.stdout, sys.stderr, history_file)
    except OSError as error:
        print(f"failed to create REPL session source: {error}", file=sys.stderr)
        return 2
    finally:
        if session_source_path:
            session_source_path.unlink(missing_ok=True)


if __name__ == "__main__":
    raise SystemExit(main())
