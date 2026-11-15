/*
 * statx.h — statx(2) 返回结构（内核 UAPI，四架构布局一致）
 * 不依赖 libc 头的 struct stat（各架构布局不同），救援环境用 statx 统一处理。
 */
#ifndef AETHER_STATX_H
#define AETHER_STATX_H

#define STATX_TYPE         0x00000001U
#define STATX_MODE         0x00000002U
#define STATX_NLINK        0x00000004U
#define STATX_UID          0x00000008U
#define STATX_GID          0x00000010U
#define STATX_ATIME        0x00000020U
#define STATX_MTIME        0x00000040U
#define STATX_CTIME        0x00000080U
#define STATX_INO          0x00000100U
#define STATX_SIZE         0x00000200U
#define STATX_BLOCKS       0x00000400U
#define STATX_BASIC_STATS  0x000007FFU

/* AT_SYMLINK_NOFOLLOW = 0x100 */
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif

typedef struct {
    unsigned long long tv_sec;
    long long          tv_nsec;
} aether_timespec_t;

typedef struct {
    unsigned int       stx_mask;        /* 0x000 */
    unsigned int       stx_blksize;     /* 0x004 */
    unsigned long long stx_attributes;  /* 0x008 */
    unsigned int       stx_nlink;       /* 0x010 */
    unsigned int       stx_uid;         /* 0x014 */
    unsigned int       stx_gid;         /* 0x018 */
    unsigned short     stx_mode;        /* 0x01c */
    unsigned short     __spare0;        /* 0x01e */
    unsigned long long stx_ino;         /* 0x020 */
    unsigned long long stx_size;        /* 0x028 */
    unsigned long long stx_blocks;      /* 0x030 */
    unsigned long long stx_attributes_mask; /* 0x038 */
    aether_timespec_t  stx_atime;      /* 0x040 */
    aether_timespec_t  stx_btime;      /* 0x050 */
    aether_timespec_t  stx_ctime;      /* 0x060 */
    aether_timespec_t  stx_mtime;      /* 0x070 */
    unsigned int       stx_rdev_major; /* 0x080 */
    unsigned int       stx_rdev_minor; /* 0x084 */
    unsigned int       stx_dev_major;  /* 0x088 */
    unsigned int       stx_dev_minor;  /* 0x08c */
    unsigned long long stx_mnt_id;     /* 0x090 */
    unsigned int       stx_dio_mem_align;       /* 0x098 */
    unsigned int       stx_dio_offset_align;    /* 0x09c */
    unsigned long long stx_subvol;              /* 0x0a0 */
    unsigned int       stx_atomic_write_unit_min;             /* 0x0a8 */
    unsigned int       stx_atomic_write_unit_max;             /* 0x0ac */
    unsigned int       stx_atomic_write_segments_max;         /* 0x0b0 */
    unsigned int       stx_dio_read_offset_align;             /* 0x0b4 */
    unsigned int       stx_atomic_write_unit_max_opt;         /* 0x0b8 */
    unsigned int       __spare2[1];                           /* 0x0bc */
    unsigned long long __spare3[8];                           /* 0x0c0 */
} aether_statx_t;                      /* 0x100 = 256，与内核 struct statx 一致 */

/* 编译期防线：内核不可变更布局，结构体必须 256 字节且关键字段偏移一致 */
#include <stddef.h>
_Static_assert(sizeof(aether_statx_t) == 256, "aether_statx_t must match kernel struct statx (256 bytes)");
_Static_assert(offsetof(aether_statx_t, stx_ino) == 0x20, "stx_ino offset");
_Static_assert(offsetof(aether_statx_t, stx_size) == 0x28, "stx_size offset");
_Static_assert(offsetof(aether_statx_t, stx_mtime) == 0x70, "stx_mtime offset");
_Static_assert(offsetof(aether_statx_t, stx_rdev_major) == 0x80, "stx_rdev_major offset");
_Static_assert(offsetof(aether_statx_t, stx_dev_major) == 0x88, "stx_dev_major offset");
_Static_assert(offsetof(aether_statx_t, stx_mnt_id) == 0x90, "stx_mnt_id offset");

#endif /* AETHER_STATX_H */
