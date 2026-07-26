#!/usr/bin/env python3

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(message: str) -> int:
    print(message, file=sys.stderr)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Check module-product debug metadata after Rust linking."
    )
    parser.add_argument("compiler", type=Path)
    parser.add_argument("vm", type=Path)
    args = parser.parse_args()

    compiler = args.compiler.resolve()
    manifest = args.vm.resolve() / "Cargo.toml"
    if not compiler.is_file() or not manifest.is_file():
        return fail("compiler or Rust VM manifest not found")

    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        library = root / "lib.cd"
        entry = root / "entry.cd"
        library.write_text(
            "fun fail() { return 1 / 0; }\n"
            "export fail;\n",
            encoding="utf-8",
        )
        entry.write_text(
            'import "./lib.cd";\n'
            "fail();\n",
            encoding="utf-8",
        )

        modules = root / "modules"
        emitted = run([
            str(compiler),
            "--emit-module-bytecode",
            str(modules),
            str(entry),
        ])
        if emitted.returncode != 0 or emitted.stdout or emitted.stderr:
            return fail(
                "module debug fixture emission failed\n"
                f"exit={emitted.returncode}\nstdout={emitted.stdout}\nstderr={emitted.stderr}"
            )

        linked_path = root / "linked.cdbc"
        linked = run([
            "cargo",
            "run",
            "--quiet",
            "--manifest-path",
            str(manifest),
            "--",
            "link",
            str(modules),
            str(linked_path),
        ])
        if linked.returncode != 0 or linked.stdout or linked.stderr:
            return fail(
                "module debug fixture link failed\n"
                f"exit={linked.returncode}\nstdout={linked.stdout}\nstderr={linked.stderr}"
            )

        linked_text = linked_path.read_text(encoding="utf-8")
        dumped = run([
            "cargo",
            "run",
            "--quiet",
            "--manifest-path",
            str(manifest),
            "--",
            "dump",
            str(linked_path),
        ])
        if dumped.returncode != 0 or dumped.stdout != linked_text or dumped.stderr:
            return fail(
                "linked module debug artifact failed canonical Rust dump\n"
                f"exit={dumped.returncode}\nstdout={dumped.stdout}\nstderr={dumped.stderr}"
            )
        if "debug_sources:\n" not in linked_text or "debug_locations:\n" not in linked_text:
            return fail("linked module artifact omitted debug metadata sections")
        if "debug_ranges:\n" not in linked_text:
            return fail("linked module artifact omitted full source-range metadata")
        def metadata_lines(header: str) -> list[str]:
            section = linked_text.split(f"{header}\n", 1)[1].split("\n\n", 1)[0]
            return [line for line in section.splitlines() if line.startswith("  ")]

        location_lines = metadata_lines("debug_locations:")
        range_lines = metadata_lines("debug_ranges:")
        if not location_lines or len(range_lines) != len(location_lines):
            return fail(
                "linked module artifact did not preserve one full range per location\n"
                f"locations={len(location_lines)} ranges={len(range_lines)}"
            )
        for line in range_lines:
            match = re.match(r"^  (?:main|function f\d+) \d+ = s(\d+):(\d+):(\d+)$", line)
            if match is None or int(match.group(2)) > int(match.group(3)):
                return fail(f"linked module artifact contained an invalid source range: {line}")
        if 'path="' not in linked_text or 'lib.cd"' not in linked_text or 'entry.cd"' not in linked_text:
            return fail("linked module artifact lost one module source path")
        for module_path in (entry.resolve(), library.resolve()):
            identity = f'module="{module_path}" path="'
            if identity not in linked_text:
                return fail(
                    "linked module artifact lost canonical source-to-module identity\n"
                    f"missing={identity}\ntext={linked_text}"
                )

        def run_linked() -> subprocess.CompletedProcess[str]:
            return run([
                "cargo",
                "run",
                "--quiet",
                "--manifest-path",
                str(manifest),
                "--",
                "run",
                str(linked_path),
            ])

        first = run_linked()
        second = run_linked()
        if (first.returncode, first.stdout, first.stderr) != (
            second.returncode,
            second.stdout,
            second.stderr,
        ):
            return fail("linked module runtime diagnostic was not deterministic")
        if first.returncode == 0 or first.stdout:
            return fail(
                "linked module runtime failure did not preserve failure behavior\n"
                f"exit={first.returncode}\nstdout={first.stdout}\nstderr={first.stderr}"
            )
        required = (
            "Runtime error at ",
            "division by zero",
            "lib.cd:1:",
            "entry.cd:2:",
            "Call stack:",
            "at fail (",
            "at main (",
        )
        missing = [fragment for fragment in required if fragment not in first.stderr]
        if missing:
            return fail(
                "linked module runtime diagnostic lost source/frame metadata\n"
                f"missing={missing}\nstderr={first.stderr}"
            )
        if first.stderr.index("at fail (") > first.stderr.index("at main ("):
            return fail("linked module runtime call-stack order changed")

    print("module debug metadata tests: linked source paths and runtime frames validated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
