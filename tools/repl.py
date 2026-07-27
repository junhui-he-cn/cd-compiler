#!/usr/bin/env python3

"""Run the source-backed Compiler Design incremental session prototype.

Each blank-line-delimited form is appended to the accepted transcript, then
the production compiler and Rust VM replay that transcript. A failed form is
never committed to the transcript, and only the newly produced stdout suffix
is exposed to the user.
    """

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Optional, TextIO


HELP_TEXT = """Compiler Design REPL prototype

Submit one source form at a time. A blank line commits a multi-line form.
Forms are compiled by the production compiler and evaluated by the Rust VM.

Commands at a form boundary:
  :help   show this help
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

    def submit(self, form: str, error_stream: TextIO) -> bool:
        source = self._candidate_source(form)
        compiled = self._compile(source)
        if compiled.returncode != 0:
            error_stream.write(self._normalize_diagnostics(compiled.stderr))
            return False

        executed = self._run_vm()
        if executed.returncode != 0:
            error_stream.write(self._normalize_diagnostics(executed.stderr))
            return False

        if not executed.stdout.startswith(self.accepted_output):
            error_stream.write(
                "REPL error: transcript replay did not preserve prior output\n"
            )
            return False

        new_output = executed.stdout[len(self.accepted_output):]
        if new_output:
            sys.stdout.write(new_output)
            sys.stdout.flush()
        self.accepted_source = source
        self.accepted_output = executed.stdout
        return True

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
    return parser.parse_args()


def run_session(session: TranscriptSession, input_stream: TextIO, error_stream: TextIO) -> int:
    pending: list[str] = []
    interactive = input_stream.isatty() and error_stream.isatty()

    def show_prompt() -> None:
        if interactive:
            error_stream.write("... " if pending else ">>> ")
            error_stream.flush()

    show_prompt()
    for raw_line in input_stream:
        line = raw_line.rstrip("\n")
        if not pending and line.startswith(":"):
            if line == ":quit":
                return 0
            if line == ":reset":
                session.reset()
            elif line == ":help":
                error_stream.write(HELP_TEXT)
            else:
                error_stream.write(f"REPL error: unknown command `{line}`\n")
            show_prompt()
            continue

        if not line.strip():
            if pending:
                session.submit("\n".join(pending), error_stream)
                pending.clear()
            show_prompt()
            continue

        pending.append(line)
        show_prompt()

    if pending:
        session.submit("\n".join(pending), error_stream)
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
            return run_session(session, sys.stdin, sys.stderr)
    except OSError as error:
        print(f"failed to create REPL session source: {error}", file=sys.stderr)
        return 2
    finally:
        if session_source_path:
            session_source_path.unlink(missing_ok=True)


if __name__ == "__main__":
    raise SystemExit(main())
