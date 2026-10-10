#!/usr/bin/env python3
"""Generate cli/data/phases.json from the checklists in docs/phases/P*.md.

The phase guides are the single source of truth for progress: tick a box
(`- [ ]` -> `- [x]`) in the markdown, on GitHub or locally, then run

    python tools/gen_phases.py          # rewrite cli/data/phases.json
    python tools/gen_phases.py --check  # exit 1 if phases.json is stale (CI)

Mapping:
  * each `# P<n> — <name> (...)` file           -> one phase
  * each `### <n.m> — <title>` section           -> one task, progress = % of its boxes ticked
  * boxes under a `##` heading (e.g. `## Gate`) -> one task named after that heading
  * phase state: done (all ticked) | in_progress (some) | optional | next | todo

Standard library only, so it runs anywhere Python 3.8+ does.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PHASES_DIR = ROOT / "docs" / "phases"
OUT = ROOT / "cli" / "data" / "phases.json"

TITLE_RE = re.compile(r"^#\s+(P\d+)\s+[—-]\s+(.+?)\s*$")
H2_RE = re.compile(r"^##\s+(.+?)\s*$")
H3_RE = re.compile(r"^###\s+(.+?)\s*$")
SECTION_NUM_RE = re.compile(r"^(\d+\.\d+[a-z]?)\s+[—-]\s+(.+)$")
BOX_RE = re.compile(r"^- \[([ xX])\]\s")


def strip_md(text: str) -> str:
    """Plain text for a terminal: drop links, emphasis and code ticks."""
    text = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", text)
    text = text.replace("**", "").replace("`", "")
    text = re.sub(r"(?<!\w)_([^_]+)_(?!\w)", r"\1", text)
    return re.sub(r"\s+", " ", text).strip()


def phase_name(raw: str) -> tuple[str, bool]:
    """'Execution engine (XL)' -> ('Execution engine', False);
    'Embeddings (M, optional)' -> ('Embeddings', True)."""
    m = re.match(r"^(.*?)\s*\(([^)]*)\)\s*$", raw)
    if not m:
        return strip_md(raw), False
    return strip_md(m.group(1)), "optional" in m.group(2).lower()


def parse_phase(path: Path) -> dict:
    lines = path.read_text(encoding="utf-8-sig").splitlines()
    m = TITLE_RE.match(lines[0]) if lines else None
    if not m:
        raise SystemExit(f"{path}: first line must look like '# P<n> — <name>'")
    pid, (name, optional) = m.group(1), phase_name(m.group(2))

    tasks: list[dict] = []
    current: dict | None = None  # task that boxes are counted into
    pending: dict | None = None  # heading seen, no box yet

    for line in lines[1:]:
        if h3 := H3_RE.match(line):
            heading = strip_md(h3.group(1))
            sm = SECTION_NUM_RE.match(heading)
            tid, title = (sm.group(1), sm.group(2)) if sm else (heading.split()[0][:5].lower(), heading)
            pending, current = {"id": tid, "title": title, "done": 0, "total": 0}, None
            continue
        if h2 := H2_RE.match(line):
            heading = strip_md(h2.group(1))
            pending = {"id": heading.split()[0][:5].lower(), "title": heading, "done": 0, "total": 0}
            current = None
            continue
        if box := BOX_RE.match(line):
            if current is None:
                if pending is None:
                    pending = {"id": "misc", "title": "Unsectioned items", "done": 0, "total": 0}
                current, pending = pending, None
                tasks.append(current)
            current["total"] += 1
            current["done"] += box.group(1) in "xX"

    out_tasks = []
    for t in tasks:
        if t["done"] == t["total"]:
            status = "done"
        elif t["done"] == 0:
            status = "todo"
        else:
            status = "in_progress"
        entry = {"id": t["id"], "title": t["title"], "status": status}
        if status == "in_progress":
            entry["progress"] = round(100 * t["done"] / t["total"])
        out_tasks.append(entry)

    done = sum(t["done"] for t in tasks)
    total = sum(t["total"] for t in tasks)
    if total and done == total:
        state = "done"
    elif done:
        state = "in_progress"
    else:
        state = "optional" if optional else "todo"

    return {
        "id": pid,
        "name": name,
        "state": state,
        "note": f"{done}/{total} checklist items ticked",
        "tasks": out_tasks,
    }


def project_version() -> str:
    cm = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8-sig")
    m = re.search(r"project\(\s*\w+\s+VERSION\s+([\d.]+)", cm)
    return m.group(1) if m else ""


def build() -> dict:
    files = sorted(PHASES_DIR.glob("P*.md"), key=lambda p: int(re.sub(r"\D", "", p.stem) or 0))
    phases = [parse_phase(f) for f in files]

    # Current stage: the latest phase with any progress. Earlier phases with a
    # few unticked boxes stay "in_progress" so the drift stays visible.
    active = [p for p in phases if p["state"] in ("in_progress", "done")]
    stage_phase = next((p for p in reversed(active) if p["state"] == "in_progress"), None)
    if stage_phase is None:
        stage_phase = next((p for p in phases if p["state"] in ("todo", "optional")), None)

    if stage_phase is not None:
        idx = phases.index(stage_phase)
        nxt = next((p for p in phases[idx + 1 :] if p["state"] == "todo"), None)
        if nxt is not None:
            nxt["state"] = "next"

    stage = {"phase": "", "milestone": "", "label": "", "state": ""}
    if stage_phase is not None:
        task = next((t for t in stage_phase["tasks"] if t["status"] != "done"), None)
        stage = {
            "phase": stage_phase["id"],
            "milestone": task["id"] if task else "",
            "label": task["title"] if task else "",
            "state": stage_phase["state"],
        }

    return {
        "project": "AxolotlDB",
        "version": project_version(),
        "generated_from": "docs/phases/P*.md by tools/gen_phases.py; do not edit by hand",
        "stage": stage,
        "phases": phases,
    }


def render(data: dict) -> str:
    return json.dumps(data, indent=2, ensure_ascii=False) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="fail if phases.json is out of date")
    args = ap.parse_args()

    text = render(build())
    if args.check:
        current = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
        if current != text:
            print(
                f"{OUT.relative_to(ROOT)} is out of date with docs/phases/.\n"
                "Run: python tools/gen_phases.py   and commit the result.",
                file=sys.stderr,
            )
            return 1
        print(f"{OUT.relative_to(ROOT)} is up to date.")
        return 0

    OUT.parent.mkdir(parents=True, exist_ok=True)
    with OUT.open("w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print(f"wrote {OUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
