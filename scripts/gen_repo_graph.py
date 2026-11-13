#!/usr/bin/env python3
"""
irisOS Repository Architecture, IPC & Symbol Graph Generator / Query CLI.

Generates:
  - docs/repo_graph.md   (Compact architectural atlas + Mermaid topology + IPC & symbol maps)
  - docs/repo_graph.json (Machine-queryable index, gitignored, lazy-built on query)

Usage:
  python3 scripts/gen_repo_graph.py                  # Regenerate docs/repo_graph.{md,json}
  python3 scripts/gen_repo_graph.py op <SUBSTR>      # Query IPC opcode def/handler/callers
  python3 scripts/gen_repo_graph.py file <SUBSTR>    # Query file function line-range map
  python3 scripts/gen_repo_graph.py target <SUBSTR>  # Query CMake target & linked libs
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

ROOT_DIR = Path(__file__).resolve().parent.parent
DOCS_DIR = ROOT_DIR / "docs"
GRAPH_MD = DOCS_DIR / "repo_graph.md"
GRAPH_JSON = DOCS_DIR / "repo_graph.json"
IPC_HEADER = ROOT_DIR / "sdk/libiris/include/iris/ipc.h"
CMAKE_FILE = ROOT_DIR / "build/CMakeLists.txt"

SCAN_ROOTS = ("sdk", "servers", "apps")
SOURCE_EXTS = {".c", ".cpp", ".cc", ".h", ".hpp"}
IMPL_EXTS = {".c", ".cpp", ".cc"}
LARGE_FILE_THRESHOLD = 250

# C keywords that can precede '(' at brace depth 0 without being function definitions
C_NON_FUNC_KEYWORDS = {
    "if", "for", "while", "switch", "return", "sizeof", "alignof", "typeof",
    "_Static_assert", "static_assert", "defined", "attribute", "__attribute__",
    "__asm__", "asm", "typedef", "struct", "union", "enum",
}

OP_DEF_RE = re.compile(r"^\s*#define\s+(IRIS_[A-Z0-9_]+_OP_[A-Z0-9_]+)\s+(\S+)")
PROTO_DEF_RE = re.compile(r"^\s*#define\s+(IRIS_PROTO_[A-Z0-9_]+)\s+(\S+)")
OP_CASE_RE = re.compile(r"\bcase\s+(IRIS_[A-Z0-9_]+_OP_[A-Z0-9_]+)\s*:")
OP_ANY_RE = re.compile(r"\b(IRIS_[A-Z0-9_]+_OP_[A-Z0-9_]+)\b")


def rel_path(path: Path) -> str:
    return path.relative_to(ROOT_DIR).as_posix()


def strip_comments_and_strings(text: str) -> str:
    """Replace comments and string/char literals with spaces, preserving newlines."""
    out: list[str] = []
    i = 0
    n = len(text)
    in_line_comment = False
    in_block_comment = False
    in_str: str | None = None

    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""

        if in_line_comment:
            if ch == "\n":
                in_line_comment = False
                out.append("\n")
            else:
                out.append(" ")
            i += 1
            continue

        if in_block_comment:
            if ch == "*" and nxt == "/":
                in_block_comment = False
                out.append("  ")
                i += 2
            elif ch == "\n":
                out.append("\n")
                i += 1
            else:
                out.append(" ")
                i += 1
            continue

        if in_str is not None:
            if ch == "\\":
                out.append("  " if nxt != "\n" else " \n")
                i += 2
            elif ch == in_str:
                in_str = None
                out.append(" ")
                i += 1
            elif ch == "\n":
                out.append("\n")
                i += 1
            else:
                out.append(" ")
                i += 1
            continue

        if ch == "/" and nxt == "/":
            in_line_comment = True
            out.append("  ")
            i += 2
        elif ch == "/" and nxt == "*":
            in_block_comment = True
            out.append("  ")
            i += 2
        elif ch in ('"', "'"):
            in_str = ch
            out.append(" ")
            i += 1
        else:
            out.append(ch)
            i += 1

    return "".join(out)


def extract_functions(text: str) -> list[dict[str, Any]]:
    """Extract top-level C/C++ function definitions with accurate start/end line numbers."""
    clean = strip_comments_and_strings(text)
    lines = clean.splitlines()
    functions: list[dict[str, Any]] = []

    depth = 0
    sig_start_line: int | None = None
    sig_chars: list[str] = []
    in_preproc = False
    active_func: dict[str, Any] | None = None

    func_name_re = re.compile(r"([A-Za-z_][A-Za-z0-9_:~]*)\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?$")

    for lineno, raw_line in enumerate(lines, start=1):
        stripped = raw_line.strip()

        if depth == 0:
            if in_preproc:
                if not raw_line.rstrip().endswith("\\"):
                    in_preproc = False
                continue
            if stripped.startswith("#"):
                if raw_line.rstrip().endswith("\\"):
                    in_preproc = True
                sig_start_line = None
                sig_chars.clear()
                continue

        for ch in raw_line:
            if depth == 0 and active_func is None:
                if ch == "{":
                    joined = " ".join("".join(sig_chars).split())
                    if (
                        "(" in joined
                        and ")" in joined
                        and "=" not in joined.split("(", 1)[0]
                        and not re.match(r"^(typedef|struct|union|enum|extern\s+\"C\")\b", joined)
                    ):
                        m = func_name_re.search(joined)
                        if m:
                            fname = m.group(1)
                            if fname not in C_NON_FUNC_KEYWORDS:
                                active_func = {
                                    "name": fname,
                                    "start_line": sig_start_line or lineno,
                                }
                    sig_start_line = None
                    sig_chars.clear()
                    depth += 1
                elif ch == ";":
                    sig_start_line = None
                    sig_chars.clear()
                else:
                    if not ch.isspace() and sig_start_line is None:
                        sig_start_line = lineno
                    sig_chars.append(ch)
            else:
                if ch == "{":
                    depth += 1
                elif ch == "}":
                    depth = max(0, depth - 1)
                    if depth == 0 and active_func is not None:
                        active_func["end_line"] = lineno
                        functions.append(active_func)
                        active_func = None

        if depth == 0 and active_func is None and sig_chars:
            sig_chars.append(" ")

    return functions


def parse_cmake_targets() -> dict[str, dict[str, Any]]:
    """Parse build/CMakeLists.txt for executables, libraries, and linked dependencies."""
    if not CMAKE_FILE.exists():
        return {}

    text = CMAKE_FILE.read_text(encoding="utf-8", errors="replace")
    # Strip CMake line comments
    lines = [re.sub(r"#.*$", "", ln) for ln in text.splitlines()]
    clean = "\n".join(lines)

    targets: dict[str, dict[str, Any]] = {}

    def clean_tokens(blob: str) -> list[str]:
        tokens = []
        for tok in blob.split():
            tok = tok.strip('"').strip("'")
            if tok and not tok.startswith("$<") and tok not in {
                "PRIVATE", "PUBLIC", "INTERFACE", "STATIC", "SHARED", "OBJECT"
            }:
                tokens.append(tok)
        return tokens

    for kind, pattern in (
        ("executable", r"add_executable\s*\(([^)]+)\)"),
        ("library", r"add_library\s*\(([^)]+)\)"),
    ):
        for m in re.finditer(pattern, clean, re.MULTILINE):
            toks = clean_tokens(m.group(1))
            if not toks:
                continue
            name = toks[0]
            if name.startswith("${"):
                continue
            if name in {"mkufs2"}:
                targets[name] = {
                    "kind": "host_tool",
                    "links": [],
                    "sources": [t.replace("${IRIS_ROOT}/", "") for t in toks[1:] if "/" in t or t.endswith((".c", ".cpp"))],
                }
                continue
            entry = targets.setdefault(name, {"kind": kind, "links": [], "sources": []})
            entry["kind"] = kind
            for t in toks[1:]:
                t_norm = t.replace("${IRIS_ROOT}/", "")
                if any(t_norm.endswith(ext) for ext in SOURCE_EXTS):
                    entry["sources"].append(t_norm)

    for m in re.finditer(r"iris_add_userland_executable\s*\(([^)]+)\)", clean, re.MULTILINE):
        toks = clean_tokens(m.group(1))
        if not toks:
            continue
        name = toks[0]
        if name.startswith("${"):
            continue
        entry = targets.setdefault(name, {"kind": "userland_exec", "links": ["mlibc", "iris_static"], "sources": []})
        section = None
        for t in toks[1:]:
            if t in {"SOURCES", "LIBRARIES", "INCLUDES", "DEFINITIONS", "OPTIONS"}:
                section = t
                continue
            t_norm = t.replace("${IRIS_ROOT}/", "")
            if section == "SOURCES":
                if any(t_norm.endswith(ext) for ext in SOURCE_EXTS):
                    entry["sources"].append(t_norm)
            elif section == "LIBRARIES":
                if t_norm not in entry["links"]:
                    entry["links"].append(t_norm)

    for m in re.finditer(r"target_link_libraries\s*\(([^)]+)\)", clean, re.MULTILINE):
        toks = clean_tokens(m.group(1))
        if not toks:
            continue
        name = toks[0]
        if name in targets:
            for lib in toks[1:]:
                lib_clean = lib.replace("${IRIS_ROOT}/", "")
                if lib_clean not in targets[name]["links"]:
                    targets[name]["links"].append(lib_clean)

    return targets


def build_graph() -> dict[str, Any]:
    """Scan the repository and return the structured graph dictionary."""
    protocols: dict[str, dict[str, Any]] = {}
    opcodes: dict[str, dict[str, Any]] = {}

    if IPC_HEADER.exists():
        for lineno, line in enumerate(IPC_HEADER.read_text(encoding="utf-8", errors="replace").splitlines(), start=1):
            pm = PROTO_DEF_RE.match(line)
            if pm:
                protocols[pm.group(1)] = {"value": pm.group(2), "line": lineno}
            om = OP_DEF_RE.match(line)
            if om:
                opcodes[om.group(1)] = {
                    "value": om.group(2),
                    "def_file": rel_path(IPC_HEADER),
                    "def_line": lineno,
                    "handlers": [],
                    "callers": [],
                }

    source_files: list[Path] = []
    for root_name in SCAN_ROOTS:
        root_path = ROOT_DIR / root_name
        if not root_path.exists():
            continue
        for p in sorted(root_path.rglob("*")):
            if p.is_file() and p.suffix in SOURCE_EXTS:
                source_files.append(p)

    files_index: dict[str, dict[str, Any]] = {}
    for path in source_files:
        rpath = rel_path(path)
        text = path.read_text(encoding="utf-8", errors="replace")
        lines = text.splitlines()
        loc = len(lines)

        funcs = extract_functions(text) if path.suffix in IMPL_EXTS else []
        files_index[rpath] = {
            "loc": loc,
            "functions": funcs,
        }

        if rpath != rel_path(IPC_HEADER) and path.suffix in IMPL_EXTS:
            for lineno, line in enumerate(lines, start=1):
                # Skip pure comment lines
                stripped = line.lstrip()
                if stripped.startswith("//") or stripped.startswith("/*") or stripped.startswith("*"):
                    continue
                cm = OP_CASE_RE.search(line)
                if cm and cm.group(1) in opcodes:
                    opcodes[cm.group(1)]["handlers"].append(f"{rpath}:{lineno}")
                    continue
                for om in OP_ANY_RE.finditer(line):
                    op_name = om.group(1)
                    if op_name in opcodes:
                        loc_str = f"{rpath}:{lineno}"
                        if loc_str not in opcodes[op_name]["callers"]:
                            opcodes[op_name]["callers"].append(loc_str)

    targets = parse_cmake_targets()

    return {
        "protocols": protocols,
        "opcodes": opcodes,
        "targets": targets,
        "files": files_index,
    }


def render_markdown(graph: dict[str, Any]) -> str:
    """Render a compact, token-efficient Markdown architectural atlas."""
    out: list[str] = []
    out.append("# irisOS Repository Architecture & Symbol Graph")
    out.append("")
    out.append("> Auto-generated by `scripts/gen_repo_graph.py`. Query specific items via `./iris graph op|file|target <term>` before reading large files.")
    out.append("")

    # 1. Topology Mermaid Diagram
    out.append("## 1. System & IPC Topology")
    out.append("```mermaid")
    out.append("flowchart LR")
    out.append("    Boot[Gigaboot UEFI] --> Kernel[Zircon + userboot]")
    out.append("    Kernel -->|PA_RESOURCE / BOOTFS| Init[iris_init]")
    out.append("    Init -->|ECAM PCIe| Blk[virtio_blk]")
    out.append("    Init -->|ECAM PCIe| Gpu[virtio_gpu / gop_fb]")
    out.append("    Init -->|ECAM PCIe| Inp[virtio_input]")
    out.append("    Init --> Crypt[iris_crypt]")
    out.append("    Init --> Fs[iris_fshost]")
    out.append("    Init --> Pty[iris_pty]")
    out.append("    Init --> Comp[iris_compositor]")
    out.append("    Fs -->|IRIS_PROTO_CRYPT| Crypt")
    out.append("    Crypt -->|IRIS_PROTO_DRIVER| Blk")
    out.append("    Comp -->|IRIS_PROTO_DRIVER| Gpu")
    out.append("    Apps[apps + mlibc] -->|IRIS_PROTO_FS| Fs")
    out.append("    Apps -->|IRIS_PROTO_COMPOSITOR| Comp")
    out.append("    Apps -->|IRIS_PROTO_PTY| Pty")
    out.append("```")
    out.append("")

    # 2. CMake Targets & Dependencies
    out.append("## 2. Build Targets & Dependencies (`build/CMakeLists.txt`)")
    out.append("| Target | Kind | Linked Libraries |")
    out.append("| :--- | :--- | :--- |")
    for tname in sorted(graph["targets"]):
        tinfo = graph["targets"][tname]
        links = ", ".join(f"`{l}`" for l in tinfo["links"]) if tinfo["links"] else "—"
        out.append(f"| `{tname}` | {tinfo['kind']} | {links} |")
    out.append("")

    # 3. IPC Opcode Dispatch Table
    out.append("## 3. IPC Opcode Dispatch Map (`sdk/libiris/include/iris/ipc.h`)")
    out.append("| Opcode | Def | Handler (`case`) | Callers / References |")
    out.append("| :--- | :--- | :--- | :--- |")
    for op_name, op_info in graph["opcodes"].items():
        def_ref = f"`ipc.h:{op_info['def_line']}`"
        handlers = ", ".join(f"`{h}`" for h in op_info["handlers"]) if op_info["handlers"] else "—"
        callers_list = op_info["callers"][:4]
        more = f" (+{len(op_info['callers']) - 4})" if len(op_info["callers"]) > 4 else ""
        callers = (", ".join(f"`{c}`" for c in callers_list) + more) if callers_list else "—"
        out.append(f"| `{op_name}` | {def_ref} | {handlers} | {callers} |")
    out.append("")

    # 4. Large-File Function Maps
    out.append(f"## 4. Large-File Function Maps (Files > {LARGE_FILE_THRESHOLD} LOC)")
    out.append("Use these exact `Lstart-Lend` ranges with `read_file` instead of reading whole files.")
    out.append("")
    large_files = [
        (rpath, finfo)
        for rpath, finfo in sorted(graph["files"].items())
        if finfo["loc"] > LARGE_FILE_THRESHOLD and finfo["functions"]
    ]
    for rpath, finfo in large_files:
        out.append(f"### `{rpath}` ({finfo['loc']} LOC)")
        fn_parts = [f"`{fn['name']}` (L{fn['start_line']}-L{fn['end_line']})" for fn in finfo["functions"]]
        out.append(", ".join(fn_parts))
        out.append("")

    return "\n".join(out).rstrip() + "\n"


def write_outputs(graph: dict[str, Any]) -> None:
    DOCS_DIR.mkdir(parents=True, exist_ok=True)
    GRAPH_MD.write_text(render_markdown(graph), encoding="utf-8")
    GRAPH_JSON.write_text(json.dumps(graph, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def load_or_build_graph() -> dict[str, Any]:
    if GRAPH_JSON.exists():
        return json.loads(GRAPH_JSON.read_text(encoding="utf-8"))
    graph = build_graph()
    write_outputs(graph)
    return graph


def query_op(graph: dict[str, Any], term: str) -> int:
    q = term.upper()
    matches = [(k, v) for k, v in graph["opcodes"].items() if q in k.upper()]
    if not matches:
        print(f"No IPC opcodes matching '{term}'.")
        return 1
    for name, info in matches:
        print(f"{name} ({info['value']}) — def: {info['def_file']}:{info['def_line']}")
        print(f"  handlers: {', '.join(info['handlers']) if info['handlers'] else 'none'}")
        print(f"  callers : {', '.join(info['callers']) if info['callers'] else 'none'}")
    return 0


def query_file(graph: dict[str, Any], term: str) -> int:
    q = term.lower()
    matches = [(k, v) for k, v in sorted(graph["files"].items()) if q in k.lower()]
    if not matches:
        print(f"No indexed files matching '{term}'.")
        return 1
    for rpath, info in matches:
        print(f"{rpath} ({info['loc']} LOC):")
        if not info["functions"]:
            print("  (no top-level C/C++ functions indexed)")
        for fn in info["functions"]:
            print(f"  L{fn['start_line']}-L{fn['end_line']}: {fn['name']}")
    return 0


def query_target(graph: dict[str, Any], term: str) -> int:
    q = term.lower()
    matches = [(k, v) for k, v in sorted(graph["targets"].items()) if q in k.lower()]
    if not matches:
        print(f"No CMake targets matching '{term}'.")
        return 1
    for tname, info in matches:
        print(f"{tname} [{info['kind']}]:")
        print(f"  links  : {', '.join(info['links']) if info['links'] else 'none'}")
        if info["sources"]:
            print(f"  sources: {', '.join(info['sources'])}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="irisOS repository graph generator and query tool")
    sub = parser.add_subparsers(dest="cmd")

    p_op = sub.add_parser("op", help="Query IPC opcode definition, handler, and callers")
    p_op.add_argument("term", help="Opcode name or substring")

    p_file = sub.add_parser("file", help="Query function line-range index for a file")
    p_file.add_argument("term", help="File path or substring")

    p_target = sub.add_parser("target", help="Query CMake target dependencies")
    p_target.add_argument("term", help="Target name or substring")

    args = parser.parse_args()

    if args.cmd is None:
        graph = build_graph()
        write_outputs(graph)
        print(
            f"[irisOS graph] Generated {rel_path(GRAPH_MD)} "
            f"({len(GRAPH_MD.read_text(encoding='utf-8').splitlines())} lines) "
            f"and {rel_path(GRAPH_JSON)} "
            f"({len(graph['files'])} files, {len(graph['opcodes'])} opcodes, {len(graph['targets'])} targets)."
        )
        return 0

    graph = load_or_build_graph()
    if args.cmd == "op":
        return query_op(graph, args.term)
    if args.cmd == "file":
        return query_file(graph, args.term)
    if args.cmd == "target":
        return query_target(graph, args.term)
    return 1


if __name__ == "__main__":
    sys.exit(main())
