/* Host test for gba_boxart_decode. Build: gcc gba_boxart_test.c gba_boxart.c -o t && ./t */
#include <stdio.h>
#include <string.h>
#include "gba_boxart.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void put_hdr(unsigned char *b, unsigned w, unsigned h)
{
	b[0]='G'; b[1]='A'; b[2]='R'; b[3]='T';
	b[4]=1; b[5]=0;              /* version */
	b[6]=0; b[7]=0;              /* format 0 = RGB565 */
	b[8]=w&0xff; b[9]=(w>>8)&0xff;
	b[10]=h&0xff; b[11]=(h>>8)&0xff;
}

int main(void)
{
	unsigned char art[12 + 2*2*3*2];   /* 2x3 image */
	unsigned char dst[2*3*3 + 8];
	unsigned short w=0, h=0;
	int i, rc;

	/* valid 2x3 tile: fill 565 payload with a recognizable ramp */
	put_hdr(art, 2, 3);
	for (i = 0; i < 2*3; i++) { art[12+i*2+0] = (unsigned char)(i*2); art[12+i*2+1] = (unsigned char)(0x80+i); }

	rc = gba_boxart_decode(art, sizeof art, dst, sizeof dst, &w, &h);
	CHECK(rc == 0); CHECK(w == 2); CHECK(h == 3);
	/* each pixel expanded to 3 bytes: 565 low, 565 high, 0xFF alpha */
	for (i = 0; i < 2*3; i++) {
		CHECK(dst[i*3+0] == (unsigned char)(i*2));
		CHECK(dst[i*3+1] == (unsigned char)(0x80+i));
		CHECK(dst[i*3+2] == 0xFF);
	}

	/* bad magic */
	art[0] = 'X';
	CHECK(gba_boxart_decode(art, sizeof art, dst, sizeof dst, &w, &h) == -2);
	art[0] = 'G';

	/* wrong format */
	art[6] = 1;
	CHECK(gba_boxart_decode(art, sizeof art, dst, sizeof dst, &w, &h) == -3);
	art[6] = 0;

	/* truncated payload */
	CHECK(gba_boxart_decode(art, 12 + 2, dst, sizeof dst, &w, &h) == -5);

	/* dst too small */
	CHECK(gba_boxart_decode(art, sizeof art, dst, 5, &w, &h) == -6);

	/* too-short blob / nulls */
	CHECK(gba_boxart_decode(art, 8, dst, sizeof dst, &w, &h) == -1);
	CHECK(gba_boxart_decode(0, sizeof art, dst, sizeof dst, &w, &h) == -1);

	printf(fails ? "boxart decode: %d FAILURES\n" : "boxart decode: all OK\n", fails);
	return fails ? 1 : 0;
}
