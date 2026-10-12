"""Keep customer-facing wilibsp documentation independent of private source trees."""

import pathlib
import re


REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
PUBLIC_ROOTS = ("README.md", "docs", "apps", "libs", "skills")
FORBIDDEN = (
    (re.compile(r"freewili-firmware", re.IGNORECASE), "private repository name"),
    (re.compile(r"\bfreewilimain\b", re.IGNORECASE), "private source tree"),
    (re.compile(r"\brmpLib\b", re.IGNORECASE), "private source directory"),
    (re.compile(r"agents[/\\]hardware", re.IGNORECASE), "private documentation path"),
    (re.compile(r"panels[/\\]fw[A-Za-z0-9_]+", re.IGNORECASE), "private panel source path"),
)


def markdown_files(root):
    for entry in PUBLIC_ROOTS:
        path = root / entry
        if path.is_file():
            yield path
        elif path.is_dir():
            yield from sorted(path.rglob("*.md"))


def violations(root):
    for path in markdown_files(root):
        text = path.read_text(encoding="utf-8", errors="replace")
        for line_number, line in enumerate(text.splitlines(), 1):
            for pattern, description in FORBIDDEN:
                match = pattern.search(line)
                if match:
                    yield path.relative_to(root), line_number, description, match.group(0)


def test_repository_docs_have_no_private_refs():
    assert list(violations(REPO_ROOT)) == []


def test_private_source_reference_is_detected(tmp_path):
    docs = tmp_path / "docs"
    docs.mkdir()
    (docs / "bad.md").write_text(
        "Copied from freewili-firmware/freewilimain/rmpLib.\n",
        encoding="utf-8",
    )
    found = list(violations(tmp_path))
    assert found
    assert found[0][0] == pathlib.Path("docs/bad.md")


def test_non_public_files_are_not_scanned(tmp_path):
    (tmp_path / "AGENTS.md").write_text("freewili-firmware\n", encoding="utf-8")
    docs = tmp_path / "docs"
    docs.mkdir()
    (docs / "notes.txt").write_text("freewilimain\n", encoding="utf-8")
    assert list(violations(tmp_path)) == []
