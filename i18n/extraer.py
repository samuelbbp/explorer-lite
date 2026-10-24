#!/usr/bin/env python3
"""Collects the UI strings from src/*.cpp: what has to be translated (copied from Zas). They are
written in English, the source language. Writes i18n/fuente.json: { "string": "file:line" }.

A string counts when it is
  - a literal at the top level of the first argument of T(...) or Tf(...): T(L"Search"),
    Tf(L"{0} members", { n }), and every branch of a ternary there: T(on ? L"On" : L"Off");
  - or an L"..." with a letter on a line that ends with `// translate`: tables of strings that are
    translated later with T(variable), like `const wchar_t *tabs[] = { L"Media", L"Links" }; // translate`.
Comments are skipped (the T() examples in them are not strings).

    python3 i18n/extraer.py
"""
import bisect
import json
import re
from pathlib import Path

R = Path(__file__).resolve().parent.parent
# not UI text: fonts and the window title
NO = {"Segoe UI Variable Text", "Segoe UI", "Segoe Fluent Icons", "Segoe MDL2 Assets", "explorer-lite"}
MARK = re.compile(r"//\s*translate\s*$")


def tokens(text):
    """(kind, start, end, raw) for each piece of code: "lit" L"...", "str" "...", "chr" '...', "cmt", "ch" (one character)"""
    i, n = 0, len(text)
    while i < n:
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            yield "cmt", i, j, text[i:j]
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            yield "cmt", i, j, text[i:j]
            i = j
        elif text[i] in "\"'":
            q = text[i]
            wide = i > 0 and text[i - 1] == "L" and not (i > 1 and (text[i - 2].isalnum() or text[i - 2] == "_"))
            j = i + 1
            while j < n and text[j] != q and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            kind = "chr" if q == "'" else "lit" if wide else "str"
            yield kind, i - wide, j + 1, text[i + 1:j]
            i = j + 1
        else:
            yield "ch", i, i + 1, text[i]
            i += 1


def decode(raw):
    """the text of a C literal (the escapes that src uses)"""
    esc = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "'": "'", "0": "\0"}
    return re.sub(r"\\(U[0-9A-Fa-f]{8}|u[0-9A-Fa-f]{4}|x[0-9A-Fa-f]+|.)",
                  lambda m: chr(int(m.group(1)[1:], 16)) if len(m.group(1)) > 1 else esc.get(m.group(1), m.group(1)), raw)


def literals(text):
    """(line, raw) of every source string in one file"""
    toks = list(tokens(text))
    nl = [i for i, ch in enumerate(text) if ch == "\n"]
    line = lambda s: bisect.bisect_left(nl, s) + 1
    found = []
    # the first argument of T( and Tf(: its literals at depth 0 (not those inside a nested call)
    for k, (kind, s, e, raw) in enumerate(toks):
        if kind != "ch" or raw != "(" or not re.search(r"(?<![\w.>:])Tf?\s*$", text[max(0, s - 8):s]):
            continue
        depth = 0
        for kind2, s2, e2, raw2 in (toks[j] for j in range(k + 1, len(toks))):
            if kind2 == "ch" and raw2 in "([{":
                depth += 1
            elif kind2 == "ch" and raw2 in ")]}":
                if depth == 0:
                    break
                depth -= 1
            elif kind2 == "ch" and raw2 == "," and depth == 0:
                break
            elif kind2 == "lit" and depth == 0:
                found.append((line(s2), s2, raw2))
    # the lines marked // translate: all their literals
    marked = {line(s) for kind, s, e, raw in toks if kind == "cmt" and MARK.search(raw)}
    found += [(line(s), s, raw) for kind, s, e, raw in toks if kind == "lit" and line(s) in marked]
    return [(n, raw) for n, s, raw in sorted(found)]


def collect():
    """{ string: "file:line" } of src/*.cpp, in order of appearance"""
    out = {}
    for f in sorted((R / "src").glob("*.cpp")):
        text = f.read_text(encoding="utf-8")
        for n, raw in literals(text):
            t = decode(raw)
            if t in NO or not any(ch.isalpha() for ch in t):
                continue
            out.setdefault(t, f"{f.name}:{n}")
    return out


if __name__ == "__main__":
    frases = collect()
    (R / "i18n/fuente.json").write_text(json.dumps(frases, ensure_ascii=False, indent=1), encoding="utf-8")
    print(len(frases), "strings")
