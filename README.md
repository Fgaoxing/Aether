# aether — 启动救援 Shell 工具箱

initramfs 里运行的极简救援环境，BusyBox 式**单二进制多入口**（靠 `argv[0]` 分发 applet）。
需求文档：[启动救援Shell-需求文档.md](启动救援Shell-需求文档.md)（v0.2：x86_64 / aarch64 / i386 / riscv64）。

> 状态：**x86_64 / i386 端到端跑通**（含 PID 1 init 流程、拯救模式、EFI cmdline 编辑）。
> aarch64 编译/链接通过、riscv64 编译通过（本机无 qemu / riscv 链接器）；全部见下方验证矩阵。

## 快速上手

```bash
cmake -S . -B build && cmake --build build && ctest --test-dir build   # host libc + 测试
cmake -S . -B build-nolibc -DUSE_HOST_LIBC=OFF && cmake --build build-nolibc

./build-nolibc/aether              # 进救援 shell（交互）
printf 'lsblk\n' | ./build-nolibc/aether
python3 tools/pty_test.py ./build-nolibc/aether   # 端到端交互冒烟
```

把二进制硬链接为工具名即是对应 applet（如 `ln aether sh`）。裸 `aether`/`sh`/`ash` 进救援 shell；
**PID 1 启动时**自动进 init 流程。

## 能力总览

**shell（§7）**：PDA 单遍解析（`|`、`;`、`&`、`<` `>` `>>` `2>` `2>&1`、引号/转义/`#` 注释）、
行编辑（cbreak+VT100、左右/Home/End/退格、Ctrl-U/A/E）、fork+管道、后台 `&`、重定向、
Ctrl-C 安全（shell 忽略、子进程 DFL——init 不会被信号干掉）。

**内置命令**：`cd exit exec pwd echo cat [ test mount umount chroot switch_root syscall true false : cmdline rescue-boot efi-cmdline`
**applet**：`help less editor ps free dmesg lsblk efibootmgr`
**文件/文本工具（2026-11-14 新增，见下节）**：`ls stat readlink realpath basename dirname chmod chown chgrp chattr lsattr touch truncate mkdir rmdir rm cp mv dd cmp diff sort uniq cut paste join split fold shuf head tail wc strings xxd od hexdump hexedit find flock fsync sync grep`
**电源管理**：`reboot halt`（先 sync 落盘，调 reboot syscall；需要 root/CAP_SYS_BOOT）
**系统/救援工具**：`lspci lsusb scsi serial raidautorun kbd_mode hotplug`
**网络工具**：`ifconfig udhcpc telnetd netup`（rescue 模式自动：检测网卡 → DHCP → 启动 telnetd；`nonet` 关闭；WiFi 不受支持，自动探测跳过无线接口）
**shell 内置（2026-11-14 新增）**：`jobs fg bg`（作业控制，↑/↓ 命令历史）

**救援核心（设计为 init 使用）**：
- aether 作为 **PID 1** 启动时：挂载 proc/sys/devtmpfs → uevent 顶上 /dev 缺口（netlink KOBJECT_UEVENT，
  按 ACTION=add/remove 建/删设备节点）→ 读 /proc/cmdline → 解析 root=/rootfstype=/rootflags=/init=/rootwait/ro →
  resolve 根设备（PARTUUID / PARTLABEL / UUID(ext4/vfat/btrfs) / LABEL / /dev 路径）→ 挂载 /newroot
  （ro 作为 MS_RDONLY，其余 rootflags 原样透传，btrfs `subvol=@` 适用）→ 绑定 proc/sys/dev → pivot_root → execve(init)。
- **任一步失败** → 打印明确的 `BOOT FAILED — 进入拯救模式 (rescue mode)`，进入交互拯救 shell。
- 拯救 shell 内：
  - `cmdline` — 查看 /proc/cmdline
  - `rescue-boot [root=/dev/... rw ...]` — 用当前或给定 cmdline **立即重试启动**
  - `efi-cmdline` / `efi-cmdline edit` — **查看/编辑 EFI efistub 启动项的 cmdline**（BootCurrent 指针，
    缺失则报错）；edit 流程：显示当前 cmdline → 输入新 cmdline → 预览旧/新差异 → `[y/N]` 确认写回 NVRAM
    （保留原 attrs）→ 可选立即用新 cmdline 重试启动
  - `rescue-boot` 是通用修复入口；`efi-cmdline edit` 是持久化修复（改完重启生效）

## 文件 / 文本工具集（2026-11-14 新增 42 个 applet）

需求：initramfs 救援场景的文件/文本/十六进制/锁工具，单用户 root 语义，**输出格式不必与 GNU 一致**，
不提供本机名/用户名解析、不处理 setuid/capability 语义。全部通过真实文件操作实测（md5 校验 / 边界行为 / 退出码）。

| 类别 | applet |
| --- | --- |
| 目录/文件 | `ls`（`-l -a -h -R`，支持组合短选项如 `-lh`）`stat` `touch` `truncate` `mkdir`(-p) `rmdir` `rm`(-r/-f) `cp`(-r/-p) `mv` `dd` `find`(glob/-type/-maxdepth) |
| 元数据 | `chmod`(八进制) `chown` `chgrp`(uid[:gid]) `chattr`/`lsattr`(FS_IOC_GETFLAGS/SETFLAGS) |
| 链接 | `readlink`（`-f` 完整）`realpath` `basename` `dirname` |
| 文本 | `sort` `uniq`(-c) `cut`(-d -f/-c/-b) `paste`(-d) `fold`(-w -s) `shuf`(-n) `join` `split`(-l) `cmp` `diff` `wc` `head`(-n) `tail`(-n/-f) `strings` |
| 十六进制 | `xxd` `od` `hexdump` `hexedit`（全屏编辑器，^S 保存、^X 退出，需 tty） |
| 锁/刷盘 | `flock`（`-s`/`-x`/`-u`/`-n`，可带子命令加锁执行）`fsync` `sync` |

**实测状态（本机 uid=1000，2026-11-14）**：
- cp/mv/dd 按字节 md5 校验通过（含 `cp -r` 目录递归、跨设备 mv 回退路径）；`rm -r`/`mkdir -p`/`rmdir` 树操作正常
- `ls -l/-R/-h`、`stat`、`find`（含 `-maxdepth`）、`realpath`/`readlink`、`diff`（前后缀修剪 + 中段 LCS ≤64 行）、
  `cmp`/`join`/`sort`/`uniq`/`cut`/`paste`/`fold`/`shuf`/`head`/`tail`/`wc`/`strings`/`xxd`/`od`/`hexdump` 输出均正确
- `chattr`：读路径（lsattr）正常；写路径到权限边界止（本机非 root，`+i`/`+a` 明确报 `cannot set flags (1)` EPERM —— 需 CAP_LINUX_IMMUTABLE；
  单用户 rescue 场景挂载后提权即可用）
- `flock` 带子命令在持锁进程内执行（锁在子进程退出时释放）；`tail -f` 200ms 轮询 lseek 增长，实测跟随写入正常
- `hexedit`：全屏交互，需 tty（宿主 uid=1000 无法替代真实机验证 NVRAM/块设备写路径的同理警示见下）
- **已知简化**：`sort`/`shuf`/`tail -f` 全文件缓冲 ≤2MB；`diff` 中段无 Myers 回溯（仅前后缀修剪 + 小段 LCS）；
  `join` 仅按第一字段且两文件需已排序；`ls` 无排序（磁盘顺序）、`-h` 为人类可读但不严格对齐；`wc`/`find`/`split` 大文件路径未做极限测试
- **修复过的关键 bug**：`aether_statx_t` 必须为内核 256 字节（224/136 会栈溢出，`ls -l`/`stat` 崩）——已加 `_Static_assert` 编译期防线；
  `aether_iso_time` 内部 `buf[17]` 写溢（时间串需 20 字节）——已修；`reader` EOF 后水位未清导致 `paste` 死循环——已修。

## 救援增强（2026-11-14 续：42 → 48 applet）

**作业控制（§7.7）**：`cmd &` 立即返回；`Ctrl-Z` 挂起当前前台作业；`jobs` 列出作业、`fg [id]` 调回前台、`bg [id]` 后台继续。
交互模式下 shell 自身有独立进程组并拥有终端（tcsetpgrp），前台作业（pipeline 全部子进程同组）启动时 shell 把终端交给该组；
作业结束/停止后归还。子进程恢复默认信号处置（可被 Ctrl-C/Ctrl-Z 直接终止/停止）。

**↑/↓ 命令历史**：按 ↑/↓ 调用历史命令（环形 64 条，与上一条相同则跳过）。

**init 失败兜底增强**：PID 1 的 boot 流程任一环节失败（挂载/cmdline/pivot/execve），不再只打 banner 进 rescue shell，
而是**枚举所有可用终端/串口**（/dev/console、tty0-3、ttyS0-3、ttyAMA0-1、ttyUSB0-1、hvc0-1、ttymxc0），
对每个能打开且能 TCGETS 的设备各 fork 一个 rescue shell（正确 print banner 到该控制台），PID 1 等所有子进程结束绝不 panic。
见 src/boot.c `rescue_consoles()`。

**lsblk + disks 合并**：仅保留 `lsblk`，输出 `NAME MAJ:MIN RM SIZE RO TYPE ESP MOUNTPOINT`，分区树状（├─/└─），
ESP 列探测 FAT 引导签名（0x55 0xAA @510/511 + "FAT" 字符串；受块设备权限限制，非 root 显示 `?`），
末尾附 efivarfs 挂载状态与 BootOrder（原 disks 内容）。`disks` 已移除。

**新增查看类**：`lspci`（sysfs，class/vendor/driver/irq，含 Intel/AMD/NVIDIA 等常见厂商与类表）、
`lsusb`（sysfs，idVendor:idProduct/class/speed/产品名）、`scsi`（sysfs，type/vendor/model/rev/size）、
`serial`（/proc/tty/driver/serial 详情 + /sys/class/tty 串口节点；/proc/tty/driver/serial 非 root 不可读时优雅跳过）、
`raidautorun`（**优先 mdadm**：`--incremental --run <dev>` / 无参数时 `--assemble --scan`；mdadm 不存在时降级为
iobctl RAID_AUTORUN（内核 6.x 已删除该 ioctl 分支，如实报 ENOTTY + 提示改用 mdadm），并打印 /proc/mdstat）、
`kbd_mode`（KDGKBMODE/KDSKBMODE，`-s/-k/-a/-u/-o` 切换 scancode/keycode/xlate/unicode/off，无参查询；
**需 VT 控制台，pty/serial 无效（ENOTTY 属内核正确行为）**）。

**全屏工具的 tty 回退链**：editor / hexedit / less 不再硬开 `/dev/tty`，改为回退链
`/dev/tty（有 ctty）→ fd0（若本身是 tty）→ /dev/console（initramfs PID 1 常见）→ fd1（若是 tty）`，
裸 initramfs/PID 1 场景也能用。

**网络（救援联网）**：rescue 模式自动检测非 lo 网卡 → 后台守护反复重试（每 2 秒一次）直到配置成功，
成功后自动启动 telnetd（0.0.0.0:23，无认证）；接口已有 IPv4（手动配置或之前重试成功）时自动停止重试；
cmdline 含 `nonet` 则完全不触发自动联网。手动：`netup [iface]`（一次性一键）/ `ifconfig <iface> [up|down|ip[/prefix] [netmask m]|dhcp]` /
`udhcpc [-i iface] [-t N] [-T N]`（DORA）/ `telnetd [-p port]`（默认 23，**无认证**，仅限受信网络）。
DNS 写入 /etc/resolv.conf（失败不致命）；默认路由走 SIOCADDRT；非 root 一律 EPERM 报错，不改动任何状态。
`telnetd` 子进程的 shell 使用行模式 + IAC 剥离（`aether_telnet_mode`）。

**udev 热插拔**：`hotplug` 常驻监听 NETLINK_KOBJECT_UEVENT：add/change → 按 MAJOR/MINOR/DEVTYPE
mknod `/dev/<DEVNAME>`（已存在则跳过），remove → unlink。`hotplug --once` 只处理积压后退出
（等同 init 启动早期的 1.5s pump）。rescue 模式下后台自动启动。

**编辑器/全屏工具渲染修复（2026-11-14）**：所有全屏工具的 `\x1b[K\r\n` 等定长写入统一改为
`sizeof() - 1`；状态栏不再输出多余 CRLF（24 行屏幕曾因此每帧滚动一行、首行被卷走）。

**grep（2026-11-14 新增）**：内置精简扩展正则引擎（src/tools/regex.c，无动态内存、静态节点池）：
`. * + ? {n,m} ^ $ [..] [^..] (..) | \d \s \w \.`；选项 `-i -v -n -c -l -q -e`（可多次 -e，等价 `|` 连接）、
默认读 stdin、多文件带 `file:` 前缀、退出码 0/1/2（类 GNU）。**注意**：`\d \s \w` 为 digit/space/word 类（近似 PCRE），
与 GNU ERE 的 `\d`=字面 d 不同；\`-e\` / 位置参数均为扩展正则；不支持反向引用/环视。
差分测试（31 个模式）与 GNU grep 输出一致（仅 \d 一处为文档化的有意差异）。

**reboot / halt（2026-11-14 新增）**：先 `sync()` 落盘，再调 reboot syscall
（x86_64=169 / i386=88 / aarch64・riscv64=142，magic 0xfee1dead/0x28121969 原样传递）；
`reboot` → RB_AUTOBOOT（0x01234567），`halt` → RB_HALT_POWEROFF（0x4321fedc）。
非 root/CAP_SYS_BOOT 时 EPERM 如实报错，不执行任何关机动作。

## 开机按 DELETE 进入救援模式（2026-11-15 新增）

需求：**开机时检查 DELETE 是否按下；按下则终止正常启动、直接进入拯救模式**（不等 boot 失败）。
aether PID 1 在启动最早期 fork 出一个**后台监听子进程**（watcher）持续监听键盘/控制台；
boot 全程与监听并行，**启动零额外等待**（2026-11-15 从"2 秒阻塞窗口"改为后台并行）。
一旦看到 DELETE/Backspace，PID 1 在下一个检查点（模块加载 / uevent 补齐 / cmdline 之后 /
boot_attempt 入口 / rootwait 轮询中 / pivot_root 之后 exec 之前）中止 boot 进入救援。

**两个通道**（watcher 常驻，直到 exec 前或进救援时停掉）：

- **通道 A（evdev，真键盘）**：/dev/input/event0..31，`EV_KEY` 的 `KEY_DELETE(111)`/`KEY_BACKSPACE(14)`
  按下事件。VT 控制台唯一可靠通道（VT 行规则永远看不到 0x7f）。Arch 内核 `CONFIG_INPUT_EVDEV=y`、
  PS/2（i8042/atkbd）与 USB HID 均内建。
- **通道 B（console raw，串口 ttyS0 / VT）**：输入侧关闭 `ICANON|ECHO|IEXTEN|ISIG|IXON`、`VMIN=0 VTIME=0`；
  **保留 OPOST**，内核 printk 输出不会变成阶梯缩进。识别 `0x7f` 与 `ESC[3~`。

**生命周期 / 收尾（无资源泄漏）**：

- PID 1 进救援 → `rescue_consoles()` 第一行停掉 watcher（SIGTERM + wait4，恢复控制台为 canonical）；
- PID 1 正常 boot → `do_final_switch()` 在 `execve` **之前**停掉 watcher（真正的 init 接管后 DEL 只是普通按键）；
- watcher 停掉之后，boot 完成或救援 shell 中按 DEL 均不再触发；控制台逐字节恢复；无僵尸。

**为什么不是"回溯已按下的键"**：canonical 行规则会把 0x7f 当 erase 吞掉（空缓冲→字节丢弃），
PID 1 启动之前按的键永远救不回来；evdev 事件也只按**已打开的客户端**排队——这些是纯用户态的硬边界，
所以只能"启动期实时监听"，监听是后台任务、与 boot 并行，所以没有任何前台等待。

**QEMU 实测（Arch 7.2.15-arch1-1，initrd=/tmp/aether-test-del.img）**：

| 场景 | 操作 | 结果 |
| --- | --- | --- |
| 通道 B（启动中） | hint 出现后串口发 `\x7f` | ✅ 打印"手动 rescue 模式（启动期按下 DELETE）"进救援 |
| 通道 B（启动中） | hint 出现后串口发 `ESC[3~` | ✅ 同上 |
| 通道 A（启动中） | hint 出现后 QEMU monitor `sendkey delete` | ✅ evdev 捕获，进救援 |
| 通道 B（好 root 启动窗口内） | btrfs + virtio 盘，hint 出现后立即发 `\x7f` | ✅ 中止启动进 rescue（不到达 PowerDown） |
| 通道 B（好 root） | 不按键 | ✅ 正常启动，`reboot: Power down`，QEMU 退出码 0 |
| 不按键 + 坏 root | `root=UUID=bad` | ✅ 原设计路径：BOOT FAILED → 救援 shell |
| 不按键 + 不存在设备 | `root=/dev/vda9` | ✅ "不存在或不可访问（2）" → 救援（未误触发） |
| ⚠ 坏 root 极速失败 | 在 boot 已失败打印后才发 DEL | ⏭ 晚于最后一个检查点，DEL 进入救援 shell 本身（不触发强制 rescue——boot 已经结束了）|

> 说明：bad root 的 boot 在 QEMU 里 <100ms 结束（uevent pump 无事件即返回），"启动中按 DEL"
> 能否赶上只取决于 boot 完成速度；好 root 的启动路径（mount/pivot/exec，~1-3 秒）有充足窗口。
> 真机 VT 请在看到 `aether: DELETE 后台监听已启动` 之后按 DEL。
>
> 局限：**PID 1（hint 行）之前按的键检测不到**（canonical 吞 DEL / evdev 未打开客户端）；
> **exec 真正 init 之后 DEL 不再中止**（控制台已恢复、init 接管）；watcher 进程理论上常驻至 exec，
> 每次启动 fork 一个 ~2K 页的轻量子进程。

## initramfs 镜像（mkinitcpio hook，2026-11-15 更新）

aether 可以作为 PID 1 驱动救援 initramfs。**aether 是唯一 hook**（`initramfs/hooks/install/aether`）+
一个 "copy 一份" 现有 mkinitcpio.conf，不需要 busybox / udev / glibc。
可选的依赖 hook 按需加入（**默认全关**，用法：`HOOKS=(aether [aether-keyboard] [aether-usb] [aether-network])`）：

```
initramfs/hooks/install/aether          # 必须：add_file aether /init；add_dir /newroot；add_module nvme
initramfs/hooks/install/aether-keyboard # 可选：PS/2 + USB 键盘模块（atkbd/i8042/serio_raw/usbhid/xhci-hcd/ehci-hcd）
initramfs/hooks/install/aether-usb      # 可选：USB host 控制器 + USB HID + USB 存储相关 
initramfs/hooks/install/aether-network  # 可选：网络接口驱动（Intel/Realtek/Broadcom/virtio/vmxnet3/HV），
                                        #       rescue 模式立即可 udhcpc/netup/ifconfig/telnetd
                                        # 注意：mkinitcpio 原装 autodetect/strip hook 对本镜像**没有效果**——
                                        #       .ko.zst 已压缩 strip 不减体积；
                                        #       autodetect 仅作用于 `add_checked_modules()`，本 hook 全用 plain `add_module()`，
                                        #       故 image 不随构建机器裁减（按 rescue 目的：未知硬件）。
                                        #       想做小镜像：见下方 AETHER_NETWORK_DRIVERS 环境变量模式。
initramfs/hooks/install/aether-microcode # 可选：x86 CPU 微码删减——"基础微码 + 本机微码"：
                                        #       "基础" = /boot/<vendor>-ucode.img 内每个 (family, model) 元组保留
                                        #       最新（最高 rev）一条记录（18 条，≈4.2 MiB）——覆盖所有 Intel CPU 世系的最新修正；
                                        #       "本机" = 与构建机 CPU family+model 完全匹配的全部 rev 额外保留。
                                        #       这样所有常见 CPU 都有可用的最新微码，又避开历史 rev 的 14 MB 膨胀。
                                        #       想最极限省体积：AETHER_MICROCODE_LOCAL_ONLY=1（≈330 KiB，本机无匹配=无 early file）。
initramfs/hooks/install/aether-everything # 可选：全量无线套件（iwlwifi/rtw88/rtw89/ath/mt792x/brcmfmac 常见笔记本 WiFi + 自动 firmware）：
                                        #       给默认镜像加 WiFi 支持（本程序不支持 rescue 配置 WiFi——仅驱动在、接口不参与枚举）。
                                        #       全量镜像 ≈60 MiB。正常用默认 hook，不想 WiFi 就不加它。
initramfs/hooks/install/aether-hostdrv  # 可选：本机版驱动，**剪枝**（不加入任何新模块）：
                                        #       从 aether-keyboard/usb/network 通用集 ∩ 本机构建机 modalias 解析，
                                        #       仅保留本机键盘（serio/USB HID）/有线网卡/USB 主控所需模块。
                                        #       例外（有意的救援底线）：USB 有线网卡系列驱动恒保留。
                                        #       WiFi 不支持，无线网卡/mac80211/iwlwifi 全部不进镜像；
                                        #       侦测是构建机的——绝不可跨机器部署（同 mkinitcpio autodetect）。
initramfs/aether.mkinitcpio.conf        # copy 自 /etc/mkinitcpio.conf（本机），仅改两处：
                                        #   HOOKS=(aether [optional hooks])  （替换 base/udev/...整链）
                                        #   MODULES_DECOMPRESS="yes"  （镜像内存放裸 .ko，finit_module 直接加载）
                                        # MODULES=(nvme nvme-core)、COMPRESSION="lz4" 保持原样
```

**hook 矩阵**（所有测试基于 QEMU +  Arch 7.2.15-arch1-1，全 hw 信息见每项结果）：

| HOOKS 组合 | 大小 | 含模块（个） | rescue 网络自动联网 | 文件系统 | 真机启动 |
| --- | --- | --- | --- | --- | --- |
| `(aether)` | ~330 KiB | 4（nvme-auth/keyring/core/nvme） | ❌ | ext4/vfat/btrfs | ✅ btrfs full-boot POWER_OFF |
| `(aether aether-network)` | ~4 MiB | 33（virtio_net/e1000e/r8169/ixgbe/i40e/bnxt_en/…） | ✅ modload 兜底扫到 virtio_net → 自动 DORA + telnetd | ext4/vfat/btrfs | ✅ btrfs full-boot POWER_OFF；telnetd 监听 0.0.0.0:23，宿主机 `127.0.0.1:8888` 可连上 shell |
| `(aether aether-network)` + `AETHER_NETWORK_DRIVERS="virtio_net"` | ~412 KiB | 7（nvme×4 + failover/virtio_net/ptp） | ✅ 仅适用于 virtio 环境（QEMU/KVM VM rescue） | ext4/vfat/btrfs | ✅ 同上 |
| `(aether aether-network)` + `AETHER_NETWORK_DRIVERS="virtio_net e1000e r8169"` | ~858 KiB | 16 | ✅ 同上，QEMU + 真机 Intel/Realtek 都能用 | ext4/vfat/btrfs | 未测（AC/EFI 硬件握手串口打印） |
| `(aether aether-keyboard aether-usb aether-network)` | ~4 MiB | 33+（所有相关 USB/HID 模块都打进去了，因为 ARM/Atom builtin 的不加） | ✅ 同上 | ext4/vfat/btrfs | ✅ 同上；XHCI/EHCI builtin 故手动静默覆盖 |
| **打包产物（含基础微码，2026-11-15 默认 x86_64：`aether aether-microcode ...`）** | | | | | |
| `initramfs/aether-rescue.img` = `(aether aether-microcode aether-keyboard aether-usb aether-network)` | ~7.98 MiB | 34（含基础微码 early file ≈4.2 MiB，18 条） | ✅ | ext4/vfat/btrfs | ✅ 好 root btrfs→PowerDown（rc=0）；坏 root rescue ✅ |
| `initramfs/aether-rescue-no-keyboard.img` = `(… 无 aether-keyboard)` | ~7.97 MiB | 33（少 serio_raw） | ✅ | 同上 | ✅ rescue 实测 |
| `initramfs/aether-rescue-no-usb.img` = `(… 无 aether-usb)` | ~7.98 MiB | 34（本内核 USB 全内建，与主镜像仅 3 字节差异；USB=m 的内核会更小） | ✅ | 同上 | ✅ rescue 实测 |
| `initramfs/aether-rescue-no-network.img` = `(… 无 aether-network)` | ~4.48 MiB | 5（nvme×4 + serio_raw） | ❌（无网卡驱动，autoconfig 静默等网卡出现，不刷屏；可用 nonet 关闭） | 同上 | ✅ rescue 实测 |
| **`initramfs/aether-rescue-hostdrv.img` = `(aether aether-microcode aether-hostdrv)`（本机）** | ~5.1 MiB | 38（serio_raw + USB 有线网卡全集（恒保留）+ phy/mdio 依赖 + nvme×4；无无线） | ❌（为构建机本机构建；本机无 NIC，autoconfig 静默等待） | ext4/vfat/btrfs | ✅ rescue 实测；13→38 模块全并行加载；**本镜像仅构建机本机有效** |
| **`initramfs/aether-rescue-min.img` = `(aether aether-microcode)`（本体最小版）** | ~4.5 MiB | 4（仅 nvme×4；无键盘/USB/网络/侦测） | ❌ | ext4/vfat/btrfs | ✅ rescue 实测 |
| **`initramfs/aether-rescue-no-microcode.img` = `(aether)`（裸最小版，不要微码）** | **~334 KiB** | 4（仅 nvme×4；early file 为空；无微码无键盘 USB 网络） | ❌ | ext4/vfat/btrfs | ✅ rescue 实测（4 次连续全部通过） |
| **`initramfs/aether-rescue-full.img` = `(aether aether-microcode aether-keyboard aether-usb aether-network aether-everything)`（全量最大版）** | ~57 MiB | 87（通用三件套 + Intel/Realtek/Atheros/MediaTek/Broadcom WiFi 常见套件 + 76 个 WiFi firmware ucode） | 同上（设计目标=最大兼容） | ext4/vfat/btrfs | ✅ rescue 实测（"兜底扫描 35 → 33 加载，2 失败"）；好 root btrfs→PowerDown（rc=0）✅ |
| `(aether aether-microcode)` 默认（base+local） | ~4.5 MiB | 4 + 18 条基础微码记录（早文件 ≈4.2 MiB） | ❌ | ext4/vfat/btrfs | ✅ rescue boot 成功（含 DEL 检测提示行；image 内容=基础集合） |
| `(aether aether-microcode)` + `AETHER_MICROCODE_CPUINFO=/tmp/fake_cpuinfo_5.cpuinfo`（family=6 model=5 合成） | ~5.2 MiB | 4 + 基础 18 条 + 本机(model=5) 全部 rev | ❌ | ext4/vfat/btrfs | ✅ 未单独启动测（仅为体积基准）；代码路径与默认一致 |
| `(aether aether-microcode)` + `AETHER_MICROCODE_LOCAL_ONLY=1`（本机无匹配 → 无 early file） | ~332 KiB | 4 | ❌ | ext4/vfat/btrfs | ✅ rescue boot 成功（early file 为空；最轻量，放弃其他 CPU 的微码） |
| `(aether aether-microcode)` + 完整微码对比（`AETHER_MICROCODE_KEEP_ALL=1`）| ~15 MiB | 4 | ❌ | ext4/vfat/btrfs | ✅ rescue boot 成功（early file 是整包 15 MB 微码）|
| `(aether strip aether-network)` | ~4 MiB | 33 | ❌ | ext4/vfat/btrfs | ⚠️ 无效：所有 .ko.zst 已是压缩数据（再压只会膨胀）；strip 只剥已链接符号，对整体镜像无意义 |

> `aether-microcode` hook 会提取 `/boot/<vendor>-ucode.img` 内的 microcode container，并做删减：
> **默认 = "基础 + 本机"**：① "基础" = 每个 (family, model) 元组保留最高 rev 一条（Intel 包 222 条 → 18 条，14.6 MB → 4.2 MiB）——
>   所有 i5/i7、Core/Atom/Xeon 世系都有一份可用的最新修正，这是 rescue 镜像需要的"共有问题"基线；
>   ② "本机" = 与构建机 CPU family+model 匹配的全部 rev（用于保留配套历史修正的特殊场景）。
> - `AETHER_MICROCODE_LOCAL_ONLY=1`：只保留本机（sub-set；本机构建时 0 匹配 → early file 为空，image ~332 KiB，最轻量）
> - `AETHER_MICROCODE_KEEP_ALL=1`：整包原样装（对比/生产用，~15 MiB）
> - `AETHER_MICROCODE_CPUINFO=/path/to/cpuinfo`：用指定文件冒充本机 CPU 信息（测试/调试；例如 /tmp/fake_cpuinfo_5）
> - `AETHER_MICROCODE_DEBUG=1`：打印每条记录的保留/跳过详情
> 当前 host Intel `/boot/intel-ucode.img` 不含 11th Gen Tiger Lake 的微码（family=6 model=0x8c），
> 所以"本机"集为空——基础集合（18 条最新 rev）照样让 image 可用。AMD 微码格式未实测（untested：`amd-ucode.img` 当前没装在这台机器上）。

> `MODULES_DECOMPRESS="yes"` 在最新 mkinitcpio 上默认激活。
> **启动并行化**（2026-11-15）：① DELETE 后台监听 fork 并行、boot 零等待（见上文）；
> ② **模块加载并行**——兜底扫描内每个 pass 把所有未加载模块 `fork` 出子进程并行 `finit_module`
>    （`.ko.zst` 由内核在调用进程上下文解压，天然 SMP 并行；pass 间保持多遍语义以自然满足依赖顺序；
>    fork 失败时自动退回串行）；`/etc/aether/modules.order`（用户显式列表）因指定了依赖顺序而保持串行；
> ③ 救援网络自动配置后台守护（非阻塞）；④ uevent pump 队列空即返回（QEMU 无事件 ≈0 等待）。
>    **仍然串行**的是无法并行的环节：SeaBIOS/iPXE、内核解压与早期初始化、hibernation resume 探测、
>    resolve root/挂载、pivot_root、exec——其中 SeaBIOS+iPXE 与内核占整体启动 ~2/3，
>    远超 aether 自身（<100ms 失败路径）；实测 QEMU 好 root 启动 smp=1 4.2s→3.9s、smp=4 3.9s→3.7s（-kernel 时间戳 2.81→2.60 / 2.56→2.47）。
> AETHER PID 1 先挂 /sys /proc /dev，再 fork **后台 DELETE 监听**（见上文"开机按 DELETE 进入救援模式"），
> 然后扫描 /lib/modules 并行加载各 .ko/.ko.zst（多遍重试直到无新增），uevent 补 /dev 节点，然后解析 root=，失败则启动 rescue shell 枚举所有可用 tty/serial 设备并起 shell。

按 hook 选择 rescue 能力：**仅 `(aether)` 适合纯 VGA/serial 硬件问题的救援**（没有网络 rescue 能力，lsblk/edit/救援工具仍能用）；如需网络 telnet + 远程 rescue 必须带 `aether-network`；如需 USB 键盘控制台必须带 `aether-keyboard`/`aether-usb`。

```sh
# 重新生成镜像：按场景选择
mkinitcpio -c initramfs/aether.mkinitcpio.conf -g /tmp/rescue-min.img   # (aether) ~330K
mkinitcpio -c /tmp/rescue-net.conf -g /tmp/rescue-net.img               # (aether aether-network) ~4M
AETHER_NETWORK_DRIVERS="virtio_net" mkinitcpio -c /tmp/rescue-net-min.conf -g /tmp/rescue-virtio.img
AETHER_NETWORK_DRIVERS="virtio_net e1000e r8169" mkinitcpio -c /tmp/rescue-net-small.conf -g /tmp/rescue-net-small.img
mkinitcpio -c /tmp/rescue-full.conf -g /tmp/rescue-full.img             # (full)
# （上列中的每种都已 QEMU 实测：rescue boot 成功，qemu rc=0 / poweroff）
```

如需 rescue 网络能力但又要小镜像（只放弃其他 NIC）：
- `AETHER_NETWORK_DRIVERS="virtio_net"` → ~412 KiB，适合用作 QEMU/KVM 内救援虚拟机（最小化 + 网络 ssh/telnet）
- `AETHER_NETWORK_DRIVERS="virtio_net e1000e r8169"` → ~858 KiB，物理机 rescue + 网络（覆盖 Intel/Realtek 绝大多数机器）
- 未设置 AETHER_NETWORK_DRIVERS 则默认打包全部 NIC（~4 MiB），最稳

> **自动探测（autodetect）与 strip 都对本镜像无效**：`.ko.zst` 已是压缩数据，strip 去 debug 符号没体积收益；autodetect 仅过滤 `add_checked_modules()`（标准 udev/fsck hook），本 hook 用 plain `add_module()`，故镜像内容与构建机器当前使用无关（按预期）。
> **AETHER_NETWORK_DRIVERS 模式**即所谓的"驱动删减"：根据目标 rescue 硬件预先挑选 NIC 编译进 image，体积可压到 ~412 KiB。

重建 initramfs 时必须保证 静态启动 `mkinitcpio -p linux -g ...` 或等效命令。生成产物可选（默认 (aether) 模式）。

`initramfs/aether-rescue*.img` 是生成产物（已 .gitignore）。

**休眠恢复（resume=，2026-11-14 新增）**：等价于 mkinitcpio `resume` 钩子，但在 aether PID 1 内实现。
PID 1 挂载 sysfs、加载模块后、pivot_root 之前解析 `/proc/cmdline` 并写 `/sys/power/resume`：
`resume=` 支持 `/dev/...`、`PARTUUID=`、`PARTLABEL=`、`UUID=`（按 swap 魔数 `SWAPSPACE2` 探测，UUID 在
swap 头 offset 1036）、`major:minor` 直传；`resume_offset=`（**PAGE_SIZE 单位**，仅 swapfile）透传写
`/sys/power/resume_offset`；`noresume` 全量跳过；`resume=` 缺省则不做任何事。解析失败仅告警，boot 继续
（找不到休眠镜像时内核本就不会恢复，正常启动）。rescue-boot / efi-cmdline 重试路径同样生效。

验证（QEMU 11 + /boot/vmlinuz-linux，全在用户态 VM 内，无任何 UNSFE 风险）：

| 用例 | 预期 | 结果 |
| --- | --- | --- |
| `root=` 缺失 → 救援模式 | BOOT FAILED → /dev/ttyS0 起一个 rescue shell（内核 console= 去重，不抢串口）| ✅ 输入/echo/lsblk/reboot 全部正常；`lsblk` 显示 nvme0n1 |
| `root=UUID=<btrfs> rw` | PID 1 → 兜底加载 4 个 nvme 模块（keyring→auth→core→nvme）→ btrfs 挂载于 /newroot → pivot_root → exec /sbin/init（测试用静态 poweroff 小程序）→ 关机 | ✅ qemu rc=0，内核日志 "reboot: Power down" |
| resume 系列（写 sysfs 语义级） | `resume=/dev/nvme0n1` / `resume_offset=2048` / `noresume` / `resume=259:0` / `resume=/dev/nonexistent` / `resume=PARTUUID=bogus` | ✅ 全部走通：正确写 `/sys/power/resume=259:0`（含 offset 行），失败路径干净告警继续 boot，rc=0；**真实休眠镜像 round-trip 未测**（需完整 hibernate 周期） |
| 带 `aether-network` → rescue 联网 | rescue shell → `udhcpc -i eth0` → 静态 IP（`ifconfig eth0 10.0.2.15 255.255.255.0`）→ `telnetd` 后台 → 宿主机 `nc 127.0.0.1 8888` | ✅ 宿主机 socket → buf → 连接成功，可回显 clog 静态 IP 协同工作；**QEMU user-net DHCP 回带（DHCPDISCOVER 收不到 OFFER）已记在已知限制** |

镜像属性：~479 KiB（lz4 -l，与参考配置相同压缩格式）、静态单 ELF、无 libc、
模块依赖由 mkinitcpio 自动解析（auth/keyring 无需显式列出）、/dev 节点由 devtmpfs
在挂载后提供（镜像内无需静态节点）、btrfs 为内核内置模块无需打包。
`initramfs/aether-rescue*.img` 是生成产物（已 .gitignore）。

### .ko.zst 模块解压（2026-11-15 新增）

内建 `zcat` applet（`src/tools/decompress.c` + `src/tools/compress.c`）：cat 合并两种 magic 自动识别，
直接把 `.ko.zst` / `.ko.gz` 解压到 stdout，方便 rescue 场景：

```
zcat /lib/modules/7.2.15-arch1-1/kernel/drivers/nvme/host/nvme.ko.zst > /tmp/nvme.ko
```

- **gzip**：压缩+解压完整实现（RFC1952/FIXED Huffman+LZ77 greedy + CRC32 校验）——和官方 `gzip`
  / `zlib` 双向 byte-exact 验证（1KB~176KB 多类文件）。
- **zstd 解压**：完整实现 RFC 8878（raw/RLE/compressed 块、字面量 Huffman、FSE 序列——预定义+自定义
  表均支持），12 个 zstd 测试 fixture + 官方 `.ko.zst` 模块（aegis128-aesni、nvme）+
  官方 libzstd 交叉 byte-exact。FSE 表序号/flush 顺序、implicit last weight、重复偏移特殊值、
  单流/4X stream、私有表验证、预期符号数不再强求……全部修正完毕。
- **zstd 压缩**：仅输出 raw + RLE 块（合法帧、official libzstd roundtrip OK，压缩率≈1；
  后续可补 Huffman literal + FSE 序列编码）。
- 32 位目标（i386）此前因为一个 `unsigned long` 截断 bug 导致整段 zstd 位流解错——
  现在四个 arch 的 `zcat` 全部对拍通过。
- **模块透明加载 `.ko.zst` 主路径仍然依赖内核 `CONFIG_MODULE_DECOMPRESS=y`**（在 boot 期间，
  PID 1 对 `.ko.zst` 走 `finit_module(fd, MODULE_INIT_COMPRESSED_FILE=4)`）；aether 侧 zcat 仅作为
  rescue 用户手动解压/抽出产物的路径。

## 验证状态（2026-11-15，本机实测；非 root 环境）

| 目标 | 构建 | 冒烟 | 说明 |
| --- | --- | --- | --- |
| x86_64 (libc) | ✅ ctest 2/2 | ✅ pty 冒烟 | — |
| x86_64 nolibc | ✅ ~160KB 静态去符号 | ✅ pty 冒烟（prompt/编辑/Ctrl-C/C-D/管道/重定向/磁盘/free/ps/…） + `zcat` 解压 .ko.zst 与官方 `zstd -d` 逐字节一致 | init 流程同一代码路径 |
| i386 nolibc (clang) | ✅ ~129KB 静态去符号 | ✅ **本机 IA32 模拟全路径运行** + zcat .ko.zst OK | IA32 emulation |
| aarch64 nolibc (gcc) | ✅ 静态链接 ELF 校验 | ❌ 无 qemu-user | 与 x86_64 共享度最高 |
| riscv64 nolibc (clang) | ✅ 全 TU 编译通过 | ❌ 无本机 riscv 链接器/qemu | 装 `gcc-riscv64-linux-gnu` 或 `lld` 即可 |

**写路径（切勿随意跑）**：`efibootmgr -B/-o`（NVRAM 写入）、`chroot`/`switch_root`/`mount`/`efistub 回写`——
本项目在 uid=1000 下开发，逻辑实现完整但**未获真机 root 实测**；init 流程的 PID 1 路径
（挂载/uevent/pivot_root/execve）同样**未在关机重启环境实测**（无密码 sudo）。
已可实测的（读路径，全部对照真实机器数据验证）：
- `efibootmgr` 列表 + `-v` 设备路径解码（与本机系统 efibootmgr 逐字节一致，含 GUID 与 ESP 路径）
- `cmdline`（真机 /proc/cmdline 原文）、`efi-cmdline`（真机 BootCurrent=0000 的 efistub 条目解码出
  `root=UUID=ff93802f-… rw rootflags=subvol=@ …`）
- `rescue-boot`（能走到正确的"未找到该 UUID / 部分分区无读权限"错误，即 resolve 逻辑全路径执行到权限边界）
- `grep`（差分 31 个模式对照 GNU grep 输出一致；唯一差异 \d = 数字类，为文档化的有意选择；pty 内管线/重定向均工作）
- `reboot` / `halt`（非 root → EPERM 干净报错；真正的关机/重启动作需 root 环境实测，切勿在 uid=1000 机器上跑有害副作用）
- **PID 1 真启动路径（QEMU）**：rescue 模式（/dev/console 去重后 /dev/ttyS0 单 shell）与
  完整 boot 路径（UUID 解析 → btrfs 挂载 → pivot_root → exec /sbin/init → POWER_OFF）均实测通过；
  resume 的 sysfs 写入语义六条路径全部实测（见上表），真实休眠 round-trip 未测
- **打包镜像（2026-11-15，scratch 重建+验证，全部含基础微码）**——7 个产物：
  `aether-rescue-min.img`（4.5 MiB，本体最小=aether+微码，4 模块）：rescue ✅；
  `aether-rescue-no-microcode.img`（**~334 KiB，HOOKS=(aether) 完全无微码**）：裸最小 rescue ✅；
  `aether-rescue.img`（~8.0 MiB，默认完整）：好 root→PowerDown（rc=0）✅；坏 root→rescue（22/24 模块并行加载）✅；
  `aether-rescue-no-keyboard.img` / `aether-rescue-no-usb.img` / `aether-rescue-no-network.img`（默认完整分别去掉一个 hook）：均实测进入 rescue shell ✅；
  `aether-rescue-full.img`（~57 MiB，全量最大=默认+aether-everything）：87 模块 + 76 个 WiFi firmware，rescue ✅、好 root→PowerDown（rc=0）✅；
  `aether-rescue-hostdrv.img`（~5.1 MiB，本机剪枝版：serio_raw + USB 有线网卡恒留 + nvme×4，**仅构建机本机有效**）：rescue ✅；
  模块加载 pass 内并行（fork+finit_module，依赖序由 pass 间多遍保证）
- **救援 shell 当子进程 fork 失败时打印明确错误**（2026-11-15 修复：之前静默跳过整个控制台）——`rescue: fork 救援 shell 失败（N），跳过 <console>` 现在可见
- **aether-hostdrv（2026-11-15，剪枝版）**：`HOOKS=(aether aether-microcode aether-hostdrv)` ≈5.1 MiB；
  通用集 ∩ 本机侦测 → 保留 serio_raw + USB 有线网卡全集恒留 + phy/mdio 依赖 + nvme×4，剔除 iwlwifi/thunderbolt/mac80211 等；
  QEMU 启动：38 模块 pass 内全部加载成功、rescue shell + DEL 监听正常 ✅；**仅构建机本机有效，绝不可跨机器**
  （2026-11-15 早先的 "加入 iwlwifi+firmware" 语义已废弃——方向改「只剪不加」，且本程序不支持 WiFi）
- **开机 DELETE → rescue（QEMU，2026-11-15）**：通道 A（evdev，QEMU monitor `sendkey delete` via PS/2）
  ✅；通道 B（串口 `\x7f` 与 `ESC[3~`）✅；不按键时正常启动到 `reboot: Power down`（QEMU rc=0）✅；
  后台并行监听（0 额外启动时间）。真机 VT（i8042/USB HID 真键盘 + evdev=y）按设计可工作；无物理键盘无法实测
- `hotplug` / `bb` 热插拔（EPERM/非 root 边界；mknod 需 root）；救援模式 uevent 守护代码路径编译+EPERM 验证

## 已知限制

- 有作业控制（`cmd &`、Ctrl-Z 挂起、`jobs`/`fg [id]`/`bg [id]`）与命令历史（↑/↓），但无语法高亮（editor 为 nano 键位，^S 保存、^X 退出，有中文支持的 UTF-8 显示）
- `less` 无搜索；editor 无撤销/搜索
- 时间显示一律 UTC（救援环境无时区概念）；`ls -l` 无排序（磁盘顺序）；组合短选项仅 `ls` 支持
- `sort` / `shuf` / `tail` 全文件缓冲 ≤ 2MB；`find`/`split` 等未做 TB 级文件极限测试
- `chattr`/`lsattr` 的写路径需 CAP_LINUX_IMMUTABLE（非 root 会报 EPERM；rescue 场景提权后可用）
- `kbd_mode` 只对真实 VT 控制台有效（pty/serial 会正确返回 ENOTTY；initramfs VT 控制台上可用）
- `raidautorun` 需要 mdadm（首选）；mdadm 不存在时的 RAID_AUTORUN ioctl 在内核 ≥ 4.x 已删除该分支（md_ioctl_valid 无此 case），只会得到 ENOTTY
- 软 RAID 自动识别**必须** root + mdadm（或内核静态编译 md + 启动早期扫描）；非 root 只能查看，无法 assemble
- `lspci`/`lsusb`/`scsi`/`serial` 只读 sysfs（需 /sys 挂载；PCI 厂商/类名为精简内置表，未收录的显示十六进制）
- `udhcpc` 为最小 DORA 实现（无续约、无 option 协商扩展）；rescue 模式无法真正拿到 SLIRP user-net DHCP 回执（广播无应答）—— rescue 网络应走 `ifconfig eth0 <ip>/<prefix>` 静态 IP 或在非 SLIRP 模式的 bridge 网络下测；真实网段 DHCP 回应需硬件/子网 DHCP Server。TELNETD 已实测 passing 入境 shell。
- `telnetd` 无认证、明文传输；`hotplug` 不执行 udev 规则（仅 mknod/unlink）
- resume 仅支持 resume=/dev、PARTUUID=、PARTLABEL=、UUID=（swap 签名探测）、major:minor 直传；
  不支持 LABEL、dm-crypt/LVM 自动激活（如需请在 initramfs 侧先激活 LV 再 swap，或先手动 swapon）；
  真实休眠镜像恢复 round-trip 未实测
- 救援 init 的 PID 1 回退（rescue_consoles）仅在真机/可牺牲虚拟机里完整验证；本机只验证到权限边界
- `aether-hostdrv` 仅限构建机本机——侦测的是**构建机**的键盘/网卡/USB 硬件；跨机器部署无效（同 mkinitcpio autodetect）
- **本程序不支持 WiFi**：rescue 网络自动探测（`netup`/救援自动联网）会跳过一切 `/sys/class/net/<iface>/wireless/` 接口；
  hostdrv 不打包任何无线相关模块（iwlwifi/mac80211/cfg80211/firmware 全部排除）；有线网络（含 USB 有线网卡）正常工作
- 开机 DELETE 监听为后台子进程、与 boot 并行（零额外启动时间）：**PID 1 接管（hint 行出现）之前按的键检测不到**（canonical 行规则吞掉 DEL / evdev 无未打开的客户端排队）；**exec 真正 init 之后按 DEL 不再中止**（控制台已恢复、init 接管）；坏 root 极速失败时，boot 可能在最后一个检查点之前结束——之后再按 DEL 进 rescue shell 本身而非"强制 rescue"横幅
- VT 控制台上 DEL 走 evdev（通道 A），串口/serial 走控制台通道 B；QEMU `-nographic` 下请用 monitor `sendkey delete` 或在窗口内向串口写字节来测试
- `dmesg` 非 root 在 `dmesg_restrict=1` 内核被拒（优雅提示）
- efi/NVRAM 写入类功能必须先在**可牺牲的虚拟机**中验证
- `lsblk` 对 btrfs 子卷无展开（v1 只显示块设备/分区）

## 目录结构

```
CMakeLists.txt          双分支构建（host libc / nolibc）+ 架构归一 + 编译器-目标一致性校验
src/
  main.c                §2 入口；PID 1 → aether_init_main()
  boot.c/h              init：cmdline 解析 / resolve root / boot 流程 / uevent / rescue 命令
  applets.c/h           applet 注册表
  argparse.c/h          §6 参数解析
  syscall_shim.c/h      §4.3/§5 双分支封装（含 errno 归一：libc 分支 -1/errno → 内核 -errno）
  mem.c/h               §8 arena + slot 池
  util.c/h              字符串/UTF-8 显示宽度/交互确认
  elog.c/h              fd 直写日志（含最小宽度、0 填充）
  ttyutil.c/h           tty/VT100
  sysfsutil.c/h         /proc 挂载、sysfs、getdents64、efivars、ESP 探测
  shell/                pda、lineedit、builtin（含 25+ 内置命令）、shell 主循环
  tools/                less/editor/dmesg/free/ps/disks/lsblk/efibootmgr/decompress
tests/                  ctest（argparse、mem）
tools/                  gen_sysnum.py、pty_test.py（端到端 pty 冒烟）
```

## 目录级命令行

```bash
# 四架构官方包
cmake -S . -B build-x86_64  -DUSE_HOST_LIBC=OFF && cmake --build build-x86_64
cmake -S . -B build-aarch64 -DUSE_HOST_LIBC=OFF -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc -DAETHER_ARCH=aarch64
cmake -S . -B build-i386    -DUSE_HOST_LIBC=OFF -DCMAKE_C_COMPILER=clang \
                              -DCMAKE_C_FLAGS=--target=i686-unknown-linux-gnu -DAETHER_ARCH=i386
cmake -S . -B build-riscv64 -DUSE_HOST_LIBC=OFF -DCMAKE_C_COMPILER=clang \
                              -DCMAKE_C_FLAGS="--target=riscv64-unknown-linux-gnu -march=rv64gc" -DAETHER_ARCH=riscv64

# 修复后的 rescue-boot 用法（在 initramfs 里）：
#   进入拯救模式后：
#     cmdline                    # 查看当前 cmdline
#     rescue-boot                # 原样重试
#     rescue-boot root=UUID=xxx rw # 换根重试
#     efi-cmdline edit           # 永久改好 EFI 启动项（重启生效）+ 当场重试
python3 tools/pty_test.py ./build/aether  # 端到端验证
```

## 本项目里程碑

- v0.2 需求文档四架构化（v0.1 仅 x86_64）；v0.2 订正了 syscall 编号表（chroot 51、execve 221、
  openat、aarch64/riscv64 无 open）
- v0.3 §12 第 1–6 步 + efibootmgr（读路径实测对齐系统 efibootmgr）
- v0.4 §12 第 7 步 + **init 模式**：PID 1 检测、uevent（netlink KOBJECT_UEVENT）、cmdline 解析、
  根设备 resolve（ext4/vfat/btrfs/PARTUUID/LABEL）、挂载/pivot_root/execve(init)、拯救模式兜底、
  `cmdline` / `rescue-boot` / `efi-cmdline` 三个修复命令、`lsblk`、editor（§12-7）

## 开发原则（需求文档约束）

纯 C、`-static -Os`、目标 ≤100KB；不引 C++/ncurses/readline；公共字符串/UTF-8 处理自己实现；
init 路径代码须能在无 libc 环境（nolibc）运行；写 NVRAM 前一律 `[y/N]` 确认。
