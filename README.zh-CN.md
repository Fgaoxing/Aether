# aether · Aether 救援工具箱

[![build](https://github.com/Fgaoxing/Aether/actions/workflows/build.yml/badge.svg)](https://github.com/Fgaoxing/Aether/actions/workflows/build.yml)

[English](README.md)

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
| 系统 | `lspci lsusb scsi serial raidautorun kbd_mode hotplug swapon swapoff` |
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
| `aether` | **必需**——添加 `/init`（aether)、`/newroot`、`nvme` 模块；**内置：镜像构建时自动探测 `/etc/fstab` 里的 swap UUID 烘焙进 `/etc/aether/resume`**——休眠恢复**无需 cmdline 参数** |
| `aether-keyboard` | PS/2 + USB 键盘（`atkbd`/`i8042`/`serio_raw`/`usbhid`/`xhci-hcd`/`ehci-hcd`） |
| `aether-usb` | USB 主控、HID、USB 存储 |
| `aether-network` | 约 60 个网卡驱动：Intel/Realtek/Broadcom + virtio/vmxnet3/Hyper-V + USB 有线网卡——救援即可 DHCP + telnetd |
| `aether-resume` | **deprecated no-op**——休眠恢复自动检测已内置到 `aether` 核心 hook（本 hook 仅为兼容保留） |
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
- **init 救援 shell 里的 `exit` / `rescue-boot` 是交给 PID 1 的启动重试请求**（退出码 42）：
  由 PID 1 本人执行重试——`pivot_root`/`execve` 替换的是 PID 1,成功后真正的 init 成为 PID 1。
  重试失败（root 错误/设备缺失/init 不存在……）时，PID 1 会先清掉其余 rescue 子进程，
  **重新拉起整套救援环境**——启动失败永远能回到救援 shell（已在 QEMU 验证）。
  覆盖 cmdline:`rescue-boot root=/dev/sdb2 rw`（写入 `/etc/.rescue-boot-override`,PID 1 重试时消费）。

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

**aether** hook 在镜像构建时扫描 `/etc/fstab` 中的 `swap` 条目，并把它烘焙进镜像的
`/etc/aether/resume`（可用 `AETHER_RESUME=resume=UUID=…` 或 `AETHER_RESUME="resume=/dev/sda2 resume_offset=…"`
构建期覆盖——用于给别的机器构建镜像）。启动时，当 cmdline 没有 `resume=` / `resume_offset=`，PID 1 读取它，
把 `UUID=` 解析为 major:minor 后写入 `/sys/power/resume`。cmdline 上的
`resume=` / `noresume` 始终优先；文件型 swap 仅在页大小 == 文件系统块大小（都是 4096）时
才用 `filefrag`。

```
resume: using /etc/aether/resume (auto-detected): UUID=fe90b9d4-…
resume: using /dev/disk/by-uuid/fe90b9d4-… -> /dev/vda
resume: wrote /sys/power/resume=254:0 (device /dev/vda)
```

PID 1 在 uevent 排空后还会填充 **`/dev/disk/by-uuid/<uuid> -> ../../<dev>`** 软链接
（ext4 / vfat / btrfs / swap 签名；initramfs 里没有 udev 做这件事）——resume 解析
`UUID=` 时优先走这条链接，失败再回退到 swap 签名探测。

已在 QEMU 上验证到写入 `/sys/power/resume` 这一步（2026-11-15)；**完整往返
（休眠 → 掉电 → 启动 → 会话恢复）已在 QEMU 验证（2026-11-17)**（见验证状态）。

**验证笔记（2026-11-17，QEMU)：**

- QEMU 不实现 ACPI S4，默认 `platform` 休眠模式会一直挂住。虚拟机里测休眠用
  `echo shutdown > /sys/power/disk` 再 `echo disk > /sys/power/state`。
- 休眠后 swap offset 4086 的签名变成 `S1SUSPEND`（不是 `SWAPSPACE2`)——内核会用休眠镜像头覆盖
  swap 头。aether 的 `swap_probe` 两种都识别，所以休眠前后都能定位设备。
- 真实机器上，**普通** Arch 启动项不会自动恢复：除非 cmdline 带 `resume=UUID=…` 或者普通镜像
  镜像是用 `aether` hook 构建的（自动烘焙 fallback，两者都没有时，正常启动会被 `systemd-hibernate-clear.service`
  悄悄清掉残余镜像——这正是"和没有一样"的表象）。推荐在 aether 救援入口的用法：休眠 → 从 aether
  救援入口启动 → 在救援提示符下按 `exit`(PID 1 在重试前完成 resume 写入，早于启动流程）→
  内核在恢复点直接切回之前的会话。这就是上面 QEMU 端到端验证的路径。

#### EFI 启动项集成

`tools/register-aether-efi.sh` 可以把 `/efi/aether/` 下的救援镜像注册为 EFI 启动项
（loader `\EFI\arch\vmlinuz-linux`，每项自带 `initrd=`）。

```sh
sudo ./tools/register-aether-efi.sh                  # 注册全部镜像
sudo ./tools/register-aether-efi.sh --add-rescue-entry   # 注册唯一的"永远救援"启动项（当前 cmdline + aether_rescue)
sudo ./tools/register-aether-efi.sh --list / --uninstall / --dry-run
```

### 打包与 CI

**GitHub Actions**（`.github/workflows/build.yml`）在每次 push/PR 和打 tag 时构建：

| job | 内容 |
| --- | --- |
| `x86_64` | host libc 构建 + `ctest` + nolibc 静态构建 |
| `i386` | nolibc 静态交叉构建（clang `--target=i386-unknown-linux-gnu`) |
| `aarch64` | nolibc 静态交叉构建（`aarch64-linux-gnu-gcc`) |
| `riscv64` | nolibc 静态交叉构建（`riscv64-linux-gnu-gcc`) |
| `release`(tag `v*` 时） | 创建 GitHub Release，包含 `aether-{x86_64,i386,aarch64,riscv64}-nolibc` + `SHA256SUMS` |

**AUR 打包模板**（尚未提交到 AUR——见 `packaging/aur/README.md` 提交方法）：

| 包 | 构建方式 |
| --- | --- |
| `aether-git` | 从 git HEAD 构建；静态 nolibc 二进制，无运行时依赖 |
| `aether-bin` | 预编译静态 nolibc 二进制 + 来自 GitHub Release 的 hook |

两个包都安装 `/usr/bin/aether`，并把 mkinitcpio install hook 安装到 `/usr/lib/initcpio/install/`,
互斥（都 `provide=('aether')`）。

## 已知限制

- 无语法高亮；`less` 不支持搜索；编辑器无撤销/搜索
- 所有时间均为 UTC；`ls -l` 不排序（磁盘顺序）
- `sort`/`shuf`/`tail` 会把 ≤ 2 MB 的文件整个读入内存
- `chattr` 写入需要 CAP_LINUX_IMMUTABLE（否则 EPERM——救援中挂载后以 root 操作即可）
- `kbd_mode` 仅 VT 控制台可用（pty/serial 正确返回 `ENOTTY`）
- `raidautorun` 需要 mdadm（内核 RAID_AUTORUN ioctl 自 ≥ 4.x 起已移除）
- resume 支持 `resume=/dev`、`PARTUUID=`、`PARTLABEL=`、`UUID=`（swap 签名探测，含休眠态 `S1SUSPEND`)
  及 major:minor 直通——不支持 `LABEL=`，不自动激活 dm-crypt/LVM
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
- ✅ **QEMU 完整休眠往返**:`echo disk > /sys/power/state` → 掉电 → 再次启动自动检测休眠中的
  swap(S1SUSPEND 签名）→ 写入 `/sys/power/resume=254:0` → 内核恢复之前的会话
  （端到端验证；恢复后会话中 marker 文件仍在）
- ✅ init 救援 shell `exit` → PID 1 本人重试启动；失败重试会重新拉起救援 shell（QEMU 验证两次）
- ✅ 启动时填充 `/dev/disk/by-uuid`（无 udev);resume 经该链接解析 swap `UUID=`（QEMU)
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
.github/workflows/      CI（push/PR/tag 构建，tag 时发布产物）
packaging/aur/          AUR 模板（aether-git、aether-bin）
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
- v0.2.4 (2026-11-17) ——
  - GitHub Actions CI:x86_64 libc+测试、nolibc;i386/aarch64/riscv64 交叉构建；tag 时发布产物
  - AUR 模板（`aether-git`、`aether-bin`),`packaging/aur/`
- v0.2.3 (2026-11-17) ——
  - **休眠恢复自动检测并入 `aether` 核心 hook**（不再需要单独 hook）
  - `aether-resume` 弃用为 no-op（兼容保留）
  - `AETHER_RESUME` 构建期覆盖
- v0.2.2 (2026-11-17) ——
  - `swapon` / `swapoff` applet
  - `swap_probe` 识别休眠态 swap(`S1SUSPEND`，与 `SWAPSPACE2`/`SWAP-SPACE` 并列）
  - `AETHER_RESUME` 环境变量构建期显式指定 resume spec
  - 休眠恢复端到端验证通过（QEMU：掉电→再启动→内核恢复会话）
- v0.2.1 (2026-11-15) ——
  - zstd + gzip 压缩/解压；`zcat`；与官方工具字节级一致
  - `resume=` 休眠支持 + **无 cmdline 休眠恢复**（烘焙进 `/etc/aether/resume`，现已内置到 `aether` 核心 hook）
  - `aether-microcode`（base + local 裁剪）、`aether-everything`、`aether-hostdrv` hook
  - DELETE-to-rescue 后台监视器（evdev + 控制台原始模式）
  - `aether_rescue` cmdline token → 确定性救援
  - DHCP 报文路径修复（msgtype / XID 比较）、USB 网卡热插自动配置、telnetd EOF 修复
  - 所有面向用户的输出英文化
  - `tools/register-aether-efi.sh`（EFI 启动项注册；`--add-rescue-entry`）

### 许可

[MIT](LICENSE) © 2026 Fgaoxing
