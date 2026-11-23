/* Intel e1000 driver - polling mode */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "../kernel/k.h"
#include "../kernel/arch/arch.h"
#include "e1000.h"

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}

/* ---- PCI ---- */
static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    uint32_t addr = 0x80000000 | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                    ((uint32_t)func << 8) | (off & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}
static void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t val) {
    uint32_t addr = 0x80000000 | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                    ((uint32_t)func << 8) | (off & 0xFC);
    outl(0xCF8, addr);
    outl(0xCFC, val);
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* ---- page-table MMIO mapping ----
 * Limine's HHDM only covers RAM (top ~1GB). PCI MMIO BARs live above
 * that (e.g. 0xf0000000), so we must add our own PTEs. We map the
 * device's 2MB-aligned physical range at hhdm+phys using scratch
 * PDPT/PD pages from .bss. */
#define PT_P   0x001ULL
#define PT_RW  0x002ULL
#define PT_PS  0x080ULL               /* PD entry: 2MB page */
#define PT_MASK 0x000FFFFFFFFFF000ULL

static uint8_t mmu_scratch[2][4096] __attribute__((aligned(4096)));

static int mmio_map_2mb(uint64_t phys) {
    uint64_t hhdm = limine_hhdm_offset();
    uint64_t cr3;
    __asm__ volatile("mov %%cr3,%0" : "=r"(cr3));
    cr3 &= PT_MASK;
    uint64_t va = (hhdm + phys) & ~0x1FFFFFULL;
    uint64_t pa = phys & ~0x1FFFFFULL;

    uint64_t *pml4 = (uint64_t *)(uintptr_t)(hhdm + cr3);
    int i4 = (int)((va >> 39) & 0x1FF);
    int i3 = (int)((va >> 30) & 0x1FF);
    int i2 = (int)((va >> 21) & 0x1FF);

    uint64_t e = pml4[i4];
    uint64_t *pdpt;
    if (!(e & PT_P)) {
        pdpt = (uint64_t *)mmu_scratch[0];
        memset(pdpt, 0, 4096);
        pml4[i4] = kvirt_to_phys(mmu_scratch[0]) | PT_P | PT_RW;
    } else {
        pdpt = (uint64_t *)(uintptr_t)(hhdm + (e & PT_MASK));
    }
    e = pdpt[i3];
    uint64_t *pd;
    if (!(e & PT_P)) {
        pd = (uint64_t *)mmu_scratch[1];
        memset(pd, 0, 4096);
        pdpt[i3] = kvirt_to_phys(mmu_scratch[1]) | PT_P | PT_RW;
    } else if (e & PT_PS) {
        return -1; /* 1GB page already covers this VA - can't split */
    } else {
        pd = (uint64_t *)(uintptr_t)(hhdm + (e & PT_MASK));
    }
    if (!(pd[i2] & PT_P)) {
        pd[i2] = pa | PT_P | PT_RW | PT_PS;
        __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
    }
    return 0;
}

/* ---- e1000 regs ---- */
#define E1000_CTRL      0x0000
#define E1000_STATUS    0x0008
#define E1000_EECD      0x0010
#define E1000_EERD      0x0014
#define E1000_CTRL_EXT  0x0018
#define E1000_FCAL      0x0028
#define E1000_FCAH      0x002C
#define E1000_FCT      0x0010
#define E1000_VET       0x0038
#define E1000_ICR       0x00C0
#define E1000_ITR       0x00C4
#define E1000_IMS       0x00D0
#define E1000_RCTL      0x0100
#define E1000_TCTL      0x0400
#define E1000_TIPG      0x0410
#define E1000_RDBAL     0x2800
#define E1000_RDBAH     0x2804
#define E1000_RDLEN     0x2808
#define E1000_RDH       0x2810
#define E1000_RDT       0x2818
#define E1000_RDTR      0x2820
#define E1000_RXDCTL    0x2828
#define E1000_TDBAL     0x3800
#define E1000_TDBAH     0x3804
#define E1000_TDLEN     0x3808
#define E1000_TDH       0x3810
#define E1000_TDT       0x3818
#define E1000_TCTL_EXT  0x03D0
#define E1000_RAL0      0x5400
#define E1000_RAH0      0x5404
#define E1000_MTA       0x5200
#define E1000_VFTA      0x5600
#define E1000_MDIC      0x0020

#define RX_DESC_COUNT 32
#define TX_DESC_COUNT 8
#define RX_BUF_SIZE   2048
#define TX_BUF_SIZE   2048

#define CTRL_RST       (1 << 26)
#define CTRL_SLU       (1 << 6)
#define CTRL_ASDE      (1 << 5)
#define CTRL_LU        (1 << 3)
#define CTRL_FRCSPD    (1 << 11)
#define CTRL_FRCDPX_MSB (1 << 12)
#define CTRL_FRCDPX_LSB (1 << 13)
#define CTRL_SECRC     (1 << 26)

#define STATUS_LU      (1 << 1)
#define STATUS_FD      (1 << 2)

#define RCTL_EN        (1 << 1)
#define RCTL_SBP       (1 << 2)
#define RCTL_UPE       (1 << 3)
#define RCTL_MPE       (1 << 4)
#define RCTL_LPE       (1 << 5)
#define RCTL_LBM_NONE  (0 << 6)
#define RCTL_RDMTS_HALF (0 << 8)
#define RCTL_BAM       (1 << 15)
#define RCTL_BSIZE_2048 (0 << 16)
#define RCTL_SECRC     (1 << 26)
#define RCTL_BSEX       (1 << 25) /* big-endian desc - keep 0 (little) */

#define TCTL_EN        (1 << 1)
#define TCTL_PSP       (1 << 3)
#define TCTL_CT        (0xF << 4)
#define TCTL_COLD      (0x40 << 12)

#define RXD_STAT_DD    (1 << 0)
#define RXD_STAT_EOP   (1 << 1)
#define RXD_STAT_IXSM  (1 << 2)
#define RXD_STAT_VP    (1 << 3)
#define RXD_STAT_UDP   (1 << 4)
#define RXD_STAT_TCP   (1 << 5)
#define RXD_STAT_IPCS  (1 << 6)
#define RXD_STAT_TCPCS (1 << 7)

#define TXD_CMD_EOP    (1 << 0)
#define TXD_CMD_IFCS   (1 << 1)
#define TXD_CMD_RS     (1 << 3)   /* report status: HW writes DD back */

static volatile uint32_t *mmio;
static uint8_t g_mac[ETH_ALEN];
static bool g_present;
static e1000_rx_cb g_rx_cb;

/* descriptor rings (physically contiguous via kmalloc page alignment:
   kmalloc returns page-multiple-aligned? Our heap returns page+16 offset,
   NOT page aligned! DMA needs page-aligned physical addresses for rings?
   Actually e1000 requires 128-byte alignment for descriptor rings.
   kmalloc returns base+16 where base is 4096-aligned -> +16 keeps 16-align
   but not 128. Fix: allocate rings with alignment - use a dedicated
   aligned static array in .bss. */
static uint8_t rx_ring_mem[RX_DESC_COUNT * 16] __attribute__((aligned(128)));
static uint8_t tx_ring_mem[TX_DESC_COUNT * 16] __attribute__((aligned(128)));
static uint8_t rx_bufs[RX_DESC_COUNT][RX_BUF_SIZE] __attribute__((aligned(128)));
static uint8_t tx_bufs[TX_DESC_COUNT][TX_BUF_SIZE] __attribute__((aligned(128)));

static uint16_t rx_tail; /* next descriptor to hand to hw */
static uint16_t tx_tail, tx_head;
static uint32_t g_rx_frames, g_tx_sent, g_tx_done;

static inline void mmio_write(uint32_t off, uint32_t val) { mmio[off / 4] = val; }
static inline uint32_t mmio_read(uint32_t off) { return mmio[off / 4]; }

/* MDIO/PHY access via MDIC (82540EM internal PHY is address 1) */
#define MDIC_DATA   0x0000FFFFu
#define MDIC_REG    (16u)
#define MDIC_PHY    (21u)
#define MDIC_RD     (1u << 27)
#define MDIC_WR     (1u << 26)
#define MDIC_READY  (1u << 28)
#define MDIC_ERR    (1u << 30)

static int phy_read(uint8_t phy, uint8_t reg, uint16_t *out) {
    mmio_write(E1000_MDIC, (1u << 31) | (phy << MDIC_PHY) |
               ((uint32_t)reg << MDIC_REG) | MDIC_RD);
    for (int i = 0; i < 100000; i++) {
        uint32_t v = mmio_read(E1000_MDIC);
        if (v & MDIC_ERR) return -1;
        if (v & MDIC_READY) { *out = v & MDIC_DATA; return 0; }
    }
    return -1;
}

static int phy_write(uint8_t phy, uint8_t reg, uint16_t val) {
    mmio_write(E1000_MDIC, (1u << 31) | (phy << MDIC_PHY) |
               ((uint32_t)reg << MDIC_REG) | MDIC_WR | (val & MDIC_DATA));
    for (int i = 0; i < 100000; i++) {
        uint32_t v = mmio_read(E1000_MDIC);
        if (v & MDIC_ERR) return -1;
        if (v & MDIC_READY) return 0;
    }
    return -1;
}

/* rx descriptor (little-endian, 16 bytes) */
typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint16_t csum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} rx_desc_t;

/* tx descriptor (little-endian, 16 bytes) */
typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} tx_desc_t;

static rx_desc_t *rx_ring = (rx_desc_t *)rx_ring_mem;
static tx_desc_t *tx_ring = (tx_desc_t *)tx_ring_mem;

static uint64_t virt_to_phys_dma(const void *v) {
    /* use kvirt_to_phys from k.h */
    extern uint64_t kvirt_to_phys(void *v);
    return kvirt_to_phys((void *)v);
}

static int eeprom_read(uint16_t addr, uint16_t *data) {
    /* EERD: START bit31, ADDR bits 8-? , DONE bit16? Common impl:
       EERD = (1<<31) | (addr<<8); poll bit16? Actually DONE is bit 16? 
       Per Intel: EERD register: bit0-15 data, bit16 DONE, bits 20-28 addr? 
       We'll use the widely used form: write addr<<8 | start, poll (EERD>>16)&1 */
    uint32_t r = (1u << 31) | ((uint32_t)addr << 8);
    mmio_write(E1000_EERD, r);
    for (int i = 0; i < 100000; i++) {
        uint32_t v = mmio_read(E1000_EERD);
        if (v & 0x00010000) { /* DONE */
            *data = v & 0xFFFF;
            return 0;
        }
    }
    return -1;
}

static void e1000_get_mac(void) {
    /* try EEPROM first */
    uint16_t eep[3];
    if (eeprom_read(0, &eep[0]) == 0 && eeprom_read(1, &eep[1]) == 0 &&
        eeprom_read(2, &eep[2]) == 0) {
        /* EEPROM word0: bytes 0-1 = MAC[0..1]? Layout: word0 bits0-15 = mac0,mac1;
           word1: mac2,mac3; word2: mac4,mac5 + other */
        g_mac[0] = eep[0] & 0xFF;
        g_mac[1] = eep[0] >> 8;
        g_mac[2] = eep[1] & 0xFF;
        g_mac[3] = eep[1] >> 8;
        g_mac[4] = eep[2] & 0xFF;
        g_mac[5] = eep[2] >> 8;
        kprintf("e1000 MAC (eeprom): %02x:%02x:%02x:%02x:%02x:%02x",
                g_mac[0],g_mac[1],g_mac[2],g_mac[3],g_mac[4],g_mac[5]);
        return;
    }
    /* fallback: RAL/RAH */
    uint32_t ral = mmio_read(E1000_RAL0);
    uint32_t rah = mmio_read(E1000_RAH0);
    g_mac[0] = ral & 0xFF;
    g_mac[1] = (ral >> 8) & 0xFF;
    g_mac[2] = (ral >> 16) & 0xFF;
    g_mac[3] = (ral >> 24) & 0xFF;
    g_mac[4] = rah & 0xFF;
    g_mac[5] = (rah >> 8) & 0xFF;
    kprintf("e1000 MAC (ral/rah): %02x:%02x:%02x:%02x:%02x:%02x",
            g_mac[0],g_mac[1],g_mac[2],g_mac[3],g_mac[4],g_mac[5]);
}

int e1000_init(void) {
    /* PCI scan for Intel ethernet */
    uint8_t bus = 0, dev = 0, func = 0;
    uint8_t fbus = 0, fdev = 0, ffunc = 0;
    bool found = false;
    uint16_t vendor = 0, device = 0;
    for (bus = 0; bus < 8 && !found; bus++) {
        for (dev = 0; dev < 32 && !found; dev++) {
            for (func = 0; func < 8; func++) {
                uint32_t v = pci_read32(bus, dev, func, 0x00);
                vendor = v & 0xFFFF;
                device = (v >> 16) & 0xFFFF;
                if (vendor == 0x8086 &&
                    (device == 0x100E || device == 0x100F ||
                     device == 0x10D3 || device == 0x1502 || device == 0x1503)) {
                    found = true;
                    fbus = bus; fdev = dev; ffunc = func;
                    break;
                }
                if (func == 0 && (v & 0xFFFF) == 0xFFFF) break; /* empty dev */
            }
        }
    }
    if (!found) {
        kprint("e1000: device not found");
        return -1;
    }
    bus = fbus; dev = fdev; func = ffunc;
    kprintf("e1000: found dev %02x:%02x.%x id=%04x", bus, dev, func, device);
    for (int o = 0; o < 0x40; o += 4)
        kprintf("cfg[%02x]=%08x ", o, pci_read32(bus, dev, func, o));
    kprint("\n");

    /* BAR0 = MMIO */
    uint32_t bar0 = pci_read32(bus, dev, func, 0x10);
    uint32_t bar1 = pci_read32(bus, dev, func, 0x14);
    kprintf("e1000: bar0=%x bar1=%x", bar0, bar1);
    /* bit0=0 -> memory BAR, bit0=1 -> IO BAR (we need memory).
     * all-ones means the device didn't respond. */
    if ((bar0 & 1) || (bar0 & 0xFFFFFFF0u) == 0xFFFFFFF0u) {
        kprint("e1000: BAR0 not usable MMIO");
        return -1;
    }
    /* BAR could be 64-bit (bar0 low, bar1 high) */
    uint64_t base = bar0 & ~0xFULL;
    if (bar0 & 0x4) base |= ((uint64_t)(bar1 & ~0xFULL)) << 32;
    /* BAR is a physical MMIO address. If it sits above the HHDM RAM
     * window we must add a page-table mapping for it ourselves. */
    extern uint64_t limine_hhdm_offset(void);
    if (mmio_map_2mb(base) != 0) {
        kprint("e1000: mmio map failed");
        return -1;
    }
    mmio = (volatile uint32_t *)(uintptr_t)(limine_hhdm_offset() + base);
    kprintf("e1000: MMIO base=%p", mmio);

    /* enable bus mastering + memory space */
    uint32_t cmd = pci_read32(bus, dev, func, 0x04);
    pci_write32(bus, dev, func, 0x04, cmd | 0x6); /* IO+MEM+BM */

    /* reset */
    mmio_write(E1000_CTRL, mmio_read(E1000_CTRL) | CTRL_RST);
    for (int i = 0; i < 1000000; i++) {
        if (!(mmio_read(E1000_CTRL) & CTRL_RST)) break;
    }
    pit_delay_ms(10);
    /* Set Link Up + Auto-Speed Detection. After reset the 82540EM holds the
     * PHY link down until SLU is set - without it STATUS.LU never asserts
     * and no frames flow (the NAT failure: link=0, rx=0). */
    mmio_write(E1000_CTRL, mmio_read(E1000_CTRL) | CTRL_SLU | CTRL_ASDE);

    /* get MAC */
    e1000_get_mac();

    /* clear multicast table */
    kprint("e1000: mta\n");
    for (int i = 0; i < 128; i++) mmio_write(E1000_MTA + i * 4, 0);

    /* setup RX ring */
    kprint("e1000: rxring\n");
    memset(rx_ring_mem, 0, sizeof(rx_ring_mem));
    for (int i = 0; i < RX_DESC_COUNT; i++) {
        rx_ring[i].addr = virt_to_phys_dma(rx_bufs[i]);
        rx_ring[i].status = 0;
    }
    mmio_write(E1000_RDBAL, (uint32_t)virt_to_phys_dma(rx_ring_mem));
    mmio_write(E1000_RDBAH, (uint32_t)(virt_to_phys_dma(rx_ring_mem) >> 32));
    mmio_write(E1000_RDLEN, RX_DESC_COUNT * 16);
    mmio_write(E1000_RDH, 0);
    rx_tail = 0;
    /* RDT points to last descriptor hw may write */
    mmio_write(E1000_RDT, RX_DESC_COUNT - 1);

    /* setup TX ring */
    kprint("e1000: txring\n");
    memset(tx_ring_mem, 0, sizeof(tx_ring_mem));
    for (int i = 0; i < TX_DESC_COUNT; i++) {
        tx_ring[i].addr = virt_to_phys_dma(tx_bufs[i]);
        tx_ring[i].status = 0;
    }
    mmio_write(E1000_TDBAL, (uint32_t)virt_to_phys_dma(tx_ring_mem));
    mmio_write(E1000_TDBAH, (uint32_t)(virt_to_phys_dma(tx_ring_mem) >> 32));
    mmio_write(E1000_TDLEN, TX_DESC_COUNT * 16);
    mmio_write(E1000_TDH, 0);
    mmio_write(E1000_TDT, 0);
    tx_tail = 0; tx_head = 0;
    kprint("e1000: filters\n");

    /* filter: unicast+multicast+broadcast, no VLAN strip needed */
    mmio_write(E1000_RCTL, RCTL_EN | RCTL_SBP | RCTL_UPE | RCTL_MPE |
               RCTL_BAM | RCTL_BSIZE_2048 | RCTL_SECRC);
    /* TX: enable, PSP, CT=15, COLD=64 */
    mmio_write(E1000_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);
    mmio_write(E1000_TIPG, 0x0060200A);
    mmio_write(E1000_TCTL_EXT, 0x00000040); /* TCP timestamps? keep 0 safer */
    mmio_write(E1000_TCTL_EXT, 0);

    /* set MAC address in RAL0/RAH0 */
    uint32_t ral = g_mac[0] | (g_mac[1] << 8) | (g_mac[2] << 16) | ((uint32_t)g_mac[3] << 24);
    uint32_t rah = g_mac[4] | (g_mac[5] << 8);
    mmio_write(E1000_RAL0, ral);
    mmio_write(E1000_RAH0, rah);
    mmio_write(E1000_RAH0, rah | (1u << 31)); /* AV bit */

    /* link up wait - also probe the internal PHY via MDIO so we can see
     * whether the PHY itself reports a link (BMSR bit2) */
    kprint("e1000: linkwait\n");
    uint16_t bmcr = 0, bmsr = 0;
    for (int i = 0; i < 300; i++) {
        if (mmio_read(E1000_STATUS) & STATUS_LU) break;
        pit_delay_ms(10);
    }
    phy_read(1, 0, &bmcr);
    phy_read(1, 1, &bmsr);
    kprintf("e1000: status=%x link=%d bmcr=%x bmsr=%x\n",
            mmio_read(E1000_STATUS),
            (mmio_read(E1000_STATUS) & STATUS_LU) ? 1 : 0, bmcr, bmsr);
    kprint("e1000: done-init\n");

    g_present = true;
    kprint("e1000: init complete");
    return 0;
}

const uint8_t *e1000_mac(void) { return g_mac; }
bool e1000_link_up(void) {
    return g_present && (mmio_read(E1000_STATUS) & STATUS_LU);
}

void e1000_set_rx_callback(e1000_rx_cb cb) { g_rx_cb = cb; }

void e1000_poll(void) {
    if (!g_present) return;
    while (true) {
        uint16_t idx = rx_tail;
        rx_desc_t *d = &rx_ring[idx];
        if (!(d->status & RXD_STAT_DD)) break;
        uint16_t len = d->length;
        if (len > RX_BUF_SIZE) len = RX_BUF_SIZE;
        if (g_rx_cb && len >= ETH_HEADER_LEN)
            g_rx_cb(rx_bufs[idx], len);
        g_rx_frames++;
        /* hand buffer back to hw: RDT = LAST descriptor hw may write,
         * i.e. one BEFORE the next one we'll check. Writing rx_tail
         * (next index) was off-by-one: once the hw write cursor caught
         * rdt==rdh the NIC thought the ring was full and RX died after
         * a few packets (DHCP worked, then every ARP timed out). */
        d->status = 0;
        rx_tail = (idx + 1) % RX_DESC_COUNT;
        mmio_write(E1000_RDT, (rx_tail + RX_DESC_COUNT - 1) % RX_DESC_COUNT);
    }
}

/* reap completed TX descriptors: advance tx_head past any with DD set */
static void tx_reap(void) {
    while (tx_head != tx_tail) {
        if (!(tx_ring[tx_head].status & 0x01)) break; /* DD not set */
        tx_ring[tx_head].status = 0;
        tx_head = (tx_head + 1) % TX_DESC_COUNT;
        g_tx_done++;
    }
}

int e1000_send(const uint8_t *frame, uint16_t len) {
    if (!g_present) return -1;
    if (len > TX_BUF_SIZE) return -1;
    tx_reap();
    uint16_t next = (tx_tail + 1) % TX_DESC_COUNT;
    if (next == tx_head) return -1; /* ring full */
    tx_desc_t *d = &tx_ring[tx_tail];
    memcpy(tx_bufs[tx_tail], frame, len);
    if (len < 60) {
        memset(tx_bufs[tx_tail] + len, 0, 60 - len);
        len = 60;
    }
    d->length = len;
    d->cmd = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
    d->status = 0;
    uint16_t ntail = (tx_tail + 1) % TX_DESC_COUNT;
    mmio_write(E1000_TDT, ntail);
    tx_tail = ntail;
    g_tx_sent++;
    /* brief poll for completion so tx_head advances quickly */
    for (int i = 0; i < 20000; i++) {
        tx_reap();
        if (tx_head == tx_tail) break;
    }
    return 0;
}

uint32_t e1000_rx_frames(void) { return g_rx_frames; }

void e1000_diag(uint32_t *rx, uint32_t *txs, uint32_t *txd, uint32_t *status) {
    if (rx) *rx = g_rx_frames;
    if (txs) *txs = g_tx_sent;
    if (txd) *txd = g_tx_done;
    if (status) *status = mmio_read(E1000_STATUS);
    kprintf("e1000 regs: rctl=%x icr=%x rdh=%x rdt=%x tdt=%x tdh=%x ctrl=%x\n",
            mmio_read(E1000_RCTL), mmio_read(E1000_ICR),
            mmio_read(E1000_RDH), mmio_read(E1000_RDT),
            mmio_read(E1000_TDT), mmio_read(E1000_TDH),
            mmio_read(E1000_CTRL));
}
