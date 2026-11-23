#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
static uint32_t be32(const uint8_t*p){return(p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3];}
int main(int c,char**v){FILE*f=fopen(v[1],"rb");fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);uint8_t*d=malloc(n);if(fread(d,1,n,f)!=(size_t)n)return 1;fclose(f);
 size_t pos=8;uint8_t*id=NULL;size_t il=0,ic=0;
 while(pos+12<=(size_t)n){uint32_t L=be32(d+pos);const uint8_t*t=d+pos+4,*ch=d+pos+8;if(pos+12+L>(size_t)n)break;
  if(!memcmp(t,"IDAT",4)){if(il+L>ic){ic=(il+L)*2+4096;id=realloc(id,ic);}memcpy(id+il,ch,L);il+=L;}if(!memcmp(t,"IEND",4))break;pos+=12+L;}
 int NC=16; size_t cl=il/NC;
 for(int k=0;k<NC;k++){size_t s=k*cl,e=(k==NC-1)?il:(k+1)*cl;uint64_t h=1469598103934665603ULL;
  for(size_t i=s;i<e;i++){h^=id[i];h*=1099511628211ULL;}printf("%d:%llx ",k,(unsigned long long)(h&0xffffffff));}
 printf("\n");return 0;}
