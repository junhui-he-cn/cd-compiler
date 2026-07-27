#!/usr/bin/env python3

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Optional


def run_repl(
    compiler: Path,
    vm_manifest: Path,
    transcript: str,
    import_paths: tuple[Path, ...] = (),
    session_root: Optional[Path] = None,
    json_lines: bool = False,
) -> subprocess.CompletedProcess[str]:
    repl = Path(__file__).resolve().parents[1] / "tools" / "repl.py"
    command = [sys.executable, str(repl), str(compiler), str(vm_manifest)]
    for import_path in import_paths:
        command.extend(["--import-path", str(import_path)])
    if session_root:
        command.extend(["--session-root", str(session_root)])
    if json_lines:
        command.append("--json-lines")
    return subprocess.run(
        command,
        input=transcript,
        text=True,
        capture_output=True,
        check=False,
    )


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} COMPILER VM_MANIFEST", file=sys.stderr)
        return 2

    compiler = Path(sys.argv[1]).resolve()
    vm_manifest = Path(sys.argv[2]).resolve()

    session = run_repl(
        compiler,
        vm_manifest,
        """let value = 1;

print value;

value = 2;

print value;

let broken = missing;

print value;

:reset

print value;

let value = 7;

print value;

:quit
""",
    )
    require(session.returncode == 0, f"session returned {session.returncode}: {session.stderr}")
    require(session.stdout == "1\n2\n2\n7\n", f"unexpected session stdout: {session.stdout!r}")
    require(session.stderr.count("Type error") == 2, f"unexpected session diagnostics: {session.stderr!r}")
    require("compiler-repl-" not in session.stderr, f"session diagnostics leaked temp path: {session.stderr!r}")

    runtime = run_repl(
        compiler,
        vm_manifest,
        """let values = [1];

print values[0];

print values[1];

print values[0];

:quit
""",
    )
    require(runtime.returncode == 0, f"runtime session returned {runtime.returncode}: {runtime.stderr}")
    require(runtime.stdout == "1\n1\n", f"runtime rollback leaked output: {runtime.stdout!r}")
    require(
        "array index out of range" in runtime.stderr,
        f"runtime diagnostic missing: {runtime.stderr!r}",
    )
    require("compiler-repl-" not in runtime.stderr, f"runtime diagnostic leaked temp path: {runtime.stderr!r}")

    expressions = run_repl(
        compiler,
        vm_manifest,
        """:eval 1 + 2;

let value = 4;

:eval value * 2

:eval value = 9

:eval missing

:eval value

:quit
""",
    )
    require(expressions.returncode == 0, f"expression session returned {expressions.returncode}: {expressions.stderr}")
    require(expressions.stdout == "3\n8\n9\n9\n", f"unexpected expression stdout: {expressions.stdout!r}")
    require(expressions.stderr.count("Type error") == 1, f"unexpected expression diagnostics: {expressions.stderr!r}")
    require("compiler-repl-" not in expressions.stderr, f"expression diagnostic leaked temp path: {expressions.stderr!r}")

    json_session = run_repl(
        compiler,
        vm_manifest,
        "\n".join(
            json.dumps(request, separators=(",", ":"))
            for request in (
                {"source": "let value = 3;"},
                {"source": "print value;"},
                {"source": "let broken = missing;"},
                {"source": "print value;"},
                {"command": "reset"},
                {"source": "print value;"},
                {"source": "let value = 8;"},
                {"expression": "value * 2"},
                {"source": "print value;"},
                {"command": "quit"},
            )
        )
        + "\n",
        json_lines=True,
    )
    require(json_session.returncode == 0, f"JSON session returned {json_session.returncode}: {json_session.stderr}")
    require(json_session.stderr == "", f"JSON protocol wrote stderr: {json_session.stderr!r}")
    json_responses = [json.loads(line) for line in json_session.stdout.splitlines()]
    require(len(json_responses) == 10, f"unexpected JSON response count: {json_responses!r}")
    require(json_responses[0] == {"ok": True, "stdout": ""}, f"unexpected JSON declaration response: {json_responses[0]!r}")
    require(json_responses[1] == {"ok": True, "stdout": "3\n"}, f"unexpected JSON output response: {json_responses[1]!r}")
    require(not json_responses[2]["ok"], f"JSON compile failure unexpectedly succeeded: {json_responses[2]!r}")
    require(json_responses[2]["stdout"] == "", f"JSON compile failure leaked stdout: {json_responses[2]!r}")
    require("Type error" in json_responses[2]["error"], f"JSON compile diagnostic missing: {json_responses[2]!r}")
    require(json_responses[3] == {"ok": True, "stdout": "3\n"}, f"JSON failure rollback leaked state: {json_responses[3]!r}")
    require(json_responses[4] == {"ok": True, "stdout": ""}, f"unexpected JSON reset response: {json_responses[4]!r}")
    require(not json_responses[5]["ok"], f"JSON reset did not clear state: {json_responses[5]!r}")
    require(json_responses[5]["stdout"] == "", f"JSON reset failure leaked stdout: {json_responses[5]!r}")
    require(json_responses[6] == {"ok": True, "stdout": ""}, f"unexpected JSON post-reset declaration: {json_responses[6]!r}")
    require(json_responses[7] == {"ok": True, "stdout": "16\n"}, f"unexpected JSON expression response: {json_responses[7]!r}")
    require(json_responses[8] == {"ok": True, "stdout": "8\n"}, f"unexpected JSON post-expression output: {json_responses[8]!r}")
    require(json_responses[9] == {"ok": True, "stdout": ""}, f"unexpected JSON quit response: {json_responses[9]!r}")

    json_runtime = run_repl(
        compiler,
        vm_manifest,
        "\n".join(
            json.dumps(request, separators=(",", ":"))
            for request in (
                {"source": "let values = [1];"},
                {"source": "print values[0];"},
                {"source": "print values[1];"},
                {"source": "print values[0];"},
                {"command": "quit"},
            )
        )
        + "\n",
        json_lines=True,
    )
    require(json_runtime.returncode == 0, f"JSON runtime session returned {json_runtime.returncode}: {json_runtime.stderr}")
    require(json_runtime.stderr == "", f"JSON runtime protocol wrote stderr: {json_runtime.stderr!r}")
    runtime_responses = [json.loads(line) for line in json_runtime.stdout.splitlines()]
    require(runtime_responses[0] == {"ok": True, "stdout": ""}, f"unexpected JSON runtime declaration: {runtime_responses!r}")
    require(runtime_responses[1] == {"ok": True, "stdout": "1\n"}, f"unexpected JSON runtime output: {runtime_responses!r}")
    require(not runtime_responses[2]["ok"], f"JSON runtime failure unexpectedly succeeded: {runtime_responses!r}")
    require(runtime_responses[2]["stdout"] == "", f"JSON runtime failure leaked stdout: {runtime_responses!r}")
    require("array index out of range" in runtime_responses[2]["error"], f"JSON runtime diagnostic missing: {runtime_responses!r}")
    require(runtime_responses[3] == {"ok": True, "stdout": "1\n"}, f"JSON runtime rollback leaked state: {runtime_responses!r}")
    require(runtime_responses[4] == {"ok": True, "stdout": ""}, f"unexpected JSON runtime quit: {runtime_responses!r}")

    multiline = run_repl(
        compiler,
        vm_manifest,
        """fun add(a: number, b: number): number {
  return a + b;
}

print add(2, 3);

:quit
""",
    )
    require(multiline.returncode == 0, f"multiline session returned {multiline.returncode}: {multiline.stderr}")
    require(multiline.stdout == "5\n", f"unexpected multiline stdout: {multiline.stdout!r}")
    require(multiline.stderr == "", f"unexpected multiline stderr: {multiline.stderr!r}")

    with tempfile.TemporaryDirectory(prefix="compiler-repl-import-") as directory:
        library = Path(directory) / "library"
        library.mkdir()
        (library / "math.cd").write_text(
            "fun answer(): number { return 42; }\nexport answer;\n",
            encoding="utf-8",
        )
        imported = run_repl(
            compiler,
            vm_manifest,
            """import "math";

print answer();

let result = answer();

print result;

:quit
""",
            (library,),
        )
        require(imported.returncode == 0, f"import session returned {imported.returncode}: {imported.stderr}")
        require(imported.stdout == "42\n42\n", f"unexpected imported stdout: {imported.stdout!r}")
        require(imported.stderr == "", f"unexpected imported stderr: {imported.stderr!r}")

        failed_import = run_repl(
            compiler,
            vm_manifest,
            """import "missing";

let value = 9;

print value;

:quit
""",
            (library,),
        )
        require(failed_import.returncode == 0, f"failed import session returned {failed_import.returncode}")
        require(failed_import.stdout == "9\n", f"failed import polluted stdout: {failed_import.stdout!r}")
        require("Import error" in failed_import.stderr, f"missing import diagnostic: {failed_import.stderr!r}")

    with tempfile.TemporaryDirectory(prefix="compiler-repl-root-") as directory:
        project = Path(directory) / "project"
        project.mkdir()
        (project / "lib.cd").write_text(
            "let message = \"relative\";\nexport message;\n",
            encoding="utf-8",
        )
        relative = run_repl(
            compiler,
            vm_manifest,
            """import "./lib.cd";

print message;

:quit
""",
            session_root=project,
        )
        require(relative.returncode == 0, f"relative import session returned {relative.returncode}: {relative.stderr}")
        require(relative.stdout == "relative\n", f"unexpected relative import stdout: {relative.stdout!r}")
        require(relative.stderr == "", f"unexpected relative import stderr: {relative.stderr!r}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL {error}", file=sys.stderr)
        raise SystemExit(1)
