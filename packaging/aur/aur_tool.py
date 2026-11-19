#!/usr/bin/env python3
"""
aur_tool.py — regenerate the AUR package files (PKGBUILD/.SRCINFO/.install) and push them.

Usage:
  prepare-git <pkgver>     e.g. 1.0.0  (pkgver is used verbatim)
  prepare-bin <vtag>       e.g. 1.0.0  (pkgver := vtag, hashes taken from GitHub release)
  push <pkgname>           clone aur/<pkgname>, copy the 3 files, commit, push

Deterministic (no makepkg needed) so it runs on ubuntu-latest CI runners.
"""
import hashlib
import os
import re
import subprocess
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
GITHUB = "https://github.com/Fgaoxing/Aether"
GIT_RAW = "https://raw.githubusercontent.com/Fgaoxing/Aether"


def die(msg):
    print("aur_tool: " + msg, file=sys.stderr)
    sys.exit(1)


def git(*args, cwd=None):
    subprocess.run(["git", *args], cwd=cwd, check=True)


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def write(path, text):
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def set_version_pkgbuild(src, pkgver):
    return re.sub(r"^pkgver=.*$", "pkgver=" + pkgver, src, flags=re.M)


def set_version_srcinfo(src, pkgver):
    return re.sub(r"^\tpkgver = .*$", "\tpkgver = " + pkgver, src, flags=re.M)


def sha256_url(url):
    with urllib.request.urlopen(url, timeout=60) as r:
        return hashlib.sha256(r.read()).hexdigest()


def prepare_git(pkgver):
    d = os.path.join(HERE, "aether-rescue-git")
    pkgbuild = os.path.join(d, "PKGBUILD")
    srcinfo = os.path.join(d, ".SRCINFO")

    write(pkgbuild, set_version_pkgbuild(read(pkgbuild), pkgver))
    write(srcinfo, set_version_srcinfo(read(srcinfo), pkgver))
    print("aether-rescue-git: pkgver ->", pkgver)


def prepare_bin(vtag):
    pkgver = vtag
    d = os.path.join(HERE, "aether-rescue-bin")
    pkgbuild = os.path.join(d, "PKGBUILD")
    srcinfo = os.path.join(d, ".SRCINFO")

    url_bin = GITHUB + "/releases/download/v" + vtag + "/aether-x86_64-nolibc"
    url_src = GITHUB + "/archive/refs/tags/v" + vtag + ".tar.gz"
    url_lic = GIT_RAW + "/v" + vtag + "/LICENSE"

    bin_hash = sha256_url(url_bin)
    src_hash = sha256_url(url_src)
    lic_hash = sha256_url(url_lic)

    out = set_version_pkgbuild(read(pkgbuild), pkgver)
    out = re.sub(
        r"sha256sums=\([^)]*\)",
        "sha256sums=('%s'\n            '%s'\n            '%s')" % (bin_hash, src_hash, lic_hash),
        out,
        flags=re.S,
    )
    write(pkgbuild, out)

    info = set_version_srcinfo(read(srcinfo), pkgver)
    hashes = iter([bin_hash, src_hash, lic_hash])
    out_lines = []
    for line in info.splitlines():
        if line.startswith("\tsha256sums = "):
            line = "\tsha256sums = " + next(hashes)
        out_lines.append(line)
    write(srcinfo, "\n".join(out_lines) + "\n")
    print("aether-rescue-bin: pkgver ->", pkgver)


def push(pkgname):
    d = os.path.join(HERE, pkgname)
    gitdir = "/tmp/aur-" + pkgname
    url = "ssh://aur@aur.archlinux.org/" + pkgname + ".git"
    if not os.path.isdir(gitdir):
        subprocess.run(["git", "clone", url, gitdir], check=True)
    else:
        git("pull", "--rebase", cwd=gitdir)
    for f in ("PKGBUILD", ".SRCINFO", pkgname + ".install"):
        subprocess.run(["cp", os.path.join(d, f), os.path.join(gitdir, f)], check=True)
    git("add", "PKGBUILD", ".SRCINFO", pkgname + ".install", cwd=gitdir)
    dirty = subprocess.run(
        ["git", "-C", gitdir, "diff", "--cached", "--quiet"]
    ).returncode != 0
    if not dirty:
        print(pkgname + ": nothing to push")
        return
    git("-c", "user.name=Fgaoxing", "-c", "user.email=Fgaoxing0206@163.com",
        "commit", "-q", "-m", "release " + pkgname + " " + os.environ.get("PKGVER", "update"),
        cwd=gitdir)
    git("push", "origin", "master", cwd=gitdir)
    print(pkgname + ": pushed to AUR")


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else ""
    if mode == "prepare-git":
        prepare_git(sys.argv[2])
    elif mode == "prepare-bin":
        prepare_bin(sys.argv[2])
    elif mode == "push":
        push(sys.argv[2])
    else:
        print(__doc__)
        sys.exit(1)


if __name__ == "__main__":
    main()
