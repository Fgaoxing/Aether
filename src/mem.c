#include "mem.h"
#include "syscall_shim.h"

/* 页内 slot 全空时 munmap 还给内核（§8.2 B 中标注的可选优化，这里实现） */
#define MEM_PAGE 4096

#define ARENA_MAX_BLOCKS 120          /* 控制页放得下（120*32 + 头部 ≤ 4K）；120 个 4K 页 = 480K 临时数据，一行解析足够 */
#define ARENA_ALIGN 16

struct arena_block {
    void  *base;   /* mmap 起点 */
    size_t size;   /* 总字节数 */
    size_t used;   /* 已 bump 字节数 */
    int    in_use; /* 该描述符被占用 */
};

struct mem_arena {
    struct arena_block blocks[ARENA_MAX_BLOCKS];
    int                nblocks;      /* 已（曾）使用的描述符数 */
    int                cur;          /* 当前 bump 的块下标，没有则为 -1 */
    void              *control;      /* arena 自身所在页（mmap） */
};

/* 控制结构必须塞进一页（依赖 anon mmap 的内核清零页） */
typedef char aether_arena_control_fits_in_page[(sizeof(struct mem_arena) <= MEM_PAGE) ? 1 : -1];

static size_t align_up(size_t n, size_t a)
{
    return (n + a - 1) & ~(a - 1);
}

mem_arena_t *mem_arena_create(void)
{
    void *page = (void *)sys_mmap(0, MEM_PAGE, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (AETHER_IS_ERR((long)page))
        return 0;

    mem_arena_t *a = (mem_arena_t *)page;
    a->control = page;
    a->nblocks = 0;
    a->cur = -1;
    /* 控制页本身已由内核清零（匿名 mmap），无需再 memset */
    return a;
}

void mem_arena_reset(mem_arena_t *a)
{
    int i;
    if (!a) return;
    for (i = 0; i < a->nblocks; i++) {
        if (a->blocks[i].in_use)
            sys_munmap(a->blocks[i].base, a->blocks[i].size);
    }
    for (i = 0; i < a->nblocks; i++)
        a->blocks[i].in_use = 0;
    a->nblocks = 0;
    a->cur = -1;
}

void mem_arena_destroy(mem_arena_t *a)
{
    void *control;
    if (!a) return;
    mem_arena_reset(a);
    control = a->control;
    sys_munmap(control, MEM_PAGE);
}

size_t mem_arena_block_count(const mem_arena_t *a)
{
    return a ? (size_t)a->nblocks : 0;
}

static int arena_new_block(mem_arena_t *a, size_t need)
{
    size_t size = need > MEM_PAGE ? align_up(need, MEM_PAGE) : MEM_PAGE;
    void *base;
    struct arena_block *blk;

    if (a->nblocks >= ARENA_MAX_BLOCKS)
        return -1; /* 临时池满：不自动扩容，上层报错 */

    base = (void *)sys_mmap(0, size, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (AETHER_IS_ERR((long)base))
        return -1;

    blk = &a->blocks[a->nblocks];
    blk->base = base;
    blk->size = size;
    blk->used = 0;
    blk->in_use = 1;
    a->cur = a->nblocks;
    a->nblocks++;
    return 0;
}

void *mem_arena_alloc(mem_arena_t *a, size_t n)
{
    struct arena_block *blk;
    size_t need = align_up(n, ARENA_ALIGN);
    void *p;

    if (!a || n == 0)
        return 0;

    if (a->cur < 0 || a->blocks[a->cur].size - a->blocks[a->cur].used < need) {
        if (arena_new_block(a, need) != 0)
            return 0;
    }

    blk = &a->blocks[a->cur];
    p = (char *)blk->base + blk->used;
    blk->used += need;
    return p;
}

/* ---------------- slot_pool ---------------- */

#define POOL_MAX_PAGES 64 /* 最多 64 个 4K 页；AETHER_SLOT_SIZE=64 时即 4096 个 slot */

struct pool_page {
    void  *base;    /* mmap 起点 */
    size_t used;    /* 已分配 slot 数 */
};

struct slot_pool {
    size_t        slot_size;
    size_t        nslots_per_page;
    size_t        max_pages;
    struct pool_page pages[POOL_MAX_PAGES];
    size_t        npages;
    void         *free_head; /* 空闲 slot 链表头（用槽位自身存 next） */
    void         *control;
};

typedef char aether_pool_control_fits_in_page[(sizeof(struct slot_pool) <= MEM_PAGE) ? 1 : -1];

/* 匿名 mmap 的内存由内核清零，不能假定结构体被 zero-initialized 之外的成员——
 * 但 create 时会显式按需清零关键字段，依赖“内核给的页是零”的前提，如上 */

slot_pool_t *slot_pool_create(size_t slot_size, size_t max_pages)
{
    void *page = (void *)sys_mmap(0, MEM_PAGE, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    slot_pool_t *p;

    if (AETHER_IS_ERR((long)page))
        return 0;

    p = (slot_pool_t *)page;
    if (slot_size < sizeof(void *))
        slot_size = sizeof(void *);
    slot_size = align_up(slot_size, sizeof(void *));
    if (slot_size > MEM_PAGE)
        return 0; /* slot 装不进一页：拒绝创建 */
    p->slot_size = slot_size;
    p->nslots_per_page = MEM_PAGE / p->slot_size;
    p->max_pages = max_pages > POOL_MAX_PAGES ? POOL_MAX_PAGES : max_pages;
    if (p->max_pages == 0)
        p->max_pages = 1;
    p->npages = 0;
    p->free_head = 0;
    p->control = page;
    return p;
}

static struct pool_page *pool_find_page(slot_pool_t *p, const void *ptr)
{
    size_t i;
    for (i = 0; i < p->npages; i++) {
        char *base = (char *)p->pages[i].base;
        if ((const char *)ptr >= base && (const char *)ptr < base + MEM_PAGE)
            return &p->pages[i];
    }
    return 0;
}

void *slot_pool_alloc(slot_pool_t *p)
{
    struct pool_page *pg;
    void *slot;

    if (!p)
        return 0;

    if (p->free_head) {
        slot = p->free_head;
        p->free_head = *(void **)slot;
        pg = pool_find_page(p, slot);
        if (pg) pg->used++;
        return slot;
    }

    if (p->npages >= p->max_pages)
        return 0; /* 池满：不自动扩容 */

    {
        void *base = (void *)sys_mmap(0, MEM_PAGE, PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        char *slot0;
        if (AETHER_IS_ERR((long)base))
            return 0;
        p->pages[p->npages].base = base;
        p->pages[p->npages].used = 1;
        p->npages++;
        /* 把这一页切成 slot，全部挂进 free 链表，再交出一个 */
        slot0 = (char *)base + (p->nslots_per_page - 1) * p->slot_size;
        for (;;) {
            size_t idx = (size_t)((char *)slot0 - (char *)base) / p->slot_size;
            if (idx == 0) break;
            *(void **)slot0 = p->free_head;
            p->free_head = slot0;
            slot0 -= p->slot_size;
        }
        /* slot0 现在指向第 0 个 slot，直接返回，不进链表 */
        return slot0;
    }
}

void slot_pool_free(slot_pool_t *p, void *ptr)
{
    struct pool_page *pg;

    if (!p || !ptr)
        return;

    pg = pool_find_page(p, ptr);
    if (!pg)
        return; /* 不是本池的指针：静默忽略，避免污染链表 */

    /* 挂回 free 链表 */
    *(void **)ptr = p->free_head;
    p->free_head = ptr;
    pg->used--;

    /* 该页全空且还有别的页：munmap 还给内核 */
    if (pg->used == 0 && p->npages > 1) {
        size_t idx = (size_t)(pg - p->pages);
        void *base = pg->base;
        void *cur = p->free_head, *prev = 0;

        /* 从 free 链表摘掉属于这一页的所有 slot */
        while (cur) {
            void *next = *(void **)cur;
            if ((char *)cur >= (char *)base && (char *)cur < (char *)base + MEM_PAGE) {
                if (prev) *(void **)prev = next; else p->free_head = next;
            } else {
                prev = cur;
            }
            cur = next;
        }
        sys_munmap(base, MEM_PAGE);
        p->pages[idx] = p->pages[p->npages - 1]; /* 与末页交换删除 */
        p->npages--;
    }
}

void slot_pool_destroy(slot_pool_t *p)
{
    size_t i;
    void *control;
    if (!p) return;
    for (i = 0; i < p->npages; i++)
        sys_munmap(p->pages[i].base, MEM_PAGE);
    control = p->control;
    sys_munmap(control, MEM_PAGE);
}

size_t slot_pool_pages(const slot_pool_t *p)
{
    return p ? p->npages : 0;
}
