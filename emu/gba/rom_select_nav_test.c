/* Host test for the ROM-select navigation math (rs_move / rs_scroll). Pins the
 * edge cases that only otherwise show up on-device: wrap at both ends, scroll
 * clamp for lists longer than the window, and lists shorter than the window. */
#include <stdio.h>
#include "rom_select_nav.h"

static int fails;
static void ck(int got, int want, const char *what)
{
	if (got != want) { printf("  FAIL %s: got %d want %d\n", what, got, want); fails++; }
}

int main(void)
{
	int top;

	/* move: wrap up from 0 -> n-1, wrap down from n-1 -> 0 */
	ck(rs_move(0, 5, 1, 0), 4, "up-wrap");
	ck(rs_move(4, 5, 0, 1), 0, "down-wrap");
	ck(rs_move(2, 5, 1, 0), 1, "up-mid");
	ck(rs_move(2, 5, 0, 1), 3, "down-mid");
	ck(rs_move(0, 0, 0, 1), 0, "empty-list");
	ck(rs_move(0, 1, 1, 0), 0, "single-up");
	ck(rs_move(0, 1, 0, 1), 0, "single-down");

	/* scroll: long list (n=20, rows=5) */
	ck(rs_scroll(0, 0, 5, 20), 0, "long-top");
	ck(rs_scroll(0, 4, 5, 20), 0, "long-lastvisible");
	ck(rs_scroll(0, 5, 5, 20), 1, "long-scrolldown1");
	ck(rs_scroll(0, 19, 5, 20), 15, "long-bottom");     /* top clamps to n-rows */
	top = rs_scroll(15, 0, 5, 20);
	ck(top, 0, "long-jumptop");                          /* wrap to first shows top=0 */
	top = rs_scroll(0, 19, 5, 20);
	ck(top, 15, "long-jumpbottom");                      /* wrap to last shows last page */

	/* scroll: list shorter than the window (n=3, rows=10) must never go negative */
	ck(rs_scroll(0, 0, 10, 3), 0, "short-top");
	ck(rs_scroll(0, 2, 10, 3), 0, "short-bottom");
	ck(rs_scroll(0, 1, 10, 3), 0, "short-mid");

	/* scroll: exactly full window (n==rows) */
	ck(rs_scroll(0, 4, 5, 5), 0, "exact-bottom");

	printf(fails ? "ROM_SELECT_NAV TEST: %d FAIL\n" : "ROM_SELECT_NAV TEST: PASS\n", fails);
	return fails ? 1 : 0;
}
