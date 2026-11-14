/*
 * pico_fold.c - games' names in UTF-8 compared and shown without their accents (pico_fold.h).
 */
#include "pico_fold.h"
#include "fat_ro.h"                  /* fat_u8_next */

uint32_t pico_utf8_next(const char **ps)
{
	return fat_u8_next(ps);
}

/* The plain letter of each code point, in blocks: a '*' is a character that is not a letter and
 * stays as it is. Latin-1 Supplement from U+00C0 (the letters after the symbols). */
static const char k_latin1[64] =
	"AAAAAAACEEEEIIII" "DNOOOOO*OUUUUYTs" "aaaaaaaceeeeiiii" "dnooooo*ouuuuyty";
/* Latin Extended-A, U+0100..U+017F (capital and small letters alternate) */
static const char k_ext_a[128] =
	"AaAaAaCcCcCcCcDd" "DdEeEeEeEeEeGgGg" "GgGgHhHhIiIiIiIi" "IiIiJjKkkLlLlLlL"
	"lLlNnNnNnnNnOoOo" "OoOoRrRrRrSsSsSs" "SsTtTtTtUuUuUuUu" "UuUuWwYyYZzZzZzs";
/* Latin Extended Additional, U+1E00..U+1E95: one letter for each capital and small pair */
static const char k_ext_add_pairs[75] =
	"ABBBCDDDDDEEEEEFGHHHHHIIKKKLLLLMMMNNNNOOOOPPRRRRSSSSSTTTTUUUUUVVWWWWWXXYZZZ";
/* U+1E96..U+1E9F, single letters */
static const char k_ext_add_single[10] = { 'h', 't', 'w', 'y', 'a', 's', 's', 's', 'S', 'd' };
/* U+1EA0..U+1EF9, the Vietnamese letters: pairs of A, E, I, O, U and Y in runs */
static const struct { uint16_t last; char base; } k_viet[] = {
	{ 0x1EB7, 'A' }, { 0x1EC7, 'E' }, { 0x1ECB, 'I' }, { 0x1EE3, 'O' }, { 0x1EF1, 'U' }, { 0x1EF9, 'Y' },
};
/* U+1EFA..U+1EFF */
static const char k_ext_add_tail[6] = { 'L', 'l', 'V', 'v', 'Y', 'y' };
/* the letters of Latin Extended-B that names use (Vietnamese, Romanian, pinyin and the like) */
static const struct { uint16_t cp; char base; } k_ext_b[] = {
	{ 0x0180, 'b' }, { 0x0197, 'I' }, { 0x01A0, 'O' }, { 0x01A1, 'o' }, { 0x01AF, 'U' }, { 0x01B0, 'u' },
	{ 0x01B5, 'Z' }, { 0x01B6, 'z' }, { 0x01C4, 'D' }, { 0x01C5, 'D' }, { 0x01C6, 'd' }, { 0x01C7, 'L' },
	{ 0x01C8, 'L' }, { 0x01C9, 'l' }, { 0x01CA, 'N' }, { 0x01CB, 'N' }, { 0x01CC, 'n' },
	{ 0x01CD, 'A' }, { 0x01CE, 'a' }, { 0x01CF, 'I' }, { 0x01D0, 'i' }, { 0x01D1, 'O' }, { 0x01D2, 'o' },
	{ 0x01D3, 'U' }, { 0x01D4, 'u' }, { 0x01D5, 'U' }, { 0x01D6, 'u' }, { 0x01D7, 'U' }, { 0x01D8, 'u' },
	{ 0x01D9, 'U' }, { 0x01DA, 'u' }, { 0x01DB, 'U' }, { 0x01DC, 'u' }, { 0x01DE, 'A' }, { 0x01DF, 'a' },
	{ 0x01E0, 'A' }, { 0x01E1, 'a' }, { 0x01E2, 'A' }, { 0x01E3, 'a' }, { 0x01E4, 'G' }, { 0x01E5, 'g' },
	{ 0x01E6, 'G' }, { 0x01E7, 'g' }, { 0x01E8, 'K' }, { 0x01E9, 'k' }, { 0x01EA, 'O' }, { 0x01EB, 'o' },
	{ 0x01EC, 'O' }, { 0x01ED, 'o' }, { 0x01F0, 'j' }, { 0x01F1, 'D' }, { 0x01F2, 'D' }, { 0x01F3, 'd' },
	{ 0x01F4, 'G' }, { 0x01F5, 'g' }, { 0x01F8, 'N' },
	{ 0x01F9, 'n' }, { 0x01FA, 'A' }, { 0x01FB, 'a' }, { 0x01FC, 'A' }, { 0x01FD, 'a' }, { 0x01FE, 'O' },
	{ 0x01FF, 'o' }, { 0x0200, 'A' }, { 0x0201, 'a' }, { 0x0202, 'A' }, { 0x0203, 'a' }, { 0x0204, 'E' },
	{ 0x0205, 'e' }, { 0x0206, 'E' }, { 0x0207, 'e' }, { 0x0208, 'I' }, { 0x0209, 'i' }, { 0x020A, 'I' },
	{ 0x020B, 'i' }, { 0x020C, 'O' }, { 0x020D, 'o' }, { 0x020E, 'O' }, { 0x020F, 'o' }, { 0x0210, 'R' },
	{ 0x0211, 'r' }, { 0x0212, 'R' }, { 0x0213, 'r' }, { 0x0214, 'U' }, { 0x0215, 'u' }, { 0x0216, 'U' },
	{ 0x0217, 'u' }, { 0x0218, 'S' }, { 0x0219, 's' }, { 0x021A, 'T' }, { 0x021B, 't' }, { 0x021E, 'H' },
	{ 0x021F, 'h' }, { 0x0226, 'A' }, { 0x0227, 'a' }, { 0x0228, 'E' }, { 0x0229, 'e' }, { 0x022A, 'O' },
	{ 0x022B, 'o' }, { 0x022C, 'O' }, { 0x022D, 'o' }, { 0x022E, 'O' }, { 0x022F, 'o' }, { 0x0230, 'O' },
	{ 0x0231, 'o' }, { 0x0232, 'Y' }, { 0x0233, 'y' },
};

uint32_t pico_fold_cp(uint32_t cp)
{
	char b = 0;
	if (cp < 0x80) return cp;
	if (cp >= 0x0300 && cp <= 0x036F) return 0;                   /* a combining mark: dropped */
	if (cp >= 0xFF01 && cp <= 0xFF5E) return cp - 0xFEE0;          /* fullwidth ASCII */
	if (cp >= 0x00C0 && cp <= 0x00FF) b = k_latin1[cp - 0xC0];
	else if (cp >= 0x0100 && cp <= 0x017F) b = k_ext_a[cp - 0x100];
	else if (cp >= 0x0180 && cp <= 0x0233) {
		int lo = 0, hi = (int)(sizeof k_ext_b / sizeof k_ext_b[0]) - 1;
		while (lo <= hi) {
			int mid = (lo + hi) / 2;
			if (k_ext_b[mid].cp == cp) { b = k_ext_b[mid].base; break; }
			if (k_ext_b[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
		}
	} else if (cp >= 0x1E00 && cp <= 0x1E95) {
		b = k_ext_add_pairs[(cp - 0x1E00) / 2];
		if (cp & 1) b = (char)(b + 32);                            /* the small letter of the pair */
	} else if (cp >= 0x1E96 && cp <= 0x1E9F) b = k_ext_add_single[cp - 0x1E96];
	else if (cp >= 0x1EA0 && cp <= 0x1EF9) {
		unsigned i;
		for (i = 0; i < sizeof k_viet / sizeof k_viet[0] && cp > k_viet[i].last; i++) ;
		b = k_viet[i].base;
		if (cp & 1) b = (char)(b + 32);
	} else if (cp >= 0x1EFA && cp <= 0x1EFF) b = k_ext_add_tail[cp - 0x1EFA];
	return b && b != '*' ? (uint32_t)(unsigned char)b : cp;
}

int pico_fold_ascii(const char *utf8, char *out, int cap)
{
	int o = 0;
	if (cap < 1) return 0;
	while (*utf8 && o < cap - 1) {
		uint32_t c = pico_fold_cp(pico_utf8_next(&utf8));
		if (c == 0) continue;
		out[o++] = c < 0x80 ? (char)c : '_';
	}
	out[o] = 0;
	return o;
}

/* the next folded code point to compare (marks skipped, ASCII letters small), 0 at the end */
static uint32_t cmp_next(const char **ps)
{
	while (**ps) {
		uint32_t c = pico_fold_cp(pico_utf8_next(ps));
		if (c == 0) continue;
		if (c >= 'A' && c <= 'Z') c += 32;
		return c;
	}
	return 0;
}

int pico_fold_cmp(const char *a, const char *b)
{
	for (;;) {
		uint32_t x = cmp_next(&a), y = cmp_next(&b);
		if (x != y) return x < y ? -1 : 1;
		if (!x) return 0;
	}
}
