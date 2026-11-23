/* Address translation helpers (kernel image is physically contiguous) */
#define LIMINE_API_REVISION 2
#include <stdint.h>
#include "k.h"
#include "limine.h"

uint64_t kvirt_to_phys(void *v) {
    uint64_t va = (uint64_t)v;
    struct limine_executable_address_response *r = limine_exec_addr_response();
    if (r == NULL)
        return va; /* fallback: assume identity */
    uint64_t vb = r->virtual_base;
    uint64_t pb = r->physical_base;
    if (va >= vb)
        return va - vb + pb;
    /* inside HHDM or low identity map */
    return va - limine_hhdm_offset();
}

void *kphys_to_virt(uint64_t p) {
    uint64_t hhdm = limine_hhdm_offset();
    return (void *)(hhdm + p);
}
