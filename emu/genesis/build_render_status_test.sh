#!/bin/bash
# Build the sprites-only render check (render_status_test.c) NATIVELY (x86-64, hosted) from the
# SAME core sources that go into the ARM blob, the same way as build_host_test.sh. The ROM is fed as
# a buffer via GET_GAME_INFO_EXT exactly like the blob. Output: /tmp/gpgx_render_status_test.
#   emu/genesis/build_render_status_test.sh && /tmp/gpgx_render_status_test rom.bin [frames]
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
CORE="$DIR/core"
INC="-I$CORE -I$CORE/z80 -I$CORE/m68k -I$CORE/ntsc -I$CORE/sound -I$CORE/input_hw \
     -I$CORE/cd_hw -I$CORE/cart_hw -I$CORE/cart_hw/svp \
     -I$DIR/libretro -I$DIR/libretro/libretro-common/include"
DEFS="-DLSB_FIRST -DBYTE_ORDER=LITTLE_ENDIAN -D__LIBRETRO__ -DALIGN_LONG -DALIGN_WORD \
      -DM68K_OVERCLOCK_SHIFT=20 -DZ80_OVERCLOCK_SHIFT=20 -DMAXROMSIZE=33554432 -DUSE_DYNAMIC_ALLOC \
      -DUSE_16BPP_RENDERING -DFRONTEND_SUPPORTS_RGB565 -DHAVE_YM3438_CORE -DHAVE_OPLL_CORE"
F="-O2 -w -fno-strict-aliasing $DEFS $INC"
OBJ="/tmp/gpgx_render_status_obj"; rm -rf "$OBJ"; mkdir -p "$OBJ"

SRCS="$(cd "$DIR" && find core -name '*.c' | grep -v '/minimp3/' | grep -v '/libchdr/' | grep -v 'yx5200'; echo libretro/libretro.c)"
for f in $SRCS; do
	gcc $F -std=gnu99 -c "$DIR/$f" -o "$OBJ/$(echo "${f%.c}" | tr / _).o"
done

# Host-only stubs: no filesystem (ROM is fed as a buffer), no yx5200 MP3 cart. crc32/_ctype_/
# setjmp/memcpy come from glibc/zlib. GPGX's file I/O paths are optional and unused.
cat > "$OBJ/host_stubs.c" <<'EOF'
typedef unsigned long usize;
void *filestream_open(const char*p,unsigned m,unsigned h){(void)p;(void)m;(void)h;return 0;}
long filestream_read(void*f,void*b,long n){(void)f;(void)b;(void)n;return 0;}
long filestream_write(void*f,const void*b,long n){(void)f;(void)b;(void)n;return 0;}
long filestream_seek(void*f,long o,int w){(void)f;(void)o;(void)w;return -1;}
long filestream_tell(void*f){(void)f;return -1;}
int filestream_close(void*f){(void)f;return -1;}
char *filestream_gets(void*f,char*b,unsigned n){(void)f;(void)b;(void)n;return 0;}
void filestream_vfs_init(const void*i){(void)i;}
void *rfopen(const char*p,const char*m){(void)p;(void)m;return 0;}
long rfread(void*b,long s,long n,void*f){(void)b;(void)s;(void)n;(void)f;return 0;}
long rfwrite(const void*b,long s,long n,void*f){(void)b;(void)s;(void)n;(void)f;return 0;}
int rfseek(void*f,long o,int w){(void)f;(void)o;(void)w;return -1;}
long rftell(void*f){(void)f;return -1;}
int rfclose(void*f){(void)f;return -1;}
char *rfgets(char*b,int n,void*f){(void)b;(void)n;(void)f;return 0;}
void yx5200_init(void){} void yx5200_reset(void){}
void yx5200_write(unsigned a,unsigned d){(void)a;(void)d;}
int yx5200_update(int*b,int n){(void)b;(void)n;return 0;}
int yx5200_context_save(unsigned char*s){(void)s;return 0;}
int yx5200_context_load(unsigned char*s){(void)s;return 0;}
usize strlcpy_retro__(char*d,const char*s,usize n){usize i=0;if(n){while(i<n-1&&s[i]){d[i]=s[i];i++;}d[i]=0;}while(s[i])i++;return i;}
usize strlcat_retro__(char*d,const char*s,usize n){usize l=0;while(l<n&&d[l])l++;return l+strlcpy_retro__(d+l,s,n>l?n-l:0);}
usize fill_pathname_join(char*o,const char*a,const char*b,usize n){usize l=strlcpy_retro__(o,a,n);if(l&&l<n&&o[l-1]!='/'){o[l++]='/';o[l]=0;}return strlcat_retro__(o,b,n);}
EOF
gcc $F -std=gnu99 -c "$OBJ/host_stubs.c" -o "$OBJ/host_stubs.o"

gcc $F -std=gnu99 -c "$DIR/render_status_test.c" -o "$OBJ/render_status_test.o"
gcc $F "$OBJ"/*.o -o /tmp/gpgx_render_status_test -lm -lz
echo "built /tmp/gpgx_render_status_test - run: /tmp/gpgx_render_status_test [rom.md] [frames]"
