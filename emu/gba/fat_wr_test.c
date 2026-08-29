#include <stdio.h>
#include <string.h>
#include "fat_ro.h"
#include "fat_wr.h"
static FILE*g;
static unsigned rd(void*c,uint32_t l,uint32_t n,void*b){(void)c;fseek(g,(long)l*512,0);return fread(b,512,n,g);}
static unsigned wr(void*c,uint32_t l,uint32_t n,const void*b){(void)c;fseek(g,(long)l*512,0);return fwrite(b,512,n,g);}
int main(int ac,char**av){ fat_vol v; static unsigned char pay[5000], pay2[100];
 for(int i=0;i<5000;i++)pay[i]=(unsigned char)(i*7+3);
 for(int i=0;i<100;i++)pay2[i]=(unsigned char)(200-i);
 g=fopen(ac>1?av[1]:"/tmp/fatw.img","r+b"); if(!g){puts("open FAIL");return 1;}
 if(fat_mount(&v,rd,0)){puts("mount FAIL");return 1;} fat_set_writer(&v,wr);
 int r=fat_wr_put(&v,"/saves/gba","TEST.SAV",pay,5000); printf("create TEST.SAV(5000) rc=%d\n",r); if(r){puts("FAIL");return 1;}
 int r2=fat_wr_put(&v,"/states/gba","ZELDA.ST0",pay,3000); printf("create ZELDA.ST0(3000) rc=%d\n",r2); if(r2){puts("FAIL");return 1;}
 /* replace with smaller */
 int r3=fat_wr_put(&v,"/saves/gba","TEST.SAV",pay2,100); printf("replace TEST.SAV(100) rc=%d\n",r3); if(r3){puts("FAIL");return 1;}
 fflush(g); fclose(g);
 /* verify via our own reader too */
 g=fopen(av[1]?av[1]:"/tmp/fatw.img","rb"); fat_mount(&v,rd,0);
 fat_file f; unsigned char rb[5000];
 fat_open(&v,"/saves/gba/TEST.SAV",&f); uint32_t n=fat_read(&f,0,rb,5000);
 printf("readback TEST.SAV size=%u read=%u match100=%d\n",f.size,n,(f.size==100 && n==100 && !memcmp(rb,pay2,100)));
 fat_open(&v,"/states/gba/ZELDA.ST0",&f); n=fat_read(&f,0,rb,3000);
 printf("readback ZELDA.ST0 size=%u match=%d\n",f.size,(f.size==3000 && n==3000 && !memcmp(rb,pay,3000)));
 return 0; }
