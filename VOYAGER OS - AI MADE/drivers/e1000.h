/* Intel e1000 (82540EM and friends) driver - polling mode, no interrupts.
 * Works with VirtualBox PRO/1000 MT Desktop and QEMU e1000.
 */
#ifndef E1000_H
#define E1000_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ETH_ALEN 6
#define ETH_HEADER_LEN 14
#define ETH_MTU 1500
#define ETH_MAX_FRAME (ETH_MTU + ETH_HEADER_LEN + 4) /* +FCS */

typedef struct {
    uint8_t mac[ETH_ALEN];
    uint32_t mmio;          /* BAR0 base (virtual) */
    bool present;
} e1000_dev;

int  e1000_init(void);                    /* returns 0 ok */
void e1000_poll(void);                    /* pump RX ring */
typedef void (*e1000_rx_cb)(const uint8_t *frame, uint16_t len);
void e1000_set_rx_callback(e1000_rx_cb cb);
int  e1000_send(const uint8_t *frame, uint16_t len); /* returns 0 ok */
const uint8_t *e1000_mac(void);
bool e1000_link_up(void);
void e1000_diag(uint32_t *rx, uint32_t *txs, uint32_t *txd, uint32_t *status);
uint32_t e1000_rx_frames(void);


#ifdef __cplusplus
}
#endif

#endif
