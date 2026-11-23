/* AC97 (Intel ICH compatible) audio driver - PCM playback.
 * VirtualBox "Intel AC97" and QEMU -device AC97 both implement this.
 * NAMBAR (BAR0) = codec mixer regs (16-bit), NABMBAR (BAR1) = DMA engine.
 * Polling only - no IRQ needed: the app paces itself against CIV/LVI. */
#ifndef AC97_H
#define AC97_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

int  ac97_init(void);            /* 0 = ok, nonzero = no device */
int  ac97_ready(void);
int  ac97_set_rate(uint32_t hz); /* stereo s16; 0 ok. call while stopped */
uint32_t ac97_rate(void);
/* streaming: frames = stereo sample pairs. write() queues as much as the
 * ring accepts and returns the count actually written (may be < frames);
 * the caller retries the remainder on the next update tick. */
uint32_t ac97_write(const int16_t *pcm, uint32_t frames);
void ac97_flush(void);           /* drop queued audio, rewind to silence */
uint32_t ac97_level(void);       /* approx ms of audio still buffered */
void ac97_debug(void);           /* serial: civ/lvi/picb/mixer state */
void ac97_diag(void);            /* serial: PCM stats at play position */
void ac97_set_volume(int pct);   /* 0-100, 0 = muted */
int  ac97_volume(void);

/* single-producer lock: the media player claims the DAC while a song
 * plays so UI sounds (beeps) never inject samples into the music stream */
int  ac97_claim(void *owner);    /* 1 = claimed, 0 = held by someone else */
void ac97_release(void *owner);
int  ac97_claimed(void);

/* real-time spectrum of what the DAC is ACTUALLY playing right now:
 * reads the PCM window at the DMA read cursor (CIV/PICB) and runs a
 * 20-band Goertzel filterbank (log-spaced 40Hz..16kHz @48kHz).
 * out[] gets 0..255 per band - feed it to visualisers. */
#define AC97_SPEC_BANDS 20
void ac97_spectrum(uint8_t out[AC97_SPEC_BANDS]);

#ifdef __cplusplus
}
#endif

#endif
