# aether

[![build](https://github.com/Fgaoxing/Aether/actions/workflows/build.yml/badge.svg)](https://github.com/Fgaoxing/Aether/actions/workflows/build.yml)

[中文](README.zh-CN.md)

**aether** is a BusyBox-style rescue toolbox for Linux initramfs: one single, static binary
with many entry points (applets dispatched via `argv[0]`). It can run standalone as a shell,
and — the primary purpose — it can act as **PID 1** in an initramfs image and take over the
whole boot chain: mount `/proc`, `/sys`, `/dev`, drain kernel uevents, parse the kernel
command line, resolve and mount the root filesystem, `pivot_root`, and `execve` the real init.
Every step that can fail drops into an interactive **rescue shell** — never a kernel panic.

- Pure C, no C++ / ncurses / readline / pthreads
- Static `-Os` build; **nolibc** variant has no libc dependency at all
- Dual build: host libc (`cmake -S . -B build`, with tests) or nolibc (`cmake -S . -B build-nolibc -DUSE_HOST_LIBC=OFF`)
- Targets: **x86_64 verified** · i386 verified · aarch64 links · riscv64 compiles
- All user-visible output is English (Chinese only inside internal comments)

> **Status**: x86_64 end-to-end verified on QEMU (full boot path, rescue path, hibernation
> resume write-back, DELETE-to-rescue). aarch64/riscv64 are build-only. See
> [Verification status](#verification-status) for exactly what has and has not been tested.

### Features

#### Shell

- PDA single-pass parser: `|` `;` `&` `<` `>` `>>` `2>` `2>&1`, quotes/escapes, `#` comments
- Line editing: cbreak + VT100 (arrow keys, Home/End, Backspace, Ctrl-U/A/E)
- Exit-status pipeline semantics, fork + pipe, background jobs (`cmd &`)
- Job control: `Ctrl-Z` suspend, `jobs` / `fg [id]` / `bg [id]`; shell owns the tty (tcsetpgrp)
- `↑`/`↓` command history (ring of 64, duplicates suppressed)
- Redirects, pipes, `$VAR` expansion (no command substitution)
- Ctrl-C safety: the shell ignores SIGINT; children get the default disposition (init cannot be killed by signals)

#### Applets (~83 entry points: 65 applets + 18 builtins, BusyBox-style `argv[0]` dispatch)

| Category | Applets |
| --- | --- |
| core | `help less editor ps free dmesg lsblk efibootmgr clear` |
| file / text | `ls stat readlink realpath basename dirname chmod chown chgrp chattr lsattr touch truncate mkdir rmdir rm cp mv dd cmp diff sort uniq cut paste join split fold shuf head tail wc strings find grep` |
| hex | `xxd od hexdump hexedit` |
| lock / sync | `flock fsync sync` |
| compress | `zcat` (zstd + gzip, byte-exact vs official tools) |
| network | `ifconfig udhcpc telnetd netup` |
| system | `lspci lsusb scsi serial raidautorun kbd_mode hotplug swapon swapoff` |
| power | `reboot halt` |
| builtins | `cd exit exec pwd echo [ test mount umount chroot switch_root syscall true false : cmdline rescue-boot efi-cmdline` |

Design rules: single user, single root, no uid/gid name resolution, no setuid/capability
semantics, GNU-compatible output **not** required — output is for humans.

#### Init (PID 1) flow

1. Mount `/proc`, `/sys`, `/dev` (devtmpfs)
2. Fork the **background DELETE watcher** (zero boot-time cost)
3. Load modules from `/lib/modules` (parallel `fork` + `finit_module`, multi-pass until no new loads)
4. Pump netlink `KOBJECT_UEVENT` and patch `/dev` nodes (add/remove)
5. Parse `/proc/cmdline`: `root=` `rootfstype=` `rootflags=` `init=` `ro` `rootwait=` `resume=` `resume_offset=` `noresume` …
6. Resolve root: `PARTUUID=` / `PARTLABEL=` / `UUID=` (ext4/vfat/btrfs probe) / `LABEL=` / `/dev/` path
7. Mount → bind-mount proc/sys/dev → `pivot_root` → `execve(init)`
8. Hibernation resume: write `/sys/power/resume` major:minor (see below)
9. **Any failure anywhere** → `BOOT FAILED — entering rescue mode` + interactive rescue shell(s)

### Installing / building

#### Host build (development + tests)

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build
./build/aether                      # interactive rescue shell
printf 'lsblk\n' | ./build/aether
python3 tools/pty_test.py ./build/aether
```

#### Static nolibc build (the one that ships in initramfs)

```sh
cmake -S . -B build-nolibc -DUSE_HOST_LIBC=OFF && cmake --build build-nolibc
```

#### As an initramfs image (mkinitcpio hooks, Arch Linux)

The `aether` hook is the **only required hook**; everything else is à la carte
(see `initramfs/aether.mkinitcpio.conf`):

| Hook | Purpose |
| --- | --- |
| `aether` | **required** — adds `/init` (aether), `/newroot`, `nvme` modules; **built-in: sniffs `/etc/fstab` at image build time and bakes the swap UUID as `/etc/aether/resume`** — hibernation resume works with **no cmdline parameter** |
| `aether-keyboard` | PS/2 + USB keyboard (`atkbd`/`i8042`/`serio_raw`/`usbhid`/`xhci-hcd`/`ehci-hcd`) |
| `aether-usb` | USB host controllers, HID, USB storage |
| `aether-network` | ~60 NIC drivers: Intel/Realtek/Broadcom + virtio/vmxnet3/Hyper-V + USB wired NICs — rescue gets DHCP + telnetd |
| `aether-resume` | **deprecated no-op** — hibernation resume auto-detection is built into the `aether` core hook (this hook only exists for backwards compatibility) |
| `aether-microcode` | x86 microcode pruning ("base + local" strategy; ≈4.2 MiB instead of 14.6 MB) |
| `aether-hostdrv` | build-host-only driver pruning (like `autodetect`, but strictly prunes; keeps USB wired NICs) |
| `aether-everything` | common WiFi drivers + firmware (big; add only if needed) |

mkinitcpio's stock `autodetect`/`strip` hooks are **no-ops** for these images
(`.ko.zst` is already compressed; the hooks use plain `add_module()`).

Image flavors (all gitignored, built locally — not committed):

| Artifact | Size | Notes |
| --- | --- | --- |
| `aether-rescue-no-microcode.img` | ~334 KiB | smallest — bare rescue (aether hook only) |
| `aether-rescue-min.img` | ~4.5 MiB | aether + microcode |
| `aether-rescue-no-network.img` | ~4.5 MiB | no network hook |
| `aether-rescue-no-keyboard.img` / `aether-rescue-no-usb.img` | ~8.5 MiB | default minus one hook |
| `aether-rescue.img` | ~8.5 MiB | default (keyboard + USB + network + resume) |
| `aether-rescue-hostdrv.img` | ~5.1 MiB | build-host-only drivers, **do not deploy elsewhere** |
| `aether-rescue-full.img` | ~57 MiB | everything + WiFi drivers + firmware |

Rebuild:

```sh
mkinitcpio -c initramfs/aether.mkinitcpio.conf -g /tmp/aether-rescue.img
```

Deploy (example, ESP mounted at `/efi`):

```sh
sudo cp initramfs/aether-rescue*.img /efi/aether/
```

### Using it

#### Entering rescue deterministically — `aether_rescue` cmdline token

The most reliable way: add `aether_rescue` anywhere in the kernel command line.
PID 1 skips mount-root/pivot_root/exec entirely and drops into rescue (after module load,
so storage drivers are available for repair). `aether_rescue=1|y|yes|true|on` behaves the
same; `=0|n|no|false|off` disables it.

```
root=UUID=ff93802f-… rw rootflags=subvol=@ … aether_rescue   →  FORCED RESCUE MODE (cmdline token: aether_rescue)
```

#### No boot modification — press `DELETE` at boot

PID 1 forks a **background watcher** early that watches the keyboard/console in parallel
with boot — **zero extra boot time**. The moment `DELETE`/`Backspace` is seen, PID 1 aborts
at the next checkpoint and drops into rescue.

- **Channel A (evdev, real VT keyboards)**: `/dev/input/event0..31`, `EV_KEY`,
  `KEY_DELETE(111)` / `KEY_BACKSPACE(14)`; re-scans every 100 ms for late-enumerated USB HID
- **Channel B (console raw, ttyS0/VT)**: raw-mode console, recognizes `0x7f` and `ESC[3~`

#### Fixing things from rescue

- `cmdline` — show `/proc/cmdline`
- `rescue-boot [root=/dev/... rw …]` — retry boot right now (with current or given cmdline)
- `efi-cmdline` / `efi-cmdline edit` — view/edit the EFI efistub entry's cmdline
  (BootCurrent): preview old/new, `y/N` confirm, writes NVRAM (attributes preserved),
  then optionally retry boot immediately
- `rescue-boot` is the general fix entry point; `efi-cmdline edit` persists it for reboots
- **`exit` / `rescue-boot` in an init-spawned rescue shell hand off to PID 1** (exit code 42):
  PID 1 then retries the boot **itself** — `pivot_root`/`execve` replace PID 1, so a successful
  boot makes the real init PID 1. If the retry fails (bad root, missing device, missing init…),
  PID 1 kills the remaining rescue children and **re-spawns the rescue environment** — a failed
  boot always lands you back in the rescue shell (verified on QEMU). Override cmdline:
  `rescue-boot root=/dev/sdb2 rw` (written to `/etc/.rescue-boot-override`, consumed by PID 1 on retry).

#### Networking in rescue

A background daemon retries every 2 s: detect non-lo NIC → DHCP → start **telnetd on
0.0.0.0:23 (no auth, plain text — trusted networks only)**. `nonet` in cmdline disables
auto-config entirely. WiFi is unsupported by design; wireless interfaces are skipped.

Manual: `netup [iface]` (one-shot) · `ifconfig eth0 up` / `ifconfig eth0 10.0.0.5/24` ·
`udhcpc [-i iface] [-t N] [-T N]` · `telnetd [-p port]`.

> DHCP **does work** against QEMU SLIRP (verified 2026-11-15, boot-attached and
> hotplug-attached virtio-net-pci: leased `10.0.2.15`, telnetd reachable, inbound shell
> round-trip, Ctrl-C / IAC IP disconnects). A real DHCP server or static IP is still more
> robust on an empty segment.

#### Hibernation resume — no cmdline required

The **aether** hook scans `/etc/fstab` for a `swap` entry at image build time and bakes it into the
image as `/etc/aether/resume` (override with `AETHER_RESUME=resume=UUID=…` or `AETHER_RESUME="resume=/dev/sda2 resume_offset=…"`
at build time for building an image targeting another machine). At boot, PID 1 reads it whenever cmdline has no
`resume=` / `resume_offset=`, resolves `UUID=` → major:minor, and writes
`/sys/power/resume`. `resume=` / `noresume` on cmdline always win; file-swap entries use
`filefrag` only when page size == fs block size (both 4096).

```
resume: using /etc/aether/resume (auto-detected): UUID=fe90b9d4-…
resume: using /dev/disk/by-uuid/fe90b9d4-… -> /dev/vda
resume: wrote /sys/power/resume=254:0 (device /dev/vda)
```

Right after the uevent pump, PID 1 also populates **`/dev/disk/by-uuid/<uuid> -> ../../<dev>`**
links for ext4 / vfat / btrfs / swap signatures (initramfs has no udev to do this) — resume
resolves `UUID=` through this link first and falls back to swap-signature probing if needed.

Verified up to the `/sys/power/resume` write on QEMU (2026-11-15); **full round-trip
(hibernate → poweroff → boot → session restored) verified on QEMU 2026-11-17** (see Verification status).

**Notes from verification (2026-11-17, QEMU):**

- QEMU does not implement ACPI S4, so the default `platform` hibernation mode just hangs.
  For hibernation testing in a VM, use `echo shutdown > /sys/power/disk` before `echo disk > /sys/power/state`.
- A hibernated swap's signature at offset 4086 becomes `S1SUSPEND` (not `SWAPSPACE2`) — the kernel
  overwrites the swap header with the hibernation image header. aether's `swap_probe` detects both,
  so devices resolve correctly pre- and post-hibernate.
- On a real machine, the **normal** Arch entry will not resume by itself unless it carries
  `resume=UUID=…` on its cmdline **or** its image was built with the `aether` hook (which auto-bakes the fallback)
  (a normal boot with neither will silently wipe the stale image via `systemd-hibernate-clear.service`,
  which is exactly what "no effect" looked like). The recommended flow for an aether rescue entry:
  hibernate → boot the aether rescue entry → at the rescue prompt press `exit` (PID 1 performs
  the resume write at retry, before falling through to the boot path) → the kernel restores
  the previous session mid-restore. This is the path verified end-to-end above on QEMU.

#### EFI boot-entry integration

`tools/register-aether-efi.sh` registers the rescue images under `/efi/aether/` as EFI
boot entries (loader `\EFI\arch\vmlinuz-linux`, each entry gets its own `initrd=`).

```sh
sudo ./tools/register-aether-efi.sh                  # register all images
sudo ./tools/register-aether-efi.sh --add-rescue-entry   # one always-rescue entry (current cmdline + aether_rescue)
sudo ./tools/register-aether-efi.sh --list / --uninstall / --dry-run
```

### Packaging & CI

**GitHub Actions** (`.github/workflows/build.yml`) builds on every push/PR and on tags:

| job | what it does |
| --- | --- |
| `x86_64` | host-libc build + `ctest` + nolibc static build |
| `i386` | nolibc static cross-build (clang `--target=i386-unknown-linux-gnu`) |
| `aarch64` | nolibc static cross-build (`aarch64-linux-gnu-gcc`) |
| `riscv64` | nolibc static cross-build (`riscv64-linux-gnu-gcc`) |
| `release` (tag `v*` only) | creates a GitHub Release with `aether-{x86_64,i386,aarch64,riscv64}-nolibc` + `SHA256SUMS` |

**AUR packaging templates** (not yet on the AUR — publish via `packaging/aur/README.md`):

| package | how it builds |
| --- | --- |
| `aether-rescue-git` | builds from git HEAD; static nolibc binary, no runtime deps |
| `aether-rescue-bin` | prebuilt static nolibc binary + hooks from a GitHub Release |

Both install `/usr/bin/aether` and the mkinitcpio install hooks into `/usr/lib/initcpio/install/`,
and conflict with each other (both `provide=('aether')`).

## Known limitations

- No syntax highlighting; `less` has no search; the editor has no undo/search
- All times are UTC; `ls -l` does not sort (disk order)
- `sort`/`shuf`/`tail` buffer whole files ≤ 2 MB
- `chattr` write path needs CAP_LINUX_IMMUTABLE (EPERM otherwise — root after mounting in rescue is fine)
- `kbd_mode` is VT-only (pty/serial correctly return `ENOTTY`)
- `raidautorun` needs mdadm (the kernel RAID_AUTORUN ioctl was removed in ≥ 4.x)
- Resume supports `resume=/dev`, `PARTUUID=`, `PARTLABEL=`, `UUID=` (swap-signature probe, incl. hibernated `S1SUSPEND`),
  major:minor pass-through — no `LABEL=`, no automatic dm-crypt/LVM activation
- EFI/NVRAM writes (`efibootmgr -B/-o`, `efi-cmdline edit`) must be tried in a **sacrificial VM** first
- `lsblk` does not expand btrfs subvolumes
- aarch64 / riscv64 builds only; runtime untested

### Verification status

What is **verified** (host machine + QEMU, 2026-11-15):

- ✅ x86_64 libc build, `ctest` 2/2, pty smoke
- ✅ x86_64 nolibc build (~160 KB static), pty smoke (shell, pipes, redirects, jobs, editor)
- ✅ i386 nolibc (IA32 emulation, full boot path)
- ✅ PID 1 full boot (UUID resolve → btrfs mount → pivot_root → exec → power off, QEMU rc=0)
- ✅ Rescue fallback on bad root; DELETE-to-rescue (evdev channel A + serial channel B)
- ✅ `aether_rescue` cmdline token → deterministic rescue
- ✅ rescue shell → continue boot: `exit` (init-spawned rescue shells) or `rescue-boot` reaches the real root (POWERDOWN on btrfs+virtio good root)
- ✅ DHCP + telnetd end-to-end (boot & hotplug NIC auto-config, leased 10.0.2.15; inbound shell round-trip; Ctrl-C / IAC IP/AO close the session)
- ✅ Hibernation resume write-back (`/sys/power/resume=254:0`) with zero cmdline parameters
- ✅ **Hibernation round-trip on QEMU**: `echo disk > /sys/power/state` → poweroff → next boot auto-detects
  the hibernated swap (S1SUSPEND signature) → `/sys/power/resume=254:0` → kernel restores the previous session
  (verified end-to-end; marker file in the restored session confirms state)
- ✅ init-rescue `exit` → PID 1 retries boot itself; failed retry re-spawns rescue shell (verified twice on QEMU)
- ✅ `/dev/disk/by-uuid` populated at boot (no udev); resume resolves swap `UUID=` via the link on QEMU
- ✅ `zcat` byte-exact vs official zstd/gzip decompressors
- ✅ `grep` vs GNU grep on 31 patterns (only `\d` differs, PCRE-like — documented)
- ✅ English output verified on the wire (`strings` + QEMU console capture)

What is **not** verified: aarch64/riscv64 runtime; hibernate round-trip on real hardware;
EFI/NVRAM writes on real hardware; `resume` with dm-crypt/LVM swap; USB HID DEL on a
physical VT.

All EFI/NVRAM write paths and init write paths are developed at uid=1000 and are **not**
root-tested on real hardware — verify in a sacrificial VM first.

### Repository layout

```
CMakeLists.txt          dual-branch build (host libc / nolibc) + arch normalization
.github/workflows/      CI (build on push/PR/tag, release artifacts on tags)
packaging/aur/          AUR templates (aether-rescue-git, aether-rescue-bin) + CI auto-publish
src/
  main.c                entry point; PID 1 → aether_init_main()
  boot.c/h              init: cmdline parse / root resolve / boot flow / rescue commands
  modload.c/h           early finit_module loop (modules.order or /lib/modules fallback scan)
  uevent.c/h            netlink uevent pump + hotplug monitor
  applets.c/h           applet registry (argv[0] dispatch)
  elog.c/h              logging
  mem.c/h               arena allocator (no malloc)
  util.c/h              string/utility helpers
  argparse.c/h          CLI parser
  ttyutil.c/h           tty handling
  syscall_shim.c/h      dual-branch syscall shim (libc / nolibc asm)
  statx.h               statx layout
  nolibc/               nolibc runtime (start_*.S)
  nolibc_mini.c         nolibc glue
  shell/                lineedit, builtin, jobs, pda parser
  tools/                text/file/disk/network/hex applets
initramfs/              mkinitcpio hooks (install/) + example aether.mkinitcpio.conf
tools/                  host tooling (gen_sysnum.py, pty_test.py, register-aether-efi.sh)
tests/                  host-build tests (test_argparse, test_mem)
```

### Building from source (developers)

```sh
git clone https://github.com/Fgaoxing/Aether.git
cd Aether
cmake -S . -B build && cmake --build build && ctest --test-dir build   # host
cmake -S . -B build-nolibc -DUSE_HOST_LIBC=OFF && cmake --build build-nolibc   # static nolibc
python3 tools/pty_test.py ./build-nolibc/aether                           # interactive smoke
```

Design principles:

1. **Single static binary**, BusyBox-style dispatch, no dynamic linking
2. **PID 1 semantics** — aether performs the whole boot chain itself
3. **Rescue first** — every failure drops into rescue, never a panic
4. **No host services required** — network is best-effort (DHCP retry or static)
5. **English output**; Chinese only in internal comments

### Milestones

- v1.0.0 (2026-11-19) — first tagged release: full PID 1 init flow (root parse/resolve/mount/pivot/execve),
  rescue on every failure, `aether_rescue` deterministic rescue, boot-time DELETE watcher, full rescue toolbox
  (65 applets + 18 builtins across file/text/hex/network/disk), DHCP + telnetd end-to-end (incl. hotplug auto-config),
  hibernation resume with zero cmdline params (auto-detected, end-to-end verified), English output,
  GitHub Actions CI (x86_64/i386/aarch64/riscv64) + AUR templates
- v0.1 — core shell + init flow
- v0.2 — full rescue toolchain + PID 1 + EFI cmdline editing
- v0.2.5 (2026-11-19) —
  - AUR packaging renamed to `aether-rescue-git` / `aether-rescue-bin` (old names unavailable on AUR)
  - CI auto-publish to AUR on tag push (archlinux:base-devel container; see packaging/aur/README.md)
  - release job in CI: build artifacts, create GitHub Release on tags
- v0.2.4 (2026-11-17) —
  - GitHub Actions CI: x86_64 libc+tests, nolibc, i386/aarch64/riscv64 cross-builds, release artifacts on tags
  - AUR templates (`aether-rescue-git`, `aether-rescue-bin`) under `packaging/aur/`
- v0.2.3 (2026-11-17) —
  - **hibernation resume auto-detection now built into the `aether` core hook** (no separate hook needed);
    `aether-resume` deprecated to a no-op for compatibility
  - `AETHER_RESUME` build-time override
- v0.2.2 (2026-11-17) —
  - `swapon` / `swapoff` applets
  - `swap_probe` recognizes hibernated swap (`S1SUSPEND`) alongside `SWAPSPACE2`/`SWAP-SPACE`
  - `AETHER_RESUME` env override for explicit resume spec at build time
  - hibernation restore end-to-end verified on QEMU (session restored after poweroff→boot)
- v0.2.1 (2026-11-15) —
  - zstd + gzip (de)compression; `zcat`; byte-exact vs official tools
  - `resume=` hibernation support + **no-cmdline resume** via baked `/etc/aether/resume` (now built into the `aether` core hook)
  - `aether-microcode` (base + local pruning), `aether-everything`, `aether-hostdrv` hooks
  - DELETE-to-rescue background watch (evdev + console raw)
  - `aether_rescue` cmdline token → deterministic rescue
  - DHCP wire-path fixes (msgtype / XID compare), USB NIC hotplug auto-config, telnetd EOF fix
  - all user-visible output in English
  - `tools/register-aether-efi.sh` (EFI entries; `--add-rescue-entry`)

### License

[MIT](LICENSE) © 2026 Fgaoxing

---
