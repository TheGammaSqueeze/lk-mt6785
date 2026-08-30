/* Shared GBA ROM display-name cleanup, used by BOTH the on-device driver
 * (emu/gba/gba_snes_menu.c) and the host validation harness (host_render.c) so the
 * off-device render reflects the real on-device title (strips a trailing ".gba" and
 * any trailing No-Intro tag groups like " (USA, Australia)" / " [!]"). Keep this the
 * single source of truth: a title-cleanup change must show up in host validation. */
#ifndef GBA_NAME_H
#define GBA_NAME_H

/* Write the cleaned display name of `nm` into `dst` (must hold >=128 bytes). */
static void gba_clean_name(const char *nm, char *dst)
{
	int L = 0;
	while (nm[L] && L < 127) { dst[L] = nm[L]; L++; }
	dst[L] = 0;
	if (L >= 4 && dst[L-4] == '.' && (dst[L-3]|32) == 'g' &&
	    (dst[L-2]|32) == 'b' && (dst[L-1]|32) == 'a') { L -= 4; dst[L] = 0; }
	for (;;) {
		int e = L, c;
		while (e > 0 && dst[e-1] == ' ') e--;          /* trailing spaces */
		if (e <= 0) break;
		c = dst[e-1];
		if (c == ')' || c == ']') {                    /* a trailing tag group */
			int open = (c == ')') ? '(' : '[', j = e - 1, depth = 0;
			while (j >= 0) {
				if (dst[j] == c) depth++;
				else if (dst[j] == open && --depth == 0) break;
				j--;
			}
			if (j <= 0) { L = e; break; }          /* no opener / whole name */
			L = j; dst[L] = 0;                     /* cut before the group */
		} else { L = e; dst[L] = 0; break; }
	}
	if (L == 0) { dst[0] = 0; }                            /* keep at least "" */
}

#endif /* GBA_NAME_H */
