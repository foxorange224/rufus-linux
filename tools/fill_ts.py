#!/usr/bin/env python3
"""Fill unfinished translations in a .ts file from a Python dict.

Usage: fill_ts.py <file.ts> <translations.py>

translations.py defines TRANSLATIONS = { "<context>::<source>": "<translation>", ... }
Messages already finished are left untouched. Newlines inside the source are
reproduced in the translation (literal \n escapes stay escaped, real
newlines stay real).
"""
import re
import sys
import xml.etree.ElementTree as ET


def xml_escape(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def decode_text(raw):
    return ET.fromstring(f"<t>{raw}</t>").text


def load_dict(path):
    ns = {}
    exec(open(path, encoding="utf-8").read(), ns)
    return ns["TRANSLATIONS"]


def main():
    ts_path, dict_path = sys.argv[1], sys.argv[2]
    translations = load_dict(dict_path)
    text = open(ts_path, encoding="utf-8").read()

    msg_re = re.compile(r"<message>(.*?)</message>", re.S)
    context_re = re.compile(r"<context>.*?<name>(.*?)</name>", re.S)

    def ctx_for(pos):
        best = None
        for c in context_re.finditer(text):
            if c.start() < pos:
                best = c
            else:
                break
        return best.group(1) if best else "?"

    replaced = 0
    missing = set()
    while True:
        blocks = list(msg_re.finditer(text))
        done = True
        for b in blocks:
            block = b.group(1)
            if 'type="unfinished"' not in block:
                continue
            ctx = ctx_for(b.start())
            src_m = re.search(r"<source>(.*?)</source>", block, re.S)
            if not src_m:
                continue
            source = decode_text(src_m.group(1))
            key = f"{ctx}::{source}"
            if key not in translations:
                missing.add(key)
                continue
            tr = translations[key]
            if "\\n" in src_m.group(1):
                tr = tr.replace("\n", "\\n")
            tr_esc = xml_escape(tr)
            tr_start = block.find("<translation")
            tr_end = block.find("</translation>") + len("</translation>")
            new_block = block[:tr_start] + "<translation>" + tr_esc + "</translation>" + block[tr_end:]
            text = text[: b.start()] + "<message>" + new_block + "</message>" + text[b.end() :]
            replaced += 1
            done = False
            break
        if done:
            break

    open(ts_path, "w", encoding="utf-8").write(text)
    print(f"Replaced {replaced} translations in {ts_path}")
    if missing:
        print(f"MISSING ({len(missing)}):")
        for k in sorted(missing):
            print(" ", k.replace("\n", "\\n"))


if __name__ == "__main__":
    main()