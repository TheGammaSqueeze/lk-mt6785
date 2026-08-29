/*
 * ROM-select navigation math, factored out so it has a single source of truth
 * and can be host-tested (the selector UI itself only runs on-device). Pure
 * integer logic: cursor move with wrap, and scroll-window clamp for a list that
 * may be longer OR shorter than the visible row count.
 */
#ifndef ROM_SELECT_NAV_H
#define ROM_SELECT_NAV_H

/* Move the cursor within [0,n): wraps at both ends. Empty list stays at 0. */
static inline int rs_move(int sel, int n, int up, int down)
{
	if (n <= 0) return 0;
	if (up)   sel = (sel - 1 + n) % n;
	if (down) sel = (sel + 1) % n;
	return sel;
}

/* Recompute the top-of-window row so `sel` stays visible in `rows` rows over a
 * list of `n` entries. Handles n < rows (clamps top to 0) and keeps top in
 * [0, max(0, n-rows)]. */
static inline int rs_scroll(int top, int sel, int rows, int n)
{
	if (sel < top)          top = sel;
	if (sel >= top + rows)  top = sel - rows + 1;
	if (top > n - rows)     top = n - rows;
	if (top < 0)            top = 0;
	return top;
}

#endif
