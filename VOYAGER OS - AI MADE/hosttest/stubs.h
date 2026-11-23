#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
void *kmalloc(size_t s){ return malloc(s); }
void kfree(void *p){ free(p); }
void *kzalloc(size_t s){ return calloc(1,s); }
void kprintf(const char*f,...){ va_list a; va_start(a,f); vprintf(f,a); va_end(a); }
void kprint(const char*s){ fputs(s,stdout); }
void kprint_len(const char*s,size_t n){ fwrite(s,1,n,stdout); }
void kprint_hex(uint64_t v){ printf("%llx",(unsigned long long)v); }
void kprint_dec(int64_t v){ printf("%lld",(long long)v); }
int png_decode(const uint8_t*,size_t,uint8_t**,uint32_t*,uint32_t*);
void *krealloc(void *p, size_t s){ return realloc(p, s); }
