#include <stdio.h>
#include <string.h>
#include "fat_ro.h"
#include "fat_wr.h"
static FILE*g;
static unsigned rd(void*c,uint32_t l,uint32_t n,void*b){(void)c;fseek(g,(long)l*512,0);return fread(b,512,n,g);}
static unsigned wr(void*c,uint32_t l,uint32_t n,const void*b){(void)c;fseek(g,(long)l*512,0);return fwrite(b,512,n,g);}
int main(int ac,char**av){ fat_vol v; static unsigned char p1[4000],p2[80],p3[2500];
 for(int i=0;i<4000;i++)p1[i]=(unsigned char)(i*3+1); for(int i=0;i<80;i++)p2[i]=(unsigned char)(255-i); for(int i=0;i<2500;i++)p3[i]=(unsigned char)(i^0x5a);
 g=fopen(av[1],"r+b"); if(fat_mount(&v,rd,0)){puts("mnt FAIL");return 1;} fat_set_writer(&v,wr);
 printf("put1 rc=%d\n", fat_wr_put(&v,"/saves/gba","Pokemon Emerald (USA).sav",p1,4000));
 printf("put2 rc=%d\n", fat_wr_put(&v,"/states/gba","Legend of Zelda, The.st0",p3,2500));
 printf("put3(8.3) rc=%d\n", fat_wr_put(&v,"/saves/gba","Big.sav",p2,80));
 printf("replace1 rc=%d\n", fat_wr_put(&v,"/saves/gba","Pokemon Emerald (USA).sav",p2,80));
 fflush(g); fclose(g); return 0; }
