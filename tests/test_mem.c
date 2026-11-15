/* test_mem.c — arena bump + slot_pool 的单元测试 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mem.h"

static int failures, checks;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { failures++; fprintf(stderr, "FAIL %d: %s\n", __LINE__, #cond); } \
} while (0)

/* 与 mem.c 一致：slot 按 sizeof(void*) 对齐 */
static size_t slot_stride(size_t s)
{
    size_t a = sizeof(void *);
    s = (s + a - 1) & ~(a - 1);
    return s < a ? a : s;
}

int main(void)
{
    const size_t stride = slot_stride(AETHER_SLOT_SIZE);
    const size_t per_page = 4096 / stride;

    /* arena：小分配，内容可写 */
    {
        mem_arena_t *a = mem_arena_create();
        CHECK(a != 0);
        char *p = mem_arena_alloc(a, 100);
        CHECK(p != 0);
        memset(p, 0x5A, 100);
        CHECK(p[0] == 0x5A && p[99] == 0x5A);
        mem_arena_destroy(a);
    }

    /* arena：分配 > 单页的大块 */
    {
        mem_arena_t *a = mem_arena_create();
        CHECK(a != 0);
        char *big = mem_arena_alloc(a, 10000);
        CHECK(big != 0);
        memset(big, 0xA5, 10000);
        CHECK((unsigned char)big[9999] == 0xA5);
        mem_arena_destroy(a);
    }

    /* arena：reset 后从头再来，首个分配拿到同一地址 */
    {
        mem_arena_t *a = mem_arena_create();
        CHECK(a != 0);
        char *p1 = mem_arena_alloc(a, 32);
        mem_arena_reset(a);
        char *p2 = mem_arena_alloc(a, 32);
        CHECK(p1 == p2);
        mem_arena_destroy(a);
    }

    /* arena：reset 后仍可正常分配、可销毁 */
    {
        mem_arena_t *a = mem_arena_create();
        CHECK(a != 0);
        mem_arena_alloc(a, 5000);
        mem_arena_reset(a);
        char *p = mem_arena_alloc(a, 64);
        CHECK(p != 0);
        mem_arena_destroy(a);
    }

    /* pool：基本分配/释放/复用 */
    {
        slot_pool_t *p = slot_pool_create(AETHER_SLOT_SIZE, 2);
        CHECK(p != 0);
        void *a1 = slot_pool_alloc(p);
        void *a2 = slot_pool_alloc(p);
        CHECK(a1 && a2 && a1 != a2);
        slot_pool_free(p, a1);
        void *a3 = slot_pool_alloc(p);
        CHECK(a3 == a1); /* 空闲槽位被复用 */
        slot_pool_destroy(p);
    }

    /* pool：容量 = max_pages 页，超出返回 NULL */
    {
        slot_pool_t *p = slot_pool_create(AETHER_SLOT_SIZE, 2);
        CHECK(p != 0);
        size_t cap = 2 * per_page, i;
        void **all = malloc(cap * sizeof(void *));
        CHECK(all != 0);
        for (i = 0; i < cap; i++) {
            all[i] = slot_pool_alloc(p);
            CHECK(all[i] != 0);
        }
        CHECK(slot_pool_alloc(p) == 0); /* 满了 */
        for (i = 0; i < cap; i++) slot_pool_free(p, all[i]);
        free(all);
        slot_pool_destroy(p);
    }

    /* pool：某页全空且 npages > 1 时 munmap 归还 */
    {
        slot_pool_t *p = slot_pool_create(AETHER_SLOT_SIZE, 2);
        CHECK(p != 0);
        size_t i;
        void **pg1 = malloc(per_page * sizeof(void *));
        CHECK(pg1 != 0);
        for (i = 0; i < per_page; i++) {
            pg1[i] = slot_pool_alloc(p);
            CHECK(pg1[i] != 0);
        }
        CHECK(slot_pool_pages(p) == 1);
        void *extra = slot_pool_alloc(p); /* 触发第二页 */
        CHECK(extra != 0);
        CHECK(slot_pool_pages(p) == 2);
        for (i = 0; i < per_page; i++)
            slot_pool_free(p, pg1[i]);   /* 第一页全空 → 应被 munmap */
        CHECK(slot_pool_pages(p) == 1);
        free(pg1);
        slot_pool_free(p, extra);        /* 只剩 1 页且全空：不 munmap（保留一页） */
        CHECK(slot_pool_pages(p) == 1);
        slot_pool_destroy(p);
    }

    /* pool：slot 大于一页 → 拒绝创建 */
    CHECK(slot_pool_create(8192, 1) == 0);

    /* pool：槽大小小于指针 → 按 sizeof(void*) 起步 */
    {
        slot_pool_t *p = slot_pool_create(1, 1);
        CHECK(p != 0);
        CHECK(slot_pool_alloc(p) != 0);
        slot_pool_destroy(p);
    }

    printf("test_mem: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
