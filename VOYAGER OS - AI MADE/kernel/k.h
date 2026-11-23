/* Minimal libc-ish helpers + serial debug output */
#ifndef KC_H
#define KC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

void kprintf(const char *fmt, ...);
void kprint_hex(uint64_t v);
void kprint_dec(int64_t v);
void panic(const char *msg);
void kprint(const char *s);
void kprint_len(const char *s, size_t n);

struct limine_framebuffer;
struct limine_framebuffer *limine_get_framebuffer(void);
uint64_t limine_hhdm_offset(void);
struct limine_executable_address_response;
struct limine_executable_address_response *limine_exec_addr_response(void);

void uart_init(void);
int  uart_try_getc(void);
void uart_putchar(char c);

void *kmalloc(size_t size);
void kfree(void *ptr);
void *kzalloc(size_t size);
void *krealloc(void *ptr, size_t size);
size_t kmalloc_used(void);
size_t kmalloc_total(void);
void heap_selftest(void);

/* physical <-> virtual for kernel image (physically contiguous) */
uint64_t kvirt_to_phys(void *v);
void *kphys_to_virt(uint64_t p);


#ifdef __cplusplus
}
#endif

#endif
