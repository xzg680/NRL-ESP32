#!/usr/bin/env python3
"""Render the repository's bilingual README files as self-contained HTML.

Usage: python scripts/render_readme_html.py
"""

from __future__ import annotations

import html
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
CSS = r"""
:root{color-scheme:light dark;--bg:#f6f8fa;--card:#fff;--fg:#1f2328;--muted:#57606a;--line:#d0d7de;--accent:#0969da;--code:#f0f3f6}
@media(prefers-color-scheme:dark){:root{--bg:#0d1117;--card:#161b22;--fg:#e6edf3;--muted:#9198a1;--line:#30363d;--accent:#58a6ff;--code:#1b222b}}
*{box-sizing:border-box} body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.7 -apple-system,BlinkMacSystemFont,"Segoe UI","PingFang SC","Microsoft YaHei",Arial,sans-serif}
.layout{max-width:1240px;margin:auto;display:flex;gap:28px;padding:0 24px}.toc{position:sticky;top:0;width:230px;height:100vh;overflow:auto;padding:28px 0;flex:none}.toc b{color:var(--muted);font-size:12px;letter-spacing:.08em}.toc a{display:block;padding:5px 10px;color:var(--muted);text-decoration:none;border-left:2px solid transparent}.toc a:hover{color:var(--accent);border-color:var(--accent)}
main{min-width:0;flex:1;padding:42px 0 64px}article{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:42px;box-shadow:0 8px 24px #0000000d}h1,h2,h3{line-height:1.25;scroll-margin-top:20px}h1{font-size:2.15em;margin-top:0}h2{margin-top:2.2em;padding-top:.15em;border-top:1px solid var(--line)}h3{margin-top:1.5em}a{color:var(--accent)}code{background:var(--code);padding:.12em .35em;border-radius:4px;font-family:ui-monospace,Consolas,monospace;font-size:.9em}pre{overflow:auto;background:var(--code);padding:16px;border-radius:8px}pre code{padding:0;background:none}table{width:100%;border-collapse:collapse;display:block;overflow:auto}th,td{border:1px solid var(--line);padding:9px 12px;text-align:left;vertical-align:top}th{background:var(--code)}blockquote{margin:1em 0;padding:.2em 1em;border-left:4px solid var(--accent);background:var(--code);color:var(--muted)}img{max-width:100%;height:auto;border-radius:8px}footer{margin-top:36px;color:var(--muted);font-size:.9em}@media(max-width:820px){.layout{display:block;padding:0 12px}.toc{position:static;width:auto;height:auto;padding:16px 0}.toc a{display:inline-block}main{padding:12px 0 32px}article{padding:24px 18px}}
"""


def slug(text: str) -> str:
    return re.sub(r"[^a-z0-9\u4e00-\u9fff]+", "-", text.lower()).strip("-") or "section"


def inline(text: str) -> str:
    text = html.escape(text, quote=False)
    text = re.sub(r"!\[([^]]*)\]\(([^ )]+)\)", r'<img src="\2" alt="\1">', text)
    text = re.sub(r"\[([^]]+)\]\(([^ )]+)\)", r'<a href="\2">\1</a>', text)
    text = re.sub(r"`([^`]+)`", r"<code>\1</code>", text)
    text = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", text)
    return text


def render_list(lines: list[str], start: int, indent: int) -> tuple[str, int]:
    """Render one Markdown bullet-list level, preserving nested bullets."""
    items: list[str] = []
    i = start
    while i < len(lines):
        match = re.match(r"^(\s*)-\s+(.+)$", lines[i])
        if match is None or len(match.group(1)) != indent:
            break
        item = inline(match.group(2))
        i += 1
        nested = ""
        if i < len(lines):
            child = re.match(r"^(\s*)-\s+(.+)$", lines[i])
            if child is not None and len(child.group(1)) > indent:
                nested, i = render_list(lines, i, len(child.group(1)))
        items.append(f"<li>{item}{nested}</li>")
    return "<ul>" + "".join(items) + "</ul>", i


def render(source: Path, output: Path, language: str, title: str) -> None:
    lines = source.read_text(encoding="utf-8").splitlines()
    headings = [(m.group(2), slug(m.group(2))) for line in lines if (m := re.match(r"^(#{1,3})\s+(.+)$", line))]
    body: list[str] = []
    i = 0
    while i < len(lines):
        line = lines[i]
        if not line.strip():
            i += 1
            continue
        if line.startswith("```"):
            lang = line[3:].strip()
            code: list[str] = []
            i += 1
            while i < len(lines) and not lines[i].startswith("```"):
                code.append(lines[i]); i += 1
            body.append(f'<pre><code class="language-{lang}">{html.escape(chr(10).join(code))}</code></pre>')
        elif m := re.match(r"^(#{1,3})\s+(.+)$", line):
            level, text = len(m.group(1)), m.group(2)
            body.append(f'<h{level} id="{slug(text)}">{inline(text)}</h{level}>')
        elif line.startswith("|") and i + 1 < len(lines) and re.match(r"^\|?\s*:?-{3,}", lines[i + 1]):
            cells = lambda row: [inline(c.strip()) for c in row.strip().strip("|").split("|")]
            head = cells(line); i += 2; rows = []
            while i < len(lines) and lines[i].startswith("|"):
                rows.append(cells(lines[i])); i += 1
            table = "<table><thead><tr>" + "".join(f"<th>{x}</th>" for x in head) + "</tr></thead><tbody>"
            body.append(table + "".join("<tr>" + "".join(f"<td>{x}</td>" for x in row) + "</tr>" for row in rows) + "</tbody></table>")
            continue
        elif re.match(r"^(\s*)-\s+", line):
            marker = re.match(r"^(\s*)-\s+", line)
            assert marker is not None
            rendered, i = render_list(lines, i, len(marker.group(1)))
            body.append(rendered); continue
        elif re.match(r"^\d+\.\s+", line):
            items = []
            while i < len(lines) and re.match(r"^\d+\.\s+", lines[i]):
                items.append("<li>" + inline(re.sub(r"^\d+\.\s+", "", lines[i])) + "</li>"); i += 1
            body.append("<ol>" + "".join(items) + "</ol>"); continue
        elif line.startswith("> "):
            body.append(f"<blockquote><p>{inline(line[2:])}</p></blockquote>")
        elif line.startswith("<"):
            raw = [line]; i += 1
            while i < len(lines) and lines[i].strip(): raw.append(lines[i]); i += 1
            body.append("\n".join(raw)); continue
        else:
            paragraph = [line]; i += 1
            while i < len(lines) and lines[i].strip() and not re.match(r"^(#{1,3})\s+|```|\||> |<|\s*- |\d+\.\s+", lines[i]):
                paragraph.append(lines[i]); i += 1
            body.append(f"<p>{inline(' '.join(paragraph))}</p>"); continue
        i += 1
    toc = "".join(f'<a href="#{ident}">{html.escape(text)}</a>' for text, ident in headings if ident)
    page = f'<!doctype html><html lang="{language}"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>{html.escape(title)}</title><style>{CSS}</style></head><body><div class="layout"><nav class="toc"><b>CONTENTS</b>{toc}</nav><main><article>{"".join(body)}<footer>Generated from {source.name} · NRL ESP32 Radio Bridge</footer></article></main></div></body></html>'
    output.write_text(page, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    render(ROOT / "README.md", ROOT / "README.html", "zh-CN", "NRL ESP32 Radio Bridge · 中文说明")
    render(ROOT / "README.en.md", ROOT / "README.en.html", "en", "NRL ESP32 Radio Bridge · English Manual")
