#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Put the WebAssembly inside the page (base64), so it needs nothing next to it.

  web/emu/make_page.py MNM.wasm PAGE.html OUTDIR
    -> OUTDIR/index.html  the page as a fragment (an artifact host adds the document around it)
       OUTDIR/MNM-1.html  a whole document, to open from disk
"""
import base64
import sys
from pathlib import Path

HEAD = """<!doctype html>
<html lang="it">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
"""


def main(wasm, page, out):
    out = Path(out)
    src = Path(page).read_text()
    if src.count('"__WASM_B64__"') != 1:
        raise SystemExit("make_page: the page has no __WASM_B64__ placeholder")
    frag = src.replace('"__WASM_B64__"', '"' + base64.b64encode(Path(wasm).read_bytes()).decode() + '"')
    (out / "index.html").write_text(frag)
    head, body = frag.split("</style>", 1)
    (out / "MNM-1.html").write_text(HEAD + head + "</style>\n</head>\n<body>" + body + "\n</body>\n</html>\n")
    print(f"emu: {out / 'index.html'}, {out / 'MNM-1.html'} ({len(frag) // 1024} KiB)")


if __name__ == "__main__":
    main(*sys.argv[1:4])
