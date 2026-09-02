#!/bin/bash
# Build the headless snes9x core validator (host_test.cpp) NATIVELY (x86-64, hosted) from
# the same core sources that go into the ARM blob. Confirms the core loads a ROM and runs
# real frames without needing the device. Output: /tmp/s9x_host_test.
#   emu/snes9x/build_host_test.sh && /tmp/s9x_host_test "/path/rom.sfc" [frames]
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
INC="-I$DIR -I$DIR/apu -I$DIR/apu/bapu -I$DIR/libretro -I$DIR/libretro/libretro-common/include"
F="-D__LIBRETRO__ -DVIDEO_RGB565 -O2 -w -fno-strict-aliasing $INC"
OBJ="/tmp/s9x_host_obj"; rm -rf "$OBJ"; mkdir -p "$OBJ"

C_SRCS="c4emu msu1 srtc obc1 bsflash tile bsx spc7110 fxemu sdd1 seta sa1hw dsp filter/snes_ntsc"
CXX_SRCS="apu/apu apu/bapu/dsp/sdsp apu/bapu/smp/smp apu/bapu/smp/smp_state cheats2 clip \
          controls cpu cpuexec cpuops crosshairs dma gfx globals memmap ppu sa1 sa1cpu \
          snapshot sha256 bml fscompat libretro/libretro"
for f in $C_SRCS;  do gcc $F -std=gnu99 -c "$DIR/$f.c"   -o "$OBJ/$(echo $f|tr / _).o"; done
for f in $CXX_SRCS; do g++ $F           -c "$DIR/$f.cpp" -o "$OBJ/$(echo $f|tr / _).o"; done

# Host-only stubs for the file/zip/VFS symbols (ROM is fed as a buffer; no files). LoadZip
# is C++ (uint32 == unsigned int on x86-64, unlike the ARM blob's unsigned long).
cat > "$OBJ/host_stubs.cpp" <<'EOF'
typedef unsigned long usize;
extern "C" {
void*rfopen(const char*,const char*){return 0;} int rfclose(void*){return -1;}
long long rftell(void*){return -1;} long long rfseek(void*,long long,int){return -1;}
long long rfread(void*,usize,usize,void*){return 0;} long long rfwrite(const void*,usize,usize,void*){return 0;}
char*rfgets(char*,int,void*){return 0;} int rfgetc(void*){return -1;} int rfputc(int,void*){return -1;}
long long rfflush(void*){return 0;} int rferror(void*){return 0;} int rfeof(void*){return 1;}
int rfprintf(void*,const char*,...){return 0;} int rfscanf(void*,const char*,...){return 0;}
void*filestream_open(const char*,unsigned,unsigned){return 0;} int filestream_close(void*){return -1;}
long long filestream_seek(void*,long long,int){return -1;} long long filestream_read(void*,void*,long long){return -1;}
long long filestream_write(void*,const void*,long long){return -1;} long long filestream_tell(void*){return -1;}
long long filestream_read_file(const char*,void**b,long long*l){if(b)*b=0;if(l)*l=0;return 0;}
void vfs_hybrid_init(void*,void*){}
int zip_archive_open(void*,const char*){return -1;} void zip_archive_close(void*){}
int zip_find_name(const void*,const char*,int){return -1;} int zip_find_ext(const void*,const char*,int){return -1;}
int zip_find_suffix(const void*,const char*,int){return -1;} int zip_file_open(void*,void*,int){return -1;}
void zip_file_close(void*){} unsigned zip_file_size(const void*){return 0;}
unsigned zip_file_read(void*,unsigned,unsigned char*,unsigned){return 0;}
unsigned char*zip_read_entry(void*,int,unsigned*l){if(l)*l=0;return 0;}
}
unsigned char LoadZip(const char*,unsigned int*,unsigned char*){return 0;}
EOF
g++ $F -c "$OBJ/host_stubs.cpp" -o "$OBJ/host_stubs.o"

g++ $F "$DIR/host_test.cpp" "$OBJ"/*.o -o /tmp/s9x_host_test -lm
echo "built /tmp/s9x_host_test - run: /tmp/s9x_host_test <rom.sfc> [frames]"
