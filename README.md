# aether

[![build](https://github.com/Fgaoxing/Aether/actions/workflows/build.yml/badge.svg)](https://github.com/Fgaoxing/Aether/actions/workflows/build.yml)
[中文](README.zh-CN.md)

**aether** is a BusyBox-style rescue toolbox for Linux initramfs: one static binary, many applets, dispatched by `argv[0]`. It can run as an interactive shell, and its primary role is to act as **PID 1** in an initramfs: mount `/proc`/`/sys`/`/dev`, load modules, parse the kernel cmdline, resolve and mount root, `pivot_root`, then `execve` the real init. Any failure drops into an interactive rescue shell instead of panicking.

## Highlights

- **Small & static**: pure C, no C++/ncurses/readline/pthreads; static `-Os` build; nolibc variant has no libc dependency.
- **Dual build**: host libc for development/tests, nolibc for initramfs deployment.
- **PID 1 boot flow**: module loading, uevent pumping, root resolution, hibernation resume, `pivot_root`, `execve`.
- **Rescue-first**: every boot failure lands in a usable shell; `exit`/`rescue-boot` retries boot from PID 1.
- **Deterministic rescue**: add `aether_rescue` to cmdline, or press `DELETE` during boot (background watcher, zero boot-time cost).
- **Networking in rescue**: auto DHCP + telnetd on `0.0.0.0:23` (trusted networks only), or manual `netup`/`ifconfig`/`udhcpc`.
- **Hibernation resume**: auto-detects swap from `/etc/fstab` at image build time; no cmdline `resume=` required.

> **Status**: x86_64 end-to-end verified on QEMU (boot, rescue, DELETE-to-rescue, DHCP/telnetd, hibernation resume). i386 verified. aarch64/riscv64/loongarch64 build only. See [Verification status](#verification-status).

## Quick Start

### Build

```sh
# Host build (development + tests)
cmake -S . -B build && cmake --build build && ctest --test-dir build

# Static nolibc build (for initramfs)
cmake -S . -B build-nolibc -DUSE_HOST_LIBC=OFF && cmake --build build-nolibc
```

### Build an initramfs image (Arch Linux / mkinitcpio)

```sh
mkinitcpio -c initramfs/aether.mkinitcpio.conf -g /tmp/aether-rescue.img
sudo cp initramfs/aether-rescue*.img /efi/aether/
```

The `aether` hook is required; other hooks are optional. See [Initramfs hooks](#initramfs-hooks).

## Initramfs Hooks

| Hook | Purpose |
| --- | --- |
| `aether` | **Required**. Installs `/init`, `/newroot`, nvme modules. Also bakes swap UUID from `/etc/fstab` into `/etc/aether/resume` for hibernation resume without cmdline parameters. |
| `aether-microcode` | x86 microcode pruning (Intel/AMD). Default local-only, consistent with `iucode_tool -S`; supports `AETHER_MICROCODE_BASE_LOCAL=1`, `AETHER_MICROCODE_KEEP_ALL=1`, `AETHER_MICROCODE_DEBUG=1`. |
| `aether-keyboard` | PS/2 + USB keyboard drivers. |
| `aether-usb` | USB host controllers, HID, USB storage. |
| `aether-network` | Common NIC drivers (Intel/Realtek/Broadcom/virtio/vmxnet3/Hyper-V/USB wired). WiFi is not supported. |
| `aether-hostdrv` | Build-host-only driver pruning. **Do not deploy to other machines.** |
| `aether-everything` | Large: common WiFi drivers + firmware. |
| `aether-resume` | Deprecated no-op; resume detection is built into `aether`. |

> mkinitcpio stock `autodetect`/`strip` are not needed for these images.

## Image Flavors

Sizes below are from the current local build artifacts in `initramfs/` (approximate, may vary by kernel/modules/microcode).

| Artifact | Size | Contents |
| --- | ---: | --- |
| `aether-rescue-no-microcode.img` | 328K | `aether` only |
| `aether-rescue-min.img` | 538K | `aether` + microcode |
| `aether-rescue-no-network.img` | 548K | default minus network |
| `aether-rescue-no-keyboard.img` | 4.5M | default minus keyboard |
| `aether-rescue-no-usb.img` | 4.5M | default minus USB |
| `aether-rescue.img` | 4.5M | default: keyboard + USB + network |
| `aether-rescue-hostdrv.img` | 1.1M | host-only drivers (build machine only) |
| `aether-rescue-full.img` | 54M | everything + WiFi drivers/firmware |

## Using Rescue

### Enter rescue

- **Cmdline token**: add `aether_rescue` (or `=1|y|yes|true|on`) to kernel cmdline. PID 1 skips normal boot and enters rescue after module loading.
- **Keyboard**: press `DELETE`/`Backspace` during boot. A background watcher monitors evdev and console raw input with no boot-time cost.

### Fix and continue boot

- `cmdline` — show `/proc/cmdline`
- `rescue-boot [root=... rw ...]` — retry boot now (current or overridden cmdline)
- `efi-cmdline` / `efi-cmdline edit` — inspect/edit EFI BootCurrent cmdline, confirm, write NVRAM, optionally retry boot
- `exit` in an init-spawned rescue shell asks PID 1 to retry boot. If retry fails, PID 1 respawns rescue.

### Network

A background daemon retries every 2s: detect non-lo NIC → DHCP → start telnetd on `0.0.0.0:23` (**no auth, plain text; trusted networks only**). `nonet` disables auto config.

Manual: `netup [iface]`, `ifconfig`, `udhcpc [-i iface] [-t N] [-T N]`, `telnetd [-p port]`.

### Hibernation resume

The `aether` hook scans `/etc/fstab` for swap at build time and writes `/etc/aether/resume`. At boot, PID 1 uses it when cmdline has no `resume=`/`resume_offset=`. Cmdline `resume=`/`noresume` always wins.

PID 1 also populates `/dev/disk/by-uuid/<uuid>` links for ext4/vfat/btrfs/swap signatures so UUID-based resume works without udev.

Notes:
- QEMU has no ACPI S4; for VM hibernation tests use `echo shutdown > /sys/power/disk` before `echo disk > /sys/power/state`.
- Hibernated swap signature becomes `S1SUSPEND`; aether recognizes it.
- Recommended flow: hibernate → boot aether rescue entry → `exit` at rescue prompt → kernel restores session.

## Applets

~83 entry points (65 applets + 18 builtins), BusyBox-style `argv[0]` dispatch.

| Category | Applets |
| --- | --- |
| Core | `help less editor ps free dmesg lsblk efibootmgr clear` |
| File/Text | `ls stat readlink realpath basename dirname chmod chown chgrp chattr lsattr touch truncate mkdir rmdir rm cp mv dd cmp diff sort uniq cut paste join split fold shuf head tail wc strings find grep` |
| Hex | `xxd od hexdump hexedit` |
| Lock/Sync | `flock fsync sync` |
| Compress | `zcat` (zstd + gzip, byte-exact vs official tools) |
| Network | `ifconfig udhcpc telnetd netup` |
| System | `lspci lsusb scsi serial raidautorun kbd_mode hotplug swapon swapoff` |
| Power | `reboot halt` |
| Builtins | `cd exit exec pwd echo [ test mount umount chroot switch_root syscall true false : cmdline rescue-boot efi-cmdline` |

Design rules: single user, root-only, no uid/gid name resolution, no setuid/capability semantics, GNU-compatible output not required.

## CI / Packaging

GitHub Actions (`.github/workflows/build.yml`) builds on push/PR/tags: x86_64 host+tests+nolibc, i386/aarch64/riscv64/loongarch64 nolibc cross builds, and release artifacts on `v*` tags.

AUR templates live under `packaging/aur/`: `aether-rescue-git` and `aether-rescue-bin`.

## Known Limitations

- No syntax highlighting; `less` has no search; editor has no undo/search.
- All times UTC; `ls -l` does not sort.
- `sort`/`shuf`/`tail` buffer whole files ≤ 2 MB.
- `chattr` write path needs `CAP_LINUX_IMMUTABLE`.
- `kbd_mode` is VT-only (`ENOTTY` on pty/serial).
- `raidautorun` needs mdadm (kernel RAID_AUTORUN ioctl removed in newer kernels).
- Resume supports `resume=/dev`, `PARTUUID=`, `PARTLABEL=`, `UUID=` (including hibernated `S1SUSPEND`), and major:minor; no `LABEL=`, no automatic dm-crypt/LVM activation.
- EFI/NVRAM writes should be tested in a sacrificial VM first.
- `lsblk` does not expand btrfs subvolumes.
- aarch64/riscv64/loongarch64 runtime not tested.

## Verification Status

Verified (x86_64, QEMU/host, as of 2026-11):
- libc build + `ctest` 2/2; nolibc build; pty smoke.
- i386 nolibc full boot path.
- PID 1 full boot (UUID resolve → btrfs mount → pivot_root → exec → poweroff).
- Rescue fallback on bad root; DELETE-to-rescue (evdev + serial raw); `aether_rescue` token.
- `exit`/`rescue-boot` retry; failed retry respawns rescue.
- DHCP + telnetd end-to-end (boot/hotplug, inbound shell, Ctrl-C/IAC disconnect).
- Hibernation resume writeback and QEMU hibernation round-trip (S1SUSPEND detected, session restored).
- `/dev/disk/by-uuid` populated without udev.
- `zcat` byte-exact vs official zstd/gzip; `grep` compared against GNU grep on 31 patterns.

Not verified: aarch64/riscv64/loongarch64 runtime; real-hardware hibernate round-trip; real-hardware EFI/NVRAM writes; dm-crypt/LVM resume; physical VT USB HID DELETE.

## Repository Layout

```text
CMakeLists.txt          dual-branch build (host libc / nolibc)
.github/workflows/      CI
packaging/aur/          AUR templates
src/                    main, boot, modload, uevent, applets, shell, tools, nolibc
initramfs/              mkinitcpio hooks + aether.mkinitcpio.conf
tools/                  host tooling (gen_sysnum.py, pty_test.py, register-aether-efi.sh)
tests/                  host-build tests
```

## License

[MIT](LICENSE) © 2026 Fgaoxing
