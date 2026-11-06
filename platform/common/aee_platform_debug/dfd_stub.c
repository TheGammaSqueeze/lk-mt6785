/*
 * Konkr Pocket Advance: the DFD decoder (dfd.c) is left out of the LK (see rules.mk): its
 * register tables made a quarter of the image the preloader reads on every boot. These stand
 * ins report that there is no DFD dump, so the latch dump after a kernel crash takes its
 * ordinary path; nothing else changes. The Android kernel's own crash records are unaffected.
 */
#include <sys/types.h>

typedef unsigned long long u64_t;
struct reg_collector;

u64_t dfd_decode(const u64_t *raw, const struct reg_collector *collector, const int count_header_row)
{
	(void)raw; (void)collector; (void)count_header_row;
	return 0;
}
unsigned int dfd_get_decoded_return_stack(char *buf, unsigned int max_buf_size) { (void)buf; (void)max_buf_size; return 0; }
unsigned int get_efuse_dfd_disabled(void) { return 1; }
unsigned int dfd_internal_dump_before_reboot(void) { return 0; }
unsigned int dfd_internal_dump_get_decoded_lastpc(char *buf, unsigned int max_buf_size) { (void)buf; (void)max_buf_size; return 0; }
int dfd_get(void **data, int *len) { (void)data; if (len) *len = 0; return 0; }
void dfd_put(void **data) { (void)data; }
