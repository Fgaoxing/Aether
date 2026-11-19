# AUR packages for aether

Two packages are maintained here:

| Package | How it builds | Source |
| --- | --- | --- |
| `aether-git` | Builds from git HEAD (static nolibc, no runtime deps) | `git+https://github.com/Fgaoxing/Aether.git` |
| `aether-bin` | Prebuilt static nolibc binary + hooks from GitHub Releases | release tag `vX.Y.Z` |

Both install the binary as `/usr/bin/aether` and the mkinitcpio install hooks into
`/usr/lib/initcpio/install/`; user-local hooks live in `/etc/initcpio/install/`.
They conflict with each other; both provide `aether`.

## CI-based auto publish (recommended)

`.github/workflows/aur.yml` publishes both packages on every `v*` tag push.
One-time setup (on the machine and on GitHub):

1. **AUR — add a second, CI-only SSH key.** AUR supports multiple keys per account
   (separate them with newlines in *My Account → SSH Public Key*).
   Generate a passphrase-less key for CI, and register its **public** part there:

   ```sh
   ssh-keygen -t ed25519 -f aur-ci -N ""   # -N "" = no passphrase (CI can't type it)
   cat aur-ci.pub                           # → AUR account → SSH Public Key (append on a new line)
   ```

2. **GitHub — add secrets** (repo → *Settings → Secrets and variables → Actions*):

   | Secret | Value |
   | --- | --- |
   | `AUR_SSH_PRIVATE_KEY` | contents of `aur-ci` (the **private** key; never committed) |
   | `AUR_KNOWN_HOSTS` | output of `ssh-keyscan aur.archlinux.org` (pins the AUR host key) |
   | `AUR_GIT_USER` | `Fgaoxing0206@163.com` (git committer identity on the AUR commit) |

3. **Enjoy.** Push a tag and both packages are updated automatically:

   ```sh
   git tag v1.1.0 && git push --tags
   ```

   Or run *Actions → publish-aur → Run workflow* manually on a tag.

## Manual publish (alternative)

```sh
git clone ssh://aur@aur.archlinux.org/aether-git.git
cp packaging/aur/aether-git/{PKGBUILD,.SRCINFO,aether-git.install} aether-git/
cd aether-git && makepkg --printsrcinfo > .SRCINFO && makepkg -si
git add PKGBUILD .SRCINFO aether-git.install
git commit -m "aether-git 1.0.0" && git push
```

`aether-bin` follows the same three files; the sha256sums must match the GitHub
Release assets and source tarball — regenerate them with:

```sh
python3 packaging/aur/aur_tool.py prepare-bin <vtag>
python3 packaging/aur/aur_tool.py push aether-bin    # after git remote/config set up
```

`aur_tool.py` downloads the release assets, rewrites PKGBUILD/.SRCINFO, clones
the AUR repo, copies the three files in, commits and pushes.
