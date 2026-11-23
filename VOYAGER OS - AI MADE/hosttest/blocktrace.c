#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef struct{const uint8_t*d;size_t l,p;uint32_t bb;int bc;} BR;
static int bit(BR*b){if(!b->bc){if(b->p>=b->l)return-1;b->bb=b->d[b->p++];b->bc=8;}int x=b->bb&1;b->bb>>=1;b->bc--;return x;}
static int bits(BR*b,int n){int v=0;for(int i=0;i<n;i++){int x=bit(b);if(x<0)return-1;v|=x<<i;}return v;}
static uint32_t be32(const uint8_t*p){return(p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3];}
int main(int c,char**v){FILE*f=fopen(v[1],"rb");fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);uint8_t*d=malloc(n);if(fread(d,1,n,f)!=(size_t)n)return 1;fclose(f);
 size_t pos=8;uint8_t*id=NULL;size_t il=0,ic=0;
 while(pos+12<=(size_t)n){uint32_t L=be32(d+pos);const uint8_t*t=d+pos+4,*ch=d+pos+8;if(pos+12+L>(size_t)n)break;
  if(!memcmp(t,"IDAT",4)){if(il+L>ic){ic=(il+L)*2+4096;id=realloc(id,ic);}memcpy(id+il,ch,L);il+=L;}if(!memcmp(t,"IEND",4))break;pos+=12+L;}
 /* skip zlib 2-byte hdr */
 BR b={id+2,il-2,0,0,0};
 for(int blk=0;blk<20;blk++){int fin=bit(&b);int ty=bits(&b,2);printf("block %d final=%d type=%d bitpos=%zu bytepos=%zu\n",blk,fin,ty,(size_t)0,b.p);
  if(ty==0){b.bc=0;b.bb=0;int l0=bit(&b);/*wrong*/ /*read bytes*/ b.bc=0;b.bb=0; /* emulate byte reads */ int L0=id[2+b.p],L1=id[3+b.p],N0=id[4+b.p],N1=id[5+b.p]; int nn=L0|(L1<<8),nn2=N0|(N1<<8); printf("  stored LEN=%d NLEN=%d pos=%zu chk=%d\n",nn,nn2,b.p,((nn^0xFFFF)==nn2)); b.p+=4+nn; if(fin)break;}
  else break; /* can't easily skip huff blocks */
 }
 return 0;}
