/*
 * mem.h — arena bump + 页内固定 slot 双池分配器（§8）
 *
 * A. mem_arena：mmap 出来的连续页块链表，bump 指针分配（不 free 单个对象），
 *    回收粒度 = 整个 arena。典型用途：解析一行产生的临时数据。
 * B. slot_pool：一页切成固定大小 slot，页内空闲链表复用槽位自身存 next，
 *    分配/释放均 O(1)；不做自动扩容（池满返回 NULL，上层报错）。
 *    典型用途：生命周期交错、需要单独 free 的小节点。
 *
 * 只用 mmap，不用 sbrk；单线程，不加锁。长期存活资源（fd、tty、子进程）不要放进 arena。
 */
#ifndef AETHER_MEM_H
#define AETHER_MEM_H

#include <stddef.h>

/* 待定项（§14）：SLOT_SIZE 定 64B 还是 128B。当前按 64B 实现。 */
#define AETHER_SLOT_SIZE 64

typedef struct mem_arena mem_arena_t;
mem_arena_t *mem_arena_create(void);
void        *mem_arena_alloc(mem_arena_t *a, size_t n); /* 16B 对齐；arena 必须先 create；失败返回 NULL */
void         mem_arena_reset(mem_arena_t *a);           /* munmap 所有数据页，结构清零，可立即复用 */
void         mem_arena_destroy(mem_arena_t *a);
size_t       mem_arena_block_count(const mem_arena_t *a); /* 已用页块数（调试/测试用） */

typedef struct slot_pool slot_pool_t;
/* slot_size < sizeof(void*) 时按 8B 对齐最小取值；max_pages 限制池最多占用的 4K 页数 */
slot_pool_t *slot_pool_create(size_t slot_size, size_t max_pages);
void        *slot_pool_alloc(slot_pool_t *p);           /* 满则返回 NULL（不自动扩容） */
void         slot_pool_free(slot_pool_t *p, void *ptr); /* 该页全部 slot 空闲时 munmap */
void         slot_pool_destroy(slot_pool_t *p);
size_t       slot_pool_pages(const slot_pool_t *p);     /* 当前占用的 4K 页数（调试/测试用） */

#endif /* AETHER_MEM_H */
