#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 fm-1-chimera-vocal contributors
"""Make the firmware's site: fm1-base's site (base/tools/make_site.py: the front page, the web installer, the manual,
the package, m-bootloader and the licences) with CHIMERA's credit at the top of every page. The browser emulator is
included when it has been built (base/tools/build.py app --web, which needs Emscripten), left out otherwise.

  tools/make_site.py VERSION [OUT_DIR]     (default OUT_DIR: app/build/site)

It builds the release package itself (base/tools/build.py app --release VERSION): its identity, FM-1_042 and the
version as six digits, is what the installer checks the download against and what the FM-1 reports after the
install. Serve OUT_DIR on localhost (Web MIDI needs a secure context; localhost is one) and open install/ in Chrome
or Edge:

  python3 -m http.server 8042 --bind 127.0.0.1 --directory app/build/site"""
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "app"
BASE = ROOT / "base"
sys.path.insert(0, str(BASE / "tools"))
import make_site as base_site  # noqa: E402  (fm1-base's, MPL-2.0)

CREDIT = ('<p class="credit"><strong>A port of <a href="https://github.com/jamesondh/chimera-vocal-synth">CHIMERA</a>'
          ' by Jameson Hodge</strong> (MIT licence) to the FM-1: the voice is his. Built on '
          '<a href="https://github.com/charlesvestal/fm1-base">fm1-base</a> by Charles Vestal and '
          '<a href="https://github.com/hugelton/felucca-bsp">felucca-bsp</a> by Leo Kuroshita, Hügelton Instruments. '
          'Unofficial: not affiliated with M-VAVE or with CHIMERA\'s author.</p>')
CREDIT_CSS = (".credit { border-left: 3px solid var(--accent); padding: 4px 0 4px 12px; margin: 0 0 20px; "
              "font-size: 14px; }")


def build_release(version):
    env = dict(os.environ)
    env.setdefault("JIELI_TOOLCHAIN", str(Path.home() / ".jieli" / "toolchain"))
    sdk = env.get("AC79_SDK", str(Path.home() / "fw-AC79_AIoT_SDK"))
    subprocess.run([sys.executable, str(BASE / "tools" / "build.py"), str(APP), "--release", version, "--sdk", sdk],
                   check=True, env=env)
    return APP / "build" / f"vocal-{version}.fwsc"


def credit_installer(path):
    """the credit right under the installer's title (fm1-base's page credits its sources in the footer)"""
    html = path.read_text(encoding="utf-8")
    title = re.search(r"<h1>[^<]*</h1>\n", html)
    if not title or "</style>" not in html:
        raise SystemExit(f"{path}: no <h1> or <style> to put the credit by (fm1-base's installer.html changed?)")
    html = html[:title.end()] + "  " + CREDIT + "\n" + html[title.end():]
    html = html.replace("</style>", "  " + CREDIT_CSS + "\n</style>", 1)
    path.write_text(html, encoding="utf-8")


def main(version, out):
    pkg = build_release(version)
    out = Path(out).resolve()
    emu = APP / "build" / "emu"
    made_emu = False
    if not (emu / "app.wasm").exists():   # fm1-base's site wants the emulator: an empty stand-in, removed after
        emu.mkdir(parents=True, exist_ok=True)
        (emu / "app.wasm").write_bytes(b"")
        made_emu = True
    try:
        base_site.main(APP, pkg, version, out)
    finally:
        if made_emu:
            (emu / "app.wasm").unlink()
            if not any(emu.iterdir()):
                emu.rmdir()
    if made_emu:
        import shutil
        shutil.rmtree(out / "emu")
        index = out / "index.html"
        index.write_text(re.sub(r"<li>[^\n]*?href=\"emu/\".*?</li>\n?", "", index.read_text(encoding="utf-8"),
                                flags=re.S), encoding="utf-8")
    credit_installer(out / "install" / "index.html")
    print(f"site: {out} (credit added{'; no emulator: Emscripten not set up' if made_emu else ''})")
    print(f"serve it: python3 -m http.server 8042 --bind 127.0.0.1 --directory {out}")
    print("then open http://127.0.0.1:8042/install/ in Chrome or Edge")


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2] if len(sys.argv) == 3 else APP / "build" / "site")
