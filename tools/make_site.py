#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Shan-Sherif
"""Make the firmware's site: fm1-base's site (base/tools/make_site.py: the front page, the web installer, the manual,
the package, m-bootloader and the licences) with CHIMERA's credit at the top of every page. The browser emulator is
included when it has been built (base/tools/build.py app --web, which needs Emscripten), left out otherwise.

  tools/make_site.py [VERSION [OUT_DIR]]   (VERSION: fm1.toml's version; OUT_DIR: app/build/site)

It builds the release package itself (base/tools/build.py app --release VERSION): its identity, FM-1_042 and the
version as six digits, is what the installer checks the download against and what the FM-1 reports after the
install. Serve OUT_DIR on localhost (Web MIDI needs a secure context; localhost is one) and open install/ in Chrome
or Edge:

  python3 -m http.server 8042 --bind 127.0.0.1 --directory app/build/site"""
import json
import os
import re
import shutil
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
# the installer in CHIMERA's vector-display palette, as the Studio (app/web/studio.html): black, blue for the text and
# the frame, green for the actions, red for what went wrong. After fm1-base's own rules (same specificity: these win),
# whatever light / dark the browser asks for
THEME_CSS = """
  /* VOCAL: CHIMERA's palette (src/ui/colors.ts), as the Studio */
  :root, :root[data-theme], :root:not([data-theme="light"]) {
    color-scheme: dark; --bg: #000; --fg: #e2f5fc; --dim: #3487c4; --accent: #4fb6f2; --on-accent: #000;
    --bad: #ff5d40; --focus: #5be381; --frame: #133954; --blue-dim: #1f5f8c; --green: #5be381; --white: #eef6f8;
  }
  body { font: 15px/1.6 ui-sans-serif, system-ui, -apple-system, "Segoe UI", sans-serif; }
  h1 { color: var(--accent); letter-spacing: .08em; text-transform: uppercase; font-size: 26px; }
  h2 { color: var(--accent); letter-spacing: .06em; text-transform: uppercase; font-size: 15px; }
  a { color: var(--accent); }
  button { background: #000; color: var(--green); border: 1px solid var(--green); border-radius: 3px;
           padding: 7px 16px; font-size: 13px; letter-spacing: .06em; text-transform: uppercase; }
  button:hover:not(:disabled) { color: var(--white); border-color: var(--white); }
  button:disabled { opacity: 1; color: var(--blue-dim); border-color: var(--frame); }
  button.plain { color: var(--accent); border-color: var(--dim); }
  .lang { border: 0; text-transform: none; letter-spacing: 0; color: var(--dim); }
  #status { color: var(--fg); }
  progress { accent-color: var(--green); }
  #log, textarea { border-color: var(--frame); }
  .backup { border: 1px solid var(--accent); border-radius: 4px; }
  dialog { background: #000; border: 1px solid var(--dim); border-radius: 4px; box-shadow: 0 0 40px rgba(79, 182, 242, .18); }
  input[type=file]::file-selector-button { background: #000; color: var(--accent); border: 1px solid var(--dim);
           border-radius: 3px; padding: 5px 10px; font: inherit; text-transform: uppercase; letter-spacing: .06em; }
  .credit { border-left-color: var(--accent); }
  /* the site's top bar, as the other pages */
  .site { display: flex; flex-wrap: wrap; align-items: baseline; gap: .3rem 1.2rem; margin: 0 0 1.4rem;
          padding-bottom: .5rem; border-bottom: 1px solid var(--frame); }
  .site .home { color: var(--accent); font-weight: 600; letter-spacing: .1em; text-decoration: none; }
  .site nav { margin-left: auto; display: flex; flex-wrap: wrap; gap: .2rem 1rem; font-size: 14px; }
  .site nav a { color: var(--fg); text-decoration-color: var(--dim); }
  .site nav a[aria-current] { color: var(--accent); text-decoration: none; }
"""
INSTALL_NAV = ('<header class="site"><a class="home" href="../">VOCAL_NAME</a><nav><a href="../studio/">Studio</a>'
               '<a href="./" aria-current="page">Install</a><a href="../manual/">Manual</a>'
               '<a href="https://github.com/shan-sherif/fm-1-chimera-vocal">Source</a></nav></header>')


def build_release(version):
    env = dict(os.environ)
    env.setdefault("JIELI_TOOLCHAIN", str(Path.home() / ".jieli" / "toolchain"))
    sdk = env.get("AC79_SDK", str(Path.home() / "fw-AC79_AIoT_SDK"))
    # the JieLi toolchain runs in Docker (an x86 image under emulation): now and then one of its tools crashes there
    # (a segfault, "objdump failed") with nothing wrong in the build; the same build again goes through
    for attempt in range(3):
        r = subprocess.run([sys.executable, str(BASE / "tools" / "build.py"), str(APP), "--release", version, "--sdk", sdk],
                           env=env)
        if r.returncode == 0:
            break
        print(f"make_site: the firmware build failed (try {attempt + 1} of 3)" + (": again" if attempt < 2 else ""))
    else:
        raise SystemExit("make_site: the firmware build failed three times")
    return APP / "build" / f"chimera-{version}.fwsc"


# the front page and the manual (fm1-base's "paper and ink" sheet) in the same palette: paper black, ink the Studio's
# text, the vermilion CHIMERA's blue, the mustard asides amber
DOCS_CSS = """
/* VOCAL: CHIMERA's palette (src/ui/colors.ts), as the Studio and the installer */
:root {
  --paper: #000; --sheet: #07141e; --rule: #133954; --ink: #e2f5fc; --muted: #3487c4;
  --hot: #4fb6f2; --hot-t: #0b2233; --cool: #ffb340; --cool-t: #1d1404; --hot-ink: #4fb6f2;
  color-scheme: dark;
}
body { font: 400 16px/1.6 ui-sans-serif, system-ui, -apple-system, "Segoe UI", sans-serif; }
h1, h2, h3 { color: var(--hot); text-transform: uppercase; }
h1 { letter-spacing: .08em; font-weight: 600; }
h2 { letter-spacing: .06em; font-size: 1.05rem; font-weight: 600; border-top: 1px solid var(--rule); }
h3 { letter-spacing: .05em; font-size: .95rem; font-weight: 600; }
a { color: var(--hot); text-decoration-color: var(--muted); text-decoration-thickness: 1px; }
a:hover { background: none; color: #eef6f8; }
th { color: var(--muted); border-bottom: 1px solid var(--muted); }
.site { border-bottom: 1px solid var(--rule); }
.site .home { color: var(--hot); letter-spacing: .1em; }
.site nav a { color: var(--ink); }
.site nav a[aria-current] { color: var(--hot); }
:focus-visible { outline-color: #5be381; }
figure.shot img { box-shadow: 0 0 0 1px var(--rule); }
.credit { border-left: 3px solid var(--hot); padding: 4px 0 4px 12px; margin: 0 0 20px; font-size: 14px; }
"""


def theme_doc(path, up):
    """a page of fm1-base's site sheet in CHIMERA's palette; its navigation without the emulator (not built here) and
    with the Studio (`up`: the path back to the site's root from this page)"""
    html = path.read_text(encoding="utf-8")
    if "</style>" not in html:
        raise SystemExit(f"{path}: no <style> to theme (fm1-base's site changed?)")
    html = html.replace("</style>", DOCS_CSS + "</style>", 1)
    # (no Emulator link: the Studio embeds the emulator; emu/ stays, for it)
    html = re.sub(r'<a href="[./]*emu/"[^>]*>Emulator</a>', "", html)
    nav = re.search(r"<nav>.*?</nav>", html)
    if nav and "studio/" not in nav.group(0):
        html = html[:nav.start() + 5] + f'<a href="{up}studio/">Studio</a>' + html[nav.start() + 5:]
    path.write_text(html, encoding="utf-8")


# the emulator's page (fm1-base's web/emu) in the same palette: its ground black, its cards the frame's, its start
# button an action (green); the drawn FM-1 itself as it is
EMU_CSS = """
/* VOCAL: CHIMERA's palette (src/ui/colors.ts), as the Studio */
:root, :root[data-theme], :root:not([data-theme="light"]) {
  --ground: #000; --ink: #e2f5fc; --muted: #3487c4; --rule: #133954; --accent: #4fb6f2; --teal: #5be381;
  --card: #07141e; --bad: #ff5d40; --focus: #5be381; color-scheme: dark;
}
body { font-family: ui-sans-serif, system-ui, -apple-system, "Segoe UI", sans-serif; }
h1 { color: var(--accent); letter-spacing: .08em; text-transform: uppercase; font: 600 1.6rem/1.2 inherit; }
.start { background: rgba(0, 0, 0, .6); }
.start button { background: #000; color: #5be381; border: 1px solid #5be381; border-radius: 3px; letter-spacing: .08em;
                text-transform: uppercase; font: 600 1rem ui-sans-serif, system-ui, sans-serif; }
.bar button, .speak button { background: #000; color: var(--accent); border-color: var(--muted); border-radius: 3px; }
.bar button[aria-pressed="true"] { background: var(--accent); color: #000; }
"""


# visit counts (GoatCounter: no cookies, no personal data; it skips localhost, so a local preview is not counted)
GOATCOUNTER = ('<script data-goatcounter="https://shan-sherif.goatcounter.com/count" async '
               'src="//gc.zgo.at/count.js"></script>')


# the name the pages show (fm1.toml's name, CHIMERA, stays the build's: its file names, the USB name)
NAME = "CHIMERA"
DISPLAY_NAME = "FM-1 Chimera Vocal"


def rename(path):
    """the pages' titles, headings and home links say DISPLAY_NAME where fm1-base's site put fm1.toml's name (the name
    alone: "ported from CHIMERA" in a title stays)"""
    html = path.read_text(encoding="utf-8")
    html = re.sub(rf"<title>{NAME}( ·| in )", rf"<title>{DISPLAY_NAME}\1", html)
    html = re.sub(rf"· {NAME}</title>", f"· {DISPLAY_NAME}</title>", html)
    html = re.sub(rf"(<h1[^>]*>(?:Install )?){NAME}</h1>", rf"\g<1>{DISPLAY_NAME}</h1>", html)
    html = re.sub(rf'(<a class="home"[^>]*>){NAME}</a>', rf"\g<1>{DISPLAY_NAME}</a>", html)
    path.write_text(html, encoding="utf-8")


def add_counter(path):
    html = path.read_text(encoding="utf-8")
    if "data-goatcounter" in html:
        return
    if "</body>" in html:
        html = html.replace("</body>", GOATCOUNTER + "\n</body>", 1)
    else:
        html = html.rstrip("\n") + "\n" + GOATCOUNTER + "\n"
    path.write_text(html, encoding="utf-8")


def build_web(version):
    """the browser emulator (base/tools/build.py --web: the firmware in WebAssembly), with the release's version;
    Emscripten from PATH or ~/emsdk. False when there is none (the site goes without the emulator and the Studio's)"""
    emsdk = Path.home() / "emsdk" / "emsdk_env.sh"
    cmd = f'"{sys.executable}" "{BASE / "tools" / "build.py"}" "{APP}" --web --release {version}'
    if not shutil.which("emcc"):
        if not emsdk.exists():
            return False
        cmd = f'source "{emsdk}" >/dev/null 2>&1 && {cmd}'
    subprocess.run(["bash", "-c", cmd], check=True)
    return True


# the two flash chips, as the README and the front page say it
FLASH_NOTE = ('<p class="small flash">Works with both FM-1 flash chips, the older Puya P25Q80 (0x856014) and the newer '
              'UC25HQ80 (0xB36014): the same fix as <a href="https://github.com/hugelton/Felucca/commit/'
              'f4cbb6a2a0a35e068d4f063083d464eb4dbaa415">Felucca\'s</a> (<a href="https://github.com/hugelton/Felucca/'
              'issues/320">#320</a>) and <a href="https://github.com/isod89/sloop-fm1/commit/'
              'bac9c05ea7627449405aa10e48da21d75b6c8c56">SLOOP\'s</a>, here from fm1-base (Laurens, PR #35). Not yet '
              'tested with this firmware on a newer unit.</p>')


def release_files(version, pkg):
    """the files to attach to a GitHub release, in app/build/release-VERSION/: the package, the firmware image of the
    same release build (chimera.bin, renamed with its version), and their SHA-256 (SHA256SUMS.txt, as sha256sum and
    shasum -a 256 write it: `shasum -a 256 -c SHA256SUMS.txt` checks them)"""
    import hashlib
    d = APP / "build" / f"release-{version}"
    d.mkdir(parents=True, exist_ok=True)
    files = [(pkg, f"chimera-{version}.fwsc"), (APP / "build" / "chimera.bin", f"chimera-{version}.bin")]
    sums = []
    for src, name in files:
        shutil.copyfile(src, d / name)
        sums.append(f"{hashlib.sha256((d / name).read_bytes()).hexdigest()}  {name}")
    (d / "SHA256SUMS.txt").write_text("\n".join(sums) + "\n", encoding="utf-8")
    return d


def credit_installer(path):
    """the credit right under the installer's title (fm1-base's page credits its sources in the footer)"""
    html = path.read_text(encoding="utf-8")
    title = re.search(r"<h1>[^<]*</h1>\n", html)
    if not title or "</style>" not in html:
        raise SystemExit(f"{path}: no <h1> or <style> to put the credit by (fm1-base's installer.html changed?)")
    html = html[:title.end()] + "  " + CREDIT + "\n  " + FLASH_NOTE + "\n" + html[title.end():]
    # the site's top bar above the title
    html = html[:title.start()] + INSTALL_NAV.replace("VOCAL_NAME", DISPLAY_NAME) + "\n  " + html[title.start():]
    html = html.replace("</style>", "  " + CREDIT_CSS + "\n" + THEME_CSS + "</style>", 1)
    path.write_text(html, encoding="utf-8")


def add_studio(out, version):
    """the Studio (app/web/studio.html) as studio/, with CHIMERA's copy of CMUdict and its licence beside it, the
    firmware's built-in pronunciations (engine/ch_dict_data.h) and the version filled in; a link on the front page"""
    src = (APP / "web" / "studio.html").read_text(encoding="utf-8")
    builtin = dict(re.findall(r'\{"([^"]+)", "([^"]+)"\}', (ROOT / "engine" / "ch_dict_data.h").read_text(encoding="utf-8")))
    src = re.sub(r"/\*BUILTIN\*/.*?/\*END\*/", lambda m: json.dumps(builtin, separators=(",", ":")), src, count=1)
    src = re.sub(r"/\*VERSION\*/.*?/\*END\*/", lambda m: json.dumps(version), src, count=1)
    d = out / "studio"
    d.mkdir(parents=True, exist_ok=True)
    (d / "index.html").write_text("<!doctype html>\n<meta charset=\"utf-8\">\n" + src, encoding="utf-8")
    data = ROOT / "external" / "chimera-vocal-synth" / "public" / "data"
    for f in ("cmudict.txt.gz", "CMUDICT-LICENSE.txt"):
        shutil.copyfile(data / f, d / f)
    index = out / "index.html"
    html = index.read_text(encoding="utf-8")
    li = ('<li>The <a href="studio/">Studio</a> prepares the words the FM-1 sings, with the pronouncing dictionary, '
          'and sends them over USB.</li>')
    if 'href="studio/"' not in html:
        html = html.replace('<ul><li><a href="install/">', "<ul>" + li + '<li><a href="install/">', 1)
        if 'href="studio/"' not in html:
            raise SystemExit("index.html: no list to link the Studio from (fm1-base's front page changed?)")
        index.write_text(html, encoding="utf-8")


def theme_emu(path):
    html = path.read_text(encoding="utf-8")
    if "</style>" not in html:
        raise SystemExit(f"{path}: no <style> to theme (fm1-base's emulator page changed?)")
    html = html.replace("</style>", EMU_CSS + "</style>", 1)
    if "class=\"credit\"" not in html:   # the credit under its title, as every page
        html = re.sub(r"(<main>)", r"\1\n" + CREDIT.replace("\\", "\\\\"), html, count=1)
    path.write_text(html, encoding="utf-8")


def main(version, out):
    build_web(version)
    pkg = build_release(version)
    rel = release_files(version, pkg)
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
        shutil.rmtree(out / "emu")
        index = out / "index.html"
        index.write_text(re.sub(r"<li>[^\n]*?href=\"emu/\".*?</li>\n?", "", index.read_text(encoding="utf-8"),
                                flags=re.S), encoding="utf-8")
    credit_installer(out / "install" / "index.html")
    add_studio(out, version)
    # the licence texts beside the package (fm1-base's site ships its own: Apache-2.0, micro-ecc, m-bootloader)
    for f in ("MIT-CHIMERA.txt", "MIT.txt", "MPL-2.0.txt", "CMUdict.txt"):
        shutil.copyfile(ROOT / "LICENSES" / f, out / "firmware" / f)
    shutil.copyfile(ROOT / "LICENSE", out / "firmware" / "LICENSE.txt")
    if (out / "emu" / "index.html").exists():
        theme_emu(out / "emu" / "index.html")
    for page in ("index.html", "install/index.html", "manual/index.html", "studio/index.html", "emu/index.html"):
        if (out / page).exists():
            add_counter(out / page)
            rename(out / page)
    theme_doc(out / "index.html", "")
    theme_doc(out / "manual" / "index.html", "../")
    print(f"site: {out} (credit added{'; no emulator: Emscripten not set up' if made_emu else ''})")
    (out / ".nojekyll").touch()                   # (GitHub Pages serves it as it is: no Jekyll)
    print(f"release files (to attach to the GitHub release): {rel}")
    print(f"serve it: python3 -m http.server 8042 --bind 127.0.0.1 --directory {out}")
    print("then open http://127.0.0.1:8042/install/ in Chrome or Edge")


def toml_version():
    """fm1.toml's version = "X.Y.Z" (the release a push to main publishes: .github/workflows/pages.yml)"""
    m = re.search(r'^version\s*=\s*"([^"]+)"', (APP / "fm1.toml").read_text(encoding="utf-8"), re.M)
    if not m:
        sys.exit("make_site: no version given and none in app/fm1.toml (version = \"X.Y.Z\")")
    return m.group(1)


if __name__ == "__main__":
    if len(sys.argv) > 3:
        sys.exit(__doc__)
    main(sys.argv[1] if len(sys.argv) > 1 else toml_version(), sys.argv[2] if len(sys.argv) == 3 else APP / "build" / "site")
