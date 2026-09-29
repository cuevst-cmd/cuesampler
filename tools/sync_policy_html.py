#!/usr/bin/env python3
"""Render the canonical policy Markdown into the existing Gumroad page sections."""
import argparse
import html
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def inline(text):
    text = html.escape(text)
    text = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", text)
    text = re.sub(r"(?<!\*)\*([^*]+)\*(?!\*)", r"<em>\1</em>", text)
    def link(match):
        url = match[0].rstrip(".,;")
        trailing = match[0][len(url):]
        return f'<a href="{url}" target="_blank" rel="noopener noreferrer">{url}</a>{trailing}'
    return re.sub(r"https://[^\s<]+", link, text)


def render_markdown(source):
    result = []
    for block in re.split(r"\n\s*\n", source.strip()):
        if block == "---":
            continue
        if block.startswith("## "):
            result.append("<h3>" + inline(block[3:]) + "</h3>")
        elif block.startswith("# "):
            result.append("<p><strong>" + inline(block[2:]) + "</strong></p>")
        elif block.startswith("- "):
            result.append("<ul>" + "".join("<li>" + inline(line[2:]) + "</li>"
                                          for line in block.splitlines()) + "</ul>")
        else:
            result.append("<p>" + inline(block).replace("\n", "<br>") + "</p>")
    return "\n        ".join(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    text = args.profile.read_text(encoding="utf-8")
    for section, filename, title in (("eula", "EULA.md", "EULA"),
                                     ("privacy", "PRIVACY_POLICY.md", "Privacy")):
        source = ROOT.joinpath(filename).read_text(encoding="utf-8")
        date = re.search(r"Last updated: ([^*\n]+)", source)[1]
        replacement = f'''<section id="{section}" class="legal" aria-labelledby="{section}-h">
    <div class="wrap">
      <div class="sec-head">
        <h1 class="all" id="{section}-h">{title}</h1>
        <p class="sec-caption">CUE Software<br>Last updated: {date}</p>
      </div>
      <div class="legal-body">
        {render_markdown(source)}
      </div>
    </div>
  </section>'''
        text, count = re.subn(r'<section id="' + section + r'"[^>]*>.*?</section>',
                              lambda _: replacement, text, flags=re.S)
        if count != 1:
            raise ValueError(f"Expected exactly one {section} section; found {count}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    main()
