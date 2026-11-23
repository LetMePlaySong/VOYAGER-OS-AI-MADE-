#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
static uint32_t be32(const uint8_t*p){return ((uint32_t)p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3];}
int main(int argc,char**argv){
 FILE*f=fopen(argv[1],"rb");fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);
 uint8_t*d=malloc(n);if(fread(d,1,n,f)!=(size_t)n)return 1;fclose(f);
 size_t pos=8;uint8_t*idat=NULL;size_t il=0,ic=0;
 while(pos+12<=(size_t)n){uint32_t len=be32(d+pos);const uint8_t*t=d+pos+4;const uint8_t*c=d+pos+8;
  if(pos+12+len>(size_t)n)break;
  if(!memcmp(t,"IDAT",4)){if(il+len>ic){ic=(il+len)*2+4096;idat=realloc(idat,ic);}memcpy(idat+il,c,len);il+=len;}
  if(!memcmp(t,"IEND",4))break;pos+=12+len;}
 uint64_t s=1469598103934665603ULL;
 for(size_t i=0;i<il;i++){s^=idat[i];s*=1099511628211ULL;}
 printf("idat_len=%zu fnv=%llx first=%02x%02x%02x%02x last=%02x%02x%02x%02x\n",il,(unsigned long long)s,idat[0],idat[1],idat[2],idat[3],idat[il-4],idat[il-3],idat[il-2],idat[il-1]);
 return 0;}
