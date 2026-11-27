#!/usr/bin/env python3
"""Build docs/pdfs/Codeline-Complete.pdf: every Markdown document in the repo as one multi-chapter book.

Title page, contents with page numbers, Parts and Chapters, PDF bookmarks. Links between documents
become jumps inside the book; links to other repo files go to GitHub.

Pipeline: pandoc (GFM -> HTML fragment per document) -> one HTML book -> headless Chromium (Playwright)
-> PDF. Chromium is run twice: the first pass finds the page each chapter starts on, the second
writes those page numbers into the contents.

Install (one time)
-----------------
1. pandoc (the Markdown -> HTML converter), on your PATH:
       macOS:          brew install pandoc
       Debian/Ubuntu:  sudo apt-get install pandoc
       Windows:        winget install --id JohnMacFarlane.Pandoc   (or the installer from pandoc.org)
2. The Python packages (a virtual environment is recommended):
       python3 -m pip install playwright pypdf
3. The headless Chromium that Playwright drives:
       python3 -m playwright install chromium
   (Skip this if you already have Chrome/Chromium and pass it with --chromium instead.)

Run
---
From anywhere in the repository:
       python3 tools/build_docs_pdf.py
This overwrites docs/pdfs/Codeline-Complete.pdf; commit it alongside the Markdown changes.

Options:
       --out PATH        write the PDF somewhere else
       --chromium PATH   use this Chrome/Chromium executable instead of Playwright's
                         (e.g. "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome")

Adding or reordering documents: edit PARTS below. Any Markdown file in the repo that is not listed
there is reported as a note when the script runs and left out of the book.
"""
from __future__ import annotations

import argparse
import datetime
import html as htmllib
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from urllib.parse import unquote

from pypdf import PdfReader, PdfWriter
from pypdf.generic import NameObject, TextStringObject

REPO = Path(__file__).resolve().parent.parent
OUT = REPO / "docs" / "pdfs" / "Codeline-Complete.pdf"
GITHUB = "https://github.com/plocher/Codeline/blob/main/"

# The book's structure: (part title or None, [documents in chapter order]).
# A Markdown document not listed here is reported and left out.
PARTS: list[tuple[str | None, list[str]]] = [
    (None, ["README.md"]),
    ("Part I — Overview", [
        "docs/Studio.md", "docs/Codeline.md", "docs/SmartAppliances.md",
        "docs/BuildingAppliances.md", "docs/Walkthrough.md", "docs/Background.md"]),
    ("Part II — Reference", [
        "docs/codeline/README.CodeLine.md", "docs/clusters/README.Clusters.md",
        "docs/appliances/README.Appliances.md", "docs/bindings/Cpp.md",
        "docs/bindings/MQTT.md", "docs/bindings/CodeLineMQTT.md"]),
    ("Part III — Roadmap & Research", [
        "docs/Roadmap.md", "docs/research/cats-ctc-research.md",
        "docs/research/engine-closed-loop-spike.md"]),
]

CSS = """
body{font-family:-apple-system,"Segoe UI",Helvetica,Arial,sans-serif;font-size:11pt;line-height:1.5;color:#1f2328;margin:0}
h1,h2,h3,h4,h5,h6{page-break-after:avoid;margin-top:1.2em}
img{max-width:100%;page-break-inside:avoid}
code{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:85%;background:#eff1f3;padding:.1em .3em;border-radius:4px}
pre{background:#f6f8fa;padding:12px;border-radius:6px;overflow:hidden;white-space:pre-wrap;font-size:85%}
pre code{background:none;padding:0}
table{border-collapse:collapse;margin:1em 0;font-size:95%}
th,td{border:1px solid #d1d9e0;padding:4px 10px}
tr:nth-child(even){background:#f6f8fa}
blockquote{border-left:4px solid #d1d9e0;color:#59636e;margin:0;padding:0 1em}
a{color:#0969da;text-decoration:none}
hr{border:0;border-top:1px solid #d1d9e0}
.title{height:8.5in;display:flex;flex-direction:column;justify-content:center;align-items:center;text-align:center}
.title img{width:4in}.title .tt{font-size:32pt;font-weight:600;margin:.4em 0 .1em}.title p{color:#59636e;font-size:13pt}
.toc{break-before:page}.toc .tt{font-size:2em;font-weight:600;border-bottom:1px solid #d1d9e0;margin:.67em 0}
.toc ol{list-style:none;padding:0}.toc li{display:flex;margin:.25em 0}
.toc li a{flex:1;display:flex;color:#1f2328}.toc li a::after{content:"";flex:1;border-bottom:1px dotted #aaa;margin:0 .4em .3em}
.toc .pt{font-weight:600;margin-top:1em}.toc .pg{min-width:2em;text-align:right}
.part{break-before:page;display:flex;flex-direction:column;justify-content:center;height:8in;text-align:center}
.part h1{border:0;font-size:28pt}
.chapter{break-before:page}
.chapnum{color:#59636e;font-size:11pt;text-transform:uppercase;letter-spacing:.1em;margin-bottom:-.6em}
.chapter h2.ct{font-size:2em;border-bottom:1px solid #d1d9e0;margin-top:.8em}
.chapter h3{font-size:1.6em;border-bottom:1px solid #d1d9e0}.chapter h4{font-size:1.3em;border-bottom:1px solid #d1d9e0}
.chapter h5{font-size:1.1em}.chapter h6{font-size:1em}
.sources{color:#59636e;font-size:9pt}
"""

PDF_OPTIONS = dict(
    format="Letter", print_background=True, outline=True, tagged=True,
    margin=dict(top="0.7in", bottom="0.7in", left="0.7in", right="0.7in"),
    display_header_footer=True, header_template="<span></span>",
    footer_template='<div style="font-size:8pt;width:100%;text-align:center;color:#888">'
                    '<span class="pageNumber"></span></div>',
)


def text_of(fragment: str) -> str:
    """Plain text of an HTML fragment, entities decoded and whitespace collapsed."""
    return re.sub(r"\s+", " ", htmllib.unescape(re.sub(r"<[^>]+>", "", fragment))).strip()


def chapter_html(src: Path, chap_id: str, chap_ids: dict[Path, str]) -> tuple[str, str]:
    """Render one document as a chapter body; return (title, html)."""
    h = subprocess.run(["pandoc", "-f", "gfm", "-t", "html5", "--mathml", "--id-prefix", chap_id + "-", str(src)],
                       check=True, capture_output=True, text=True).stdout
    first = re.search(r"<h1[\s>]", h)
    if first:
        h = h[first.start():]                      # drop the per-file logo and navigation bar
    m = re.search(r"<h1[^>]*>(.*?)</h1>", h, re.S)
    title = text_of(m.group(1)) if m else src.stem

    def relink(m: re.Match) -> str:
        attr, url = m.group(1), m.group(2)
        if re.match(r"(https?:|mailto:|#)", url):
            return m.group(0)
        path, _, frag = url.partition("#")
        target = (src.parent / unquote(path)).resolve()
        if attr == "src":
            return f'src="{target.as_uri()}"'
        if target in chap_ids:
            return f'href="#{chap_ids[target]}{"-" + frag if frag else ""}"'
        if target.exists():
            return f'href="{GITHUB}{target.relative_to(REPO).as_posix()}{"#" + frag if frag else ""}"'
        print(f"warning: unresolved link in {src.relative_to(REPO)}: {url}", file=sys.stderr)
        return m.group(0)

    h = re.sub(r'(src|href)="([^"]+)"', relink, h)
    h = re.sub(r"<br\s*/?>", " <br>", h, flags=re.I)                 # keep a space in bookmark text
    h = re.sub(r"<(/?)h([1-6])\b", lambda m: f"<{m.group(1)}h{min(int(m.group(2)) + 2, 6)}", h)  # nest under Part/Chapter
    h = re.sub(r"<h3([^>]*)>(.*?)</h3>", r'<h2 class="ct"\1>\2</h2>', h, count=1, flags=re.S)
    return title, h


def build_html(chapters: dict[str, tuple[str, str, str]], pages: dict[str, int]) -> str:
    toc, body, n = [], [], 0
    for pi, (part, docs) in enumerate(PARTS):
        if part:
            toc.append(f'<li class="pt"><a href="#part{pi}">{part}</a><span class="pg">{pages.get(f"part{pi}", "")}</span></li>')
            body.append(f'<section class="part" id="part{pi}"><h1>{part}</h1></section>')
        for doc in docs:
            n += 1
            cid, title, h = chapters[doc]
            toc.append(f'<li><a href="#{cid}">{n}. {htmllib.escape(title)}</a><span class="pg">{pages.get(cid, "")}</span></li>')
            body.append(f'<section class="chapter" id="{cid}"><div class="chapnum">Chapter {n}</div>{h}'
                        f'<p class="sources">Source: {doc}</p></section>')
    # Chromium only records named destinations for in-document link targets; the contents links
    # cover chapters and parts, which is what the page-number pass needs.
    logo = (REPO / "docs" / "images" / "codeline-logo.svg").as_uri()
    return f"""<!doctype html><html><head><meta charset="utf-8"><title>Codeline</title><style>{CSS}</style></head><body>
<section class="title"><img src="{logo}"><div class="tt">Codeline</div><p>Collected Documentation</p>
<p>{datetime.date.today().isoformat()}</p></section>
<section class="toc"><div class="tt">Contents</div><ol>{"".join(toc)}</ol></section>
{"".join(body)}</body></html>"""


def fix_bookmarks(pdf: Path, html: str) -> None:
    """Chromium drops the space where a heading wraps; restore bookmark titles from the real headings."""
    key = lambda t: re.sub(r"\s+", "", t)
    heads = {key(t): t for t in (text_of(m) for m in re.findall(r"<h[1-6][^>]*>(.*?)</h[1-6]>", html, re.S))}
    w = PdfWriter(clone_from=pdf)

    def walk(node):
        item = node.get("/First")
        while item is not None:
            it = item.get_object()
            title = str(it["/Title"])
            if heads.get(key(title), title) != title:
                it[NameObject("/Title")] = TextStringObject(heads[key(title)])
            walk(it)
            item = it.get("/Next")

    if "/Outlines" in w._root_object:
        walk(w._root_object["/Outlines"].get_object())
    w.write(pdf)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, default=OUT, help=f"output PDF (default: {OUT.relative_to(REPO)})")
    ap.add_argument("--chromium", help="Chromium executable, if not the one Playwright installed")
    args = ap.parse_args()

    listed = [d for _, docs in PARTS for d in docs]
    for md in sorted(REPO.glob("**/*.md")):
        rel = md.relative_to(REPO).as_posix()
        if rel not in listed and not any(p.startswith(".") or p == "node_modules" for p in md.parts):
            print(f"note: {rel} is not in PARTS and is left out of the book", file=sys.stderr)

    chap_ids = {(REPO / d).resolve(): f"ch{i + 1}" for i, d in enumerate(listed)}
    chapters = {}
    for d in listed:
        cid = chap_ids[(REPO / d).resolve()]
        title, h = chapter_html(REPO / d, cid, chap_ids)
        chapters[d] = (cid, title, h)

    from playwright.sync_api import sync_playwright
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with sync_playwright() as p, tempfile.TemporaryDirectory() as tmp:
        browser = p.chromium.launch(executable_path=args.chromium)
        page = browser.new_page()
        book = Path(tmp) / "book.html"
        pages: dict[str, int] = {}
        for _ in range(2):                      # pass 1 finds chapter pages, pass 2 prints them in the contents
            html = build_html(chapters, pages)
            book.write_text(html, encoding="utf-8")
            page.goto(book.as_uri(), wait_until="networkidle")
            page.pdf(path=str(args.out), **PDF_OPTIONS)
            r = PdfReader(args.out)
            pages = {k.lstrip("/"): r.get_destination_page_number(v) + 1
                     for k, v in r.named_destinations.items() if re.fullmatch(r"/?(ch|part)\d+", k)}
        browser.close()
    fix_bookmarks(args.out, html)
    print(f"wrote {args.out.relative_to(REPO) if args.out.is_relative_to(REPO) else args.out}: "
          f"{len(listed)} chapters, {len(PdfReader(args.out).pages)} pages")


if __name__ == "__main__":
    main()
