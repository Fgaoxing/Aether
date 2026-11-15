/*
 * sysinfo.c — 系统信息查看工具（均读 sysfs / /proc，ioctl 兜底）
 * lspci lsusb scsi serial raidautorun kbd_mode
 * 单用户 rescue 语义；非标准输出格式。
 */
#include <stddef.h>

#include "applets.h"
#include "elog.h"
#include "shell/shell_internal.h"
#include "syscall_shim.h"
#include "sysfsutil.h"
#include "ttyutil.h"
#include "util.h"

/* ---------- 通用小工具 ---------- */

/* 读 sysfs/proc 小文件到 buf，去掉尾部换行，返回 0 成功 */
static int sysfs_read(const char *path, char *buf, size_t cap)
{
    size_t len = 0;
    if (aether_read_file(path, buf, cap - 1, &len) != 0) return -1;
    buf[len] = '\0';
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = '\0';
    return 0;
}

/* 目录拼接 dir + "/" + name + suffix */
static int path_cat(char *dst, size_t cap, const char *dir, const char *name, const char *suffix)
{
    size_t d = 0;
    if (cap < 1) return -1;
    while (dir[d] && d + 1 < cap) { dst[d] = dir[d]; d++; }
    if (d + 2 > cap) return -1;
    dst[d++] = '/';
    for (size_t i = 0; name[i] && d + 1 < cap; i++) dst[d++] = name[i];
    if (suffix) for (size_t i = 0; suffix[i] && d + 1 < cap; i++) dst[d++] = suffix[i];
    dst[d] = '\0';
    return 0;
}

static int hexval(const char *s) /* 0x... → int */
{
    int v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while (*s) {
        char c = *s++;
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = v * 16 + d;
    }
    return v;
}

/* ---------- lspci ---------- */

struct pci_ids { unsigned short id; const char *name; };
static const struct pci_ids pci_vendors[] = {
    { 0x8086, "Intel" }, { 0x1022, "AMD" }, { 0x10de, "NVIDIA" }, { 0x1002, "AMD/ATI" },
    { 0x1b36, "Red Hat/VirtIO" }, { 0x1af4, "Red Hat" }, { 0x15ad, "VMware" },
    { 0x80ee, "Oracle" }, { 0x1106, "VIA" }, { 0x1b21, "ASMedia" },
    { 0x8087, "Intel" }, { 0x1d6b, "Linux Foundation" }, { 0x14e4, "Broadcom" },
    { 0x10ec, "Realtek" }, { 0x1814, "Ralink" }, { 0x1969, "Atheros" },
};
static const char *pci_vendor_name(unsigned short id)
{
    for (size_t i = 0; i < sizeof(pci_vendors) / sizeof(pci_vendors[0]); i++)
        if (pci_vendors[i].id == id) return pci_vendors[i].name;
    return 0;
}
static const char *pci_class_name(unsigned cls)
{
    switch ((cls >> 16) & 0xff) {
    case 0x00: return "Unclassified"; case 0x01: return "Mass storage";
    case 0x02: return "Network";      case 0x03: return "Display";
    case 0x04: return "Multimedia";   case 0x05: return "Memory";
    case 0x06: return "Bridge";       case 0x07: return "Communication";
    case 0x08: return "Generic system"; case 0x09: return "Input";
    case 0x0a: return "Docking";      case 0x0b: return "Processor";
    case 0x0c: return "Serial bus";   case 0x0d: return "Wireless";
    case 0x0e: return "Intelligent IO"; case 0x0f: return "Satellite";
    case 0x10: return "Encryption";   case 0x11: return "Data acq";
    case 0xff: return "Vendor-specific"; default: return "Unknown";
    }
}

typedef struct { int count; } lspci_ctx_t;

static void lspci_cb(const char *name, unsigned char d_type, void *vctx)
{
    lspci_ctx_t *ctx = vctx;
    (void)d_type;
    if (name[0] == '.') return;               /* . 和 .. */
    char path[320], buf[96];

    int vendor = 0, device = 0, cls = 0, irq = -1;
    if (path_cat(path, sizeof(path), "/sys/bus/pci/devices", name, "/vendor") == 0 && sysfs_read(path, buf, sizeof(buf)) == 0) vendor = hexval(buf);
    if (path_cat(path, sizeof(path), "/sys/bus/pci/devices", name, "/device") == 0 && sysfs_read(path, buf, sizeof(buf)) == 0) device = hexval(buf);
    if (path_cat(path, sizeof(path), "/sys/bus/pci/devices", name, "/class")  == 0 && sysfs_read(path, buf, sizeof(buf)) == 0) cls = hexval(buf);
    if (path_cat(path, sizeof(path), "/sys/bus/pci/devices", name, "/irq")    == 0 && sysfs_read(path, buf, sizeof(buf)) == 0) irq = (int)aether_atol(buf);

    char driver[48];
    driver[0] = '\0';
    if (path_cat(path, sizeof(path), "/sys/bus/pci/devices", name, "/driver") == 0) {
        long r = sys_readlink(path, driver, sizeof(driver) - 1);
        if (r > 0) {
            driver[r] = '\0';
            const char *base = driver;
            for (const char *p = driver; *p; p++) if (*p == '/') base = p + 1;
            /* base 指向驱动名 */
            size_t d = 0;
            for (size_t i = 0; base[i] && d + 1 < sizeof(driver); i++) driver[d++] = base[i];
            driver[d] = '\0';
        } else driver[0] = '\0';
    }

    const char *vname = pci_vendor_name((unsigned short)vendor);
    elog_info("%s  class=%02x %s  %04x:%04x %s%s%s  irq=%d  driver=%s",
              name, (cls >> 16) & 0xff, pci_class_name((unsigned)cls),
              (unsigned)vendor, (unsigned)device,
              vname ? vname : "", vname ? " " : "", "",
              irq, driver[0] ? driver : "-");
    ctx->count++;
}

int tool_lspci(int argc, char **argv, char **envp)
{
    (void)argv; (void)envp;
    if (argc > 1) { elog_err("lspci: usage: lspci"); return 2; }
    lspci_ctx_t ctx = { 0 };
    if (aether_foreach_dirent("/sys/bus/pci/devices", lspci_cb, &ctx) != 0) {
        elog_err("lspci: 无法枚举 /sys/bus/pci/devices（/sys 未挂载？试试 mount -t sysfs sysfs /sys）");
        return 1;
    }
    if (ctx.count == 0) elog_info("lspci: 未发现 PCI 设备（或 /sys 未挂载）");
    return 0;
}

/* ---------- lsusb ---------- */

typedef struct { int count; } lsusb_ctx_t;

static void lsusb_cb(const char *name, unsigned char d_type, void *vctx)
{
    lsusb_ctx_t *ctx = vctx;
    (void)d_type;
    if (name[0] == '.') return; /* . 和 .. */
    for (const char *p = name; *p; p++) if (*p == ':') return; /* 接口条目（usbN:M.K），设备已列 */
    char path[320], buf[128];

    char vid[16] = "????", pid[16] = "????", cls[16] = "?", speed[16] = "?", product[64] = "";
    if (path_cat(path, sizeof(path), "/sys/bus/usb/devices", name, "/idVendor") == 0 && sysfs_read(path, buf, sizeof(buf)) == 0) aether_strcpy(vid, buf, sizeof(vid));
    if (path_cat(path, sizeof(path), "/sys/bus/usb/devices", name, "/idProduct") == 0 && sysfs_read(path, buf, sizeof(buf)) == 0) aether_strcpy(pid, buf, sizeof(pid));
    if (path_cat(path, sizeof(path), "/sys/bus/usb/devices", name, "/bDeviceClass") == 0 && sysfs_read(path, buf, sizeof(buf)) == 0) aether_strcpy(cls, buf, sizeof(cls));
    if (path_cat(path, sizeof(path), "/sys/bus/usb/devices", name, "/speed") == 0 && sysfs_read(path, buf, sizeof(buf)) == 0) aether_strcpy(speed, buf, sizeof(speed));
    if (path_cat(path, sizeof(path), "/sys/bus/usb/devices", name, "/product") == 0 && sysfs_read(path, buf, sizeof(buf)) == 0) aether_strcpy(product, buf, sizeof(product));
    else if (path_cat(path, sizeof(path), "/sys/bus/usb/devices", name, "/manufacturer") == 0 && sysfs_read(path, buf, sizeof(buf)) == 0) aether_strcpy(product, buf, sizeof(product));

    elog_info("%-12s  id=%s:%s  class=%s  speed=%s  %s", name, vid, pid, cls, speed, product);
    ctx->count++;
}

int tool_lsusb(int argc, char **argv, char **envp)
{
    (void)argv; (void)envp;
    if (argc > 1) { elog_err("lsusb: usage: lsusb"); return 2; }
    lsusb_ctx_t ctx = { 0 };
    if (aether_foreach_dirent("/sys/bus/usb/devices", lsusb_cb, &ctx) != 0) {
        elog_err("lsusb: 无法枚举 /sys/bus/usb/devices（/sys 未挂载？）");
        return 1;
    }
    if (ctx.count == 0) elog_info("lsusb: 未发现 USB 设备（或 /sys 未挂载）");
    return 0;
}

/* ---------- scsi ---------- */

static const char *scsi_type_name(int t)
{
    switch (t) {
    case 0: return "disk";     case 1: return "tape";
    case 2: return "printer";  case 3: return "processor";
    case 4: return "worm";     case 5: return "cdrom";
    case 6: return "scanner";  case 7: return "mo";
    case 8: return "changer";  case 9: return "comms";
    case 12: return "raid";    default: return "other";
    }
}

typedef struct { int count; } scsi_ctx_t;

static void scsi_cb(const char *name, unsigned char d_type, void *vctx)
{
    scsi_ctx_t *ctx = vctx;
    (void)d_type;
    if (name[0] == '.') return;
    char path[320], buf[160];

    char vendor[48] = "", model[80] = "", rev[24] = "";
    int type = -1;
    unsigned long long size = 0;
    if (path_cat(path, sizeof(path), "/sys/bus/scsi/devices", name, "/vendor") == 0 && sysfs_read(path, buf, sizeof(buf)) == 0) aether_strcpy(vendor, buf, sizeof(vendor));
    if (path_cat(path, sizeof(path), "/sys/bus/scsi/devices", name, "/model") == 0 && sysfs_read(path, buf, sizeof(buf)) == 0) aether_strcpy(model, buf, sizeof(model));
    if (path_cat(path, sizeof(path), "/sys/bus/scsi/devices", name, "/rev") == 0 && sysfs_read(path, buf, sizeof(buf)) == 0) aether_strcpy(rev, buf, sizeof(rev));
    if (path_cat(path, sizeof(path), "/sys/bus/scsi/devices", name, "/type") == 0 && sysfs_read(path, buf, sizeof(buf)) == 0) type = (int)aether_atol(buf);
    if (path_cat(path, sizeof(path), "/sys/bus/scsi/devices", name, "/size") == 0 && sysfs_read(path, buf, sizeof(buf)) == 0) size = 0; /* 单位 512 字节，仅作展示 */
    {
        size = 0;
        for (const char *p = buf; *p >= '0' && *p <= '9'; p++) size = size * 10 + (unsigned)(*p - '0');
    }

    /* 去掉 vendor/model 两侧空格 */
    {
        char *s = vendor; while (*s == ' ') s++;
        if (s != vendor) aether_strcpy(vendor, s, sizeof(vendor));
        size_t n = aether_strlen(vendor); while (n > 0 && vendor[n - 1] == ' ') vendor[--n] = '\0';
    }
    {
        char *s = model; while (*s == ' ') s++;
        if (s != model) aether_strcpy(model, s, sizeof(model));
        size_t n = aether_strlen(model); while (n > 0 && model[n - 1] == ' ') model[--n] = '\0';
    }

    elog_info("%-12s  %-8s  %s %s  rev=%s  size=%llu blk", name, scsi_type_name(type), vendor, model, rev[0] ? rev : "?", size);
    ctx->count++;
}

int tool_scsi(int argc, char **argv, char **envp)
{
    (void)argv; (void)envp;
    if (argc > 1) { elog_err("scsi: usage: scsi"); return 2; }
    scsi_ctx_t ctx = { 0 };
    if (aether_foreach_dirent("/sys/bus/scsi/devices", scsi_cb, &ctx) != 0) {
        elog_err("scsi: 无法枚举 /sys/bus/scsi/devices（/sys 未挂载？）");
        return 1;
    }
    if (ctx.count == 0) elog_info("scsi: 未发现 SCSI 设备（或 /sys 未挂载）");
    return 0;
}

/* ---------- serial ---------- */

typedef struct { int count; } serial_ctx_t;

static void serial_tty_cb(const char *name, unsigned char d_type, void *vctx)
{
    serial_ctx_t *ctx = vctx;
    (void)d_type;
    if (name[0] == '.') return;
    if (aether_strncmp(name, "ttyS", 4) == 0 || aether_strncmp(name, "ttyUSB", 6) == 0 ||
        aether_strncmp(name, "ttyAMA", 6) == 0 || aether_strncmp(name, "ttymxc", 6) == 0 ||
        aether_strncmp(name, "ttySC", 5) == 0 || aether_strncmp(name, "ttyPSC", 6) == 0) {
        elog_info("/dev/%s", name);
        ctx->count++;
    }
}

int tool_serial(int argc, char **argv, char **envp)
{
    (void)argv; (void)envp;
    if (argc > 1) { elog_err("serial: usage: serial"); return 2; }

    /* 1) /proc/tty/driver/serial 有详细行（uart/port/irq/tx/rx）的话直接显示 */
    char buf[4096];
    size_t len = 0;
    if (aether_read_file("/proc/tty/driver/serial", buf, sizeof(buf) - 1, &len) == 0) {
        buf[len] = '\0';
        elog_info("serial ports (/proc/tty/driver/serial):");
        char *p = buf;
        while (*p) {
            char *ls = p;
            while (*p && *p != '\n') p++;
            if (*p == '\n') { *p = '\0'; p++; }
            if (ls[0] && ls[0] != '\0') elog_info("  %s", ls);
        }
    } else {
        elog_info("serial: /proc/tty/driver/serial 不可读（/proc 未挂载或无串口控制器）");
    }

    /* 2) 枚举 /sys/class/tty 列出串口设备节点 */
    elog_info("serial tty devices (/sys/class/tty):");
    serial_ctx_t ctx = { 0 };
    if (aether_foreach_dirent("/sys/class/tty", serial_tty_cb, &ctx) != 0)
        elog_info("  (无法枚举 /sys/class/tty)");
    else if (ctx.count == 0)
        elog_info("  (无串口类 tty 设备)");
    return 0;
}

/* ---------- raidautorun ---------- */

#define RAID_AUTORUN_NR 0x914   /* _IO(9, 0x14)：MD_MAJOR + 0x14；现代内核不再支持（md_ioctl_valid 无此 case） */

static int run_external_redir(int argc, char **argv, char **envp, int quiet);

static int run_external(int argc, char **argv, char **envp)
{
    return run_external_redir(argc, argv, envp, 0);
}

static int run_external_redir(int argc, char **argv, char **envp, int quiet)
{
    int pid = (int)sys_fork();
    if (pid == 0) {
        if (quiet) {
            long nfd = sys_open("/dev/null", O_WRONLY, 0);
            if (!AETHER_IS_ERR(nfd)) { sys_dup2((int)nfd, 1); sys_dup2((int)nfd, 2); }
        }
        shell_exec_search(argc, argv, envp); /* 成功不返回 */
        aether_exit(127);
    }
    if (pid < 0 || AETHER_IS_ERR((long)pid)) return -1;
    for (;;) {
        int status = 0;
        long r = sys_wait4(pid, &status, 0, 0);
        if (AETHER_IS_ERR(r)) {
            if (AETHER_ERR_CODE(r) == 4) continue;
            return -1;
        }
        if (status & 0x7f) return 128 + (status & 0x7f);
        return (status >> 8) & 0xff;
    }
}

static int mdadm_available(char **envp)
{
    char *check[] = { (char *)"mdadm", (char *)"--version", 0 };
    int rc = run_external_redir(2, check, envp, 1);
    return rc == 0;
}

int tool_raidautorun(int argc, char **argv, char **envp)
{
    /* 1) 优先 mdadm（现代内核唯一可靠方式：md 模块化后 RAID_AUTORUN 已失效） */
    if (mdadm_available(envp)) {
        if (argc > 1) {
            int ok = 0, bad = 0;
            for (int i = 1; i < argc; i++) {
                char *cmd[] = { (char *)"mdadm", (char *)"--incremental", (char *)"--run", argv[i], 0 };
                int rc = run_external(4, cmd, envp);
                if (rc == 0) { elog_info("raidautorun: %s 已尝试启动（详见上方输出）", argv[i]); ok++; }
                else { elog_err("raidautorun: mdadm --incremental --run %s 失败（rc=%d）", argv[i], rc); bad++; }
            }
            return ok > 0 && bad == 0 ? 0 : 1;
        }
        elog_info("raidautorun: 尝试 mdadm --assemble --scan ...");
        char *cmd[] = { (char *)"mdadm", (char *)"--assemble", (char *)"--scan", 0 };
        int rc = run_external(3, cmd, envp);
        if (rc == 0) return 0;
        elog_err("raidautorun: mdadm --assemble --scan 失败（rc=%d）", rc);
        return 1;
    }

    elog_info("raidautorun: 未找到 mdadm；尝试内核 RAID_AUTORUN ioctl（现代内核不再支持）");

    /* 2) 传统回退：RAID_AUTORUN ioctl。现代内核里 md_ioctl_valid 无此 case → ENOTTY */
    int ok = 0, bad = 0;
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            long fd = sys_open(argv[i], O_RDONLY | O_NONBLOCK, 0);
            if (AETHER_IS_ERR(fd)) { elog_err("raidautorun: 无法打开 %s（%d）", argv[i], AETHER_ERR_CODE(fd)); bad++; continue; }
            long r = sys_ioctl((int)fd, RAID_AUTORUN_NR, 0);
            sys_close((int)fd);
            if (AETHER_IS_ERR(r)) { elog_err("raidautorun: RAID_AUTORUN %s 失败（%d）", argv[i], AETHER_ERR_CODE(r)); bad++; }
            else { elog_info("raidautorun: RAID_AUTORUN %s 成功", argv[i]); ok++; }
        }
    } else {
        /* 扫描 /proc/partitions，对整盘与分区逐个尝试 */
        char buf[8192];
        size_t len = 0;
        if (aether_read_file("/proc/partitions", buf, sizeof(buf) - 1, &len) == 0) {
            buf[len] = '\0';
            char *p = buf;
            while (*p) {
                char *ls = p;
                while (*p && *p != '\n') p++;
                if (*p == '\n') { *p = '\0'; p++; }
                const char *s = ls;
                while (*s == ' ' || *s == '\t') s++;
                if (*s < '0' || *s > '9') continue;
                const char *tok = s;
                int nt = 0;
                while (*tok && nt < 4) {
                    while (*tok == ' ' || *tok == '\t') tok++;
                    if (!*tok) break;
                    if (nt == 3) break;
                    nt++;
                    while (*tok && *tok != ' ' && *tok != '\t') tok++;
                }
                /* 最后一列是设备名 */
                const char *name = tok;
                while (*name == ' ' || *name == '\t') name++;
                if (!*name || name[0] == '\n') continue;
                char dev[80];
                size_t d = 0;
                for (size_t k = 0; "/dev/"[k]; k++) dev[d++] = "/dev/"[k];
                for (size_t k = 0; name[k] && d + 1 < sizeof(dev); k++) dev[d++] = name[k];
                dev[d] = '\0';
                if (dev[5] == 'l' && (aether_strncmp(dev + 5, "loop", 4) == 0 || aether_strncmp(dev + 5, "ram", 3) == 0)) continue;
                long fd = sys_open(dev, O_RDONLY | O_NONBLOCK, 0);
                if (AETHER_IS_ERR(fd)) { bad++; continue; }
                long r = sys_ioctl((int)fd, RAID_AUTORUN_NR, 0);
                sys_close((int)fd);
                if (!AETHER_IS_ERR(r)) { elog_info("raidautorun: RAID_AUTORUN %s 成功", dev); ok++; }
                else bad++;
            }
        } else {
            elog_err("raidautorun: 无法读取 /proc/partitions；且未找到 mdadm");
            return 2;
        }
    }

    /* 3) 显示 /proc/mdstat（若存在） */
    {
        char md[2048];
        size_t mlen = 0;
        if (aether_read_file("/proc/mdstat", md, sizeof(md) - 1, &mlen) == 0) {
            md[mlen] = '\0';
            char *ls = md, *p = md;
            while (*p) {
                ls = p;
                while (*p && *p != '\n') p++;
                if (*p == '\n') { *p = '\0'; p++; }
                if (ls[0]) elog_info("%s", ls);
            }
        } else {
            elog_info("raidautorun: /proc/mdstat 不存在（md 模块未加载；需要 modprobe md_mod 或 root）");
        }
    }
    if (ok == 0 && bad > 0) {
        elog_err("raidautorun: 全部尝试失败；内核不支持 RAID_AUTORUN 时请使用 mdadm --assemble --scan / --incremental --run <dev>");
        return 1;
    }
    return 0;
}

/* ---------- kbd_mode ---------- */

#define KDGKBMODE 0x4B44
#define KDSKBMODE 0x4B45
#define K_RAW        0x00
#define K_XLATE      0x01
#define K_MEDIUMRAW  0x02
#define K_UNICODE    0x03
#define K_OFF        0x04

static const char *kbd_mode_name(int m)
{
    switch (m) {
    case K_RAW:       return "scancode (raw)";
    case K_XLATE:     return "xlate (ascii)";
    case K_MEDIUMRAW: return "mediumraw (keycode)";
    case K_UNICODE:   return "unicode";
    case K_OFF:       return "off";
    default:          return "unknown";
    }
}

int tool_kbd_mode(int argc, char **argv, char **envp)
{
    (void)envp;
    int kbd = aether_tty_open_rw();
    if (kbd < 0) { elog_err("kbd_mode: 无法打开控制台 tty"); return 1; }

    if (argc == 1) {
        int mode = -1;
        long r = sys_ioctl(kbd, KDGKBMODE, (unsigned long)&mode);
        if (AETHER_IS_ERR(r)) { elog_err("kbd_mode: KDGKBMODE 失败（%d）（需要控制台，且内核支持）", AETHER_ERR_CODE(r)); if (kbd > 2) sys_close(kbd); return 1; }
        elog_info("kbd_mode: %s", kbd_mode_name(mode));
        if (kbd > 2) sys_close(kbd);
        return 0;
    }

    const char *req = argv[1];
    int mode = -1;
    if (aether_streq(req, "-s") || aether_streq(req, "scancode") || aether_streq(req, "raw")) mode = K_RAW;
    else if (aether_streq(req, "-k") || aether_streq(req, "keycode") || aether_streq(req, "mediumraw")) mode = K_MEDIUMRAW;
    else if (aether_streq(req, "-a") || aether_streq(req, "ascii") || aether_streq(req, "xlate")) mode = K_XLATE;
    else if (aether_streq(req, "-u") || aether_streq(req, "unicode")) mode = K_UNICODE;
    else if (aether_streq(req, "-o") || aether_streq(req, "off")) mode = K_OFF;
    else { elog_err("kbd_mode: 未知模式 '%s'（用：-s/-k/-a/-u/-o 或 scancode/keycode/xlate/unicode/off；不带参数=查询）", req); if (kbd > 2) sys_close(kbd); return 2; }

    long r = sys_ioctl(kbd, KDSKBMODE, (unsigned long)&mode);
    if (AETHER_IS_ERR(r)) {
        elog_err("kbd_mode: KDSKBMODE 设置失败（%d）（需要相应权限/控制台）", AETHER_ERR_CODE(r));
        if (kbd > 2) sys_close(kbd);
        return 1;
    }
    elog_info("kbd_mode: 已切换到 %s", kbd_mode_name(mode));
    if (kbd > 2) sys_close(kbd);
    return 0;
}
