/*
 * pico_net.c - see pico_net.h. Written for GammaOS from the protocol RFCs (826 ARP, 791 IPv4,
 * 792 ICMP, 768 UDP, 793/9293 TCP, 6298 TCP timers, 2018 SACK, 7323 window scaling, 2131/2132 DHCP, 1035 DNS, 4330 SNTP).
 * Everything is static; one frame is built at a time.
 */
#include "pico_net.h"
#include <string.h>

#define ETH_HDR		14
#define ETH_IP		0x0800
#define ETH_ARP		0x0806
#define NET_MTU		1500
#define IP_HDR		20
#define UDP_HDR		8
#define PROTO_ICMP	1
#define PROTO_UDP	17
#define PROTO_TCP	6
#define TCP_HDR		20
#define TCP_MSS		(NET_MTU - IP_HDR - TCP_HDR)
#define ARP_SLOTS	8
#define UDP_SLOTS	8
#define PING_SLOTS	16
#define DHCP_CLIENT	68
#define DHCP_SERVER	67
#define DNS_PORT	53
#define DNS_OUR_PORT	49853
#define PING_ID		0x4750			/* "GP" */
#define NTP_PORT	123
#define NTP_OUR_PORT	49123
#define NTP_1900_TO_1970 2208988800u		/* seconds from 1900 (NTP's start) to 1970 */

static struct pico_net_link s_link;
static int s_on;
static struct pico_net_conf s_conf;
static int s_have_ip;
static struct pico_net_stats s_st;
static uint16_t s_ip_id;

static uint8_t s_frame[ETH_HDR + NET_MTU];	/* the frame being built */
static uint8_t s_body[NET_MTU];			/* the payload of the packet being built */

/* ---- byte order, checksum ---- */

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v >> 16); put16(p + 2, v); }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t get32(const uint8_t *p) { return get16(p) << 16 | get16(p + 2); }

/* the Internet checksum's running sum over len bytes (RFC 1071), folded at the end */
static uint32_t sum_add(uint32_t sum, const uint8_t *p, unsigned int len)
{
	for (; len > 1; p += 2, len -= 2)
		sum += get16(p);
	if (len)
		sum += (uint32_t)p[0] << 8;
	return sum;
}
static uint16_t sum_end(uint32_t sum)
{
	while (sum >> 16)
		sum = (sum & 0xFFFF) + (sum >> 16);
	return (uint16_t)~sum;
}

static uint32_t now(void) { return s_link.now_ms ? s_link.now_ms() : 0; }

void pico_net_ip_text(uint32_t ip, char *buf)
{
	int i, n = 0;
	for (i = 3; i >= 0; i--) {
		unsigned int b = (ip >> (8 * i)) & 0xFF;
		if (b >= 100) buf[n++] = (char)('0' + b / 100);
		if (b >= 10) buf[n++] = (char)('0' + b / 10 % 10);
		buf[n++] = (char)('0' + b % 10);
		if (i) buf[n++] = '.';
	}
	buf[n] = 0;
}

/* ---- Ethernet ---- */

static const uint8_t s_everyone[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* s_frame's payload (len bytes, already in place) out to mac as type */
static int eth_send(const uint8_t *mac, uint32_t type, unsigned int len)
{
	if (!s_on || !s_link.send)
		return -1;
	memcpy(s_frame, mac, 6);
	memcpy(s_frame + 6, s_link.mac, 6);
	put16(s_frame + 12, type);
	s_st.out++;
	return s_link.send(s_frame, ETH_HDR + len);
}

/* ---- ARP ---- */

static struct { uint32_t ip; uint8_t mac[6]; } s_arp[ARP_SLOTS];
static unsigned int s_arp_next;
/* one IP packet waiting for its next hop's address */
static uint8_t s_wait[ETH_HDR + NET_MTU];
static unsigned int s_wait_len;
static uint32_t s_wait_hop, s_wait_ms, s_wait_tries;

static const uint8_t *arp_find(uint32_t ip)
{
	unsigned int i;
	for (i = 0; i < ARP_SLOTS; i++)
		if (s_arp[i].ip == ip && ip)
			return s_arp[i].mac;
	return 0;
}

static void arp_learn(uint32_t ip, const uint8_t *mac)
{
	unsigned int i;
	if (!ip) return;
	for (i = 0; i < ARP_SLOTS; i++)
		if (s_arp[i].ip == ip) { memcpy(s_arp[i].mac, mac, 6); return; }
	s_arp[s_arp_next].ip = ip;
	memcpy(s_arp[s_arp_next].mac, mac, 6);
	s_arp_next = (s_arp_next + 1) % ARP_SLOTS;
}

/* an ARP packet: op 1 (who has target_ip) or 2 (we have it) */
static int arp_send(uint32_t op, const uint8_t *to_mac, uint32_t target_ip, const uint8_t *target_mac)
{
	uint8_t *a = s_frame + ETH_HDR;
	put16(a, 1);				/* Ethernet */
	put16(a + 2, ETH_IP);
	a[4] = 6; a[5] = 4;
	put16(a + 6, op);
	memcpy(a + 8, s_link.mac, 6);
	put32(a + 14, s_have_ip ? s_conf.ip : 0);
	if (target_mac) memcpy(a + 18, target_mac, 6); else memset(a + 18, 0, 6);
	put32(a + 24, target_ip);
	return eth_send(to_mac, ETH_ARP, 28);
}

static void arp_input(const uint8_t *a, unsigned int len)
{
	uint32_t op, sender, target;
	if (len < 28 || get16(a) != 1 || get16(a + 2) != ETH_IP || a[4] != 6 || a[5] != 4)
		return;
	s_st.arp_in++;
	op = get16(a + 6);
	sender = get32(a + 14);
	target = get32(a + 24);
	arp_learn(sender, a + 8);
	if (op == 1 && s_have_ip && target == s_conf.ip)
		arp_send(2, a + 8, sender, a + 8);
	if (s_wait_len && sender == s_wait_hop) {	/* the waiting packet can go */
		memcpy(s_wait, a + 8, 6);
		s_st.out++;
		s_link.send(s_wait, s_wait_len);
		s_wait_len = 0;
	}
}

/* ---- IPv4 ---- */

/* s_body (len bytes) to dst as protocol proto: 0 sent or waiting for ARP */
static int ip_send(uint32_t dst, uint32_t proto, unsigned int len)
{
	uint8_t *h = s_frame + ETH_HDR;
	uint32_t hop;
	const uint8_t *mac;
	if (len > NET_MTU - IP_HDR)
		return -1;
	memset(h, 0, IP_HDR);
	h[0] = 0x45;
	put16(h + 2, IP_HDR + len);
	put16(h + 4, s_ip_id++);
	put16(h + 6, 0x4000);			/* don't fragment */
	h[8] = 64;
	h[9] = (uint8_t)proto;
	put32(h + 12, s_have_ip ? s_conf.ip : 0);
	put32(h + 16, dst);
	put16(h + 10, sum_end(sum_add(0, h, IP_HDR)));
	memcpy(h + IP_HDR, s_body, len);
	if (dst == 0xFFFFFFFFu || (s_have_ip && dst == (s_conf.ip | ~s_conf.mask)))
		return eth_send(s_everyone, ETH_IP, IP_HDR + len);
	if (!s_have_ip)
		return -1;
	hop = (dst & s_conf.mask) == (s_conf.ip & s_conf.mask) ? dst : s_conf.gateway;
	if ((mac = arp_find(hop)) != 0)
		return eth_send(mac, ETH_IP, IP_HDR + len);
	/* not known yet: the packet waits (a newer one replaces it) and ARP asks */
	memcpy(s_wait, s_frame, ETH_HDR + IP_HDR + len);
	memcpy(s_wait + 6, s_link.mac, 6);
	put16(s_wait + 12, ETH_IP);
	s_wait_len = ETH_HDR + IP_HDR + len;
	s_wait_hop = hop;
	s_wait_ms = now();
	s_wait_tries = 1;
	return arp_send(1, s_everyone, hop, 0);
}

/* ---- ICMP ---- */

static struct { uint16_t seq; uint32_t sent; int rtt; } s_ping[PING_SLOTS];

int pico_net_ping(uint32_t ip, uint16_t seq)
{
	unsigned int i, n = 8 + 32;
	s_body[0] = 8; s_body[1] = 0;
	put16(s_body + 2, 0);
	put16(s_body + 4, PING_ID);
	put16(s_body + 6, seq);
	for (i = 8; i < n; i++)
		s_body[i] = (uint8_t)('a' + (i - 8) % 23);
	put16(s_body + 2, sum_end(sum_add(0, s_body, n)));
	s_ping[seq % PING_SLOTS].seq = seq;
	s_ping[seq % PING_SLOTS].sent = now();
	s_ping[seq % PING_SLOTS].rtt = -1;
	return ip_send(ip, PROTO_ICMP, n);
}

int pico_net_ping_answer(uint16_t seq)
{
	return s_ping[seq % PING_SLOTS].seq == seq ? s_ping[seq % PING_SLOTS].rtt : -1;
}

static void icmp_input(uint32_t from, const uint8_t *p, unsigned int len)
{
	if (len < 8 || sum_end(sum_add(0, p, len)) != 0)
		return;
	if (p[0] == 8 && len <= NET_MTU - IP_HDR) {	/* an echo request: answered with the same data */
		memcpy(s_body, p, len);
		s_body[0] = 0;
		put16(s_body + 2, 0);
		put16(s_body + 2, sum_end(sum_add(0, s_body, len)));
		ip_send(from, PROTO_ICMP, len);
	} else if (p[0] == 0 && get16(p + 4) == PING_ID) {
		uint16_t seq = (uint16_t)get16(p + 6);
		if (s_ping[seq % PING_SLOTS].seq == seq && s_ping[seq % PING_SLOTS].rtt < 0)
			s_ping[seq % PING_SLOTS].rtt = (int)(now() - s_ping[seq % PING_SLOTS].sent);
	}
}

/* ---- UDP ---- */

static struct { uint16_t port; pico_udp_fn fn; } s_udp[UDP_SLOTS];

int pico_net_udp_listen(uint16_t port, pico_udp_fn fn)
{
	unsigned int i;
	for (i = 0; i < UDP_SLOTS; i++)
		if (s_udp[i].port == port) { s_udp[i].fn = fn; if (!fn) s_udp[i].port = 0; return 0; }
	if (!fn) return 0;
	for (i = 0; i < UDP_SLOTS; i++)
		if (!s_udp[i].port) { s_udp[i].port = port; s_udp[i].fn = fn; return 0; }
	return -1;
}

static uint16_t udp_sum(uint32_t src, uint32_t dst, const uint8_t *u, unsigned int len)
{
	uint8_t ph[12];
	uint16_t c;
	put32(ph, src);
	put32(ph + 4, dst);
	ph[8] = 0; ph[9] = PROTO_UDP;
	put16(ph + 10, len);
	c = sum_end(sum_add(sum_add(0, ph, 12), u, len));
	return c ? c : 0xFFFF;
}

int pico_net_udp_send(uint32_t ip, uint16_t to_port, uint16_t from_port, const void *data, unsigned int len)
{
	if (len > NET_MTU - IP_HDR - UDP_HDR)
		return -1;
	memmove(s_body + UDP_HDR, data, len);
	put16(s_body, from_port);
	put16(s_body + 2, to_port);
	put16(s_body + 4, UDP_HDR + len);
	put16(s_body + 6, 0);
	put16(s_body + 6, udp_sum(s_have_ip ? s_conf.ip : 0, ip, s_body, UDP_HDR + len));
	return ip_send(ip, PROTO_UDP, UDP_HDR + len);
}

static void dhcp_input(const uint8_t *d, unsigned int len);
static void tcp_input(uint32_t src, uint32_t dst, const uint8_t *t, unsigned int len);
static void dns_input(const uint8_t *d, unsigned int len);
static void ntp_input(const uint8_t *d, unsigned int len);

static void udp_input(uint32_t src, uint32_t dst, const uint8_t *u, unsigned int len)
{
	uint32_t sport, dport, ulen;
	unsigned int i;
	if (len < UDP_HDR) return;
	sport = get16(u);
	dport = get16(u + 2);
	ulen = get16(u + 4);
	if (ulen < UDP_HDR || ulen > len) return;
	{	/* a checksum given must be right: summed with the pseudo header it comes to zero */
		uint8_t ph[12];
		put32(ph, src); put32(ph + 4, dst); ph[8] = 0; ph[9] = PROTO_UDP; put16(ph + 10, ulen);
		if (get16(u + 6) && sum_end(sum_add(sum_add(0, ph, 12), u, ulen)) != 0)
			return;
	}
	s_st.udp_in++;
	if (dport == DHCP_CLIENT && sport == DHCP_SERVER) { dhcp_input(u + UDP_HDR, ulen - UDP_HDR); return; }
	if (dport == DNS_OUR_PORT && sport == DNS_PORT) { dns_input(u + UDP_HDR, ulen - UDP_HDR); return; }
	if (dport == NTP_OUR_PORT && sport == NTP_PORT) { ntp_input(u + UDP_HDR, ulen - UDP_HDR); return; }
	for (i = 0; i < UDP_SLOTS; i++)
		if (s_udp[i].port == dport && s_udp[i].fn) {
			s_udp[i].fn(src, (uint16_t)sport, u + UDP_HDR, ulen - UDP_HDR);
			return;
		}
}

static void ip_input(const uint8_t *h, unsigned int len)
{
	unsigned int hl, total;
	uint32_t src, dst;
	if (len < IP_HDR || (h[0] >> 4) != 4) return;
	hl = (h[0] & 15u) * 4u;
	total = get16(h + 2);
	if (hl < IP_HDR || total < hl || total > len || sum_end(sum_add(0, h, hl)) != 0) return;
	if (get16(h + 6) & 0x3FFF) { s_st.dropped++; return; }	/* a fragment: not taken */
	src = get32(h + 12);
	dst = get32(h + 16);
	if (s_have_ip && dst != s_conf.ip && dst != 0xFFFFFFFFu && dst != (s_conf.ip | ~s_conf.mask))
		return;
	s_st.ip_in++;
	if (h[9] == PROTO_ICMP && s_have_ip && dst == s_conf.ip)
		icmp_input(src, h + hl, total - hl);
	else if (h[9] == PROTO_UDP)
		udp_input(src, dst, h + hl, total - hl);
	else if (h[9] == PROTO_TCP && s_have_ip && dst == s_conf.ip)
		tcp_input(src, dst, h + hl, total - hl);
}

void pico_net_input(const uint8_t *f, unsigned int len)
{
	uint32_t type;
	if (!s_on || len < ETH_HDR) return;
	s_st.in++;
	if (memcmp(f, s_link.mac, 6) && !(f[0] & 1))
		return;					/* to someone else */
	type = get16(f + 12);
	if (type == ETH_ARP) arp_input(f + ETH_HDR, len - ETH_HDR);
	else if (type == ETH_IP) ip_input(f + ETH_HDR, len - ETH_HDR);
}

/* ---- DHCP ---- */

enum { DHCP_OFF, DHCP_SELECTING, DHCP_REQUESTING, DHCP_BOUND, DHCP_RENEWING };
static int s_dhcp_state;
static uint32_t s_dhcp_xid, s_dhcp_sent_ms, s_dhcp_tries, s_dhcp_offer, s_dhcp_server, s_dhcp_t1;
static char s_dhcp_host[32];

/* a DHCP message of type kind (1 discover, 3 request) */
static void dhcp_send(unsigned int kind)
{
	static uint8_t msg[548];
	unsigned int o = 240, n;
	int renew = s_dhcp_state == DHCP_RENEWING;
	memset(msg, 0, sizeof msg);
	msg[0] = 1; msg[1] = 1; msg[2] = 6;
	put32(msg + 4, s_dhcp_xid);
	put16(msg + 10, renew ? 0 : 0x8000);	/* answer to everyone while we have no address */
	if (renew) put32(msg + 12, s_conf.ip);
	memcpy(msg + 28, s_link.mac, 6);
	msg[236] = 0x63; msg[237] = 0x82; msg[238] = 0x53; msg[239] = 0x63;
	msg[o++] = 53; msg[o++] = 1; msg[o++] = (uint8_t)kind;
	msg[o++] = 61; msg[o++] = 7; msg[o++] = 1; memcpy(msg + o, s_link.mac, 6); o += 6;
	if (kind == 3 && s_dhcp_state == DHCP_REQUESTING) {
		msg[o++] = 50; msg[o++] = 4; put32(msg + o, s_dhcp_offer); o += 4;
		msg[o++] = 54; msg[o++] = 4; put32(msg + o, s_dhcp_server); o += 4;
	}
	n = (unsigned int)strlen(s_dhcp_host);
	if (n) { msg[o++] = 12; msg[o++] = (uint8_t)n; memcpy(msg + o, s_dhcp_host, n); o += n; }
	msg[o++] = 57; msg[o++] = 2; put16(msg + o, 1500); o += 2;
	msg[o++] = 55; msg[o++] = 6;
	msg[o++] = 1; msg[o++] = 3; msg[o++] = 6; msg[o++] = 15; msg[o++] = 51; msg[o++] = 54;
	msg[o++] = 255;
	if (o < 300) o = 300;
	s_dhcp_sent_ms = now();
	if (renew)
		pico_net_udp_send(s_dhcp_server, DHCP_SERVER, DHCP_CLIENT, msg, o);
	else
		pico_net_udp_send(0xFFFFFFFFu, DHCP_SERVER, DHCP_CLIENT, msg, o);
}

static void dhcp_input(const uint8_t *d, unsigned int len)
{
	unsigned int o = 240, kind = 0;
	uint32_t mask = 0, router = 0, dns0 = 0, dns1 = 0, lease = 0, server = 0;
	if (len < 240 || d[0] != 2 || get32(d + 4) != s_dhcp_xid || memcmp(d + 28, s_link.mac, 6) ||
	    d[236] != 0x63 || d[237] != 0x82 || d[238] != 0x53 || d[239] != 0x63)
		return;
	while (o < len && d[o] != 255) {
		unsigned int t = d[o], l;
		if (t == 0) { o++; continue; }
		if (o + 2 > len) break;
		l = d[o + 1];
		if (o + 2 + l > len) break;
		if (t == 53 && l == 1) kind = d[o + 2];
		if (t == 1 && l == 4) mask = get32(d + o + 2);
		if (t == 3 && l >= 4) router = get32(d + o + 2);
		if (t == 6 && l >= 4) dns0 = get32(d + o + 2);
		if (t == 6 && l >= 8) dns1 = get32(d + o + 6);
		if (t == 51 && l == 4) lease = get32(d + o + 2);
		if (t == 54 && l == 4) server = get32(d + o + 2);
		o += 2 + l;
	}
	if (kind == 2) s_st.dhcp_offers++;
	if (kind == 5) s_st.dhcp_acks++;
	if (kind == 2 && s_dhcp_state == DHCP_SELECTING) {		/* an offer: ask for it */
		s_dhcp_offer = get32(d + 16);
		s_dhcp_server = server;
		s_dhcp_state = DHCP_REQUESTING;
		s_dhcp_tries = 0;
		dhcp_send(3);
	} else if (kind == 5 && (s_dhcp_state == DHCP_REQUESTING || s_dhcp_state == DHCP_RENEWING)) {
		s_conf.ip = get32(d + 16);
		s_conf.mask = mask ? mask : 0xFFFFFF00u;
		s_conf.gateway = router;
		s_conf.dns[0] = dns0 ? dns0 : router;
		s_conf.dns[1] = dns1;
		s_conf.server = server ? server : s_dhcp_server;
		s_conf.lease_s = lease ? lease : 3600;
		s_conf.got_ms = now();
		s_dhcp_server = s_conf.server;
		s_dhcp_t1 = s_conf.lease_s / 2;
		s_have_ip = 1;
		s_dhcp_state = DHCP_BOUND;
	} else if (kind == 6) {						/* refused: start again */
		s_have_ip = 0;
		s_dhcp_state = DHCP_SELECTING;
		s_dhcp_tries = 0;
		dhcp_send(1);
	}
}

void pico_net_dhcp(const char *host_name)
{
	unsigned int i;
	strncpy(s_dhcp_host, host_name ? host_name : "", sizeof s_dhcp_host - 1);
	s_dhcp_host[sizeof s_dhcp_host - 1] = 0;
	s_dhcp_xid = now() * 2654435761u;
	for (i = 0; i < 6; i++)
		s_dhcp_xid = (s_dhcp_xid ^ s_link.mac[i]) * 16777619u;
	s_have_ip = 0;
	s_dhcp_state = DHCP_SELECTING;
	s_dhcp_tries = 0;
	dhcp_send(1);
}

int pico_net_up(struct pico_net_conf *conf)
{
	if (conf) *conf = s_conf;
	return s_have_ip;
}

static void dhcp_poll(void)
{
	uint32_t t = now(), held_s;
	switch (s_dhcp_state) {
	case DHCP_SELECTING:
	case DHCP_REQUESTING:
		if (t - s_dhcp_sent_ms < 2000u + 1000u * (s_dhcp_tries < 3 ? s_dhcp_tries : 3))
			break;
		if (++s_dhcp_tries > 4 && s_dhcp_state == DHCP_REQUESTING) {	/* no answer: from the start */
			s_dhcp_state = DHCP_SELECTING;
			s_dhcp_tries = 0;
		}
		dhcp_send(s_dhcp_state == DHCP_SELECTING ? 1 : 3);
		break;
	case DHCP_BOUND:
	case DHCP_RENEWING:
		held_s = (t - s_conf.got_ms) / 1000u;
		if (held_s >= s_conf.lease_s) {				/* the lease ran out */
			s_have_ip = 0;
			s_dhcp_state = DHCP_SELECTING;
			s_dhcp_tries = 0;
			dhcp_send(1);
		} else if (held_s >= s_dhcp_t1 && (s_dhcp_state == DHCP_BOUND || t - s_dhcp_sent_ms > 10000u)) {
			s_dhcp_state = DHCP_RENEWING;
			dhcp_send(3);
		}
		break;
	default:
		break;
	}
}

/* ---- DNS ---- */

static uint16_t s_dns_id;
static int s_dns_state;				/* 0 idle, 1 asking, 2 known, -1 failed */
static uint32_t s_dns_ip, s_dns_sent_ms, s_dns_tries;
static uint8_t s_dns_q[300];
static unsigned int s_dns_qlen;

static void dns_send(void)
{
	uint32_t server = s_conf.dns[s_dns_tries >= 3 && s_conf.dns[1] ? 1 : 0];
	s_dns_sent_ms = now();
	pico_net_udp_send(server, DNS_PORT, DNS_OUR_PORT, s_dns_q, s_dns_qlen);
}

int pico_net_dns(const char *name)
{
	unsigned int o = 12, n = (unsigned int)strlen(name);
	const char *p = name;
	if (!s_have_ip || !n || n > 250)
		return -1;
	memset(s_dns_q, 0, 12);
	s_dns_id = (uint16_t)(now() * 40503u + s_dns_id + 1);
	put16(s_dns_q, s_dns_id);
	put16(s_dns_q + 2, 0x0100);			/* recursion wanted */
	put16(s_dns_q + 4, 1);
	while (*p) {					/* the name as labels */
		const char *dot = p;
		unsigned int l;
		while (*dot && *dot != '.') dot++;
		l = (unsigned int)(dot - p);
		if (!l || l > 63) return -1;
		s_dns_q[o++] = (uint8_t)l;
		memcpy(s_dns_q + o, p, l);
		o += l;
		p = *dot ? dot + 1 : dot;
	}
	s_dns_q[o++] = 0;
	put16(s_dns_q + o, 1); o += 2;			/* A */
	put16(s_dns_q + o, 1); o += 2;			/* IN */
	s_dns_qlen = o;
	s_dns_state = 1;
	s_dns_tries = 0;
	dns_send();
	return 0;
}

int pico_net_dns_result(uint32_t *ip)
{
	if (s_dns_state == 2) { if (ip) *ip = s_dns_ip; return 1; }
	return s_dns_state < 0 ? -1 : 0;
}

/* past a name in a DNS message (labels, maybe ending in a pointer): its end, 0 when broken */
static unsigned int dns_skip(const uint8_t *d, unsigned int len, unsigned int o)
{
	while (o < len) {
		if (d[o] == 0) return o + 1;
		if ((d[o] & 0xC0) == 0xC0) return o + 2 <= len ? o + 2 : 0;
		o += 1u + d[o];
	}
	return 0;
}

static void dns_input(const uint8_t *d, unsigned int len)
{
	unsigned int o, i, qd, an;
	if (s_dns_state != 1 || len < 12 || get16(d) != s_dns_id || !(d[2] & 0x80))
		return;
	if ((d[3] & 0x0F) != 0) { s_dns_state = -1; return; }	/* no such name, or a server failure */
	qd = get16(d + 4);
	an = get16(d + 6);
	o = 12;
	for (i = 0; i < qd && o; i++)
		o = dns_skip(d, len, o) ? dns_skip(d, len, o) + 4 : 0;
	for (i = 0; i < an && o && o < len; i++) {
		uint32_t type, cls, rdlen;
		o = dns_skip(d, len, o);
		if (!o || o + 10 > len) break;
		type = get16(d + o);
		cls = get16(d + o + 2);
		rdlen = get16(d + o + 8);
		o += 10;
		if (o + rdlen > len) break;
		if (type == 1 && cls == 1 && rdlen == 4) {
			s_dns_ip = get32(d + o);
			s_dns_state = 2;
			return;
		}
		o += rdlen;
	}
	s_dns_state = -1;
}

static void dns_poll(void)
{
	if (s_dns_state != 1 || now() - s_dns_sent_ms < 2000u)
		return;
	if (++s_dns_tries >= 6) { s_dns_state = -1; return; }
	dns_send();
}

/* ---- SNTP ---- */

static int s_ntp_state;				/* 0 idle, 1 asking, 2 answered, -1 failed */
static uint32_t s_ntp_server, s_ntp_sent_ms, s_ntp_tries, s_ntp_mark[2];
static long long s_ntp_utc;

static void ntp_send(void)
{
	uint8_t q[48];
	memset(q, 0, sizeof q);
	q[0] = 0x23;					/* no warning, version 4, client */
	/* our transmit time field carries a mark the answer must give back (its originate time) */
	s_ntp_mark[0] = now() ^ 0x5A17C3E9u;
	s_ntp_mark[1] = (uint32_t)s_ntp_tries * 2654435761u ^ s_conf.ip;
	put32(q + 40, s_ntp_mark[0]);
	put32(q + 44, s_ntp_mark[1]);
	s_ntp_sent_ms = now();
	pico_net_udp_send(s_ntp_server, NTP_PORT, NTP_OUR_PORT, q, sizeof q);
}

int pico_net_time(uint32_t server_ip)
{
	if (!s_have_ip || !server_ip)
		return -1;
	s_ntp_server = server_ip;
	s_ntp_state = 1;
	s_ntp_tries = 0;
	ntp_send();
	return 0;
}

int pico_net_time_result(long long *utc)
{
	if (s_ntp_state == 2) { if (utc) *utc = s_ntp_utc; return 1; }
	return s_ntp_state < 0 ? -1 : 0;
}

static void ntp_input(const uint8_t *d, unsigned int len)
{
	uint32_t secs, frac, rtt;
	long long t;
	if (s_ntp_state != 1 || len < 48 || (d[0] & 7) != 4)
		return;
	if (get32(d + 24) != s_ntp_mark[0] || get32(d + 28) != s_ntp_mark[1])
		return;					/* not the answer to our question */
	if (d[1] == 0 || (d[0] >> 6) == 3) {		/* a refusal, or the server's clock is not set */
		s_ntp_state = -1;
		return;
	}
	secs = get32(d + 40);
	frac = get32(d + 44);
	rtt = now() - s_ntp_sent_ms;
	t = (long long)secs - NTP_1900_TO_1970;
	if (secs < 0x80000000u)				/* past 2036: NTP's next era */
		t += 4294967296LL;
	t += (long long)((frac >> 31) + (rtt / 2 + 500) / 1000);	/* round, half the trip back */
	s_ntp_utc = t;
	s_ntp_state = 2;
}

static void ntp_poll(void)
{
	if (s_ntp_state != 1 || now() - s_ntp_sent_ms < 1500u)
		return;
	if (++s_ntp_tries >= 4) { s_ntp_state = -1; return; }
	ntp_send();
}

/* ---- TCP (client side) ---- */

enum { T_CLOSED, T_SYN_SENT, T_OPEN, T_FIN_WAIT1, T_FIN_WAIT2, T_CLOSE_WAIT, T_LAST_ACK, T_TIME_WAIT };

struct tcp_conn {
	int state, error;
	uint32_t ip;
	uint16_t port, local, peer_mss;
	uint32_t iss, snd_una, snd_nxt, snd_wnd;	/* sending: oldest unacknowledged, next, their window */
	uint32_t rcv_nxt;				/* receiving: the next byte we expect */
	uint8_t *rx, *tx;				/* buffers in the link's work memory */
	uint32_t rx_head, rx_len;			/* received, waiting to be read (a ring) */
	uint32_t tx_len;				/* tx[0] is the byte at snd_una: unacked, then unsent */
	int fin_queued, fin_sent, peer_fin, retries, timing;
	uint32_t rto, srtt, rttvar;			/* ms; srtt 0 until the first measurement */
	uint32_t timer_ms;				/* when the oldest unacked byte went (0: nothing out) */
	uint32_t time_seq, time_ms;			/* the segment being timed */
	uint32_t wait_ms;				/* when TIME_WAIT began */
	int window_was_low;				/* our window fell below a segment: tell them when it opens */
	uint32_t held_s[4], held_e[4];			/* data held past a gap: [start, end) in sequence order */
	int nheld;
	uint8_t ws_ok, ws_in;				/* both scale windows: theirs is shifted by ws_in */
	uint8_t sack_ok;				/* they take SACK blocks */
	uint32_t ack_owed;				/* in-order segments not acknowledged yet */
};
static struct tcp_conn s_tcp[PICO_TCP_SLOTS];
static uint16_t s_tcp_port = 49200;
static uint32_t s_tcp_isn_salt;

/* sequence numbers wrap: a before b */
static int seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static int seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }

static uint32_t rx_free(const struct tcp_conn *c) { return PICO_TCP_RX - c->rx_len; }

/* our window scale: the shift that brings the whole receive buffer under 16 bits */
static unsigned int ws_out(void)
{
	unsigned int sh = 0;
	while ((PICO_TCP_RX >> sh) > 0xFFFF) sh++;
	return sh;
}

/* len bytes at off bytes past rcv_nxt into the receive ring */
static void ring_put(struct tcp_conn *c, uint32_t off, const uint8_t *data, uint32_t len)
{
	uint32_t at = (c->rx_head + c->rx_len + off) % PICO_TCP_RX;
	uint32_t first = PICO_TCP_RX - at < len ? PICO_TCP_RX - at : len;
	memcpy(c->rx + at, data, first);
	memcpy(c->rx, data + first, len - first);
}

/* [s, e) is held: merged into the sorted list (dropped when four gaps are held already) */
static void held_add(struct tcp_conn *c, uint32_t s, uint32_t e)
{
	int i, j;
	for (i = 0; i < c->nheld; i++)
		if (seq_le(s, c->held_e[i]) && seq_le(c->held_s[i], e)) {	/* touches: merge */
			if (seq_lt(s, c->held_s[i])) c->held_s[i] = s;
			if (seq_lt(c->held_e[i], e)) c->held_e[i] = e;
			while (i + 1 < c->nheld && seq_le(c->held_s[i + 1], c->held_e[i])) {
				if (seq_lt(c->held_e[i], c->held_e[i + 1])) c->held_e[i] = c->held_e[i + 1];
				for (j = i + 1; j + 1 < c->nheld; j++) { c->held_s[j] = c->held_s[j + 1]; c->held_e[j] = c->held_e[j + 1]; }
				c->nheld--;
			}
			return;
		}
	if (c->nheld == 4)
		return;
	for (i = c->nheld; i > 0 && seq_lt(s, c->held_s[i - 1]); i--) {
		c->held_s[i] = c->held_s[i - 1];
		c->held_e[i] = c->held_e[i - 1];
	}
	c->held_s[i] = s;
	c->held_e[i] = e;
	c->nheld++;
}

/* held data the gap's filling has reached joins the readable part */
static void held_take(struct tcp_conn *c)
{
	int j;
	while (c->nheld && seq_le(c->held_s[0], c->rcv_nxt)) {
		if (seq_lt(c->rcv_nxt, c->held_e[0])) {
			c->rx_len += c->held_e[0] - c->rcv_nxt;
			c->rcv_nxt = c->held_e[0];
		}
		for (j = 0; j + 1 < c->nheld; j++) { c->held_s[j] = c->held_s[j + 1]; c->held_e[j] = c->held_e[j + 1]; }
		c->nheld--;
	}
}


#define TCP_ACK_EVERY	8			/* in-order segments per acknowledgement at most */
#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

static uint16_t tcp_sum(uint32_t src, uint32_t dst, const uint8_t *t, unsigned int len)
{
	uint8_t ph[12];
	put32(ph, src);
	put32(ph + 4, dst);
	ph[8] = 0; ph[9] = PROTO_TCP;
	put16(ph + 10, len);
	return sum_end(sum_add(sum_add(0, ph, 12), t, len));
}

/* one segment: seq, flags, len bytes of data (from data), our ack and window */
static void tcp_seg(struct tcp_conn *c, uint32_t seq, uint32_t flags, const uint8_t *data, unsigned int len)
{
	uint8_t *t = s_body;
	uint32_t wnd = rx_free(c);
	unsigned int hl = TCP_HDR, k;
	memset(t, 0, 60);
	if (flags & F_SYN) {
		/* our largest segment, window scaling, SACK allowed */
		t[hl] = 2; t[hl + 1] = 4; put16(t + hl + 2, TCP_MSS); hl += 4;
		t[hl] = 1; t[hl + 1] = 3; t[hl + 2] = 3; t[hl + 3] = (uint8_t)ws_out(); hl += 4;
		t[hl] = 1; t[hl + 1] = 1; t[hl + 2] = 4; t[hl + 3] = 2; hl += 4;
		if (wnd > 0xFFFF) wnd = 0xFFFF;		/* a SYN's window is never scaled */
	} else {
		if (c->ws_ok) wnd >>= ws_out();
		if (wnd > 0xFFFF) wnd = 0xFFFF;
		if ((flags & F_ACK) && c->sack_ok && c->nheld) {	/* the data we hold past the gap */
			t[hl] = 1; t[hl + 1] = 1; t[hl + 2] = 5; t[hl + 3] = (uint8_t)(2 + 8 * c->nheld);
			hl += 4;
			for (k = 0; k < (unsigned)c->nheld; k++, hl += 8) {
				put32(t + hl, c->held_s[k]);
				put32(t + hl + 4, c->held_e[k]);
			}
		}
	}
	put16(t, c->local);
	put16(t + 2, c->port);
	put32(t + 4, seq);
	put32(t + 8, (flags & F_ACK) ? c->rcv_nxt : 0);
	t[12] = (uint8_t)((hl / 4) << 4);
	t[13] = (uint8_t)flags;
	put16(t + 14, wnd);
	if (len) memcpy(t + hl, data, len);
	put16(t + 16, tcp_sum(s_conf.ip, c->ip, t, hl + len));
	c->window_was_low = rx_free(c) < TCP_MSS;
	ip_send(c->ip, PROTO_TCP, hl + len);
}

/* a reset answer to a segment that belongs to nothing */
static void tcp_reset_reply(uint32_t to, const uint8_t *in, unsigned int len)
{
	struct tcp_conn r;
	uint32_t flags = in[13], seq = get32(in + 4), ack = get32(in + 8);
	unsigned int hl = (in[12] >> 4) * 4u;
	memset(&r, 0, sizeof r);
	r.ip = to;
	r.local = (uint16_t)get16(in + 2);
	r.port = (uint16_t)get16(in);
	r.rx_len = PICO_TCP_RX;			/* window 0 */
	if (flags & F_ACK) {
		tcp_seg(&r, ack, F_RST, 0, 0);
	} else {
		r.rcv_nxt = seq + (len - hl) + ((flags & F_SYN) ? 1 : 0) + ((flags & F_FIN) ? 1 : 0);
		tcp_seg(&r, 0, F_RST | F_ACK, 0, 0);
	}
}

static void tcp_done(struct tcp_conn *c, int error)
{
	c->state = T_CLOSED;
	if (error) c->error = error;
	c->timer_ms = 0;
}

/* sends what the window allows, then our FIN when it is all out */
static void tcp_output(struct tcp_conn *c)
{
	uint32_t out, unsent, room, n;
	if (c->state != T_OPEN && c->state != T_CLOSE_WAIT && c->state != T_FIN_WAIT1 && c->state != T_LAST_ACK)
		return;
	for (;;) {
		out = c->snd_nxt - c->snd_una - (c->fin_sent ? 1u : 0u);	/* data bytes in flight */
		unsent = c->tx_len - out;
		room = c->snd_wnd > out ? c->snd_wnd - out : 0;
		if (!unsent || !room)
			break;
		n = unsent;
		if (n > room) n = room;
		if (n > c->peer_mss) n = c->peer_mss;
		tcp_seg(c, c->snd_nxt, F_ACK | F_PSH, c->tx + out, n);
		if (!c->timing) { c->timing = 1; c->time_seq = c->snd_nxt; c->time_ms = now(); }
		c->snd_nxt += n;
		if (!c->timer_ms) c->timer_ms = now();
	}
	if (c->fin_queued && !c->fin_sent && c->snd_nxt - c->snd_una == c->tx_len) {
		tcp_seg(c, c->snd_nxt, F_FIN | F_ACK, 0, 0);
		c->snd_nxt++;
		c->fin_sent = 1;
		if (!c->timer_ms) c->timer_ms = now();
		c->state = c->state == T_CLOSE_WAIT ? T_LAST_ACK : T_FIN_WAIT1;
	}
}

/* an RTT measurement into the timer's estimate (RFC 6298) */
static void tcp_rtt(struct tcp_conn *c, uint32_t r)
{
	if (!c->srtt) {
		c->srtt = r ? r : 1;
		c->rttvar = r / 2;
	} else {
		uint32_t d = c->srtt > r ? c->srtt - r : r - c->srtt;
		c->rttvar = (3 * c->rttvar + d) / 4;
		c->srtt = (7 * c->srtt + r) / 8;
	}
	c->rto = c->srtt + (4 * c->rttvar > 10 ? 4 * c->rttvar : 10);
	if (c->rto < 200) c->rto = 200;
	if (c->rto > 60000) c->rto = 60000;
}

static void tcp_input(uint32_t src, uint32_t dst, const uint8_t *t, unsigned int len)
{
	struct tcp_conn *c = 0;
	uint32_t flags, seq, ack, wnd, hl, plen, i;
	const uint8_t *data;
	if (len < TCP_HDR || tcp_sum(src, dst, t, len) != 0)
		return;
	hl = (t[12] >> 4) * 4u;
	if (hl < TCP_HDR || hl > len)
		return;
	for (i = 0; i < PICO_TCP_SLOTS; i++)
		if (s_tcp[i].state != T_CLOSED && s_tcp[i].ip == src && s_tcp[i].port == get16(t) &&
		    s_tcp[i].local == get16(t + 2)) { c = &s_tcp[i]; break; }
	flags = t[13];
	if (!c) {
		if (!(flags & F_RST)) tcp_reset_reply(src, t, len);
		return;
	}
	seq = get32(t + 4);
	ack = get32(t + 8);
	wnd = get16(t + 14);
	data = t + hl;
	plen = len - hl;

	if (c->state == T_SYN_SENT) {
		if ((flags & F_ACK) && ack != c->iss + 1) {
			if (!(flags & F_RST)) tcp_reset_reply(src, t, len);
			return;
		}
		if (flags & F_RST) { if (flags & F_ACK) tcp_done(c, PICO_TCP_REFUSED); return; }
		if (!(flags & F_SYN) || !(flags & F_ACK))
			return;
		c->rcv_nxt = seq + 1;
		c->snd_una = ack;
		c->snd_wnd = wnd;
		for (i = TCP_HDR; i + 1 < hl; ) {		/* options: their largest segment */
			if (t[i] == 0) break;
			if (t[i] == 1) { i++; continue; }
			if (t[i] == 2 && t[i + 1] == 4 && i + 4 <= hl) c->peer_mss = (uint16_t)get16(t + i + 2);
			if (t[i] == 3 && t[i + 1] == 3 && i + 3 <= hl) { c->ws_ok = 1; c->ws_in = t[i + 2] > 14 ? 14 : t[i + 2]; }
			if (t[i] == 4 && t[i + 1] == 2) c->sack_ok = 1;
			if (i + 1 >= hl || t[i + 1] < 2) break;
			i += t[i + 1];
		}
		if (c->peer_mss > TCP_MSS || !c->peer_mss) c->peer_mss = c->peer_mss ? TCP_MSS : 536;
		if (c->timing) { tcp_rtt(c, now() - c->time_ms); c->timing = 0; }
		c->state = T_OPEN;
		c->timer_ms = 0;
		c->retries = 0;
		tcp_seg(c, c->snd_nxt, F_ACK, 0, 0);
		tcp_output(c);
		return;
	}

	/* the segment must touch our receive window (or be an empty one at its edge) */
	if (!(seq_le(c->rcv_nxt, seq + plen) && seq_lt(seq, c->rcv_nxt + (rx_free(c) ? rx_free(c) : 1)))) {
		if (!(flags & F_RST)) tcp_seg(c, c->snd_nxt, F_ACK, 0, 0);
		return;
	}
	if (flags & F_RST) { tcp_done(c, PICO_TCP_RESET); return; }
	if (flags & F_SYN) { tcp_seg(c, c->snd_nxt, F_ACK, 0, 0); return; }
	if (!(flags & F_ACK))
		return;

	/* what they acknowledge */
	if (seq_lt(c->snd_una, ack) && seq_le(ack, c->snd_nxt)) {
		uint32_t acked = ack - c->snd_una, data_acked = acked;
		if (c->fin_sent && ack == c->snd_nxt) data_acked--;	/* our FIN counts one */
		if (data_acked > c->tx_len) data_acked = c->tx_len;
		memmove(c->tx, c->tx + data_acked, c->tx_len - data_acked);
		c->tx_len -= data_acked;
		c->snd_una = ack;
		if (c->timing && seq_lt(c->time_seq, ack)) { tcp_rtt(c, now() - c->time_ms); c->timing = 0; }
		c->retries = 0;
		c->timer_ms = c->snd_una == c->snd_nxt ? 0 : now();
		if (c->fin_sent && ack == c->snd_nxt) {
			if (c->state == T_FIN_WAIT1) c->state = T_FIN_WAIT2;
			else if (c->state == T_LAST_ACK) { tcp_done(c, 0); return; }
		}
	}
	c->snd_wnd = c->ws_ok ? wnd << c->ws_in : wnd;

	/* a segment sent again that starts before what we have: only its new part counts */
	if (plen) s_st.tcp_in++;
	if (plen && seq_lt(seq, c->rcv_nxt)) {
		uint32_t skip = c->rcv_nxt - seq;
		s_st.tcp_old++;
		if (skip >= plen) { plen = 0; flags &= ~(uint32_t)F_FIN; tcp_seg(c, c->snd_nxt, F_ACK, 0, 0); }
		else { data += skip; plen -= skip; seq = c->rcv_nxt; }
	}
	/* their data: in order it is readable at once; past a gap it is held in place (and told
	 * with SACK) until the gap fills */
	if (plen && (c->state == T_OPEN || c->state == T_FIN_WAIT1 || c->state == T_FIN_WAIT2)) {
		if (seq == c->rcv_nxt) {
			uint32_t take = plen < rx_free(c) ? plen : rx_free(c);
			int had_held = c->nheld;
			ring_put(c, 0, data, take);
			c->rx_len += take;
			c->rcv_nxt += take;
			held_take(c);
			if (take < plen) flags &= ~(uint32_t)F_FIN;	/* not all of it: their FIN comes again */
			/* in order: acknowledged with the next few (pico_net_flush), at once when it
			 * filled a gap or the window is getting short */
			if (++c->ack_owed < TCP_ACK_EVERY && !had_held && rx_free(c) >= 4u * TCP_MSS && !(flags & F_FIN))
				goto acked_later;
		} else {
			uint32_t off = seq - c->rcv_nxt;
			if (off + plen <= rx_free(c)) {
				ring_put(c, off, data, plen);
				held_add(c, seq, seq + plen);
				s_st.tcp_held++;
			} else {
				s_st.tcp_no_room++;
			}
			flags &= ~(uint32_t)F_FIN;		/* a FIN past a gap comes again */
		}
		tcp_seg(c, c->snd_nxt, F_ACK, 0, 0);
		c->ack_owed = 0;
	}
acked_later:
	if ((flags & F_FIN) && seq + plen == c->rcv_nxt && !c->peer_fin) {
		c->rcv_nxt++;
		c->peer_fin = 1;
		if (c->state == T_OPEN) c->state = T_CLOSE_WAIT;
		else if (c->state == T_FIN_WAIT2 || c->state == T_FIN_WAIT1) { c->state = T_TIME_WAIT; c->wait_ms = now(); }
		tcp_seg(c, c->snd_nxt, F_ACK, 0, 0);
	}
	tcp_output(c);
}

static void tcp_poll(void)
{
	unsigned int i;
	uint32_t t = now();
	for (i = 0; i < PICO_TCP_SLOTS; i++) {
		struct tcp_conn *c = &s_tcp[i];
		if (c->state == T_CLOSED)
			continue;
		if (c->state == T_TIME_WAIT) {
			if (t - c->wait_ms > 2000u) tcp_done(c, 0);
			continue;
		}
		if (!c->timer_ms || t - c->timer_ms < c->rto)
			continue;
		/* nothing heard back in time: everything from the oldest unacked byte goes again */
		if (++c->retries > 8) { tcp_done(c, PICO_TCP_TIMEOUT); continue; }
		c->rto = c->rto * 2 > 60000 ? 60000 : c->rto * 2;
		c->timing = 0;
		c->timer_ms = t;
		if (c->state == T_SYN_SENT) {
			tcp_seg(c, c->iss, F_SYN, 0, 0);
			continue;
		}
		c->snd_nxt = c->snd_una;
		if (c->fin_sent) {
			c->fin_sent = 0;
			if (c->state == T_FIN_WAIT1) c->state = T_OPEN;
			else if (c->state == T_LAST_ACK) c->state = T_CLOSE_WAIT;
		}
		if (c->snd_wnd == 0 && c->tx_len) {	/* their window shut: one byte to ask again */
			tcp_seg(c, c->snd_nxt, F_ACK, c->tx, 1);
			c->snd_nxt++;
			continue;
		}
		tcp_output(c);
	}
}

int pico_tcp_connect(uint32_t ip, uint16_t port)
{
	unsigned int i;
	struct tcp_conn *c = 0;
	if (!s_have_ip || !s_link.work || s_link.work_len < PICO_TCP_SLOTS * PICO_TCP_MEM)
		return -1;
	/* a free slot; else one only waiting out TIME_WAIT (ours is a new port, nothing can mix);
	 * else one we already closed, reset to make room */
	for (i = 0; i < PICO_TCP_SLOTS && !c; i++)
		if (s_tcp[i].state == T_CLOSED) c = &s_tcp[i];
	for (i = 0; i < PICO_TCP_SLOTS && !c; i++)
		if (s_tcp[i].state == T_TIME_WAIT) c = &s_tcp[i];
	for (i = 0; i < PICO_TCP_SLOTS && !c; i++)
		if (s_tcp[i].fin_queued) { pico_tcp_abort((int)i); c = &s_tcp[i]; }
	if (!c)
		return -1;
	i = (unsigned int)(c - s_tcp);
	memset(c, 0, sizeof *c);
	c->rx = s_link.work + i * PICO_TCP_MEM;
	c->tx = c->rx + PICO_TCP_RX;
	c->ip = ip;
	c->port = port;
	c->local = s_tcp_port;
	s_tcp_port = (uint16_t)(s_tcp_port >= 65000 ? 49200 : s_tcp_port + 1);
	s_tcp_isn_salt = s_tcp_isn_salt * 1103515245u + 12345u + now();
	c->iss = s_tcp_isn_salt ^ (ip * 2654435761u) ^ ((uint32_t)c->local << 16);
	c->snd_una = c->iss;
	c->snd_nxt = c->iss + 1;
	c->peer_mss = 536;
	c->rto = 1000;
	c->state = T_SYN_SENT;
	c->timing = 1; c->time_seq = c->iss; c->time_ms = now();
	c->timer_ms = now();
	tcp_seg(c, c->iss, F_SYN, 0, 0);
	return (int)i;
}

static struct tcp_conn *tcp_get(int h)
{
	return h >= 0 && h < PICO_TCP_SLOTS ? &s_tcp[h] : 0;
}

int pico_tcp_state(int h)
{
	struct tcp_conn *c = tcp_get(h);
	if (!c || c->state == T_CLOSED) return PICO_TCP_CLOSED;
	if (c->state == T_SYN_SENT) return PICO_TCP_CONNECTING;
	if (c->state == T_OPEN || c->state == T_CLOSE_WAIT) return PICO_TCP_OPEN;
	return PICO_TCP_CLOSING;
}

int pico_tcp_error(int h)
{
	struct tcp_conn *c = tcp_get(h);
	return c ? c->error : PICO_TCP_OK;
}

int pico_tcp_send(int h, const void *data, unsigned int len)
{
	struct tcp_conn *c = tcp_get(h);
	unsigned int n;
	if (!c || (c->state != T_OPEN && c->state != T_CLOSE_WAIT) || c->fin_queued)
		return -1;
	n = PICO_TCP_TX - c->tx_len;
	if (n > len) n = len;
	memcpy(c->tx + c->tx_len, data, n);
	c->tx_len += n;
	tcp_output(c);
	return (int)n;
}

int pico_tcp_recv(int h, void *buf, unsigned int cap)
{
	struct tcp_conn *c = tcp_get(h);
	uint32_t n, first;
	if (!c) return -1;
	if (!c->rx_len)
		return (c->peer_fin || c->state == T_CLOSED) ? -1 : 0;
	n = c->rx_len < cap ? c->rx_len : cap;
	first = PICO_TCP_RX - c->rx_head < n ? PICO_TCP_RX - c->rx_head : n;
	memcpy(buf, c->rx + c->rx_head, first);
	memcpy((uint8_t *)buf + first, c->rx, n - first);
	c->rx_head = (c->rx_head + n) % PICO_TCP_RX;
	c->rx_len -= n;
	/* our window had shut: say it is open again */
	if (c->window_was_low && rx_free(c) >= 2u * TCP_MSS && c->state != T_CLOSED && c->state != T_SYN_SENT)
		tcp_seg(c, c->snd_nxt, F_ACK, 0, 0);
	return (int)n;
}

unsigned int pico_tcp_unsent(int h)
{
	struct tcp_conn *c = tcp_get(h);
	return c && c->state != T_CLOSED ? c->tx_len : 0;
}

void pico_tcp_close(int h)
{
	struct tcp_conn *c = tcp_get(h);
	if (!c || c->state == T_CLOSED) return;
	if (c->state == T_SYN_SENT) { tcp_done(c, 0); return; }
	c->fin_queued = 1;
	tcp_output(c);
}

void pico_tcp_abort(int h)
{
	struct tcp_conn *c = tcp_get(h);
	if (!c || c->state == T_CLOSED) return;
	if (c->state != T_SYN_SENT) tcp_seg(c, c->snd_nxt, F_RST | F_ACK, 0, 0);
	tcp_done(c, 0);
}

/* ---- the stack as a whole ---- */

void pico_net_start(const struct pico_net_link *link)
{
	s_link = *link;
	memset(&s_conf, 0, sizeof s_conf);
	memset(s_arp, 0, sizeof s_arp);
	memset(s_udp, 0, sizeof s_udp);
	memset(&s_st, 0, sizeof s_st);
	memset(s_ping, 0, sizeof s_ping);
	s_have_ip = 0;
	s_wait_len = 0;
	s_dhcp_state = DHCP_OFF;
	s_dns_state = 0;
	s_ntp_state = 0;
	memset(s_tcp, 0, sizeof s_tcp);
	s_on = 1;
}

void pico_net_stop(void)
{
	s_on = 0;
	s_have_ip = 0;
	s_dhcp_state = DHCP_OFF;
}

void pico_net_poll(void)
{
	if (!s_on) return;
	dhcp_poll();
	dns_poll();
	ntp_poll();
	tcp_poll();
	pico_net_flush();
	if (s_wait_len && now() - s_wait_ms > 1000u) {		/* ARP asked again, three times at most */
		if (++s_wait_tries > 3) s_wait_len = 0;
		else { s_wait_ms = now(); arp_send(1, s_everyone, s_wait_hop, 0); }
	}
}

void pico_net_flush(void)
{
	unsigned int i;
	if (!s_on) return;
	for (i = 0; i < PICO_TCP_SLOTS; i++)
		if (s_tcp[i].ack_owed && s_tcp[i].state != T_CLOSED) {
			s_tcp[i].ack_owed = 0;
			tcp_seg(&s_tcp[i], s_tcp[i].snd_nxt, F_ACK, 0, 0);
		}
}

void pico_net_get_stats(struct pico_net_stats *s)
{
	*s = s_st;
}
