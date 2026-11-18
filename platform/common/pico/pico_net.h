/*
 * pico_net.h - a small IPv4 network stack for the Pico's Wi-Fi: Ethernet frames in and out
 * through the link below it, ARP, IPv4, ICMP echo, UDP, a DHCP client and a DNS resolver.
 *
 * Single threaded and polled: the link hands every received frame to pico_net_input() and the
 * caller runs pico_net_poll() often (it keeps DHCP, ARP and the timers going). Addresses are in
 * host order (192.168.0.1 is 0xC0A80001).
 */
#ifndef PICO_NET_H
#define PICO_NET_H

#include <stdint.h>

/* what the stack sits on: our MAC address, a way to send one Ethernet frame (to, from, type,
 * payload; 0 sent) and a millisecond clock */
struct pico_net_link {
	uint8_t mac[6];
	int (*send)(const uint8_t *frame, unsigned int len);
	uint32_t (*now_ms)(void);
	uint8_t *work;				/* memory the stack may use for TCP's buffers */
	uint32_t work_len;			/* (PICO_TCP_SLOTS * PICO_TCP_MEM bytes are enough) */
};

/* the address we have and what came with it */
struct pico_net_conf {
	uint32_t ip, mask, gateway, dns[2], server;
	uint32_t lease_s;			/* seconds the address is ours, from when it was given */
	uint32_t got_ms;			/* when that was */
};

/* starts the stack on a link (forgets any old address) */
void pico_net_start(const struct pico_net_link *link);
/* stops it: nothing more is sent */
void pico_net_stop(void);

/* one received Ethernet frame */
void pico_net_input(const uint8_t *frame, unsigned int len);
/* timers: DHCP sends and renewals, ARP retries */
void pico_net_poll(void);
/* after a run of received frames: the TCP acknowledgements they are owed go out (they are
 * gathered, one for every few segments) */
void pico_net_flush(void);

/* DHCP: asks for an address (call pico_net_poll until pico_net_up) */
void pico_net_dhcp(const char *host_name);
/* 1 once we have an address; conf (may be 0) gets it */
int pico_net_up(struct pico_net_conf *conf);

/* ICMP echo to ip with sequence number seq: 0 sent (or waiting on the address's ARP answer).
 * pico_net_ping_answer gives the round trip in ms of the answer to seq, or -1 none yet. */
int pico_net_ping(uint32_t ip, uint16_t seq);
int pico_net_ping_answer(uint16_t seq);

/* UDP: a handler for datagrams to our port (one handler per port, 8 ports at most; 0 removes) */
typedef void (*pico_udp_fn)(uint32_t from_ip, uint16_t from_port, const uint8_t *data, unsigned int len);
int pico_net_udp_listen(uint16_t port, pico_udp_fn fn);
int pico_net_udp_send(uint32_t ip, uint16_t to_port, uint16_t from_port, const void *data, unsigned int len);

/* DNS: starts looking name up (A record); pico_net_dns_result gives 1 and the address once
 * known, -1 when the name has none or no server answered, 0 still asking */
int pico_net_dns(const char *name);
int pico_net_dns_result(uint32_t *ip);

/* SNTP (RFC 4330): asks server_ip for the time; pico_net_time_result gives 1 and the UTC
 * seconds since 1970 once it answered, -1 when it did not (or refused), 0 still asking */
int pico_net_time(uint32_t server_ip);
int pico_net_time_result(long long *utc);

/* TCP client connections. A handle is 0 .. PICO_TCP_SLOTS-1, -1 when none was free (or no
 * address yet). Data received waits in the connection until read; data given to send is kept
 * until the other side has it. */
#define PICO_TCP_SLOTS	2
#define PICO_TCP_RX	122880			/* receive buffer per connection (the window we offer) */
#define PICO_TCP_TX	8192			/* send buffer per connection */
#define PICO_TCP_MEM	(PICO_TCP_RX + PICO_TCP_TX)
enum { PICO_TCP_CLOSED, PICO_TCP_CONNECTING, PICO_TCP_OPEN, PICO_TCP_CLOSING };
enum { PICO_TCP_OK, PICO_TCP_REFUSED, PICO_TCP_RESET, PICO_TCP_TIMEOUT };
int pico_tcp_connect(uint32_t ip, uint16_t port);
/* PICO_TCP_*: CLOSED once both sides are done (or it failed: pico_tcp_error says why) */
int pico_tcp_state(int h);
int pico_tcp_error(int h);
/* queues up to len bytes to send: how many were taken (the rest when there is room) */
int pico_tcp_send(int h, const void *data, unsigned int len);
/* takes up to cap received bytes: how many; 0 none waiting; -1 none and the other side is done */
int pico_tcp_recv(int h, void *buf, unsigned int cap);
/* bytes the other side has not taken yet (sent or still to send) */
unsigned int pico_tcp_unsent(int h);
/* we are done sending (the rest goes first); pico_tcp_abort drops it at once */
void pico_tcp_close(int h);
void pico_tcp_abort(int h);

/* counts for a status line */
struct pico_net_stats {
	uint32_t in, out, arp_in, ip_in, udp_in, dropped;
	uint32_t tcp_in, tcp_held, tcp_old, tcp_no_room;	/* TCP data segments: all, past a gap, repeats, no room */
	uint32_t dhcp_offers, dhcp_acks;			/* DHCP answers to us */
};
void pico_net_get_stats(struct pico_net_stats *s);

/* "192.168.0.10" into buf (16 bytes) */
void pico_net_ip_text(uint32_t ip, char *buf);

#endif
