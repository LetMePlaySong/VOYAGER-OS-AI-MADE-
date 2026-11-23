/* IDT, PIC, PIT, GDT init + C-side IRQ dispatch table */
#include <stdint.h>
#include <stddef.h>
#include "arch.h"
#include "../k.h"

extern void gdt_flush(void);

/* IDT entry: 16 bytes */
typedef struct __attribute__((packed)) {
    uint16_t off_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t off_mid;
    uint32_t off_high;
    uint32_t zero;
} idt_entry_t;

static idt_entry_t idt[256] __attribute__((aligned(16)));

static struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} idt_ptr;

#define IDT_TYPE_INT64 0x8E  /* present, DPL0, 64-bit interrupt gate */

static void idt_set_raw(uint8_t vector, uint64_t handler, uint8_t type_attr) {
    idt_entry_t *e = &idt[vector];
    e->off_low   = handler & 0xFFFF;
    e->selector  = 0x28; /* Limine kernel code64 (index 5 in its GDT) */
    e->ist       = 0;
    e->type_attr = type_attr;
    e->off_mid   = (handler >> 16) & 0xFFFF;
    e->off_high  = (handler >> 32) & 0xFFFFFFFF;
    e->zero      = 0;
}

void idt_set_gate(uint8_t vector, uint64_t handler, uint8_t type_attr) {
    idt_set_raw(vector, handler, type_attr);
}

static void idt_flush(void) {
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base  = (uint64_t)&idt;
    __asm__ volatile("lidt %0" :: "m"(idt_ptr));
}

/* ISR symbols generated in isr.S */
#define ISR_DECL(n) extern void isr_##n(void);
ISR_DECL(0)  ISR_DECL(1)  ISR_DECL(2)  ISR_DECL(3)  ISR_DECL(4)
ISR_DECL(5)  ISR_DECL(6)  ISR_DECL(7)  ISR_DECL(8)  ISR_DECL(9)
ISR_DECL(10) ISR_DECL(11) ISR_DECL(12) ISR_DECL(13) ISR_DECL(14)
ISR_DECL(15) ISR_DECL(16) ISR_DECL(17) ISR_DECL(18) ISR_DECL(19)
ISR_DECL(20) ISR_DECL(21) ISR_DECL(22) ISR_DECL(23) ISR_DECL(24)
ISR_DECL(25) ISR_DECL(26) ISR_DECL(27) ISR_DECL(28) ISR_DECL(29)
ISR_DECL(30) ISR_DECL(31)
ISR_DECL(32) ISR_DECL(33) ISR_DECL(34) ISR_DECL(35) ISR_DECL(36)
ISR_DECL(37) ISR_DECL(38) ISR_DECL(39) ISR_DECL(40) ISR_DECL(41)
ISR_DECL(42) ISR_DECL(43) ISR_DECL(44) ISR_DECL(45) ISR_DECL(46)
ISR_DECL(47)

static void *isr_table[256];

void (*irq_handlers[256])(struct regs *) = {0};

void irq_handlers_install(void) {
    /* exceptions */
    for (int i = 0; i < 32; i++)
        idt_set_raw(i, (uint64_t)isr_table[i], IDT_TYPE_INT64);
    /* IRQs 32-47 */
    for (int i = 32; i < 48; i++)
        idt_set_raw(i, (uint64_t)isr_table[i], IDT_TYPE_INT64);
    idt_flush();
}

/* fill isr_table early - called from arch_init before idt_flush */
static void idt_populate_table(void) {
    isr_table[0]=(void*)isr_0;  isr_table[1]=(void*)isr_1;  isr_table[2]=(void*)isr_2;
    isr_table[3]=(void*)isr_3;  isr_table[4]=(void*)isr_4;  isr_table[5]=(void*)isr_5;
    isr_table[6]=(void*)isr_6;  isr_table[7]=(void*)isr_7;  isr_table[8]=(void*)isr_8;
    isr_table[9]=(void*)isr_9;  isr_table[10]=(void*)isr_10;isr_table[11]=(void*)isr_11;
    isr_table[12]=(void*)isr_12;isr_table[13]=(void*)isr_13;isr_table[14]=(void*)isr_14;
    isr_table[15]=(void*)isr_15;isr_table[16]=(void*)isr_16;isr_table[17]=(void*)isr_17;
    isr_table[18]=(void*)isr_18;isr_table[19]=(void*)isr_19;isr_table[20]=(void*)isr_20;
    isr_table[21]=(void*)isr_21;isr_table[22]=(void*)isr_22;isr_table[23]=(void*)isr_23;
    isr_table[24]=(void*)isr_24;isr_table[25]=(void*)isr_25;isr_table[26]=(void*)isr_26;
    isr_table[27]=(void*)isr_27;isr_table[28]=(void*)isr_28;isr_table[29]=(void*)isr_29;
    isr_table[30]=(void*)isr_30;isr_table[31]=(void*)isr_31;
    isr_table[32]=(void*)isr_32;isr_table[33]=(void*)isr_33;isr_table[34]=(void*)isr_34;
    isr_table[35]=(void*)isr_35;isr_table[36]=(void*)isr_36;isr_table[37]=(void*)isr_37;
    isr_table[38]=(void*)isr_38;isr_table[39]=(void*)isr_39;isr_table[40]=(void*)isr_40;
    isr_table[41]=(void*)isr_41;isr_table[42]=(void*)isr_42;isr_table[43]=(void*)isr_43;
    isr_table[44]=(void*)isr_44;isr_table[45]=(void*)isr_45;isr_table[46]=(void*)isr_46;
    isr_table[47]=(void*)isr_47;
}

void isr_unhandled(struct regs *r) {
    /* for error-code exceptions the frame is shifted: rip field reads
     * the error code, cs field reads the real rip. print both. */
    kprintf("Unhandled vec %u err/rip=%p cs/realrip=%p rflags=%p",
            (unsigned)r->vector, (void*)r->rip, (void*)r->cs, (void*)r->rflags);
    kprintf("  rdi=%p rsi=%p rdx=%p rcx=%p rax=%p",
            (void*)r->rdi, (void*)r->rsi, (void*)r->rdx, (void*)r->rcx, (void*)r->rax);
    if (r->vector == 14) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        kprintf("  cr2=%p", (void*)cr2);
    }
    /* dump the interrupted stack so we can reconstruct the call chain -
     * a wild rip with sane-looking regs usually means a smashed return
     * address, and the frames above the exception still hold the callers.
     * same-CPL exceptions don't save rsp, but it equals the qword right
     * above the saved rflags slot. */
    {
        uint64_t *sp = (uint64_t *)((char *)&r->rflags + 16);
        kprintf("  isp=%p", (void*)sp);
        for (int i = 0; i < 96; i++)
            kprintf("  [%d]=%p", i, (void*)sp[i]);
        kprintf("\n");
    }
    panic("CPU exception");
}

/* ---------------- PIC ---------------- */

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

void pic_remap(void) {
    uint8_t mask1 = inb(0x21), mask2 = inb(0xA1);
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
    outb(0x21, mask1); outb(0xA1, mask2);
    /* unmask IRQ0(timer) IRQ1(kbd) IRQ2(cascade->slave) on master,
     * and IRQ12(mouse=slave irq4) on slave. IRQ2 MUST be unmasked or no
     * slave interrupt (incl. the mouse) ever reaches the CPU. */
    outb(0x21, ~(1 << 0 | 1 << 1 | 1 << 2));
    outb(0xA1, ~(1 << 4));
}

void pic_eoi(uint8_t irq) {
    if (irq >= 8) outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

/* ---------------- PIT ---------------- */

static volatile uint64_t pit_tick_count;
static uint32_t pit_freq = 1000;

void pit_init(uint32_t hz) {
    pit_freq = hz;
    uint16_t div = 1193182 / hz;
    outb(0x43, 0x36); /* channel 0, lobyte/hibyte, mode 3 */
    outb(0x40, div & 0xFF);
    outb(0x40, div >> 8);
    for (int i = 0; i < 32; i++)
        irq_handlers[i] = isr_unhandled;
    irq_handlers[32] = irq0_timer;
    irq_handlers[33] = irq1_keyboard;
    irq_handlers[44] = irq12_mouse;
    for (int i = 34; i < 48; i++) {
        if (i != 44) irq_handlers[i] = irq_spurious;
    }
}

void irq_spurious(struct regs *r) {
    /* vectors 34-47 minus 44: acknowledge and ignore */
    pic_eoi((uint8_t)(r->vector - 32));
}

uint64_t pit_ticks(void) { return pit_tick_count; }
uint32_t pit_hz(void) { return pit_freq; }

static int irq0_announced;
void irq0_timer(struct regs *r) {
    (void)r;
    pit_tick_count++;
    if (!irq0_announced) { irq0_announced = 1; }
    pic_eoi(0);
}

void pit_delay_ms(uint64_t ms) {
    uint64_t target = pit_tick_count + (ms * pit_freq) / 1000;
    uint64_t guard = 0;
    while (pit_tick_count < target) {
        __asm__ volatile("hlt");
        /* safety: if the timer IRQ ever stalls, don't hard-hang the GUI */
        if (++guard > 4000000) break;
    }
}

/* ---------------- init ---------------- */

void arch_init(void) {
    kprint("arch: gdt...\n");
    gdt_flush();
    kprint("arch: idt table...\n");
    idt_populate_table();
    kprint("arch: pit...\n");
    pit_init(1000);          /* fills irq_handlers[] */
    kprint("arch: ps2...\n");
    ps2_init();
    kprint("arch: idt flush...\n");
    irq_handlers_install();  /* IDT goes live; handlers already set */
    kprint("arch: pic...\n");
    pic_remap();             /* unmask PIC last */
#ifndef NO_IRQS
    __asm__ volatile("sti");
#endif
    kprint("arch: done\n");
}
