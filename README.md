# aether · Aether 救援工具箱

[English](#english) · [中文](#中文)

> Every section below is bilingual — English first, 中文 immediately after.
> 本文件每章节均为双语：先英文,后中文。

---

<a name="english"></a>

## English

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
| system | `lspci lsusb scsi serial raidautorun kbd_mode hotplug` |
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
| `aether` | **required** — adds `/init` (aether), `/newroot`, `nvme` modules |
| `aether-keyboard` | PS/2 + USB keyboard (`atkbd`/`i8042`/`serio_raw`/`usbhid`/`xhci-hcd`/`ehci-hcd`) |
| `aether-usb` | USB host controllers, HID, USB storage |
| `aether-network` | ~60 NIC drivers: Intel/Realtek/Broadcom + virtio/vmxnet3/Hyper-V + USB wired NICs — rescue gets DHCP + telnetd |
| `aether-resume` | bakes `/etc/fstab` swap UUID into the image (`/etc/aether/resume`) — hibernation resume with **no cmdline parameter** |
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

The **aether-resume** hook scans `/etc/fstab` for a `swap` entry and bakes it into the
image as `/etc/aether/resume`. At boot, PID 1 reads it whenever cmdline has no
`resume=` / `resume_offset=`, resolves `UUID=` → major:minor, and writes
`/sys/power/resume`. `resume=` / `noresume` on cmdline always win; file-swap entries use
`filefrag` only when page size == fs block size (both 4096).

```
resume: using /etc/aether/resume (auto-detected): UUID=fe90b9d4-…
resume: wrote /sys/power/resume=254:0 (device /dev/vda)
```

Verified up to the `/sys/power/resume` write on QEMU (2026-11-15, swap image attached);
the real hibernate-image restore round-trip is **not** tested.

#### EFI boot-entry integration

`tools/register-aether-efi.sh` registers the rescue images under `/efi/aether/` as EFI
boot entries (loader `\EFI\arch\vmlinuz-linux`, each entry gets its own `initrd=`).

```sh
sudo ./tools/register-aether-efi.sh                  # register all images
sudo ./tools/register-aether-efi.sh --add-rescue-entry   # one always-rescue entry (current cmdline + aether_rescue)
sudo ./tools/register-aether-efi.sh --list / --uninstall / --dry-run
```

### Known limitations

- No syntax highlighting; `less` has no search; the editor has no undo/search
- All times are UTC; `ls -l` does not sort (disk order)
- `sort`/`shuf`/`tail` buffer whole files ≤ 2 MB
- `chattr` write path needs CAP_LINUX_IMMUTABLE (EPERM otherwise — root after mounting in rescue is fine)
- `kbd_mode` is VT-only (pty/serial correctly return `ENOTTY`)
- `raidautorun` needs mdadm (the kernel RAID_AUTORUN ioctl was removed in ≥ 4.x)
- Resume supports `resume=/dev`, `PARTUUID=`, `PARTLABEL=`, `UUID=` (swap-signature probe), major:minor pass-through —
  no `LABEL=`, no automatic dm-crypt/LVM activation
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

- v0.1 — core shell + init flow
- v0.2 — full rescue toolchain + PID 1 + EFI cmdline editing
- v0.2.1 (2026-11-15) —
  - zstd + gzip (de)compression; `zcat`; byte-exact vs official tools
  - `resume=` hibernation support + **no-cmdline resume** via the `aether-resume` hook
  - `aether-microcode` (base + local pruning), `aether-everything`, `aether-hostdrv` hooks
  - DELETE-to-rescue background watch (evdev + console raw)
  - `aether_rescue` cmdline token → deterministic rescue
  - DHCP wire-path fixes (msgtype / XID compare), USB NIC hotplug auto-config, telnetd EOF fix
  - all user-visible output in English
  - `tools/register-aether-efi.sh` (EFI entries; `--add-rescue-entry`)

### License

[MIT](LICENSE) © 2026 Fgaoxing

---

<a name="中文"></a>

## 中文

**aether** 是一个 BusyBox 风格、用于 Linux initramfs 的救援工具箱：**单一静态二进制**，
通过 `argv[0]` 分派到各个 applet。它既可以独立作为 shell 使用，也——这是它的主要目的——
可以作为 initramfs 镜像中的 **PID 1**，接管整个启动链：挂载 `/proc`、`/sys`、`/dev`，
排空内核 uevent，解析内核命令行，解析并挂载根文件系统，`pivot_root`，最后
`execve` 真正的 init。任何一步失败都会掉入交互式 **救援 shell**——绝不会 kernel panic。

- 纯 C 实现，无 C++ / ncurses / readline / pthreads
- 静态 `-Os` 构建；**nolibc** 变体完全不依赖 libc
- 双构建：宿主 libc（`cmake -S . -B build`，带测试）或 nolibc（`cmake -S . -B build-nolibc -DUSE_HOST_LIBC=OFF`）
- 目标架构：**x86_64 已验证** · i386 已验证 · aarch64 可链接 · riscv64 可编译
- 所有面向用户的输出都是英文（中文仅出现在内部注释中）

> **现状**：x86_64 已在 QEMU 上端到端验证（完整启动路径、救援路径、休眠恢复写回、
> DELETE 进入救援）。aarch64/riscv64 仅完成构建。具体验证范围见
> [验证状态](#验证状态)。

### 功能特性

#### Shell

- PDA 单遍解析器：`|` `;` `&` `<` `>` `>>` `2>` `2>&1`、引号/转义、`#` 注释
- 行编辑：cbreak + VT100（方向键、Home/End、Backspace、Ctrl-U/A/E）
- 管道退出状态语义、fork + pipe、后台任务（`cmd &`）
- 任务控制：`Ctrl-Z` 挂起、`jobs` / `fg [id]` / `bg [id]`；shell 独占 tty（tcsetpgrp）
- `↑`/`↓` 命令历史（64 环，连续重复命令去重）
- 重定向、管道、`$VAR` 展开（不支持命令替换）
- Ctrl-C 安全：shell 忽略 SIGINT；子进程恢复默认信号处置（init 不会被信号杀死）

#### Applets（约 83 个入口点：65 个 applet + 18 个 builtin，BusyBox 风格 `argv[0]` 分派）

| 类别 | Applet |
| --- | --- |
| 核心 | `help less editor ps free dmesg lsblk efibootmgr clear` |
| 文件/文本 | `ls stat readlink realpath basename dirname chmod chown chgrp chattr lsattr touch truncate mkdir rmdir rm cp mv dd cmp diff sort uniq cut paste join split fold shuf head tail wc strings find grep` |
| 十六进制 | `xxd od hexdump hexedit` |
| 锁/同步 | `flock fsync sync` |
| 压缩 | `zcat`（zstd + gzip，与官方工具字节级一致） |
| 网络 | `ifconfig udhcpc telnetd netup` |
| 系统 | `lspci lsusb scsi serial raidautorun kbd_mode hotplug` |
| 电源 | `reboot halt` |
| builtin | `cd exit exec pwd echo [ test mount umount chroot switch_root syscall true false : cmdline rescue-boot efi-cmdline` |

设计规则：单用户、root-only、无 uid/gid 名称解析、无 setuid/capability 语义、
**不**要求 GNU 兼容输出——输出是给人看的。

#### Init（PID 1）流程

1. 挂载 `/proc`、`/sys`、`/dev`（devtmpfs）
2. fork **后台 DELETE 监视器**（零启动时间开销）
3. 从 `/lib/modules` 并行加载模块（`fork` + `finit_module` 多轮直到无新模块可加载）
4. 排空 netlink `KOBJECT_UEVENT` 并修补 `/dev` 节点（add/remove）
5. 解析 `/proc/cmdline`：`root=` `rootfstype=` `rootflags=` `init=` `ro` `rootwait=` `resume=` `resume_offset=` `noresume` …
6. 解析 root：`PARTUUID=` / `PARTLABEL=` / `UUID=`（ext4/vfat/btrfs 签名探测）/ `LABEL=` / `/dev/` 路径
7. 挂载 → bind 挂载 proc/sys/dev → `pivot_root` → `execve(init)`
8. 休眠恢复：写入 `/sys/power/resume` major:minor（见下文）
9. **任何一步失败** → `BOOT FAILED — entering rescue mode` + 交互式救援 shell

### 安装 / 构建

#### 宿主机构建（开发 + 测试）

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build
./build/aether                      # 交互式 rescue shell
printf 'lsblk\n' | ./build/aether
python3 tools/pty_test.py ./build/aether
```

#### 静态 nolibc 构建（initramfs 中实际部署的版本）

```sh
cmake -S . -B build-nolibc -DUSE_HOST_LIBC=OFF && cmake --build build-nolibc
```

#### 打包为 initramfs 镜像（mkinitcpio hook，Arch Linux）

`aether` hook 是**唯一必需**的 hook，其余按需组合
（见 `initramfs/aether.mkinitcpio.conf`）：

| Hook | 用途 |
| --- | --- |
| `aether` | **必需**——添加 `/init`（aether)、`/newroot`、`nvme` 模块 |
| `aether-keyboard` | PS/2 + USB 键盘（`atkbd`/`i8042`/`serio_raw`/`usbhid`/`xhci-hcd`/`ehci-hcd`） |
| `aether-usb` | USB 主控、HID、USB 存储 |
| `aether-network` | 约 60 个网卡驱动：Intel/Realtek/Broadcom + virtio/vmxnet3/Hyper-V + USB 有线网卡——救援即可 DHCP + telnetd |
| `aether-resume` | 把 `/etc/fstab` 中的 swap UUID 烘焙进镜像（`/etc/aether/resume`)——**无需 cmdline 参数**即可休眠恢复 |
| `aether-microcode` | x86 微码裁剪（"base + local"策略；约 4.2 MiB，原 14.6 MB） |
| `aether-hostdrv` | 仅当前构建机器的驱动裁剪（类似 `autodetect`，但只做减法；USB 有线网卡始终保留） |
| `aether-everything` | 常见 WiFi 驱动 + 固件（体积大，确实需要再加） |

mkinitcpio 自带的 `autodetect`/`strip` hook 对本镜像都是**空操作**
（.ko.zst 已压缩；本 hook 直接用 `add_module()`）。

镜像规格（均被 gitignore，仅本地构建、不随仓库分发）：

| 产物 | 大小 | 说明 |
| --- | --- | --- |
| `aether-rescue-no-microcode.img` | ~334 KiB | 最小化——纯救援（仅 aether hook） |
| `aether-rescue-min.img` | ~4.5 MiB | aether + microcode |
| `aether-rescue-no-network.img` | ~4.5 MiB | 不含网络 hook |
| `aether-rescue-no-keyboard.img` / `aether-rescue-no-usb.img` | ~8.5 MiB | 默认组合各减一个 hook |
| `aether-rescue.img` | ~8.5 MiB | 默认组合（keyboard + USB + network + resume) |
| `aether-rescue-hostdrv.img` | ~5.1 MiB | 仅构建机器驱动，**不要部署到其他机器** |
| `aether-rescue-full.img` | ~57 MiB | 全量驱动 + WiFi 驱动 + 固件 |

重新构建：

```sh
mkinitcpio -c initramfs/aether.mkinitcpio.conf -g /tmp/aether-rescue.img
```

部署（示例，ESP 挂载在 `/efi`）：

```sh
sudo cp initramfs/aether-rescue*.img /efi/aether/
```

### 使用方法

#### 确定性进入救援 —— `aether_rescue` 命令行 token

最可靠的方式：在内核命令行任意位置加 `aether_rescue`。
PID 1 会完全跳过挂载 root / pivot_root / exec，直接掉入救援（在模块加载之后进入，
存储驱动已可用于修复）。`aether_rescue=1|y|yes|true|on` 效果相同；
`=0|n|no|false|off` 则关闭（正常启动）。

```
root=UUID=ff93802f-… rw rootflags=subvol=@ … aether_rescue   →  FORCED RESCUE MODE (cmdline token: aether_rescue)
```

#### 不改启动参数 —— 启动时按 `DELETE` 进入救援

PID 1 很早就 fork 一个**后台监视器**，与启动并行监听键盘/控制台——
**零额外启动时间**。一旦检测到 `DELETE`/`Backspace`，PID 1 就在下一个检查点中止启动并掉入救援。

- **通道 A（evdev，真实 VT 键盘）**：`/dev/input/event0..31`、`EV_KEY`、
  `KEY_DELETE(111)` / `KEY_BACKSPACE(14)`；每 100 ms 重新扫描一次，兼容晚枚举的 USB HID
- **通道 B（控制台原始模式，ttyS0/VT）**：原始模式 console,识别 `0x7f` 和 `ESC[3~`

#### 在救援 shell 中修复系统

- `cmdline` —— 查看 `/proc/cmdline`
- `rescue-boot [root=/dev/... rw …]` —— 立即重试启动（用当前或给定的 cmdline）
- `efi-cmdline` / `efi-cmdline edit` —— 查看/编辑 EFI efistub 启动项的 cmdline
  （BootCurrent）：预览新旧差异 → `y/N` 确认 → 写入 NVRAM（属性保留）→
  可选地立即用新 cmdline 重试启动
- `rescue-boot` 是通用修复入口；`efi-cmdline edit` 用于把修复固化到重启之后

#### 救援中的网络

后台守护每 2 秒重试：检测非 lo 网卡 → DHCP → 启动 **telnetd（0.0.0.0:23，无认证、明文——
仅限可信网络）**。cmdline 中加 `nonet` 可完全禁用自动配置。按设计不支持 WiFi;
无线接口会被跳过。

手动：`netup [iface]`（一次性）· `ifconfig eth0 up` / `ifconfig eth0 10.0.0.5/24` ·
`udhcpc [-i iface] [-t N] [-T N]` · `telnetd [-p port]`。

> DHCP **在 QEMU SLIRP 下确实可用**（2026-11-15 实测：开机即挂载的网卡和运行中热插的
> virtio-net-pci 都拿到了 `10.0.2.15`;telnetd 可连、入站 shell 可以收发；Ctrl-C / IAC IP
> 可断开会话）。空链路上有真实 DHCP 服务器或静态 IP 仍然更稳。

#### 休眠恢复 —— 不需要 cmdline 参数

**aether-resume** hook 会扫描 `/etc/fstab` 中的 `swap` 条目，并把它烘焙进镜像的
`/etc/aether/resume`。启动时，当 cmdline 没有 `resume=` / `resume_offset=`，PID 1 读取它，
把 `UUID=` 解析为 major:minor 后写入 `/sys/power/resume`。cmdline 上的
`resume=` / `noresume` 始终优先；文件型 swap 仅在页大小 == 文件系统块大小（都是 4096）时
才用 `filefrag`。

```
resume: using /etc/aether/resume (auto-detected): UUID=fe90b9d4-…
resume: wrote /sys/power/resume=254:0 (device /dev/vda)
```

已在 QEMU 上验证到写入 `/sys/power/resume` 这一步（2026-11-15，挂载了 swap 镜像）；
真正的休眠镜像恢复往返**尚未实测**。

#### EFI 启动项集成

`tools/register-aether-efi.sh` 可以把 `/efi/aether/` 下的救援镜像注册为 EFI 启动项
（loader `\EFI\arch\vmlinuz-linux`，每项自带 `initrd=`）。

```sh
sudo ./tools/register-aether-efi.sh                  # 注册全部镜像
sudo ./tools/register-aether-efi.sh --add-rescue-entry   # 注册唯一的"永远救援"启动项（当前 cmdline + aether_rescue)
sudo ./tools/register-aether-efi.sh --list / --uninstall / --dry-run
```

### 已知限制

- 无语法高亮；`less` 不支持搜索；编辑器无撤销/搜索
- 所有时间均为 UTC；`ls -l` 不排序（磁盘顺序）
- `sort`/`shuf`/`tail` 会把 ≤ 2 MB 的文件整个读入内存
- `chattr` 写入需要 CAP_LINUX_IMMUTABLE（否则 EPERM——救援中挂载后以 root 操作即可）
- `kbd_mode` 仅 VT 控制台可用（pty/serial 正确返回 `ENOTTY`）
- `raidautorun` 需要 mdadm（内核 RAID_AUTORUN ioctl 自 ≥ 4.x 起已移除）
- resume 支持 `resume=/dev`、`PARTUUID=`、`PARTLABEL=`、`UUID=`（swap 签名探测）及 major:minor 直通——
  不支持 `LABEL=`，不自动激活 dm-crypt/LVM
- EFI/NVRAM 写入（`efibootmgr -B/-o`、`efi-cmdline edit`）务必先在**牺牲用的虚拟机**上验证
- `lsblk` 不展开 btrfs 子卷
- aarch64 / riscv64 仅构建可用，运行时未测试

### 验证状态

**已验证**（宿主机 + QEMU,2026-11-15)：

- ✅ x86_64 libc 构建，`ctest` 2/2,pty 冒烟通过
- ✅ x86_64 nolibc 构建（约 160 KB 静态），pty 冒烟通过（shell、管道、重定向、任务控制、编辑器）
- ✅ i386 nolibc（IA32 仿真环境，完整启动路径）
- ✅ PID 1 完整启动（UUID 解析 → btrfs 挂载 → pivot_root → exec → 关机，QEMU rc=0)
- ✅ root 错误时回退到救援；DELETE-to-rescue（通道 A evdev + 通道 B 串口）
- ✅ `aether_rescue` cmdline token → 确定性进入救援
- ✅ rescue shell 中继续启动：`exit`（init 拉起的救援 shell）或 `rescue-boot` 可以到达真正的 root（btrfs+virtio，POWERDOWN）
- ✅ DHCP + telnetd 端到端（开机 & 热插网卡自动配置，拿到 10.0.2.15；入站 shell 收发正常；Ctrl-C / IAC IP/AO 可断开会话）
- ✅ 休眠恢复写回（`/sys/power/resume=254:0`),**无 cmdline 参数**
- ✅ `zcat` 与官方 zstd/gzip 解压字节级一致
- ✅ `grep` 与 GNU grep 对比 31 个模式（仅 `\d` 不同，按 PCRE 类处理——已记录）
- ✅ 英文输出已在链路上验证（`strings` + QEMU 控制台捕获）

**未验证**：aarch64/riscv64 运行时；真机休眠往返；真机 EFI/NVRAM 写入；
dm-crypt/LVM swap 场景的 resume；物理 VT 上 USB HID 的 DELETE 键。

所有 EFI/NVRAM 写路径和 init 写路径都是在 uid=1000 下开发的，**未在真实硬件上以 root 实测**——
请先在牺牲用的虚拟机中验证。

### 仓库结构

```
CMakeLists.txt          双分支构建（host libc / nolibc）+ 架构归一化
src/
  main.c                入口点；PID 1 → aether_init_main()
  boot.c/h              init：cmdline 解析 / root 解析 / 启动流程 / 救援命令
  modload.c/h           早期 finit_module 循环（modules.order 或 /lib/modules 回退扫描）
  uevent.c/h            netlink uevent 泵 + hotplug 监视器
  applets.c/h           applet 注册表（argv[0] 分派）
  elog.c/h              日志
  mem.c/h               arena 分配器（不用 malloc）
  util.c/h              字符串/工具函数
  argparse.c/h          命令行解析器
  ttyutil.c/h           tty 处理
  syscall_shim.c/h      双分支 syscall shim（libc / nolibc 汇编）
  statx.h               statx 布局
  nolibc/               nolibc 运行时（start_*.S）
  nolibc_mini.c         nolibc 粘合层
  shell/                行编辑、builtin、任务控制、PDA 解析器
  tools/                文本/文件/磁盘/网络/十六进制 applet
initramfs/              mkinitcpio hook（install/）+ aether.mkinitcpio.conf 示例
tools/                  宿主工具（gen_sysnum.py、pty_test.py、register-aether-efi.sh）
tests/                  宿主机构建测试（test_argparse、test_mem）
```

### 从源码构建（开发者）

```sh
git clone https://github.com/Fgaoxing/Aether.git
cd Aether
cmake -S . -B build && cmake --build build && ctest --test-dir build   # 宿主机构建
cmake -S . -B build-nolibc -DUSE_HOST_LIBC=OFF && cmake --build build-nolibc   # 静态 nolibc
python3 tools/pty_test.py ./build-nolibc/aether                           # 交互冒烟
```

设计原则：

1. **单一静态二进制**,BusyBox 风格分派，无动态链接依赖
2. **PID 1 语义**——aether 自己完成整个启动链
3. **救援优先**——任何失败都掉入救援，绝不 panic
4. **不需要宿主机服务**——网络尽力而为（DHCP 重试或静态 IP）
5. **英文输出**；中文仅出现在内部注释中

### Milestones

- v0.1 —— 核心 shell + init 流程
- v0.2 —— 完整救援工具链 + PID 1 + EFI cmdline 编辑
- v0.2.1 (2026-11-15) ——
  - zstd + gzip 压缩/解压；`zcat`；与官方工具字节级一致
  - `resume=` 休眠支持 + **无 cmdline 休眠恢复**（`aether-resume` hook)
  - `aether-microcode`（base + local 裁剪）、`aether-everything`、`aether-hostdrv` hook
  - DELETE-to-rescue 后台监视器（evdev + 控制台原始模式）
  - `aether_rescue` cmdline token → 确定性救援
  - DHCP 报文路径修复（msgtype / XID 比较）、USB 网卡热插自动配置、telnetd EOF 修复
  - 所有面向用户的输出英文化
  - `tools/register-aether-efi.sh`（EFI 启动项注册；`--add-rescue-entry`）

### 许可

[MIT](LICENSE) © 2026 Fgaoxing
