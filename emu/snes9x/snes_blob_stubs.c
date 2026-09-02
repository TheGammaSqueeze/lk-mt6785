/*
 * snes_blob_stubs.c - failure/no-op stubs for symbols the snes9x core and libstdc++
 * reference but the LK build never exercises: ALL file/zip/VFS I/O (the ROM is fed as a
 * buffer, SRAM rides the libretro memory interface, no cheats/MSU/patches), and the
 * libstdc++ locale + wide-char machinery that iostream/std::locale drags in. None of
 * these run in practice; they only need to resolve so the blob links. Prototypes are
 * hand-written (no system headers) to avoid getc/putc-macro and type clashes; int64_t
 * returns use `long long` so the r0:r1 ABI matches the real declarations.
 */
typedef unsigned long  usize;   /* size_t on arm32 (4 bytes) */
typedef unsigned int   wch;     /* wchar_t / wint_t on arm-none-eabi (4 bytes) */

/* ---- snes9x RFILE / filestream I/O (streams/file_stream*.h) -> always fail ---- */
void      *rfopen(const char *p, const char *m) { (void)p; (void)m; return 0; }
int        rfclose(void *s) { (void)s; return -1; }
long long  rftell(void *s) { (void)s; return -1; }
long long  rfseek(void *s, long long o, int w) { (void)s; (void)o; (void)w; return -1; }
long long  rfread(void *b, usize es, usize ec, void *s) { (void)b;(void)es;(void)ec;(void)s; return 0; }
long long  rfwrite(const void *b, usize es, usize ec, void *s) { (void)b;(void)es;(void)ec;(void)s; return 0; }
char      *rfgets(char *b, int n, void *s) { (void)b;(void)n;(void)s; return 0; }
int        rfgetc(void *s) { (void)s; return -1; }
int        rfputc(int c, void *s) { (void)c;(void)s; return -1; }
long long  rfflush(void *s) { (void)s; return 0; }
int        rferror(void *s) { (void)s; return 0; }
int        rfeof(void *s) { (void)s; return 1; }
int        rfprintf(void *s, const char *f, ...) { (void)s;(void)f; return 0; }
int        rfscanf(void *s, const char *f, ...) { (void)s;(void)f; return 0; }

void      *filestream_open(const char *p, unsigned m, unsigned h) { (void)p;(void)m;(void)h; return 0; }
int        filestream_close(void *s) { (void)s; return -1; }
long long  filestream_seek(void *s, long long o, int w) { (void)s;(void)o;(void)w; return -1; }
long long  filestream_read(void *s, void *d, long long n) { (void)s;(void)d;(void)n; return -1; }
long long  filestream_write(void *s, const void *d, long long n) { (void)s;(void)d;(void)n; return -1; }
long long  filestream_tell(void *s) { (void)s; return -1; }
long long  filestream_read_file(const char *p, void **buf, long long *len) { (void)p; if(buf)*buf=0; if(len)*len=0; return 0; }
void       vfs_hybrid_init(void *e, void *l) { (void)e; (void)l; }

/* ---- zip archive (zipfile.h) -> never opened (ROMs are raw buffers) ---- */
int        zip_archive_open(void *a, const char *p) { (void)a;(void)p; return -1; }
void       zip_archive_close(void *a) { (void)a; }
int        zip_find_name(const void *a, const char *n, int f) { (void)a;(void)n;(void)f; return -1; }
int        zip_find_ext(const void *a, const char *e, int f) { (void)a;(void)e;(void)f; return -1; }
int        zip_find_suffix(const void *a, const char *s, int f) { (void)a;(void)s;(void)f; return -1; }
int        zip_file_open(void *zf, void *a, int i) { (void)zf;(void)a;(void)i; return -1; }
void       zip_file_close(void *zf) { (void)zf; }
unsigned   zip_file_size(const void *zf) { (void)zf; return 0; }
unsigned   zip_file_read(void *zf, unsigned off, unsigned char *out, unsigned len) { (void)zf;(void)off;(void)out;(void)len; return 0; }
unsigned char *zip_read_entry(void *a, int i, unsigned *len) { (void)a;(void)i; if(len)*len=0; return 0; }
/* LoadZip is C++-mangled -> defined in snes_shim.cpp, not here. */

/* ---- newlib-shaped stdio the core touches only on file paths ---- */
usize fread(void *p, usize s, usize n, void *f) { (void)p;(void)s;(void)n;(void)f; return 0; }
usize fwrite(const void *p, usize s, usize n, void *f) { (void)p;(void)s;(void)n;(void)f; return 0; }
int   fseek(void *f, long o, int w) { (void)f;(void)o;(void)w; return -1; }
long  ftell(void *f) { (void)f; return -1; }
int   fflush(void *f) { (void)f; return 0; }
int   getc(void *f) { (void)f; return -1; }
int   putc(int c, void *f) { (void)c;(void)f; return -1; }
int   ungetc(int c, void *f) { (void)c;(void)f; return -1; }

/* ---- libc gaps ---- */
void *memchr(const void *s, int c, usize n)
{ const unsigned char *p = (const unsigned char *)s; while (n--) { if (*p == (unsigned char)c) return (void *)p; p++; } return 0; }
double strtod(const char *s, char **e) { (void)s; if (e) *e = (char *)s; return 0.0; }
float  strtof(const char *s, char **e) { (void)s; if (e) *e = (char *)s; return 0.0f; }
void   abort(void) { for (;;) {} }

/* ---- libstdc++ locale + wide-char: pulled in by iostream/std::locale, never used ---- */
static int lc_(int c){ return (c>='A'&&c<='Z')?c+32:c; }
char *setlocale(int cat, const char *loc) { (void)cat; (void)loc; return (char *)"C"; }
int   strcoll(const char *a, const char *b) { while(*a&&*a==*b){a++;b++;} return (int)(unsigned char)*a-(int)(unsigned char)*b; }
usize strxfrm(char *d, const char *s, usize n) { usize i=0; while(s[i]&&i+1<n){ d[i]=s[i]; i++; } if(n) d[i]=0; while(s[i]) i++; return i; }
char *strerror(int e) { (void)e; return (char *)""; }
usize strftime(char *b, usize n, const char *f, const void *tm) { (void)f;(void)tm; if(n) b[0]=0; return 0; }
int   __locale_mb_cur_max(void) { return 1; }
const char _ctype_[384] = { 0 };

/* wide char (wchar_t=4B). All degenerate: our locale is byte/C, wchar unused. */
usize wcslen(const wch *s) { usize n=0; while(s[n]) n++; return n; }
wch  *wmemcpy(wch *d, const wch *s, usize n) { for(usize i=0;i<n;i++) d[i]=s[i]; return d; }
wch  *wmemmove(wch *d, const wch *s, usize n) { if(d<s){ for(usize i=0;i<n;i++) d[i]=s[i]; } else { for(usize i=n;i>0;i--) d[i-1]=s[i-1]; } return d; }
wch  *wmemset(wch *d, wch c, usize n) { for(usize i=0;i<n;i++) d[i]=c; return d; }
wch  *wmemchr(const wch *s, wch c, usize n) { for(usize i=0;i<n;i++) if(s[i]==c) return (wch *)(s+i); return 0; }
int   wcscoll(const wch *a, const wch *b) { while(*a&&*a==*b){a++;b++;} return (int)*a-(int)*b; }
usize wcsxfrm(wch *d, const wch *s, usize n) { usize i=0; while(s[i]&&i+1<n){ d[i]=s[i]; i++; } if(n) d[i]=0; while(s[i]) i++; return i; }
usize wcsftime(wch *b, usize n, const wch *f, const void *tm) { (void)f;(void)tm; if(n) b[0]=0; return 0; }
wch   btowc(int c) { return (wch)c; }
int   wctob(wch c) { return (int)c; }
usize wcrtomb(char *s, wch wc, void *ps) { (void)ps; if(s) *s=(char)wc; return 1; }
usize mbrtowc(wch *pwc, const char *s, usize n, void *ps) { (void)n;(void)ps; if(s&&pwc) *pwc=(unsigned char)*s; return (s&&*s)?1:0; }
wch   towlower(wch c) { return (wch)lc_((int)c); }
wch   towupper(wch c) { int x=(int)c; return (wch)((x>='a'&&x<='z')?x-32:x); }
int   iswctype(wch c, unsigned long t) { (void)c;(void)t; return 0; }
unsigned long wctype(const char *p) { (void)p; return 0; }
wch   getwc(void *f) { (void)f; return (wch)-1; }
wch   putwc(wch c, void *f) { (void)f; return c; }
wch   ungetwc(wch c, void *f) { (void)c;(void)f; return (wch)-1; }
