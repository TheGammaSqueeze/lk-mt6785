#include <stdio.h>
#include <string.h>
#include "fat_ro.h"
#include "fat_wr.h"
#include "gba_sd_save.h"
static FILE*g;
static unsigned rd(void*c,uint32_t l,uint32_t n,void*b){(void)c;fseek(g,(long)l*512,0);return fread(b,512,n,g);}
static unsigned wr(void*c,uint32_t l,uint32_t n,const void*b){(void)c;fseek(g,(long)l*512,0);return fwrite(b,512,n,g);}
int main(int ac,char**av){ fat_vol v; static unsigned char sav[8192],st[262144],rb[262144];
 for(int i=0;i<8192;i++)sav[i]=(unsigned char)(i*11+5); for(int i=0;i<262144;i++)st[i]=(unsigned char)((i>>3)^i);
 const char*rom="Pokemon Emerald (USA).gba";
 g=fopen(av[1],"r+b"); if(fat_mount(&v,rd,0)){puts("mnt FAIL");return 1;} fat_set_writer(&v,wr);
 printf("write_sav rc=%d\n", gba_sd_write_sav(&v,rom,sav,8192));
 printf("write_state slot0 rc=%d\n", gba_sd_write_state(&v,rom,0,st,262144));
 fflush(g); fclose(g);
 g=fopen(av[1],"rb"); fat_mount(&v,rd,0);
 uint32_t n=gba_sd_load_sav(&v,rom,rb,sizeof rb); printf("load_sav len=%u match=%d\n",n,(n==8192 && !memcmp(rb,sav,8192)));
 uint32_t m=gba_sd_load_state(&v,rom,0,rb,sizeof rb); printf("load_state len=%u match=%d\n",m,(m==262144 && !memcmp(rb,st,262144)));
 uint32_t z=gba_sd_load_sav(&v,"NoSuch.gba",rb,sizeof rb); printf("load absent -> %u (want 0)\n",z);
 return 0; }
