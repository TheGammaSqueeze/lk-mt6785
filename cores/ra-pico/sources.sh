# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 TheGammaSqueeze
# ra-pico: the rcheevos sources the blob and the PC harness build, and how. Sourced by build.sh and
# tools/harness.sh. rcheevos comes from the copy RetroArch carries (with libretro-common's MD5, which
# RetroArch's rhash/md5.h points at), already in the tree under the Armulators app (RCHEEVOS_DEPS).
RCHEEVOS_DEPS=${RCHEEVOS_DEPS:-$ROOT/../armNES-pico/upstream/armulators/deps}
RC=$RCHEEVOS_DEPS/rcheevos
LRC=$RCHEEVOS_DEPS/libretro-common
if [ ! -f "$RC/src/rc_client.c" ]; then
	echo "rcheevos not found at $RC (git submodule update --init in cores/armNES-pico?)" >&2
	exit 1
fi
# no threads (the LK calls the blob from one thread at a time), hashing from a buffer for cartridge
# systems only: no discs, archives or encrypted images
RC_DEFS="-DRC_NO_THREADS -DRC_CLIENT_SUPPORTS_HASH -DRC_HASH_NO_DISC -DRC_HASH_NO_ZIP -DRC_HASH_NO_ENCRYPTED"
RC_INC="-I$RC/include -I$RC/src -I$LRC/include"
RC_SRCS="src/rc_client.c src/rc_compat.c src/rc_util.c src/rc_version.c src/rc_libretro.c
	src/rapi/rc_api_common.c src/rapi/rc_api_info.c src/rapi/rc_api_runtime.c src/rapi/rc_api_user.c
	src/rcheevos/alloc.c src/rcheevos/condition.c src/rcheevos/condset.c src/rcheevos/consoleinfo.c
	src/rcheevos/format.c src/rcheevos/lboard.c src/rcheevos/memref.c src/rcheevos/operand.c
	src/rcheevos/richpresence.c src/rcheevos/runtime.c src/rcheevos/runtime_progress.c
	src/rcheevos/trigger.c src/rcheevos/value.c
	src/rhash/hash.c src/rhash/hash_rom.c"
