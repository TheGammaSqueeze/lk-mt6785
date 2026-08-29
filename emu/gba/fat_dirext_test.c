#include <stdio.h>
#include <string.h>
#include "fat_ro.h"
#include "fat_wr.h"
static FILE*g;
static unsigned rd(void*c,uint32_t l,uint32_t n,void*b){(void)c;fseek(g,(long)l*512,0);return fread(b,512,n,g);}
static unsigned wr(void*c,uint32_t l,uint32_t n,const void*b){(void)c;fseek(g,(long)l*512,0);return fwrite(b,512,n,g);}
int main(int ac,char**av){ fat_vol v; unsigned char p[64]; char nm[80]; int i,fails=0;
 g=fopen(av[1],"r+b"); if(fat_mount(&v,rd,0)){puts("mnt FAIL");return 1;} fat_set_writer(&v,wr);
 for(i=0;i<50;i++){ sprintf(nm,"Long Game Title Number %02d Overflow.sav",i); memset(p,i,64);
   int r=fat_wr_put(&v,"/saves/gba",nm,p,64); if(r){printf("write %d FAIL rc=%d\n",i,r);fails++;} }
 fflush(g); fclose(g);
 g=fopen(av[1],"rb"); fat_mount(&v,rd,0);
 int found=0; for(i=0;i<50;i++){ sprintf(nm,"/saves/gba/Long Game Title Number %02d Overflow.sav",i);
   fat_file f; unsigned char rb[64]; if(fat_open(&v,nm,&f)==0){ uint32_t n=fat_read(&f,0,rb,64);
     if(n==64 && rb[0]==(unsigned char)i) found++; else {printf("readback %d bad\n",i);fails++;} } else {printf("missing %d\n",i);fails++;} }
 printf("wrote 50 long-named files, read back %d ok\n",found);
 return fails; }
