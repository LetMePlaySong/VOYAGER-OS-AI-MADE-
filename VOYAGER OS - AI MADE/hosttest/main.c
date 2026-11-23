#include "stubs.h"
#include "png_inc.c"
int main(int argc,char**argv){
    FILE*f=fopen(argv[1],"rb"); fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t*d=malloc(n); if(fread(d,1,n,f)!=(size_t)n)return 1; fclose(f);
    uint8_t*rgba; uint32_t w,h;
    int r=png_decode(d,n,&rgba,&w,&h);
    printf("decode=%d w=%u h=%u\n",r,w,h);
    return 0;
}
