/* PS/2 keyboard + mouse drivers.
 * Keyboard: scancode set 1, IRQ1 (vector 33).
 * Mouse:    3-byte packets, IRQ12 (vector 44).
 */
#include <stdint.h>
#include <stdbool.h>
#include "arch.h"
#include "../k.h"

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void io_wait(void) { inb(0x80); }

/* crude busy delay (~ms at any reasonable CPU speed); PIT ticks aren't
 * available yet during early init because the PIC isn't unmasked. */
static void busy_delay_ms(int ms) {
    for (int i = 0; i < ms; i++)
        for (volatile int j = 0; j < 200000; j++)
            __asm__ volatile("" ::: "memory");
}

/* wait until output buffer empty */
static void kbc_wait_write(void) {
    for (int i = 0; i < 100000; i++)
        if (!(inb(0x64) & 0x02)) return;
    io_wait();
}
/* KBC status bit5: the byte in the output buffer came from the AUX (mouse)
 * device, not the keyboard. This is the only reliable way to demux the
 * shared port-0x60 FIFO - without it IRQ1 steals mouse packet bytes and
 * types them as garbage characters. */
#define KBC_STAT_OBF   0x01
#define KBC_STAT_AUX   0x20
/* wait until input buffer full (data available) */
static bool kbc_wait_read(void) {
    for (int i = 0; i < 100000; i++)
        if (inb(0x64) & 0x01) return true;
    io_wait();
    return false;
}

static void kbc_send(uint8_t cmd) {
    kbc_wait_write();
    outb(0x64, cmd);
}

static void kbc_write(uint8_t data) {
    kbc_wait_write();
    outb(0x60, data);
}

/* write a command/data byte to the mouse (aux device) */
static void mouse_cmd(uint8_t cmd) {
    kbc_wait_write();
    outb(0x64, 0xD4);      /* write to aux output buffer */
    kbc_wait_write();
    outb(0x60, cmd);
}

/* read a byte the mouse sent back. The mouse ACKs every command with 0xFA
 * first, then (for queries like 0xF8 Read ID) sends the real data byte.
 * Skip ACKs so we return the actual payload, not the acknowledgement. */
static int mouse_read_response(void) {
    /* wait up to ~50ms, yielding between polls so the mouse has time to
     * answer. Skip ACK (0xFA) and resend (0xFE) to reach the real payload.
     * IRQs are masked for the whole poll: otherwise IRQ1/IRQ12 fire mid-
     * read and fifo_drain() feeds the ACK/ID bytes into the packet
     * assembler, so the response never arrives (mid=-1 -> no wheel). */
    uint64_t f;
    __asm__ volatile("pushfq; popq %0" : "=r"(f));
    __asm__ volatile("cli");
    int ret = -1;
    for (int t = 0; t < 200; t++) {
        busy_delay_ms(1);
        if (inb(0x64) & 0x01) {
            uint8_t b = inb(0x60);
            if (b == 0xFA || b == 0xFE) continue;
            ret = b;
            break;
        }
    }
    if (f & 0x200) __asm__ volatile("sti");
    return ret;
}

/* ---------------- keyboard ---------------- */

static bool kbd_pressed[256];
static char kbd_buf[256];
static volatile int kbd_head, kbd_tail;

/* US layout, scancode set 1 make codes. Index = scancode.
 * Designated initializers (Retro-95 style) - impossible to miscount
 * positional entries. Letters are case-folded at emit time. */
static const char kbd_normal[128] = {
    [0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',[0x07]='6',
    [0x08]='7',[0x09]='8',[0x0a]='9',[0x0b]='0',[0x0c]='-',[0x0d]='=',
    [0x10]='q',[0x11]='w',[0x12]='e',[0x13]='r',[0x14]='t',[0x15]='y',
    [0x16]='u',[0x17]='i',[0x18]='o',[0x19]='p',[0x1a]='[',[0x1b]=']',
    [0x1e]='a',[0x1f]='s',[0x20]='d',[0x21]='f',[0x22]='g',[0x23]='h',
    [0x24]='j',[0x25]='k',[0x26]='l',[0x27]=';',[0x28]='\'',[0x29]='`',
    [0x2b]='\\',[0x2c]='z',[0x2d]='x',[0x2e]='c',[0x2f]='v',[0x30]='b',
    [0x31]='n',[0x32]='m',[0x33]=',',[0x34]='.',[0x35]='/',[0x39]=' '
};
static const char kbd_shifted[128] = {
    [0x02]='!',[0x03]='@',[0x04]='#',[0x05]='$',[0x06]='%',[0x07]='^',
    [0x08]='&',[0x09]='*',[0x0a]='(',[0x0b]=')',[0x0c]='_',[0x0d]='+',
    [0x1a]='{',[0x1b]='}',[0x27]=':',[0x28]='"',[0x29]='~',[0x2b]='|',
    [0x33]='<',[0x34]='>',[0x35]='?'
};

static bool shift_down, caps_lock, caps_down;
static int e0_prefix;

static void kbd_emit(uint8_t c) {
    int n = (kbd_head + 1) & 0xFF;
    if (n != kbd_tail) {
        kbd_buf[kbd_head] = (char)c;
        kbd_head = n;
    }
}

/* Translate one keyboard scancode into queued output bytes.
 * Emits printable ASCII plus special codes >= 0x80:
 *   0x81-0x86 = KEY_UP/DOWN/LEFT/RIGHT/HOME/END (same values as gui.h)
 *   0x1B      = Esc (raw ESC char so dialogs can catch it directly) */
static void kbd_handle(uint8_t sc) {
    /* 0xF0 needs NO special handling (Retro-95 rule): it has the break
     * bit set, so it falls through to the break path and is ignored.
     * The byte after it is then processed normally - which is what
     * releases modifiers. (The old set2_break_next logic ATE that byte,
     * leaving Shift stuck on -> 'a' became 'A', '2' became '@'.) */
    if (sc == 0xE0) { e0_prefix = 1; return; }
    bool extended = e0_prefix;
    e0_prefix = 0;

    /* modifiers: make sets state, break clears it */
    if (sc == 0x2A || sc == 0x36) { shift_down = true;  kbd_pressed[sc] = true; return; }
    if (sc == 0xAA || sc == 0xB6) { shift_down = false; kbd_pressed[sc & 0x7F] = false; return; }
    if (sc == 0x3A) {              /* Caps Lock: toggle once per press    */
        if (!caps_down) caps_lock = !caps_lock;
        caps_down = true;
        kbd_pressed[sc] = true;
        return;
    }
    if (sc == 0xBA) { caps_down = false; kbd_pressed[0x3A] = false; return; }

    /* every other break code just releases the key. Also clears a
     * pending E0 so extended-key breaks (E0 F0 xx / E0 xx|0x80) can't
     * leak 'extended' state onto the next normal keypress. */
    if (sc & 0x80) { e0_prefix = 0; kbd_pressed[sc & 0x7F] = false; return; }
    kbd_pressed[sc] = true;

    /* extended (E0-prefixed) keys: arrows + nav cluster */
    if (extended) {
        switch (sc) {
            case 0x48: kbd_emit(0x81); return;   /* up    */
            case 0x50: kbd_emit(0x82); return;   /* down  */
            case 0x4B: kbd_emit(0x83); return;   /* left  */
            case 0x4D: kbd_emit(0x84); return;   /* right */
            case 0x47: kbd_emit(0x85); return;   /* home  */
            case 0x4F: kbd_emit(0x86); return;   /* end   */
            case 0x53: kbd_emit(0x87); return;   /* delete*/
            default:   return;
        }
    }

    /* named keys */
    switch (sc) {
        case 0x01: kbd_emit(0x1B); return;       /* Esc                  */
        case 0x0E: kbd_emit('\b'); return;       /* Backspace            */
        case 0x0F: kbd_emit('\t'); return;       /* Tab                  */
        case 0x1C: kbd_emit('\n'); return;       /* Enter                */
        case 0x53: kbd_emit(0x87); return;       /* Delete               */
        case 0x39: kbd_emit(' ');  return;       /* Space                */
        default: break;
    }
    if (!kbd_normal[sc]) return;

    char c = (shift_down && kbd_shifted[sc]) ? kbd_shifted[sc] : kbd_normal[sc];
    /* letters: Shift XOR CapsLock -> uppercase (Retro-95 rule) */
    if (c >= 'a' && c <= 'z' && (shift_down != caps_lock))
        c = (char)(c - 'a' + 'A');
    kbd_emit((uint8_t)c);
}

static void mouse_feed_byte(uint8_t b);
static void kbd_handle(uint8_t sc);
static void fifo_drain(void);

void irq1_keyboard(struct regs *r) {
    (void)r;
    fifo_drain();
    pic_eoi(1);
}

bool kbd_key_pressed(uint16_t scancode) {
    return scancode < 256 && kbd_pressed[scancode];
}

int kbd_getchar(void) {
    if (kbd_tail == kbd_head) return -1;
    uint8_t c = kbd_buf[kbd_tail];
    kbd_tail = (kbd_tail + 1) & 0xFF;
    return c;
}

bool kbd_has_char(void) { return kbd_tail != kbd_head; }
void kbd_clear(void) { kbd_tail = kbd_head; }

/* ---------------- mouse ---------------- */

static int32_t mouse_x = 400, mouse_y = 300;
static uint8_t mouse_buttons;
static uint8_t mouse_pkt[4];
static int mouse_idx;
static int mouse_wheel;
static int mouse_mode = 3; /* 3-byte or 4-byte (IntelliMouse) packets */

volatile uint32_t g_irq12_count;   /* diagnostics: how often mouse IRQ fires */

/* feed one byte from the KBC output FIFO into the packet assembler.
 * Port 0x60 is a shared FIFO: the IRQ handler and the polled reader both
 * drain it, so each byte is consumed exactly once regardless of source. */
static void mouse_feed_byte(uint8_t b) {
    int need = mouse_mode;
    if (mouse_idx == 0) {
        if (!(b & 0x08)) return;      /* resync: wait for packet start (bit3) */
        mouse_pkt[0] = b;
        mouse_idx = 1;
        return;
    }
    mouse_pkt[mouse_idx++] = b;
    if (mouse_idx == need) {
        uint8_t flags = mouse_pkt[0];
        int32_t dx = (int8_t)mouse_pkt[1];
        int32_t dy = (int8_t)mouse_pkt[2];
        mouse_x += dx;
        mouse_y -= dy;                /* mouse Y is inverted */
        mouse_buttons = flags & 0x07;
        if (need == 4) {
            /* byte3: low nibble = wheel (signed), high bits = buttons 4/5
             * - sign-extend just the nibble so side buttons don't scroll */
            int8_t w = (int8_t)((mouse_pkt[3] << 4) >> 4);
            mouse_wheel += w;
        }
        mouse_idx = 0;
    }
}

/* Single drain for the shared KBC output FIFO. Every byte is routed by
 * its own AUXDAT status bit exactly once. irq1_keyboard, irq12_mouse and
 * mouse_poll all funnel through here; mouse_poll wraps it in cli/sti so
 * a thread-context drain can never be preempted mid-byte by IRQ1 (that
 * race used to feed mouse dx/dy bytes to the keyboard -> wrong chars). */
static void fifo_drain(void) {
    for (int guard = 0; guard < 64; guard++) {
        uint8_t st = inb(0x64);
        if (!(st & KBC_STAT_OBF)) break;      /* output buffer empty */
        uint8_t b = inb(0x60);
        if (st & KBC_STAT_AUX) mouse_feed_byte(b);
        else kbd_handle(b);
    }
}

void irq12_mouse(struct regs *r) {
    (void)r;
    g_irq12_count++;
    fifo_drain();
    pic_eoi(12);
}

/* polled reader: used as a fallback so the mouse works even if the
 * IRQ12 line never reaches the CPU (some chipsets/VMs mask the cascade).
 * Atomic w.r.t. IRQ1: disables interrupts for the drain so the ISR
 * can't steal the byte this call already validated in the status port. */
void mouse_poll(void) {
    uint64_t f;
    __asm__ volatile("pushfq; popq %0" : "=r"(f));
    __asm__ volatile("cli");
    fifo_drain();
    if (f & 0x200) __asm__ volatile("sti");
}

int mouse_wheel_delta(void) {
    int d = mouse_wheel;
    mouse_wheel = 0;
    return d;
}

void mouse_get(int32_t *x, int32_t *y, uint8_t *buttons) {
    if (x) *x = mouse_x;
    if (y) *y = mouse_y;
    if (buttons) *buttons = mouse_buttons;
}

void mouse_set_pos(int32_t x, int32_t y) {
    mouse_x = x;
    mouse_y = y;
}

/* ---------------- init ---------------- */

/* drain every byte currently sitting in the KBC output buffer */
static void kbc_flush(void) {
    for (int f = 0; f < 512 && (inb(0x64) & 0x01); f++) inb(0x60);
}

void ps2_init(void) {
#if 1 /* PS2_FULL_INIT */
    kprint("ps2: kbc cmd...\n");
    kbc_send(0xAE);           /* enable keyboard */
    kbc_send(0xA8);           /* enable auxiliary (mouse) port */
    kprint("ps2: mouse cmd...\n");
    busy_delay_ms(20);
    kbc_flush();

    /* --- mouse: full reset then defaults (standard PS/2 init) --- */
    mouse_cmd(0xFF);          /* reset: mouse -> ACK + 0xAA self-test */
    busy_delay_ms(30);
    int rst = mouse_read_response();
    kprintf("ps2: mreset=%d\n", rst);
    kbc_flush();

    mouse_cmd(0xF6);          /* set defaults */
    busy_delay_ms(10);
    kbc_flush();

    /* IntelliMouse (wheel) enable: the sample-rate sequence 200/100/80 is
     * what actually switches wheel-capable mice into 4-byte packet mode.
     * Some emulated mice (VirtualBox PS/2) never answer the F8 ID query,
     * so don't rely on it - send the sequence and use 4-byte mode anyway;
     * VBox's mouse supports the wheel. */
    mouse_cmd(0xF3); mouse_cmd(200); busy_delay_ms(10);
    mouse_cmd(0xF3); mouse_cmd(100); busy_delay_ms(10);
    mouse_cmd(0xF3); mouse_cmd(80);  busy_delay_ms(10);
    mouse_cmd(0xF8);          /* read ID (best effort, may go unanswered) */
    busy_delay_ms(10);
    int mid = mouse_read_response();
    kprintf("ps2: mid=%d\n", mid);
    mouse_mode = 4;           /* 4-byte packets incl. wheel */
    kprintf("Mouse: IntelliMouse 4-byte mode (wheel enabled)\n");
    kbc_flush();
    mouse_cmd(0xF4);          /* enable streaming */
    busy_delay_ms(10);
    kbc_flush();
    /* keyboard: scancode set 1 is the controller default - do NOT send
     * 0xF0 0x01 (Retro-95 sends nothing). On VirtualBox that command
     * puts the keyboard emulation in a broken half-state: breaks stay
     * set-1 but makes come out permuted -> 'a' produced '2', 'e' hit
     * the Shift code, etc. */
    kprint("ps2: pic unmask...\n");

    /* unmask IRQ12 on slave PIC (IRQ2) and IRQ1 on master */
    outb(0xA1, inb(0xA1) & ~(1 << 2));
    outb(0x21, inb(0x21) & ~(1 << 1));
#endif
}
