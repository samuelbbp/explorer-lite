#!/usr/bin/env python3
"""Writes the translation table of explorer-lite from i18n/<code>.json (the only source; copied from
Zas, without the Lua and installer parts):

  - src/i18n_table.inc   the list of languages and the strings that appear in src/*.cpp

    python3 i18n/generar.py

The UI strings are written in English in the code (T(L"..."), Tf(L"...")): English is the source and
has no file. Each other <code>.json is { "English string": "translation" }; the strings come from the
code (i18n/extraer.py, also listed in i18n/fuente.json). A template has holes {0}, {1}...
that the translation has to keep. To add a language: its entry in lista.json and its <code>.json
(python3 i18n/comprobar.py <code> tells what is missing).
"""
import json
import re
import sys
from pathlib import Path
sys.dont_write_bytecode = True  # no __pycache__ in i18n
from extraer import collect  # noqa: E402

R = Path(__file__).resolve().parent.parent
LISTA = json.loads((R / "i18n/lista.json").read_text(encoding="utf-8"))
SOURCE = "en"  # the language of the strings in the code: no table


def c_str(s):
    return 'L"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\t", "\\t") + '"'


def main():
    fuente = collect()
    tablas, problemas = [], []
    for l in LISTA:
        codigo = l["codigo"]
        if codigo == SOURCE:
            continue
        f = R / "i18n" / f"{codigo}.json"
        if not f.exists():
            problemas.append(f"{codigo}: no {f.name}")
            continue
        tr = json.loads(f.read_text(encoding="utf-8"))
        # a translation has to keep the holes of its string
        for src, t in tr.items():
            if sorted(re.findall(r"\{\d\}|%[sd%]|%\.\d?f", src)) != sorted(re.findall(r"\{\d\}|%[sd%]|%\.\d?f", t)):
                problemas.append(f"{codigo}: different holes in {src!r} -> {t!r}")
        # only the strings that are in the code
        de_cpp = sorted(src for src in tr if src in fuente)
        tablas.append((codigo, [(src, tr[src]) for src in de_cpp]))
    out = ["// Written by i18n/generar.py from i18n/*.json: do not edit by hand.", "",
           "static const UiLanguage kLanguages[] = {"]
    for l in LISTA:
        out.append(f'    {{ "{l["codigo"]}", {c_str(l["nombre"])} }},')
    out.append("};")
    for codigo, pares in tablas:
        nombre = "k_" + codigo.replace("-", "_")
        out.append(f"static const Pair {nombre}[] = {{")
        for src, t in pares:
            out.append(f"    {{ {c_str(src)}, {c_str(t)} }},")
        if not pares:
            out.append('    { L"", L"" },')
        out.append("};")
    out.append("static const Table kTables[] = {")
    for codigo, pares in tablas:
        nombre = "k_" + codigo.replace("-", "_")
        out.append(f'    {{ "{codigo}", {nombre}, sizeof({nombre}) / sizeof({nombre}[0]) }},')
    out.append("};")
    (R / "src/i18n_table.inc").write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"{len(tablas)} languages with a table; " + (f"{len(problemas)} problems" if problemas else "no problems"))
    for p in problemas[:40]:
        print("  " + p)
    return 1 if any("holes" in p for p in problemas) else 0


if __name__ == "__main__":
    sys.exit(main())
