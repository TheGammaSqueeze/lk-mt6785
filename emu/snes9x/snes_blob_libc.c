/*
 * snes_blob_libc.c - the freestanding libc the bundled snes9x core references, compiled
 * into the SNES blob. The mem, str, ctype and number-parsing helpers plus a compact
 * vsnprintf are real;
 * file/stdio and cheat parsing (which the LK build does not use - no files, no cheats)
 * are safe stubs. __aeabi_* come from libgcc, math from libm, C++ runtime from
 * snes_shim.cpp, and time()/localtime live in snes_core_exports.cpp (they need the LK
 * imports table). Goal: a clean link with no crash on the non-critical paths.
 */
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>

/* ---- mem ---- */
void *memcpy(void *d, const void *s, size_t n)
{ unsigned char *a=d; const unsigned char *b=s; while(n--) *a++=*b++; return d; }
void *memmove(void *d, const void *s, size_t n)
{ unsigned char *a=d; const unsigned char *b=s; if(a==b||!n) return d;
  if(a<b){ while(n--) *a++=*b++; } else { a+=n; b+=n; while(n--) *--a=*--b; } return d; }
void *memset(void *d, int c, size_t n)
{ unsigned char *a=d; while(n--) *a++=(unsigned char)c; return d; }
int memcmp(const void *a, const void *b, size_t n)
{ const unsigned char *x=a,*y=b; while(n--){ if(*x!=*y) return *x-*y; x++; y++; } return 0; }

/* ---- str ---- */
size_t strlen(const char *s){ const char *p=s; while(*p) p++; return (size_t)(p-s); }
int strcmp(const char *a, const char *b){ while(*a&&*a==*b){a++;b++;} return (int)(unsigned char)*a-(int)(unsigned char)*b; }
int strncmp(const char *a, const char *b, size_t n){ while(n&&*a&&*a==*b){a++;b++;n--;} return n?(int)(unsigned char)*a-(int)(unsigned char)*b:0; }
char *strcpy(char *d, const char *s){ char *r=d; while((*d++=*s++)); return r; }
char *strncpy(char *d, const char *s, size_t n){ char *r=d; while(n&&(*d=*s)){d++;s++;n--;} while(n--) *d++=0; return r; }
char *strcat(char *d, const char *s){ char *r=d; while(*d) d++; while((*d++=*s++)); return r; }
char *strchr(const char *s, int c){ while(*s){ if(*s==(char)c) return (char*)s; s++; } return c?0:(char*)s; }
char *strrchr(const char *s, int c){ const char *r=0; do{ if(*s==(char)c) r=s; }while(*s++); return (char*)r; }
static int lc(int c){ return (c>='A'&&c<='Z')?c+32:c; }
int strcasecmp(const char *a, const char *b){ while(*a&&lc(*a)==lc(*b)){a++;b++;} return lc((unsigned char)*a)-lc((unsigned char)*b); }
int strncasecmp(const char *a, const char *b, size_t n){ while(n&&*a&&lc(*a)==lc(*b)){a++;b++;n--;} return n?lc((unsigned char)*a)-lc((unsigned char)*b):0; }
char *strtok(char *s, const char *d)
{ static char *save; if(!s) s=save; if(!s) return 0;
  while(*s&&strchr(d,*s)) s++; if(!*s){ save=0; return 0; }
  char *tok=s; while(*s&&!strchr(d,*s)) s++; if(*s){ *s=0; save=s+1; } else save=0; return tok; }

/* ---- ctype ---- */
int toupper(int c){ return (c>='a'&&c<='z')?c-32:c; }
int isalnum(int c){ return (c>='0'&&c<='9')||(c>='a'&&c<='z')||(c>='A'&&c<='Z'); }
int abs(int v){ return v<0?-v:v; }

/* ---- number parsing ---- */
long strtol(const char *s, char **end, int base)
{
	long v=0; int neg=0;
	while(*s==' '||*s=='\t') s++;
	if(*s=='+'||*s=='-'){ neg=(*s=='-'); s++; }
	if((base==0||base==16)&&s[0]=='0'&&(s[1]=='x'||s[1]=='X')){ s+=2; base=16; }
	if(base==0) base=10;
	for(;;){ int d; char c=*s;
		if(c>='0'&&c<='9') d=c-'0';
		else if(c>='a'&&c<='z') d=c-'a'+10;
		else if(c>='A'&&c<='Z') d=c-'A'+10;
		else break;
		if(d>=base) break;
		v=v*base+d; s++; }
	if(end) *end=(char*)s;
	return neg?-v:v;
}
unsigned long strtoul(const char *s, char **end, int base){ return (unsigned long)strtol(s,end,base); }
int atoi(const char *s){ return (int)strtol(s,0,10); }

/* ---- compact vsnprintf: %s %c %d %i %u %x %X %p %% with width, zero-pad, and l/ll ----
 * enough for snes9x's message/filename builds; those strings are non-critical here. */
static char *emit(char *o, char *e, char c){ if(o<e) *o=c; return o+1; }
static char *emit_str(char *o, char *e, const char *s, int width, int zero)
{
	int len=0; const char *t=s?s:"(null)"; while(t[len]) len++;
	while(!zero && width>len){ o=emit(o,e,' '); width--; }
	for(const char *p=(s?s:"(null)"); *p; p++) o=emit(o,e,*p);
	while(width>len){ o=emit(o,e,zero?'0':' '); width--; }
	return o;
}
static char *emit_num(char *o, char *e, unsigned long v, int base, int up, int neg, int width, int zero)
{
	char tmp[24]; int n=0; const char *dig= up?"0123456789ABCDEF":"0123456789abcdef";
	if(!v) tmp[n++]='0'; else while(v){ tmp[n++]=dig[v%base]; v/=base; }
	int total=n+(neg?1:0);
	if(!zero){ while(width>total){ o=emit(o,e,' '); width--; } }
	if(neg) o=emit(o,e,'-');
	if(zero){ while(width>total){ o=emit(o,e,'0'); width--; } }
	while(n) o=emit(o,e,tmp[--n]);
	return o;
}
int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
	char *o=buf, *e=buf+(cap?cap-1:0);
	for(; *fmt; fmt++){
		if(*fmt!='%'){ o=emit(o,e,*fmt); continue; }
		fmt++;
		int zero=0,width=0,lng=0;
		while(*fmt=='0'){ zero=1; fmt++; }
		while(*fmt>='0'&&*fmt<='9'){ width=width*10+(*fmt-'0'); fmt++; }
		while(*fmt=='l'){ lng++; fmt++; }
		switch(*fmt){
		case 's': o=emit_str(o,e,va_arg(ap,const char*),width,0); break;
		case 'c': o=emit(o,e,(char)va_arg(ap,int)); break;
		case 'd': case 'i': { long v= lng?va_arg(ap,long):va_arg(ap,int);
				      int neg=v<0; unsigned long u=neg?(unsigned long)(-v):(unsigned long)v;
				      o=emit_num(o,e,u,10,0,neg,width,zero); } break;
		case 'u': { unsigned long v= lng?va_arg(ap,unsigned long):va_arg(ap,unsigned int);
			    o=emit_num(o,e,v,10,0,0,width,zero); } break;
		case 'x': { unsigned long v= lng?va_arg(ap,unsigned long):va_arg(ap,unsigned int);
			    o=emit_num(o,e,v,16,0,0,width,zero); } break;
		case 'X': { unsigned long v= lng?va_arg(ap,unsigned long):va_arg(ap,unsigned int);
			    o=emit_num(o,e,v,16,1,0,width,zero); } break;
		case 'p': o=emit_num(o,e,(unsigned long)(uintptr_t)va_arg(ap,void*),16,0,0,width,zero); break;
		case '%': o=emit(o,e,'%'); break;
		default:  o=emit(o,e,'%'); if(*fmt) o=emit(o,e,*fmt); break;
		}
	}
	if(cap) *(o<e?o:e)=0;
	return (int)(o-buf);
}
int snprintf(char *buf, size_t cap, const char *fmt, ...)
{ va_list ap; va_start(ap,fmt); int r=vsnprintf(buf,cap,fmt,ap); va_end(ap); return r; }
int sprintf(char *buf, const char *fmt, ...)
{ va_list ap; va_start(ap,fmt); int r=vsnprintf(buf,(size_t)-1,fmt,ap); va_end(ap); return r; }
int vsprintf(char *buf, const char *fmt, va_list ap){ return vsnprintf(buf,(size_t)-1,fmt,ap); }

/* ---- stdio / diagnostics: no files, no console in LK -> safe stubs ---- */
int printf(const char *fmt, ...){ (void)fmt; return 0; }
int fprintf(void *f, const char *fmt, ...){ (void)f;(void)fmt; return 0; }
int sscanf(const char *s, const char *fmt, ...){ (void)s;(void)fmt; return 0; }  /* cheats only */
void perror(const char *s){ (void)s; }
void __assert_func(const char *a, int b, const char *c, const char *d){ (void)a;(void)b;(void)c;(void)d; for(;;){} }
void exit(int code){ (void)code; for(;;){} }
void *_impure_ptr = 0;   /* referenced by newlib-shaped headers; our stdio does not deref it */

/* ---- rng (snes9x seeds/uses for a few effects) ---- */
static unsigned long g_seed = 1;
int rand(void){ g_seed = g_seed*1103515245UL + 12345UL; return (int)((g_seed>>16)&0x7FFF); }
void srand(unsigned s){ g_seed = s; }

/* newlib libm's math sets errno via *__errno(); give it a cell. */
int *__errno(void){ static int e; return &e; }
