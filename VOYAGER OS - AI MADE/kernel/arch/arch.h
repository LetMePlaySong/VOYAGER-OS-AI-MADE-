/* Interrupt descriptor table + ISR stubs */
#ifndef ARCH_H
#define ARCH_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Matches the raw stack frame built by isr.S (ring0 interrupts only,
 * no user mode). For exceptions that push an error code, `rip` here
 * actually reads the error code - we don't care since we panic. */
struct regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t vector;
    uint64_t rip, cs, rflags;
};

void arch_init(void);
void idt_set_gate(uint8_t vector, uint64_t handler, uint8_t type_attr);
void pic_remap(void);
void pic_eoi(uint8_t irq);
void pit_init(uint32_t hz);
uint64_t pit_ticks(void);
uint32_t pit_hz(void);
void pit_delay_ms(uint64_t ms);

/* input */
void ps2_init(void);
bool kbd_key_pressed(uint16_t scancode);
int  kbd_getchar(void);            /* returns ASCII or -1 */
bool kbd_has_char(void);
void kbd_clear(void);

/* mouse: position in pixels, buttons bitmask */
void mouse_get(int32_t *x, int32_t *y, uint8_t *buttons);
void mouse_set_pos(int32_t x, int32_t y);
int  mouse_wheel_delta(void);
extern volatile uint32_t g_irq12_count;
void mouse_poll(void);   /* drain PS/2 mouse FIFO without relying on IRQ12 */

/* IRQ/exception handlers implemented in C */
void irq_handlers_install(void);
void irq_spurious(struct regs *r);
void irq0_timer(struct regs *r);
void irq1_keyboard(struct regs *r);
void irq12_mouse(struct regs *r);
void isr_unhandled(struct regs *r);


#ifdef __cplusplus
}
#endif

#endif
