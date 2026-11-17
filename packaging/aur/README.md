# AUR packages for aether

Two package flavors are maintained here (not yet on AUR — publish them yourself):

| Package | How it builds | Source |
| --- | --- | --- |
| `aether-git` | Builds from git HEAD (static nolibc, no runtime deps) | `git+https://github.com/Fgaoxing/Aether.git` |
| `aether-bin` | Prebuilt static nolibc binary + hooks from GitHub Releases | release tag `vX.Y.Z` |

Both packages install the binary as `/usr/bin/aether` and the mkinitcpio install hooks into
`/usr/lib/initcpio/install/` (user-local overrides go to `/etc/initcpio/install/`).
They conflict with each other; both provide `aether`.

## Publishing to AUR

Prerequisites: an AUR account (e.g. https://aur.archlinux.org), your public SSH key uploaded
to the AUR account, and `base-devel` installed.

### aether-git

```sh
git clone ssh://aur@aur.archlinux.org/aether-git.git
cp packaging/aur/aether-git/{PKGBUILD,.SRCINFO,aether-git.install} aether-git/
cd aether-git
makepkg --printsrcinfo > .SRCINFO   # regenerate if you changed PKGBUILD
makepkg -si                          # optional: build & install locally
git add PKGBUILD .SRCINFO aether-git.install
git commit -m "Initial import: aether-git"
git push
```

### aether-bin

Requires a published GitHub Release (push a tag `vX.Y.Z` — the CI workflow builds and attaches
`aether-x86_64-nolibc` and `SHA256SUMS` automatically):

```sh
git tag v0.4.0 && git push --tags         # releases are built by CI on tag push
# once the release exists, download dist/SHA256SUMS and fill in sha256sums=(...) in PKGBUILD
git clone ssh://aur@aur.archlinux.org/aether-bin.git
cp packaging/aur/aether-bin/{PKGBUILD,.SRCINFO,aether-bin.install} aether-bin/
cd aether-bin
# after editing sha256sums:
makepkg --printsrcinfo > .SRCINFO
makepkg -si
git add PKGBUILD .SRCINFO aether-bin.install
git commit -m "Initial import: aether-bin"
git push
```

## In the binary (`aether-bin`) the hooks come from the source tarball

The template downloads the GitHub source tarball (`archive/refs/tags/vX.Y.Z.tar.gz`) and copies
`initramfs/hooks/install/*` from it into the package, so the hooks always match the release.
