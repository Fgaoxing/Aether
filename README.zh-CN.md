# aether · Aether 救援工具箱

[![build](https://github.com/Fgaoxing/Aether/actions/workflows/build.yml/badge.svg)](https://github.com/Fgaoxing/Aether/actions/workflows/build.yml)
[English](README.md)

**aether** 是一个 BusyBox 风格的 Linux initramfs 救援工具箱：单一静态二进制，通过 `argv[0]` 分派多个 applet。它既可作为交互式 shell 使用，主要用途是作为 initramfs 中的 **PID 1**：挂载 `/proc`/`/sys`/`/dev`、加载模块、解析内核命令行、解析并挂载 root、`pivot_root`，最后 `execve` 真正的 init。任何失败都会进入交互式救援 shell，而不是 kernel panic。

## 特点

- **小而静态**：纯 C，无 C++/ncurses/readline/pthreads；静态 `-Os` 构建；nolibc 变体不依赖 libc。
- **双构建**：host libc 用于开发/测试，nolibc 用于 initramfs 部署。
- **完整 PID 1 启动链**：模块加载、uevent 泵、root 解析、休眠恢复、`pivot_root`、`execve`。
- **救援优先**：启动失败进入可用 shell；`exit`/`rescue-boot` 由 PID 1 重试启动。
- **确定性进入救援**：cmdline 加 `aether_rescue`，或启动时按 `DELETE`（后台监视，零启动开销）。
- **救援网络**：自动 DHCP + telnetd（`0.0.0.0:23`，无认证明文，仅限可信网络），也可手动 `netup`/`ifconfig`/`udhcpc`。
- **休眠恢复**：构建镜像时从 `/etc/fstab` 自动检测 swap；无需 cmdline `resume=`。

> **状态**：x86_64 已在 QEMU 端到端验证（启动、救援、DELETE 进入救援、DHCP/telnetd、休眠恢复）。i386 已验证。aarch64/riscv64/loongarch64 仅构建。见 [验证状态](#验证状态)。

## 快速开始

### 构建

```sh
# 宿主机构建（开发 + 测试）
cmake -S . -B build && cmake --build build && ctest --test-dir build

# 静态 nolibc 构建（initramfs 部署）
cmake -S . -B build-nolibc -DUSE_HOST_LIBC=OFF && cmake --build build-nolibc
```

### 构建 initramfs 镜像（Arch Linux / mkinitcpio）

```sh
mkinitcpio -c initramfs/aether.mkinitcpio.conf -g /tmp/aether-rescue.img
sudo cp initramfs/aether-rescue*.img /efi/aether/
```

`aether` hook 必需，其余按需选择。见 [Initramfs Hooks](#initramfs-hooks)。

## Initramfs Hooks

| Hook | 用途 |
| --- | --- |
| `aether` | **必需**。安装 `/init`、`/newroot`、nvme 模块；构建时从 `/etc/fstab` 检测 swap UUID，写入 `/etc/aether/resume`，支持无 cmdline 休眠恢复。 |
| `aether-microcode` | x86 微码裁剪（Intel/AMD）。默认仅本机匹配，与 `iucode_tool -S` 一致；支持 `AETHER_MICROCODE_BASE_LOCAL=1`、`AETHER_MICROCODE_KEEP_ALL=1`、`AETHER_MICROCODE_DEBUG=1`。 |
| `aether-keyboard` | PS/2 + USB 键盘驱动。 |
| `aether-usb` | USB 主控、HID、USB 存储。 |
| `aether-network` | 常见有线网卡驱动（Intel/Realtek/Broadcom/virtio/vmxnet3/Hyper-V/USB 有线）。不支持 WiFi。 |
| `aether-hostdrv` | 仅构建机驱动裁剪。**不要部署到其他机器。** |
| `aether-everything` | 大体积：常见 WiFi 驱动 + 固件。 |
| `aether-resume` | 已弃用 no-op；恢复检测已内置于 `aether`。 |

> mkinitcpio 自带 `autodetect`/`strip` 对这些镜像不是必需。

## 镜像规格

以下大小来自仓库当前 `initramfs/` 本地构建产物（近似值，会随内核/模块/微码变化）。

| 产物 | 大小 | 内容 |
| --- | ---: | --- |
| `aether-rescue-no-microcode.img` | 328K | 仅 `aether` |
| `aether-rescue-min.img` | 538K | `aether` + microcode |
| `aether-rescue-no-network.img` | 548K | 默认组合减 network |
| `aether-rescue-no-keyboard.img` | 4.5M | 默认组合减 keyboard |
| `aether-rescue-no-usb.img` | 4.5M | 默认组合减 USB |
| `aether-rescue.img` | 4.5M | 默认：keyboard + USB + network |
| `aether-rescue-hostdrv.img` | 1.1M | 仅构建机驱动（仅限本机构建机） |
| `aether-rescue-full.img` | 54M | 全量 + WiFi 驱动/固件 |

## 使用救援模式

### 进入救援

- **命令行 token**：在内核 cmdline 添加 `aether_rescue`（或 `=1|y|yes|true|on`）。PID 1 会在模块加载后跳过正常启动，直接进入救援。
- **键盘**：启动时按 `DELETE`/`Backspace`。后台监视器监听 evdev 和 console raw 输入，不增加启动时间。

### 修复并继续启动

- `cmdline` — 查看 `/proc/cmdline`
- `rescue-boot [root=... rw ...]` — 立即重试启动（当前或覆盖 cmdline）
- `efi-cmdline` / `efi-cmdline edit` — 查看/编辑 EFI BootCurrent cmdline，确认后写入 NVRAM，可选立即重试
- init 拉起的救援 shell 中 `exit` 会请求 PID 1 重试启动；重试失败时 PID 1 重新拉起救援

### 网络

后台守护每 2 秒重试：检测非 lo 网卡 → DHCP → 启动 telnetd `0.0.0.0:23`（**无认证、明文，仅限可信网络**）。`nonet` 禁用自动配置。

手动：`netup [iface]`、`ifconfig`、`udhcpc [-i iface] [-t N] [-T N]`、`telnetd [-p port]`。

### 休眠恢复

`aether` hook 构建时扫描 `/etc/fstab` 中的 swap，写入 `/etc/aether/resume`。启动时若 cmdline 无 `resume=`/`resume_offset=`，PID 1 自动使用它。cmdline 的 `resume=`/`noresume` 优先级更高。

PID 1 还会填充 `/dev/disk/by-uuid/<uuid>` 链接（ext4/vfat/btrfs/swap 签名），使 UUID resume 在无 udev 的 initramfs 中可用。

注意：
- QEMU 无 ACPI S4；VM 休眠测试用 `echo shutdown > /sys/power/disk` 后再 `echo disk > /sys/power/state`。
- 休眠后的 swap 签名会变为 `S1SUSPEND`；aether 可识别。
- 推荐流程：休眠 → 启动 aether 救援入口 → 救援提示符输入 `exit` → 内核恢复会话。

## Applets

约 83 个入口点（65 个 applet + 18 个 builtin），BusyBox 风格 `argv[0]` 分派。

| 类别 | Applet |
| --- | --- |
| 核心 | `help less editor ps free dmesg lsblk efibootmgr clear` |
| 文件/文本 | `ls stat readlink realpath basename dirname chmod chown chgrp chattr lsattr touch truncate mkdir rmdir rm cp mv dd cmp diff sort uniq cut paste join split fold shuf head tail wc strings find grep` |
| 十六进制 | `xxd od hexdump hexedit` |
| 锁/同步 | `flock fsync sync` |
| 压缩 | `zcat`（zstd + gzip，与官方工具字节级一致） |
| 网络 | `ifconfig udhcpc telnetd netup` |
| 系统 | `lspci lsusb scsi serial raidautorun kbd_mode hotplug swapon swapoff` |
| 电源 | `reboot halt` |
| builtin | `cd exit exec pwd echo [ test mount umount chroot switch_root syscall true false : cmdline rescue-boot efi-cmdline` |

设计规则：单用户、root-only、无 uid/gid 名称解析、无 setuid/capability 语义，不要求 GNU 兼容输出。

## CI / 打包

GitHub Actions（`.github/workflows/build.yml`）在 push/PR/tag 时构建：x86_64 host+tests+nolibc，i386/aarch64/riscv64/loongarch64 nolibc 交叉构建，`v*` tag 发布产物。

AUR 模板在 `packaging/aur/`：`aether-rescue-git` 和 `aether-rescue-bin`。

## 已知限制

- 无语法高亮；`less` 不支持搜索；编辑器无撤销/搜索。
- 所有时间 UTC；`ls -l` 不排序。
- `sort`/`shuf`/`tail` 会把 ≤ 2 MB 文件整体读入内存。
- `chattr` 写入需要 `CAP_LINUX_IMMUTABLE`。
- `kbd_mode` 仅 VT 可用（pty/serial 返回 `ENOTTY`）。
- `raidautorun` 需要 mdadm（较新内核已移除 RAID_AUTORUN ioctl）。
- resume 支持 `resume=/dev`、`PARTUUID=`、`PARTLABEL=`、`UUID=`（含休眠态 `S1SUSPEND`）和 major:minor；不支持 `LABEL=`，不自动激活 dm-crypt/LVM。
- EFI/NVRAM 写入请先在牺牲用 VM 验证。
- `lsblk` 不展开 btrfs 子卷。
- aarch64/riscv64/loongarch64 运行时未测试。

## 验证状态

已验证（x86_64，QEMU/host，截至 2026-11）：
- libc 构建 + `ctest` 2/2；nolibc 构建；pty 冒烟。
- i386 nolibc 完整启动路径。
- PID 1 完整启动（UUID 解析 → btrfs 挂载 → pivot_root → exec → 关机）。
- bad root 回退救援；DELETE-to-rescue（evdev + 串口 raw）；`aether_rescue` token。
- `exit`/`rescue-boot` 重试；失败重试重新拉起救援。
- DHCP + telnetd 端到端（开机/热插，入站 shell，Ctrl-C/IAC 断开）。
- 休眠恢复写回与 QEMU 休眠往返（识别 S1SUSPEND，会话恢复）。
- 无 udev 填充 `/dev/disk/by-uuid`。
- `zcat` 与官方 zstd/gzip 字节级一致；`grep` 与 GNU grep 对比 31 个模式。

未验证：aarch64/riscv64/loongarch64 运行时；真机休眠往返；真机 EFI/NVRAM 写入；dm-crypt/LVM resume；物理 VT USB HID DELETE。

## 仓库结构

```text
CMakeLists.txt          双分支构建（host libc / nolibc）
.github/workflows/      CI
packaging/aur/          AUR 模板
src/                    main、boot、modload、uevent、applets、shell、tools、nolibc
initramfs/              mkinitcpio hooks + aether.mkinitcpio.conf
tools/                  宿主工具（gen_sysnum.py、pty_test.py、register-aether-efi.sh）
tests/                  宿主机构建测试
```

## 许可

[MIT](LICENSE) © 2026 Fgaoxing
