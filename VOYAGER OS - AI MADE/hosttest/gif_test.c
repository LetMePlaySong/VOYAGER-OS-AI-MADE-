#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef struct { uint8_t *rgba; int delay_cs; } GifFrame;
int gif_decode_anim(const uint8_t *d, uint32_t n, GifFrame **out, int *on, int *ow, int *oh);
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n); fread(buf, 1, n, f); fclose(f);
    GifFrame *fr = NULL; int nf = 0, w = 0, h = 0;
    int rc = gif_decode_anim(buf, n, &fr, &nf, &w, &h);
    printf("rc=%d nf=%d %dx%d\n", rc, nf, w, h);
    for (int i = 0; i < nf && i < 20; i++) {
        int zero=0, op=0, part=0;
        for (int p = 0; p < w*h; p++) {
            uint8_t a = fr[i].rgba[p*4+3];
            if (a==0) zero++; else if (a==255) op++; else part++;
        }
        printf("frame%d zero=%d opaque=%d partial=%d delay=%d\n", i, zero, op, part, fr[i].delay_cs);
    }
    return 0;
}
