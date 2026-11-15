/*
 * net.c — 网络工具：ifconfig udhcpc telnetd netup
 * 救援环境语义：单根，无认证（telnetd 明文！仅限受信网络）；输出不必标准。
 * udhcpc：最小 DHCP 客户端（D-O-R-A），失败时如实报错不改动接口配置。
 * telnetd：socket + fork + aether shell（不经 pty，行模式 + IAC 过滤）。
 */
#include <stddef.h>

#include "applets.h"
#include "elog.h"
#include "shell/shell_internal.h"
#include "syscall_shim.h"
#include "sysfsutil.h"
#include "util.h"

/* ---------- 常量 ---------- */

#define AF_INET      2
#define AF_PACKET    17
#define SOCK_DGRAM   2
#define SOCK_STREAM  1
#define IPPROTO_UDP  17
#define IPPROTO_RAW  255

#define SOL_SOCKET    1
#define SO_REUSEADDR  2
#define SO_BROADCAST  6
#define SO_RCVTIMEO   20
#define SO_BINDTODEVICE 25

#define SIOCGIFCONF    0x8912
#define SIOCGIFFLAGS   0x8913
#define SIOCSIFFLAGS   0x8914
#define SIOCGIFADDR    0x8915
#define SIOCSIFADDR    0x8916
#define SIOCGIFBRDADDR 0x8919
#define SIOCSIFBRDADDR 0x891a
#define SIOCGIFNETMASK 0x891b
#define SIOCSIFNETMASK 0x891c
#define SIOCADDRT      0x890B
#define SIOCGIFMTU     0x8921
#define SIOCGIFHWADDR  0x8927

#define IFF_UP        0x1
#define IFF_BROADCAST 0x2
#define IFF_DEBUG     0x4
#define IFF_LOOPBACK  0x8
#define IFF_POINTOPOINT 0x10
#define IFF_NOTRAILERS 0x20
#define IFF_RUNNING   0x40
#define IFF_NOARP     0x80
#define IFF_PROMISC   0x100
#define IFF_ALLMULTI  0x200
#define IFF_MULTICAST 0x1000

#define RTF_UP        0x0001
#define RTF_GATEWAY   0x0002
#define RTF_HOST      0x0004

/* ---------- 小工具 ---------- */

static int net_aton(const char *s, unsigned char out[4])
{
    unsigned v[4];
    int i = 0;
    const char *p = s;
    while (i < 4) {
        if (*p < '0' || *p > '9') return -1;
        v[i] = 0;
        while (*p >= '0' && *p <= '9') { v[i] = v[i] * 10 + (unsigned)(*p - '0'); p++; }
        if (v[i] > 255) return -1;
        if (i < 3 && *p != '.') return -1;
        if (*p == '.') p++;
        out[i] = (unsigned char)v[i];
        i++;
    }
    return *p == '\0' ? 0 : -1;
}

static void net_ntoa(const unsigned char in[4], char *out, size_t cap)
{
    /* %u.%u.%u.%u */
    size_t d = 0;
    for (int i = 0; i < 4; i++) {
        unsigned v = in[i];
        char tmp[4];
        size_t m = 0;
        if (v == 0) tmp[m++] = '0';
        while (v) { tmp[m++] = (char)('0' + (v % 10)); v /= 10; }
        if (i) { if (d + 1 < cap) out[d++] = '.'; }
        while (m && d + 1 < cap) out[d++] = tmp[--m];
    }
    out[d] = '\0';
}

static size_t net_strlen(const char *s) { size_t n = 0; while (s && s[n]) n++; return n; }
static void net_strcpy(char *d, const char *s, size_t cap)
{
    size_t i = 0;
    if (cap == 0) return;
    while (s[i] && i + 1 < cap) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

/* ifreq：40 字节（内核布局） */
typedef union {
    char name[16];
    struct { unsigned short fam; unsigned char data[14]; } sa;   /* sockaddr */
    unsigned short flags;
    int mtu;
    struct { unsigned char addr[6]; unsigned char pad[10]; } hw;  /* sockaddr(mac) */
} net_ifru_t;
typedef struct {
    char name[16];
    net_ifru_t u;
} net_ifreq_t;

/* sockaddr_in（16 字节） */
typedef struct {
    unsigned short sin_family;
    unsigned short sin_port;
    unsigned char  sin_addr[4];
    unsigned char  sin_zero[8];
} net_sockaddr_in_t;

static void net_make_sockaddr_in(net_sockaddr_in_t *sa, const unsigned char addr[4], unsigned short port_be)
{
    sa->sin_family = AF_INET;
    sa->sin_port = port_be;
    for (int i = 0; i < 4; i++) sa->sin_addr[i] = addr[i];
    for (int i = 0; i < 8; i++) sa->sin_zero[i] = 0;
}

/* "255.255.255.0" → 网络序 4 字节 */
static int net_mask_from_str(const char *s, unsigned char out[4])
{
    return net_aton(s, out);
}

/* 前缀长度 → 掩码字节 */
static void net_mask_from_prefix(int prefix, unsigned char out[4])
{
    unsigned m = prefix <= 0 ? 0 : (prefix >= 32 ? 0xffffffffu : (0xffffffffu << (32 - prefix)));
    out[0] = (unsigned char)((m >> 24) & 0xff);
    out[1] = (unsigned char)((m >> 16) & 0xff);
    out[2] = (unsigned char)((m >> 8) & 0xff);
    out[3] = (unsigned char)(m & 0xff);
}

/* ---------- 接口枚举 ---------- */

#define NET_MAX_IFACES 16

typedef struct { int n; char (*names)[16]; int max; } net_list_ctx_t;

/* 通过 /sys/class/net 列举 */
static void net_list_ifaces_cb(const char *name, unsigned char d_type, void *vctx)
{
    (void)d_type;
    if (name[0] == '.') return;
    net_list_ctx_t *ctx = vctx;
    if (ctx->n < ctx->max) {
        size_t i = 0;
        while (name[i] && i + 1 < 16) { ctx->names[ctx->n][i] = name[i]; i++; }
        ctx->names[ctx->n][i] = '\0';
        ctx->n++;
    }
}

static int net_list_ifaces(char names[][16], int max)
{
    net_list_ctx_t ctx;
    ctx.n = 0; ctx.names = names; ctx.max = max;
    aether_foreach_dirent("/sys/class/net", net_list_ifaces_cb, &ctx);
    return ctx.n;
}

/* ---------- ifconfig 读取 ---------- */

static int net_ioctl_ifreq(int fd, unsigned long req, net_ifreq_t *ifr)
{
    long r = sys_ioctl(fd, req, (unsigned long)ifr);
    return AETHER_IS_ERR(r) ? -1 : 0;
}

static void net_print_iface(int fd, const char *name)
{
    net_ifreq_t ifr;
    net_strcpy(ifr.name, name, sizeof(ifr.name));

    unsigned flags = 0;
    if (net_ioctl_ifreq(fd, SIOCGIFFLAGS, &ifr) == 0) flags = ifr.u.flags;

    unsigned char hw[6] = {0};
    if (net_ioctl_ifreq(fd, SIOCGIFHWADDR, &ifr) == 0) for (int i = 0; i < 6; i++) hw[i] = ifr.u.hw.addr[i];
    int mtu = 0;
    if (net_ioctl_ifreq(fd, SIOCGIFMTU, &ifr) == 0) mtu = ifr.u.mtu;

    /* 地址/掩码（无地址则是未配置） */
    net_sockaddr_in_t addr, mask, brd;
    int have_ip = 0, have_mask = 0, have_brd = 0;
    net_strcpy(ifr.name, name, sizeof(ifr.name));
    if (net_ioctl_ifreq(fd, SIOCGIFADDR, &ifr) == 0 && ifr.u.sa.fam == AF_INET) {
        addr.sin_family = ifr.u.sa.fam;
        for (int i = 0; i < 4; i++) addr.sin_addr[i] = ifr.u.sa.data[2 + i];
        have_ip = 1;
    }
    net_strcpy(ifr.name, name, sizeof(ifr.name));
    if (net_ioctl_ifreq(fd, SIOCGIFNETMASK, &ifr) == 0 && ifr.u.sa.fam == AF_INET) {
        mask.sin_family = ifr.u.sa.fam;
        for (int i = 0; i < 4; i++) mask.sin_addr[i] = ifr.u.sa.data[2 + i];
        have_mask = 1;
    }
    net_strcpy(ifr.name, name, sizeof(ifr.name));
    if (net_ioctl_ifreq(fd, SIOCGIFBRDADDR, &ifr) == 0 && ifr.u.sa.fam == AF_INET) {
        brd.sin_family = ifr.u.sa.fam;
        for (int i = 0; i < 4; i++) brd.sin_addr[i] = ifr.u.sa.data[2 + i];
        have_brd = 1;
    }

    char flagbuf[96];
    size_t d = 0;
    flagbuf[0] = '\0';
    const char *fnames[] = { "UP", "BROADCAST", "DEBUG", "LOOPBACK", "POINTOPOINT", "NOTRAILERS", "RUNNING", "NOARP", "PROMISC", "ALLMULTI", "MULTICAST" };
    unsigned fbits[] = { IFF_UP, IFF_BROADCAST, IFF_DEBUG, IFF_LOOPBACK, IFF_POINTOPOINT, IFF_NOTRAILERS, IFF_RUNNING, IFF_NOARP, IFF_PROMISC, IFF_ALLMULTI, IFF_MULTICAST };
    for (size_t i = 0; i < sizeof(fbits) / sizeof(fbits[0]); i++) {
        if (flags & fbits[i]) {
            if (d) flagbuf[d++] = ',';
            for (size_t k = 0; fnames[i][k] && d + 1 < sizeof(flagbuf); k++) flagbuf[d++] = fnames[i][k];
            flagbuf[d] = '\0';
        }
    }

    char l1[64], l2[64], l3[64], l4[32];
    net_ntoa(addr.sin_addr, l1, sizeof(l1));
    net_ntoa(mask.sin_addr, l2, sizeof(l2));
    net_ntoa(brd.sin_addr, l3, sizeof(l3));
    net_strcpy(l4, flagbuf, sizeof(l4));

    elog_info("%-8s flags=%s mtu=%d  hw=%02x:%02x:%02x:%02x:%02x:%02x", name, l4, mtu,
              hw[0], hw[1], hw[2], hw[3], hw[4], hw[5]);
    elog_info("    inet %s netmask %s broadcast %s%s", l1, l2, l3, (have_ip && have_mask && have_brd) ? "" : " (partial)");
}

/* ---------- ifconfig 设置 ---------- */

static int net_set_flags(int fd, const char *name, unsigned flags, int on)
{
    net_ifreq_t ifr;
    net_strcpy(ifr.name, name, sizeof(ifr.name));
    if (net_ioctl_ifreq(fd, SIOCGIFFLAGS, &ifr) != 0) return -1;
    if (on) ifr.u.flags |= flags; else ifr.u.flags &= ~flags;
    return net_ioctl_ifreq(fd, SIOCSIFFLAGS, &ifr);
}

static int net_set_addr(int fd, const char *name, unsigned long req, const unsigned char addr[4])
{
    net_ifreq_t ifr;
    net_strcpy(ifr.name, name, sizeof(ifr.name));
    ifr.u.sa.fam = AF_INET;
    ifr.u.sa.data[0] = 0; ifr.u.sa.data[1] = 0; /* port */
    for (int i = 0; i < 4; i++) ifr.u.sa.data[2 + i] = addr[i];
    return net_ioctl_ifreq(fd, req, &ifr);
}

static int net_set_ifaddr(int fd, const char *name, const unsigned char ip[4], const unsigned char mask[4])
{
    if (net_set_addr(fd, name, SIOCSIFADDR, ip) != 0) return -1;
    if (mask) {
        if (net_set_addr(fd, name, SIOCSIFNETMASK, mask) != 0) return -1;
        /* 广播 = ip | ~mask */
        unsigned char brd[4];
        for (int i = 0; i < 4; i++) brd[i] = (unsigned char)(ip[i] | (unsigned char)~mask[i]);
        net_set_addr(fd, name, SIOCSIFBRDADDR, brd); /* 失败不致命 */
    }
    return 0;
}

/* ---------- ifconfig 工具 ---------- */

int tool_ifconfig(int argc, char **argv, char **envp)
{
    (void)envp;
    if (argc == 1 || (argc == 2 && (aether_streq(argv[1], "-a") || aether_streq(argv[1], "-l")))) {
        int fd = (int)sys_socket(AF_INET, SOCK_DGRAM, 0);
        if (AETHER_IS_ERR((long)fd)) { elog_err("ifconfig: socket 失败"); return 1; }
        char names[NET_MAX_IFACES][16];
        int n = net_list_ifaces(names, NET_MAX_IFACES);
        if (n == 0) elog_info("ifconfig: 未发现网络接口（或 /sys/class/net 未挂载）");
        for (int i = 0; i < n; i++) net_print_iface(fd, names[i]);
        sys_close(fd);
        return 0;
    }

    const char *iface = argv[1];
    int fd = (int)sys_socket(AF_INET, SOCK_DGRAM, 0);
    if (AETHER_IS_ERR((long)fd)) { elog_err("ifconfig: socket 失败"); return 1; }

    /* ifconfig <iface> */
    if (argc == 2) { net_print_iface(fd, iface); sys_close(fd); return 0; }

    /* ifconfig <iface> up|down */
    if (argc == 3 && (aether_streq(argv[2], "up") || aether_streq(argv[2], "down"))) {
        int on = aether_streq(argv[2], "up");
        if (net_set_flags(fd, iface, IFF_UP, on) != 0) { elog_err("ifconfig: %s %s 失败（权限不足？）", iface, on ? "up" : "down"); sys_close(fd); return 1; }
        sys_close(fd);
        elog_info("ifconfig: %s %s", iface, on ? "up" : "down");
        return 0;
    }

    /* ifconfig <iface> dhcp */
    if (argc == 3 && aether_streq(argv[2], "dhcp")) {
        /* 交给 udhcpc 实现 */
        char *cmd_argv[] = { (char *)"udhcpc", (char *)"-i", (char *)iface, 0 };
        int rc = tool_udhcpc(3, cmd_argv, envp);
        sys_close(fd);
        return rc;
    }

    /* ifconfig <iface> <ip>[/<prefix>] [netmask <mask>] */
    {
        const char *spec = argv[2];
        unsigned char ip[4], mask[4] = {255, 255, 255, 0};
        int have_mask = 0;
        /* 拆 ip/prefix */
        const char *slash = 0;
        for (const char *p = spec; *p; p++) if (*p == '/') slash = p;
        char ibuf[32];
        size_t ii = 0;
        const char *end = slash ? slash : spec + net_strlen(spec);
        while (spec + ii < end && ii + 1 < sizeof(ibuf)) { ibuf[ii] = spec[ii]; ii++; }
        ibuf[ii] = '\0';
        if (net_aton(ibuf, ip) != 0) { elog_err("ifconfig: 无效地址 '%s'", spec); sys_close(fd); return 2; }
        if (slash) {
            int prefix = (int)aether_atol(slash + 1);
            if (prefix < 0 || prefix > 32) { elog_err("ifconfig: 无效前缀 '%s'", slash); sys_close(fd); return 2; }
            net_mask_from_prefix(prefix, mask);
            have_mask = 1;
        }
        /* netmask <mask> */
        if (argc >= 5 && aether_streq(argv[3], "netmask")) {
            if (net_mask_from_str(argv[4], mask) != 0) { elog_err("ifconfig: 无效掩码 '%s'", argv[4]); sys_close(fd); return 2; }
            have_mask = 1;
        }
        if (net_set_flags(fd, iface, IFF_UP, 1) != 0) { elog_err("ifconfig: %s up 失败（权限不足？）", iface); sys_close(fd); return 1; }
        if (net_set_ifaddr(fd, iface, ip, have_mask ? mask : 0) != 0) {
            char a[16], m[16];
            net_ntoa(ip, a, sizeof(a));
            sys_close(fd);
            elog_err("ifconfig: 设置 %s 地址 %s 失败（权限不足？需要 root/CAP_NET_ADMIN）", iface, a);
            return 1;
        }
        char a[16], m[16];
        net_ntoa(ip, a, sizeof(a));
        net_ntoa(mask, m, sizeof(m));
        elog_info("ifconfig: %s inet %s netmask %s", iface, a, m);
        sys_close(fd);
        return 0;
    }
}

/* ================= udhcpc（最小 DHCP 客户端） ================= */

/* Dhcp 并发互斥：rescue 后台 auto-retry 与用户手动 udhcpc 不要同时占用 udp/68 */
static volatile int g_dhcp_active = 0;

#define DHCP_BOOTREQUEST 1
#define DHCP_BOOTREPLY   2
#define DHCP_MAGIC       0x63825363
#define DHCP_DISCOVER    1
#define DHCP_OFFER       2
#define DHCP_REQUEST     3
#define DHCP_ACK         5
#define DHCP_NAK         6

#define DHCP_TAG_SUBNET    1
#define DHCP_TAG_ROUTER    3
#define DHCP_TAG_DNS       6
#define DHCP_TAG_HOSTNAME  12
#define DHCP_TAG_DOMAIN    15
#define DHCP_TAG_LEASE     51
#define DHCP_TAG_MSGTYPE   53
#define DHCP_TAG_SERVERID  54

static size_t dhcp_put_tlv(unsigned char *buf, size_t off, unsigned char code, const unsigned char *data, size_t len)
{
    buf[off++] = code;
    buf[off++] = (unsigned char)len;
    for (size_t i = 0; i < len; i++) buf[off++] = data[i];
    return off;
}

static int net_if_hwaddr(int fd, const char *iface, unsigned char mac[6])
{
    net_ifreq_t ifr;
    net_strcpy(ifr.name, iface, sizeof(ifr.name));
    if (net_ioctl_ifreq(fd, SIOCGIFHWADDR, &ifr) != 0) return -1;
    for (int i = 0; i < 6; i++) mac[i] = ifr.u.hw.addr[i];
    return 0;
}

static size_t dhcp_build_packet(unsigned char *pkt, size_t cap, unsigned long long xid, const unsigned char mac[6], unsigned char msgtype)
{
    size_t n = 0;
    if (cap < 300) return 0;
    for (size_t i = 0; i < cap; i++) pkt[i] = 0;
    pkt[n++] = DHCP_BOOTREQUEST; /* op */
    pkt[n++] = 1;                /* htype: ethernet */
    pkt[n++] = 6;                /* hlen */
    pkt[n++] = 0;                /* hops */
    /* xid (4) */
    pkt[n++] = (unsigned char)(xid >> 24);
    pkt[n++] = (unsigned char)(xid >> 16);
    pkt[n++] = (unsigned char)(xid >> 8);
    pkt[n++] = (unsigned char)xid;
    /* secs (2) = 0; flags (2) = 0x8000 (broadcast) */
    pkt[n++] = 0; pkt[n++] = 0;
    pkt[n++] = 0x80; pkt[n++] = 0x00;
    /* ciaddr, yiaddr, siaddr, giaddr = 0 (16) */
    for (int i = 0; i < 16; i++) pkt[n++] = 0;
    /* chaddr: mac (6) + pad (10) */
    for (int i = 0; i < 6; i++) pkt[n++] = mac[i];
    for (int i = 0; i < 10; i++) pkt[n++] = 0;
    /* sname[64] + file[128] = 0 */
    for (int i = 0; i < 64; i++) pkt[n++] = 0;
    for (int i = 0; i < 128; i++) pkt[n++] = 0;
    /* magic cookie */
    pkt[n++] = 0x63; pkt[n++] = 0x82; pkt[n++] = 0x53; pkt[n++] = 0x63;
    /* options */
    unsigned char one = 1;
    n = dhcp_put_tlv(pkt, n, DHCP_TAG_MSGTYPE, &one, 1);
    unsigned char host[] = "aether";
    n = dhcp_put_tlv(pkt, n, DHCP_TAG_HOSTNAME, host, (unsigned char)(sizeof(host) - 1));
    unsigned char params[] = { DHCP_TAG_SUBNET, DHCP_TAG_ROUTER, DHCP_TAG_DNS, DHCP_TAG_DOMAIN, DHCP_TAG_LEASE };
    n = dhcp_put_tlv(pkt, n, 55, params, sizeof(params));
    pkt[n++] = 255; /* end */
    return n;
}

/* 解析回复；返回 0（拿到 OFFER/ACK，参数已写出）/ 1（非目标消息或解析失败） */
static int dhcp_parse_reply(const unsigned char *pkt, size_t n, unsigned long long want_xid,
                            unsigned char out_ip[4], unsigned char out_mask[4],
                            unsigned char out_gw[4], unsigned char out_dns[3][4], int *out_dns_n,
                            unsigned char *out_msgtype, unsigned char out_serverid[4])
{
    if (n < 240) return 1;
    if (pkt[0] != DHCP_BOOTREPLY) return 1;
    unsigned long long xid = 0;
    for (int i = 0; i < 4; i++) xid = (xid << 8) | pkt[4 + i];
    if (xid != want_xid) return 1;
    if (pkt[236] != 0x63 || pkt[237] != 0x82 || pkt[238] != 0x53 || pkt[239] != 0x63) return 1;

    for (int i = 0; i < 4; i++) out_ip[i] = pkt[16 + i]; /* yiaddr */

    size_t off = 240;
    int have_type = 0, have_mask = 0, have_gw = 0, have_sid = 0;
    *out_dns_n = 0;
    while (off + 2 <= n) {
        unsigned char code = pkt[off++];
        if (code == 255) break;
        if (code == 0) continue; /* pad */
        unsigned char len = pkt[off++];
        if (off + len > n) break;
        const unsigned char *val = pkt + off;
        if (code == DHCP_TAG_MSGTYPE && len == 1) { *out_msgtype = val[0]; have_type = 1; }
        else if (code == DHCP_TAG_SUBNET && len == 4) { for (int i = 0; i < 4; i++) out_mask[i] = val[i]; have_mask = 1; }
        else if (code == DHCP_TAG_ROUTER && len >= 4) { for (int i = 0; i < 4; i++) out_gw[i] = val[i]; have_gw = 1; }
        else if (code == DHCP_TAG_DNS && len >= 4) {
            for (int k = 0; k < 3 && k * 4 + 4 <= (int)len; k++) {
                for (int i = 0; i < 4; i++) out_dns[k][i] = val[k * 4 + i];
                (*out_dns_n)++;
            }
        } else if (code == DHCP_TAG_SERVERID && len == 4) { for (int i = 0; i < 4; i++) out_serverid[i] = val[i]; have_sid = 1; }
        off += len;
    }
    (void)have_mask; (void)have_gw; (void)have_sid;
    return have_type ? 0 : 1;
}

/* 应用租约：起接口、配地址/掩码/广播、默认路由、resolv.conf */
static int dhcp_apply_lease(int fd, const char *iface,
                            const unsigned char ip[4], const unsigned char netmask[4],
                            const unsigned char gw[4], const unsigned char dns[3][4], int dns_n)
{
    if (net_set_flags(fd, iface, IFF_UP, 1) != 0) {
        elog_err("udhcpc: %s up 失败（权限不足？）", iface);
        return -1;
    }
    if (net_set_ifaddr(fd, iface, ip, netmask) != 0) {
        char a[16];
        net_ntoa(ip, a, sizeof(a));
        elog_err("udhcpc: 配置 %s 地址 %s 失败（权限不足？需要 root/CAP_NET_ADMIN）", iface, a);
        return -1;
    }
#if defined(__LP64__)
#define NET_RT_OFF_DST 8
#define NET_RT_OFF_GW  24
#define NET_RT_OFF_MASK 40
#define NET_RT_OFF_FLAGS 56
#define NET_RT_SIZE 120
#else
#define NET_RT_OFF_DST 4
#define NET_RT_OFF_GW  20
#define NET_RT_OFF_MASK 36
#define NET_RT_OFF_FLAGS 52
#define NET_RT_SIZE 84
#endif
    /* 默认路由（gw 非零才加） */
    unsigned long long gr = 0;
    for (int i = 0; i < 4; i++) gr = (gr << 8) | gw[i];
    if (gr != 0) {
        unsigned char rt[NET_RT_SIZE];
        for (size_t i = 0; i < sizeof(rt); i++) rt[i] = 0;
        net_sockaddr_in_t sa;
        /* rt_dst = 0/0 */
        net_make_sockaddr_in(&sa, (unsigned char[4]){0,0,0,0}, 0);
        for (int i = 0; i < 16; i++) rt[NET_RT_OFF_DST + i] = ((unsigned char*)&sa)[i];
        /* rt_gateway = gw */
        net_make_sockaddr_in(&sa, gw, 0);
        for (int i = 0; i < 16; i++) rt[NET_RT_OFF_GW + i] = ((unsigned char*)&sa)[i];
        /* rt_genmask = 0/0 */
        net_make_sockaddr_in(&sa, (unsigned char[4]){0,0,0,0}, 0);
        for (int i = 0; i < 16; i++) rt[NET_RT_OFF_MASK + i] = ((unsigned char*)&sa)[i];
        unsigned short flags = RTF_UP | RTF_GATEWAY;
        rt[NET_RT_OFF_FLAGS] = (unsigned char)(flags & 0xff);
        rt[NET_RT_OFF_FLAGS + 1] = (unsigned char)(flags >> 8);
        long r = sys_ioctl(fd, SIOCADDRT, (unsigned long)rt);
        if (AETHER_IS_ERR(r)) elog_info("udhcpc: 默认路由添加失败（%d）——如需联网请手动 route add", AETHER_ERR_CODE(r));
    }
    /* /etc/resolv.conf（失败不致命） */
    if (dns_n > 0) {
        int rfd = (int)sys_open("/etc/resolv.conf", O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (!AETHER_IS_ERR((long)rfd)) {
            const char *ns = "nameserver ";
            for (int k = 0; k < dns_n; k++) {
                char line[48];
                size_t d = 0;
                for (size_t i = 0; ns[i]; i++) line[d++] = ns[i];
                char a[16];
                net_ntoa(dns[k], a, sizeof(a));
                for (size_t i = 0; a[i]; i++) line[d++] = a[i];
                line[d++] = '\n';
                sys_write(rfd, line, d);
            }
            sys_close(rfd);
        } else {
            elog_info("udhcpc: 无法写 /etc/resolv.conf（非致命，DNS 未配置）");
        }
    }
    return 0;
}

/* 主流程：DHCP DORA。quiet=1 时只返回成败不打印细节。成功返回 0 */
static int dhcp_configure_opts(const char *iface, int quiet, int retries, int timeout);

static int dhcp_configure(const char *iface, int quiet)
{
    if (g_dhcp_active) return 1; /* 后台 retry 或并发 udhcpc 已经在占 udp/68 */
    g_dhcp_active = 1;
    int r = dhcp_configure_opts(iface, quiet, 4, 2);
    g_dhcp_active = 0;
    return r;
}

static int dhcp_configure_opts(const char *iface, int quiet, int retries, int timeout)
{
    int fd = (int)sys_socket(AF_INET, SOCK_DGRAM, 0);
    if (AETHER_IS_ERR((long)fd)) { if (!quiet) elog_err("udhcpc: socket 失败（%d）", AETHER_ERR_CODE((long)fd)); return 1; }

    /* 先把接口 IFF_UP——否则 DISCOVER 走不出本机，内核直接 ENETUNREACH（101） */
    if (net_set_flags(fd, iface, IFF_UP, 1) != 0) {
        if (!quiet) elog_err("udhcpc: 接口 %s 起不来（权限不足？）", iface);
        sys_close(fd);
        return 1;
    }
    (void)retries; (void)timeout;

    int one = 1;
    sys_setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    /* 绑定到指定接口（本机 IP 未配好 / 路由表为空时 DISCOVER 才能走出去） */
    sys_setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, (const void *)iface, (unsigned)aether_strlen(iface) + 1);
    /* 和别的 DHCP 客户端共享端口 68（rescue 后台 retry 在跑时手动 udhcpc 不冲突） */
    sys_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct { long sec; long usec; } tv;
    tv.sec = timeout; tv.usec = 0;
    sys_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    unsigned char mac[6];
    if (net_if_hwaddr(fd, iface, mac) != 0) { if (!quiet) elog_err("udhcpc: 无法读 %s 的 MAC（接口存在吗？）", iface); sys_close(fd); return 1; }

    unsigned long long xid = 0x12345678ULL;
    {
        int ufd = (int)sys_open("/dev/urandom", O_RDONLY, 0);
        if (!AETHER_IS_ERR((long)ufd)) {
            unsigned char b[8];
            if (sys_read(ufd, b, sizeof(b)) == (long)sizeof(b)) {
                xid = 0;
                for (int i = 0; i < 8; i++) xid = (xid << 8) | b[i];
            }
            sys_close(ufd);
        }
    }

    /* 绑定 68 端口（需要 root） */
    net_sockaddr_in_t laddr;
    unsigned char any[4] = {0,0,0,0};
    net_make_sockaddr_in(&laddr, any, 0x4400); /* htons(68) */
    if (sys_bind(fd, &laddr, sizeof(laddr)) != 0) {
        if (!quiet) elog_err("udhcpc: bind :68 失败（%d）——需要 root", AETHER_ERR_CODE(sys_bind(fd, &laddr, sizeof(laddr))));
        sys_close(fd);
        return 1;
    }

    net_sockaddr_in_t bcast;
    unsigned char b255[4] = {255,255,255,255};
    net_make_sockaddr_in(&bcast, b255, 0x4300); /* htons(67) */

    unsigned char pkt[576];
    unsigned char server_id[4] = {0,0,0,0};
    unsigned char lease_ip[4] = {0,0,0,0}, lease_mask[4] = {255,255,255,0}, lease_gw[4] = {0,0,0,0};
    unsigned char lease_dns[3][4] = {{0,0,0,0},{0,0,0,0},{0,0,0,0}};
    int dns_n = 0;

    /* 4 次 DISCOVER，等 OFFER */
    for (int attempt = 0; attempt < retries; attempt++) {
        size_t n = dhcp_build_packet(pkt, sizeof(pkt), xid, mac, DHCP_DISCOVER);
        if (n == 0) { sys_close(fd); return 1; }
        long s = sys_sendto(fd, pkt, n, 0, &bcast, sizeof(bcast));
        if (AETHER_IS_ERR(s)) { if (!quiet) elog_err("udhcpc: 发送 DISCOVER 失败（%d）", AETHER_ERR_CODE(s)); sys_close(fd); return 1; }

        long r = sys_recvfrom(fd, pkt, sizeof(pkt), 0, 0, 0);
        if (AETHER_IS_ERR(r) || r == 0) continue; /* 超时，重试 */
        unsigned char msgtype = 0;
        if (dhcp_parse_reply(pkt, (size_t)r, xid, lease_ip, lease_mask, lease_gw, lease_dns, &dns_n, &msgtype, server_id) == 0 && msgtype == DHCP_OFFER) {
            char a[16];
            net_ntoa(lease_ip, a, sizeof(a));
            if (!quiet) elog_info("udhcpc: 收到 OFFER：%s", a);
            goto have_offer;
        }
    }
    if (!quiet) elog_err("udhcpc: 未收到 DHCP OFFER（网络无 DHCP 服务或接口不通？）");
    sys_close(fd);
    return 1;

have_offer:
    /* REQUEST */
    {
        size_t n = dhcp_build_packet(pkt, sizeof(pkt), xid, mac, DHCP_REQUEST);
        /* 附加: 50 (req ip) + 54 (server id) */
        /* 重建带参数的包更稳妥：手工补两个选项 */
        /* 重新构造 options 区（覆盖掉原 params 后追加） */
        /* 简单起见：重新构造整个包再请求 */
        unsigned char req[576];
        for (size_t i = 0; i < sizeof(req); i++) req[i] = 0;
        size_t m = dhcp_build_packet(req, sizeof(req), xid, mac, DHCP_REQUEST);
        /* 删除末尾的 255 并追加 50/54 + 255 */
        if (m > 0 && req[m - 1] == 255) m--;
        m = dhcp_put_tlv(req, m, 50, lease_ip, 4);
        unsigned char sid_tlv[4] = {server_id[0], server_id[1], server_id[2], server_id[3]};
        m = dhcp_put_tlv(req, m, DHCP_TAG_SERVERID, sid_tlv, 4);
        req[m++] = 255;
        long s = sys_sendto(fd, req, m, 0, &bcast, sizeof(bcast));
        if (AETHER_IS_ERR(s)) { if (!quiet) elog_err("udhcpc: 发送 REQUEST 失败（%d）", AETHER_ERR_CODE(s)); sys_close(fd); return 1; }
        long r = sys_recvfrom(fd, pkt, sizeof(pkt), 0, 0, 0);
        if (AETHER_IS_ERR(r) || r == 0) { if (!quiet) elog_err("udhcpc: 未收到 DHCP ACK"); sys_close(fd); return 1; }
        unsigned char msgtype = 0;
        if (dhcp_parse_reply(pkt, (size_t)r, xid, lease_ip, lease_mask, lease_gw, lease_dns, &dns_n, &msgtype, server_id) != 0 || msgtype != DHCP_ACK) {
            if (!quiet) elog_err("udhcpc: 未收到 DHCP ACK（type=%d）", msgtype);
            sys_close(fd);
            return 1;
        }
        (void)n;
        (void)pkt;
    }

    /* 应用 */
    if (dhcp_apply_lease(fd, iface, lease_ip, lease_mask, lease_gw, lease_dns, dns_n) != 0) {
        sys_close(fd);
        return 1;
    }
    {
        char a[16], m[16], g[16];
        net_ntoa(lease_ip, a, sizeof(a));
        net_ntoa(lease_mask, m, sizeof(m));
        net_ntoa(lease_gw, g, sizeof(g));
        elog_info("udhcpc: 获得地址 %s，掩码 %s，网关 %s%s", a, m, g, dns_n > 0 ? "，DNS 已写入 /etc/resolv.conf" : "");
    }
    sys_close(fd);
    return 0;
}

int tool_udhcpc(int argc, char **argv, char **envp)
{
    (void)envp;
    const char *iface = 0;
    int retries = 4, timeout = 2;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (aether_streq(a, "-i") && i + 1 < argc) iface = argv[++i];
        else if (aether_streq(a, "-t") && i + 1 < argc) retries = (int)aether_atol(argv[++i]);
        else if (aether_streq(a, "-T") && i + 1 < argc) timeout = (int)aether_atol(argv[++i]);
        else if (aether_streq(a, "-n")) { /* 保持失败即返回，已有语义 */ }
        else if (a[0] == '-' && a[1]) { elog_err("udhcpc: unknown option '%s'（支持：-i IFACE, -t N 重试次数, -T N 超时秒数, -n）", a); return 2; }
        else { iface = a; }
    }
    if (!iface) {
        /* 默认：第一个非 lo 接口 */
        char names[NET_MAX_IFACES][16];
        int n = net_list_ifaces(names, NET_MAX_IFACES);
        for (int i = 0; i < n; i++)
            if (!aether_streq(names[i], "lo")) { iface = names[i]; break; }
        if (!iface) { elog_err("udhcpc: 未找到可用网络接口（需要 -i <iface>）"); return 1; }
    }
    return dhcp_configure_opts(iface, 0, retries, timeout);
}

/* ================= telnetd（最小 telnet 服务器：socket + aether shell） ================= */
/* IAC 过滤在 shell_read_fd0_line 里（aether_telnet_mode=1 时启用） */

static volatile int telnetd_stop;
static void telnetd_handler(int sig) { (void)sig; telnetd_stop = 1; }

/* 每个连接 fork 一个子进程跑 rescue shell（不经 pty，行模式 + IAC 过滤） */
static void telnetd_handle_conn(int conn, char **envp)
{
    int pid = (int)sys_fork();
    if (pid != 0) return; /* 父进程立即返回 */

    /* 子进程：socket 绑定到 0/1/2 */
    sys_dup2(conn, 0);
    sys_dup2(conn, 1);
    sys_dup2(conn, 2);
    if (conn > 2) sys_close(conn);
    aether_telnet_mode = 1;

    elog_info("telnetd: 连接建立，启动 rescue shell（无认证）");
    shell_main(1, (char *[]){ (char *)"aether", 0 }, envp);
    aether_exit(0);
}

static int telnetd_serve(int port, char **envp)
{
    int fd = (int)sys_socket(AF_INET, SOCK_STREAM, 0);
    if (AETHER_IS_ERR((long)fd)) { elog_err("telnetd: socket 失败（%d）", AETHER_ERR_CODE((long)fd)); return 1; }
    int one = 1;
    sys_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    net_sockaddr_in_t sa;
    unsigned char any[4] = {0,0,0,0};
    unsigned short port_be = (unsigned short)(((unsigned)port >> 8) | ((unsigned)port << 8));
    net_make_sockaddr_in(&sa, any, port_be);
    if (sys_bind(fd, &sa, sizeof(sa)) != 0) {
        elog_err("telnetd: bind :%d 失败（%d）——端口被占用或权限不足（需要 root）", port, AETHER_ERR_CODE(sys_bind(fd, &sa, sizeof(sa))));
        sys_close(fd);
        return 1;
    }
    if (sys_listen(fd, 4) != 0) {
        elog_err("telnetd: listen 失败（%d）", AETHER_ERR_CODE(sys_listen(fd, 4)));
        sys_close(fd);
        return 1;
    }
    aether_signal(SIGINT, telnetd_handler);
    aether_signal(SIGTERM, telnetd_handler);
    elog_info("telnetd: 监听 0.0.0.0:%d（rescue shell，无认证；Ctrl-C 停止）", port);

    while (!telnetd_stop) {
        net_sockaddr_in_t peer;
        unsigned int peerlen = sizeof(peer);
        int conn = (int)sys_accept4(fd, &peer, &peerlen, 0);
        if (AETHER_IS_ERR((long)conn)) {
            if (!telnetd_stop && AETHER_ERR_CODE((long)conn) == 4) continue; /* EINTR */
            continue;
        }
        char peer_ip[16];
        net_ntoa(peer.sin_addr, peer_ip, sizeof(peer_ip));
        elog_info("telnetd: 连接来自 %s", peer_ip);
        telnetd_handle_conn(conn, envp);
        /* 收割已退出的子进程，避免僵尸 */
        int status = 0;
        while (sys_wait4(-1, &status, 1 /*WNOHANG*/, 0) > 0) { }
    }
    sys_close(fd);
    elog_info("telnetd: 已停止");
    return 0;
}

int tool_telnetd(int argc, char **argv, char **envp)
{
    int port = 23;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (aether_streq(a, "-p") && i + 1 < argc) { port = (int)aether_atol(argv[++i]); }
        else if (a[0] == '-' && a[1]) { elog_err("telnetd: unknown option '%s'", a); return 2; }
        else { port = (int)aether_atol(a); }
    }
    if (port < 1 || port > 65535) { elog_err("telnetd: 无效端口"); return 2; }
    return telnetd_serve(port, envp);
}

/* ================= netup（一键：检测网卡 → DHCP → 启动 telnetd） ================= */

/* 是无线网卡吗：mac80211 会为无线接口创建 /sys/class/net/<iface>/wireless/。
 * aether rescue 不支持 WiFi，自动选网卡时跳过这些接口。 */
static int iface_is_wireless(const char *iface)
{
    char p[128];
    size_t k = 0, i = 0;
    while ("/sys/class/net/"[k] && k + 1 < sizeof(p)) { p[k] = "/sys/class/net/"[k]; k++; }
    while (iface[i] && k + 1 < sizeof(p)) p[k++] = iface[i++];
    i = 0;
    while ("/wireless"[i] && k + 1 < sizeof(p)) { p[k++] = "/wireless"[i++]; }
    p[k] = '\0';
    long fd = sys_open(p, O_RDONLY | O_DIRECTORY, 0);
    if (AETHER_IS_ERR(fd)) return 0;
    sys_close((int)fd);
    return 1;
}

static int netup_find_first_nonlo(char out[16])
{
    char names[NET_MAX_IFACES][16];
    int n = net_list_ifaces(names, NET_MAX_IFACES);
    for (int i = 0; i < n; i++) {
        if (aether_streq(names[i], "lo")) continue;
        if (iface_is_wireless(names[i])) continue; /* 本程序不支持 WiFi */
        net_strcpy(out, names[i], 16);
        return 0;
    }
    return -1;
}

int tool_netup(int argc, char **argv, char **envp)
{
    const char *iface = 0;
    if (argc > 1) iface = argv[1];
    char auto_iface[16];
    if (!iface) {
        if (netup_find_first_nonlo(auto_iface) != 0) {
            elog_err("netup: 未发现非 loopback 网络接口");
            return 1;
        }
        iface = auto_iface;
    }
    elog_info("netup: 使用接口 %s", iface);
    elog_info("netup: 启动接口并请求 DHCP…");
    if (dhcp_configure(iface, 0) != 0) {
        elog_err("netup: DHCP 失败（无法联网）；接口 %s 已保留可用，请手动配置", iface);
        return 1;
    }
    /* 后台启动 telnetd */
    int pid = (int)sys_fork();
    if (pid == 0) {
        telnetd_serve(23, envp);
        aether_exit(0);
    }
    if (pid > 0)
        elog_info("netup: telnetd 已启动（端口 23；无认证！仅限受信网络；Ctrl-C 停止 shell 不影响它）");
    else
        elog_err("netup: telnetd 启动失败（%d）", AETHER_ERR_CODE((long)pid));
    return 0;
}

/* rescue 模式自动联网（boot 兜底路径用）：有网卡 → DHCP → telnetd。
 * 返回 0 成功/无需/完成，1 失败。cmdline 含 "nonet" 时跳过。 */
/* 检查接口是否已有非零 IPv4 地址（用户手动配了或之前重试成功） */
static int iface_has_ipv4(int fd, const char *iface)
{
    net_ifreq_t ifr;
    net_strcpy(ifr.name, iface, sizeof(ifr.name));
    if (net_ioctl_ifreq(fd, SIOCGIFADDR, &ifr) != 0) return 0;
    if (ifr.u.sa.fam != AF_INET) return 0;
    for (int i = 0; i < 4; i++) if (ifr.u.sa.data[2 + i] != 0) return 1;
    return 0;
}

/* 救援模式后台自动配置网络：未配置则反复重试直到成功（或 cmdline 含 nonet / 已有地址）。
 * 由 rescue_consoles fork 出的子进程运行（永不返回或返回 0）。 */
int net_rescue_auto_retry(char **envp)
{
    /* cmdline 含 nonet → 不自动配置 */
    {
        char buf[4096];
        size_t len = 0;
        if (aether_read_file("/proc/cmdline", buf, sizeof(buf) - 1, &len) == 0) {
            buf[len] = '\0';
            const char *p = buf;
            while (*p) {
                while (*p == ' ' || *p == '\t') p++;
                const char *tok = p;
                while (*p && *p != ' ' && *p != '\t') p++;
                if ((size_t)(p - tok) == 5 && aether_strncmp(tok, "nonet", 5) == 0)
                    return 0;
            }
        }
    }
    for (;;) {
        char iface[16];
        if (netup_find_first_nonlo(iface) != 0) {
            /* 暂时没有网卡（USB 网卡等稍后出现）：耐心等 */
            struct { long sec; long nsec; } ts = { 3, 0 };
            sys_nanosleep(&ts, 0);
            continue;
        }
        int fd = (int)sys_socket(AF_INET, SOCK_DGRAM, 0);
        int have = 0;
        if (fd >= 0) {
            have = iface_has_ipv4(fd, iface);
            sys_close(fd);
        }
        if (have) return 0; /* 用户已手动配置或之前重试成功 */

        if (dhcp_configure(iface, 1 /*quiet：静默重试，成功时由 udhcpc 打一行 */) == 0) {
            int pid = (int)sys_fork();
            if (pid == 0) {
                telnetd_serve(23, envp);
                aether_exit(0);
            }
            elog_info("rescue: 网络已配置（%s），telnetd 已启动（0.0.0.0:23，无认证）", iface);
            return 0;
        }
        struct { long sec; long nsec; } ts = { 2, 0 };
        sys_nanosleep(&ts, 0);
    }
}
