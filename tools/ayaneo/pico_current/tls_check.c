/*
 * tls_check.c - the Pico's HTTPS client (pico_tls, pico_pki, pico_crypto) on the PC, against a real
 * server, trusting the same roots the console's card carries (make_roots.py): does the handshake
 * hold, is the server's chain proven, and what answers. For a server the console has not met yet.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 TheGammaSqueeze
 *
 *   cc -O1 -I platform/common/pico tools/ayaneo/pico_current/tls_check.c platform/common/pico/pico_tls.c \
 *      platform/common/pico/pico_pki.c platform/common/pico/pico_crypto.c -o /tmp/tls_check
 *   /tmp/tls_check retroachievements.org /dorequest.php roots.der ["r=..." form to POST]
 *
 * Exit status 0 when the connection was proven and an HTTP answer came.
 */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "pico_tls.h"

static int sock_send(void *ctx, const uint8_t *d, unsigned int n)
{
	int fd = *(int *)ctx;
	ssize_t r = send(fd, d, n, 0);
	return r < 0 ? -1 : (int)r;
}

static int sock_recv(void *ctx, uint8_t *b, unsigned int cap)
{
	int fd = *(int *)ctx;
	ssize_t r = recv(fd, b, cap, MSG_DONTWAIT);
	if (r == 0) return -1;					/* closed */
	if (r < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
	return (int)r;
}

static int dial(const char *host)
{
	struct addrinfo hints, *ai, *p;
	int fd = -1;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, "443", &hints, &ai) != 0) return -1;
	for (p = ai; p; p = p->ai_next) {
		fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (fd >= 0 && connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
		if (fd >= 0) close(fd);
		fd = -1;
	}
	freeaddrinfo(ai);
	return fd;
}

int main(int argc, char **argv)
{
	static struct pico_tls t;
	static uint8_t roots[256 * 1024], ans[1 << 20];
	struct pico_tls_io io;
	struct pico_tls_trust trust;
	uint8_t ent[64];
	char req[2048];
	const char *host, *path, *form;
	unsigned int got = 0;
	long n;
	int fd, i, st;
	FILE *f;
	time_t t0;
	if (argc < 4) { fprintf(stderr, "usage: %s host path roots.der [form]\n", argv[0]); return 2; }
	host = argv[1]; path = argv[2]; form = argc > 4 ? argv[4] : 0;
	f = fopen(argv[3], "rb");
	if (!f) { perror(argv[3]); return 2; }
	n = (long)fread(roots, 1, sizeof roots, f);
	fclose(f);
	f = fopen("/dev/urandom", "rb");
	if (!f || fread(ent, 1, sizeof ent, f) != sizeof ent) { fprintf(stderr, "no random\n"); return 2; }
	fclose(f);
	fd = dial(host);
	if (fd < 0) { fprintf(stderr, "no connection to %s\n", host); return 1; }
	io.send = sock_send; io.recv = sock_recv; io.ctx = &fd;
	trust.roots = roots; trust.roots_len = (unsigned)n; trust.now = (long long)time(0);
	pico_tls_start(&t, &io, host, ent, &trust);
	t0 = time(0);
	while ((st = pico_tls_step(&t)) == PICO_TLS_HANDSHAKE && time(0) - t0 < 20) usleep(1000);
	printf("handshake: %s, TLS 1.%d, suite %04x, verified %d%s%s\n", st == PICO_TLS_OPEN ? "open" : "failed",
	       t.v12 ? 2 : 3, t.suite, t.verified, st == PICO_TLS_OPEN ? "" : ", ", st == PICO_TLS_OPEN ? "" : (t.why ? t.why : "?"));
	if (st != PICO_TLS_OPEN) return 1;
	if (form)
		snprintf(req, sizeof req, "POST %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: GammaOS-Pico/1.0.0 tls_check\r\n"
			 "Connection: close\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: %u\r\n\r\n%s",
			 path, host, (unsigned)strlen(form), form);
	else
		snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: GammaOS-Pico/1.0.0 tls_check\r\n"
			 "Connection: close\r\n\r\n", path, host);
	pico_tls_send(&t, (const uint8_t *)req, (unsigned)strlen(req));
	t0 = time(0);
	while (time(0) - t0 < 20) {
		int r = pico_tls_recv(&t, ans + got, (unsigned)(sizeof ans - 1 - got));
		if (r > 0) got += (unsigned)r;
		else if (r < 0) break;
		else usleep(1000);
	}
	ans[got] = 0;
	for (i = 0; i < (int)got && ans[i] != '\r'; i++) ;
	printf("answer: %u bytes, %.*s\n", got, i, (const char *)ans);
	{	/* the body's start */
		const char *b = strstr((const char *)ans, "\r\n\r\n");
		if (b) printf("body: %.200s\n", b + 4);
	}
	pico_tls_close(&t);
	close(fd);
	return t.verified && got ? 0 : 1;
}
