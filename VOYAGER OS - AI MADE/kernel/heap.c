/* Simple page-granular kernel heap over a static arena.
 * Allocation layout per block (page aligned):
 *   [0..7]   page count (uint64_t)
 *   [8..15]  reserved
 *   [16..]   user data
 * kfree(ptr) recovers base = ptr - 16.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "k.h"

#define HEAP_SIZE   (96UL * 1024 * 1024)
#define PAGE_SIZE   4096UL
#define NPAGES      (HEAP_SIZE / PAGE_SIZE)
#define HDR_SIZE    16UL

/* block header: [0..7]=page count, [8..15]=magic canary */
#define BLK_MAGIC 0x4B4C4F434B212121ULL

static uint8_t heap_mem[HEAP_SIZE] __attribute__((aligned(PAGE_SIZE)));
static uint64_t heap_bitmap[NPAGES / 64];
static uint64_t heap_used_pages;

/* touch every 1MB of the arena once at boot to verify Limine mapped it */
void heap_selftest(void) {
    volatile uint8_t *p = heap_mem;
    for (size_t off = 0; off < HEAP_SIZE; off += 1024 * 1024)
        p[off] = 0xA5;
    kprint("heap: selftest ok\n");
}

static int page_is_free(size_t p) {
    return !(heap_bitmap[p / 64] & (1ULL << (p % 64)));
}
static void page_set(size_t p) {
    heap_bitmap[p / 64] |= (1ULL << (p % 64));
}
static void page_clear(size_t p) {
    heap_bitmap[p / 64] &= ~(1ULL << (p % 64));
}

void *kmalloc(size_t size) {
    if (size == 0) size = 1;
    size_t need = (size + HDR_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
    if (need > NPAGES) return NULL;
    /* first-fit contiguous run */
    size_t run_start = 0;
    size_t i = 0;
    while (i < NPAGES) {
        if (page_is_free(i)) {
            size_t j = i;
            while (j < NPAGES && page_is_free(j)) j++;
            if (j - i >= need) {
                run_start = i;
                goto found;
            }
            i = j;
        } else {
            i++;
        }
    }
    return NULL;
found:
    for (size_t k = run_start; k < run_start + need; k++) page_set(k);
    heap_used_pages += need;
    uint8_t *base = heap_mem + run_start * PAGE_SIZE;
    *(uint64_t *)base = need;
    *(uint64_t *)(base + 8) = BLK_MAGIC;
    return base + HDR_SIZE;
}

void kfree(void *ptr) {
    if (!ptr) return;
    uint8_t *base = (uint8_t *)ptr - HDR_SIZE;
    /* sanity: base must be inside heap and page-aligned region */
    if (base < heap_mem || base >= heap_mem + HEAP_SIZE) {
        kprintf("kfree BADPTR %p", ptr);
        return;
    }
    uint64_t need = *(uint64_t *)base;
    uint64_t magic = *(uint64_t *)(base + 8);
    if (magic != BLK_MAGIC) {
        kprintf("kfree CORRUPT %p need=%u magic=%x caller=%p", ptr, (unsigned)need, (unsigned)magic, __builtin_return_address(0));
        return;
    }
    size_t idx = (size_t)(base - heap_mem) / PAGE_SIZE;
    if (need == 0 || need > NPAGES || idx + need > NPAGES) {
        kprintf("kfree BADNEED %p need=%u idx=%u", ptr, (unsigned)need, (unsigned)idx);
        return;
    }
    for (size_t k = idx; k < idx + need && k < NPAGES; k++) page_clear(k);
    heap_used_pages -= need;
    /* NOTE: no memset wipe - the block may be large and the wipe is
     * only hygiene; skipping it avoids a needless multi-MB memset. */
}

void *kzalloc(size_t size) {
    void *p = kmalloc(size);
    if (p) memset(p, 0, size);
    return p;
}

void *krealloc(void *ptr, size_t size) {
    if (!ptr) return kmalloc(size);
    if (size == 0) { kfree(ptr); return NULL; }
    /* heap blocks don't track size; caller contract: only realloc buffers
     * we allocated with known capacity is unsafe — allocate new + copy is
     * impossible without size. kmalloc stores need in header; recover it. */
    uint64_t *base = (uint64_t *)((char *)ptr - 16);
    size_t need = (size_t)base[0]; /* header: [0]=pages [1]=magic */
    size_t oldcap = need * PAGE_SIZE - 16;
    void *np = kmalloc(size);
    if (!np) return NULL;
    memcpy(np, ptr, oldcap < size ? oldcap : size);
    kfree(ptr);
    return np;
}

size_t kmalloc_used(void) { return heap_used_pages * PAGE_SIZE; }
size_t kmalloc_total(void) { return HEAP_SIZE; }
