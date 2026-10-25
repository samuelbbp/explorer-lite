#!/usr/bin/env python3
"""Checks translations: python3 i18n/comprobar.py es  (or several codes; copied from Zas).
Every UI string in src (what extraer.py finds, the list in i18n/fuente.json) has to be
there, and each translation has to keep the holes ({0}, %d, %.2f...), the tabs and the leading and
trailing spaces. English is the source language: it has no file and nothing to check."""
import json, re, sys
from pathlib import Path
sys.dont_write_bytecode = True  # no __pycache__ in i18n
from extraer import collect  # noqa: E402

R = Path(__file__).resolve().parent.parent
SOURCE = "en"
fuente = collect()
mal = 0
guardada = json.loads((R / "i18n/fuente.json").read_text(encoding="utf-8"))
if set(guardada) != set(fuente):
    print(f"fuente.json is out of date ({len(guardada)} strings, the code has {len(fuente)}): run python3 i18n/extraer.py")
    mal += 1
for codigo in sys.argv[1:]:
    if codigo == SOURCE:
        print(f"{codigo}: the source language, nothing to check"); continue
    f = R / "i18n" / f"{codigo}.json"
    try:
        tr = json.loads(f.read_text(encoding="utf-8"))
    except Exception as e:
        print(f"{codigo}: cannot read it ({e})"); mal += 1; continue
    faltan = [k for k in fuente if k not in tr or not str(tr[k]).strip()]
    sobran = [k for k in tr if k not in fuente]
    problemas = []
    huecos = lambda s: sorted(re.findall(r"\{\d\}|%[+-]?\d*(?:\.\d+)?[sdf%]", s))
    for k, v in tr.items():
        if k not in fuente: continue
        if huecos(k) != huecos(v): problemas.append(f"holes: {k!r} -> {v!r}")
        if k.count("\t") != v.count("\t"): problemas.append(f"tab: {k!r} -> {v!r}")
        if (k[:1] == " ") != (v[:1] == " ") or (k[-1:] == " ") != (v[-1:] == " "): problemas.append(f"spaces: {k!r} -> {v!r}")
    print(f"{codigo}: {len(tr)} strings, missing {len(faltan)}, extra {len(sobran)}, problems {len(problemas)}")
    for x in (faltan[:5] + problemas[:10]): print("   ", x if isinstance(x, str) else x)
    mal += bool(faltan or problemas)
sys.exit(1 if mal else 0)
