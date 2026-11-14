#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Shan-Sherif
# Publish the site (the front page, the Studio with the emulator, the installer, the manual, the release package) to
# GitHub Pages: tools/make_site.py builds release VERSION into app/build/site, then the generated site alone goes to
# the gh-pages branch of this repository, replacing what was there (the branch holds only the site). As fm1-base's
# tools/publish_site.sh, with this firmware's own site builder.
#
#   sh tools/publish_site.sh [VERSION]          build, and show what would be pushed (nothing is pushed)
#   sh tools/publish_site.sh [VERSION] --push   build and push to gh-pages
# (VERSION: fm1.toml's version when left out. The usual way to publish is a push to main: .github/workflows/pages.yml
# builds and deploys the site on GitHub; this script is the manual alternative, to a gh-pages branch)
#
# Once, on GitHub: the repository public, and Settings > Pages > Deploy from a branch: gh-pages, / (root). The site is
# then at https://shan-sherif.github.io/fm-1-chimera-vocal/ (fm1.toml's site).
set -e
cd "$(dirname "$0")/.."
if [ "${1:-}" = "--push" ] || [ -z "${1:-}" ]; then
    V="$(sed -n 's/^version *= *"\([^"]*\)".*/\1/p' app/fm1.toml)"
    PUSH="${1:-}"
else
    V="$1"
    PUSH="${2:-}"
fi
[ -n "$V" ] || { echo "publish_site: no version (give one, or set version in app/fm1.toml)"; exit 1; }
if [ -n "$(git status --porcelain)" ]; then
    echo "publish_site: the repository has uncommitted changes: the site is built from the files as they are, and its"
    echo "  commit message names the last commit ($(git rev-parse --short HEAD)). Commit first for a site that matches it."
fi
.venv/bin/python tools/make_site.py "$V"
SITE=app/build/site
touch "$SITE/.nojekyll"                         # (served as it is: no Jekyll processing)
REMOTE="$(git remote get-url origin)"
SHA="$(git rev-parse --short HEAD)"
cd "$SITE"
rm -rf .git
git init -q -b gh-pages
git add -A
git commit -q -m "Site for FM-1 Chimera Vocal $V (from $SHA)"
echo "publish_site: the site for $V: $(git ls-files | wc -l | tr -d ' ') files, $(du -sh . | cut -f1) (with .git)"
if [ "$PUSH" = "--push" ]; then
    git push -q -f "$REMOTE" gh-pages
    rm -rf .git
    echo "publish_site: pushed to gh-pages of $REMOTE"
    echo "  GitHub Pages deploys it in a minute or two (Settings > Pages shows it). The first time, if it stays on"
    echo "  'building', re-select the gh-pages branch there to start it."
else
    rm -rf .git
    echo "publish_site: not pushed (add --push to publish to gh-pages of $REMOTE)"
fi
