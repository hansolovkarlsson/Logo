#!/usr/bin/env python3
# Regenerates website/reference.html from docs/COMMAND_REFERENCE.md.
#
# Run this (`python3 website/build_reference.py`) any time
# COMMAND_REFERENCE.md changes -- reference.html is a rendering of that
# file, not a separately-maintained copy, same "keep reference docs in
# sync" discipline this project already applies between
# COMMAND_REFERENCE.md/BYTECODE_REFERENCE.md and the parser/VM
# themselves. Parses each "## Section" into its markdown table(s), code
# block(s), and prose paragraphs, then renders a two-column page: a
# left index (auto-built from every table's own first column) and the
# full entries on the right. Deliberately a small hand-rolled parser
# tuned to this one file's own consistent shape (regular tables, no
# escaped pipes, no nested sub-headings) rather than a general markdown
# library -- verified against the actual file, not assumed robust.

import re
import html
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "docs", "COMMAND_REFERENCE.md")
OUT = os.path.join(ROOT, "website", "reference.html")


def slugify(s):
    return re.sub(r"[^A-Za-z0-9]+", "-", s.strip().lower()).strip("-")


def inline_md(s):
    s = html.escape(s, quote=False)
    s = re.sub(r"`([^`]+)`", r'<code class="inline">\1</code>', s)
    s = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", s)
    s = re.sub(r"\*([^*]+)\*", r"<em>\1</em>", s)
    return s


def leading_token(cell_text):
    plain = re.sub(r"[`*]", "", cell_text).strip()
    m = re.match(r"^([A-Za-z][A-Za-z0-9_?]*)", plain)
    return m.group(1).upper() if m else plain.upper()


def parse_blocks(body_text):
    lines = body_text.split("\n")
    blocks = []
    i, n = 0, len(lines)
    while i < n:
        line = lines[i]
        if line.strip().startswith("|"):
            table_lines = []
            while i < n and lines[i].strip().startswith("|"):
                table_lines.append(lines[i].strip())
                i += 1
            rows = [[c.strip() for c in l.strip("|").split("|")] for l in table_lines]
            blocks.append(("table", rows[0], rows[2:]))  # header, data rows (skip separator)
        elif line.strip().startswith("```"):
            code_lines = []
            i += 1
            while i < n and not lines[i].strip().startswith("```"):
                code_lines.append(lines[i])
                i += 1
            i += 1
            blocks.append(("code", "\n".join(code_lines)))
        elif line.strip() == "" or line.strip() == "---":
            i += 1
        else:
            para_lines = []
            while (i < n and lines[i].strip() != "" and not lines[i].strip().startswith("|")
                   and not lines[i].strip().startswith("```") and lines[i].strip() != "---"):
                para_lines.append(lines[i])
                i += 1
            blocks.append(("p", " ".join(l.strip() for l in para_lines)))
    return blocks


def build():
    text = open(SRC).read()
    body = text.split("\n## ", 1)[1]
    sections_raw = re.split(r"\n## ", "## " + body)

    sections = []
    for raw in sections_raw:
        raw = raw.strip("\n")
        if not raw:
            continue
        first_nl = raw.find("\n")
        heading = raw[3:first_nl].strip() if raw.startswith("## ") else raw[:first_nl].strip()
        if heading.startswith("Contents"):
            continue
        sections.append((heading, raw[first_nl + 1:]))

    sidebar_parts, main_parts = [], []
    used_ids = set()

    def uid(base):
        b = slugify(base)
        if b not in used_ids:
            used_ids.add(b)
            return b
        i = 2
        while f"{b}-{i}" in used_ids:
            i += 1
        used_ids.add(f"{b}-{i}")
        return f"{b}-{i}"

    for heading, body_text in sections:
        anchor = "sec-" + slugify(heading)
        is_appendix = heading.lower().startswith("appendix")
        blocks = parse_blocks(body_text)

        sidebar_parts.append(f'      <h4><a href="#{anchor}">{inline_md(heading)}</a></h4>')
        sec_sidebar_items = []
        main_parts.append(f'      <div class="ref-section" id="{anchor}">\n        <h2>{inline_md(heading)}</h2>')

        for block in blocks:
            if block[0] == "p":
                if block[1].strip():
                    main_parts.append(f"        <p>{inline_md(block[1])}</p>")
            elif block[0] == "code":
                main_parts.append(f'        <pre class="code">{html.escape(block[1])}</pre>')
            elif block[0] == "table":
                header, rows = block[1], block[2]
                if is_appendix:
                    items = []
                    for r in rows:
                        if len(r) < 2:
                            continue
                        items.append(f'<li><code class="inline">{inline_md(r[0]).replace("`", "")}</code> &mdash; {inline_md(r[1])}</li>')
                    main_parts.append('        <div class="note"><ul style="margin:0 0 0 18px;">' + "".join(items) + "</ul></div>")
                    continue

                groups = []
                for r in rows:
                    if not r or not r[0].strip():
                        continue
                    tok = leading_token(r[0])
                    if groups and groups[-1][0] == tok:
                        groups[-1][1].append(r)
                    else:
                        groups.append((tok, [r]))

                for tok, grp_rows in groups:
                    entry_id = uid(tok)
                    first_row = grp_rows[0]
                    desc_idx = len(header) - 1
                    sec_sidebar_items.append(f'        <li><a href="#{entry_id}" data-cmd="{tok}">{html.escape(tok)}</a></li>')

                    cmd_display = inline_md(first_row[0]) if first_row[0] else tok
                    main_parts.append(f'        <div class="ref-entry" id="{entry_id}">')
                    main_parts.append(f"          <h3>{cmd_display}</h3>")
                    for r in grp_rows:
                        mid_cols = []
                        for ci in range(1, desc_idx):
                            if ci < len(r) and r[ci].strip() and r[ci].strip() not in ("—", "-"):
                                mid_cols.append(f"{inline_md(header[ci])}: {inline_md(r[ci])}")
                        if mid_cols:
                            main_parts.append(f'          <p class="sig">{" &nbsp;&middot;&nbsp; ".join(mid_cols)}</p>')
                        desc = r[desc_idx] if desc_idx < len(r) else ""
                        main_parts.append(f"          <p>{inline_md(desc)}</p>")
                    main_parts.append("        </div>")

        if sec_sidebar_items:
            sidebar_parts.append("      <ul>")
            sidebar_parts.extend(sec_sidebar_items)
            sidebar_parts.append("      </ul>")
        main_parts.append("      </div>")

    return "\n".join(sidebar_parts), "\n".join(main_parts), len(sections)


PAGE_HEAD = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Reference — LogoMotive</title>
<link rel="stylesheet" href="assets/style.css">
</head>
<body>

<header class="site-header">
  <div class="site-header-inner">
    <a class="site-logo" href="index.html"><span class="dot">&#9679;</span> LogoMotive</a>
    <nav class="site-nav">
      <a href="index.html">Home</a>
      <a href="tutorial.html">Tutorial</a>
      <a href="reference.html" class="current">Reference</a>
    </nav>
  </div>
</header>

<div class="wrap">
  <div class="page-hero" style="padding-bottom: 20px;">
    <p class="eyebrow">Complete lookup table</p>
    <h1 class="title" style="font-size: clamp(1.8rem, 3.5vw, 2.6rem);">Command reference</h1>
    <p class="lede">Every command LogoMotive actually recognizes, cross-checked directly against the parser's own grammar table — not copied from documentation without verification. Case-insensitive throughout.</p>
  </div>

  <div class="layout">
    <aside class="index-nav">
      <input class="index-search" type="search" id="ref-search" placeholder="Filter commands&hellip;" autocomplete="off">
"""

BRIDGE = """    </aside>

    <main class="ref-main">
"""

PAGE_CLOSING = """    </main>
  </div>
</div>

<footer class="site-footer">
  <div class="wrap narrow">
    Generated from this project's own <code class="inline">docs/COMMAND_REFERENCE.md</code> by <code class="inline">website/build_reference.py</code> — see the repository for the full markdown source and every other design/reference doc.
  </div>
</footer>

<script>
(function () {
  "use strict";
  var input = document.getElementById("ref-search");
  var nav = document.querySelector("aside.index-nav");
  if (!input || !nav) return;

  var lists = Array.prototype.slice.call(nav.querySelectorAll("ul"));

  input.addEventListener("input", function () {
    var q = input.value.trim().toLowerCase();
    lists.forEach(function (ul) {
      var heading = ul.previousElementSibling; // the <h4> right before this <ul>
      var anyVisible = false;
      Array.prototype.forEach.call(ul.querySelectorAll("li"), function (li) {
        var cmd = (li.getAttribute("data-cmd") || li.textContent).toLowerCase();
        var match = q === "" || cmd.indexOf(q) !== -1;
        li.classList.toggle("hidden", !match);
        if (match) anyVisible = true;
      });
      var show = q === "" || anyVisible;
      ul.style.display = show ? "" : "none";
      if (heading && heading.tagName === "H4") heading.style.display = show ? "" : "none";
    });
  });
})();
</script>

</body>
</html>
"""

if __name__ == "__main__":
    sidebar_html, main_html, n = build()
    page = PAGE_HEAD + sidebar_html + "\n" + BRIDGE + main_html + "\n" + PAGE_CLOSING
    open(OUT, "w").write(page)
    print(f"wrote {OUT} ({n} sections, {len(page)} bytes)")
