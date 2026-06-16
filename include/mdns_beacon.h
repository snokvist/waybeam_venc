#ifndef MDNS_BEACON_H
#define MDNS_BEACON_H

/*
 * mdns_beacon — announce-only mDNS device beacon for waybeam_venc.
 *
 * Advertises this encoder as a `_waybeam-venc._tcp.local` service so that
 * ground stations and Android clients can discover the device independently
 * of the optional waybeam-hub.  TXT records carry stable identity and
 * endpoint data only (backend, model, codec, version, web/sidecar ports) —
 * live state (bitrate, fps, mode) is pulled by the consumer via the HTTP
 * API, never advertised here.
 *
 * The beacon runs on its own thread: it does not discover peers, does not
 * feed any trust layer, and never blocks the encode path.  It responds to
 * PTR queries for its own service type and re-announces periodically.
 */

#include <stdbool.h>
#include <stdint.h>
#include <netinet/in.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDNS_BEACON_MAX_IPS 4
#define MDNS_BEACON_BUF_SIZE 1500

/* Resolved advertisement parameters (owned by value — no dangling pointers). */
typedef struct {
	bool     enabled;
	char     service_type[64];   /* "_waybeam-venc._tcp"        */
	char     name[64];           /* instance + hostname label   */
	char     backend[16];        /* "star6e" | "maruko"         */
	char     model[24];          /* SoC family; "" to omit      */
	char     codec[12];          /* "h265"                      */
	char     version[24];        /* app version                 */
	uint16_t web_port;           /* HTTP API port (SRV target)  */
	uint16_t sidecar_port;       /* RTP sidecar port; 0 to omit */
} MdnsBeaconParams;

typedef struct {
	int                fd;             /* -1 = inert                     */
	bool               enabled;
	pthread_t          thread;
	bool               thread_started;
	volatile int       stop;
	MdnsBeaconParams   params;
	char               hostname[64];   /* resolved "<name>"              */
	struct in_addr     local_ips[MDNS_BEACON_MAX_IPS];
	int                local_ip_count;
	struct sockaddr_in mcast_addr;
	uint8_t            response_buf[MDNS_BEACON_BUF_SIZE];
	int                response_len;
	uint8_t            goodbye_buf[MDNS_BEACON_BUF_SIZE];
	int                goodbye_len;
} MdnsBeacon;

/**
 * Build the full announce/response packet (PTR + SRV + TXT + A records)
 * into buf.  Pure and socket-free — fully unit-testable.  `hostname` is the
 * label used for the SRV target and A records ("<hostname>.local").  ttl==0
 * builds a goodbye packet.  Returns the packet length, or -1 on error.
 */
int mdns_beacon_build_packet(uint8_t *buf, int buf_size,
	const MdnsBeaconParams *p, const char *hostname,
	const struct in_addr *ips, int n_ips, uint32_t ttl);

/**
 * Start the beacon from resolved params: detect local IPv4s, build packets,
 * open the multicast socket, and spawn the servicing thread.  When
 * params->enabled is false (or no usable IP / socket error) the beacon stays
 * inert.  Returns 0 on success or inert, -1 on hard failure.
 */
int mdns_beacon_start(MdnsBeacon *b, const MdnsBeaconParams *p);

/**
 * Convenience wrapper: read the `discovery` settings and ports from a venc
 * config file, resolve the remaining params (backend name, version,
 * hostname), and start.  Inert on any error.  `backend_name` is the build
 * backend string ("star6e" / "maruko"); may be NULL.
 */
void mdns_beacon_start_from_config(MdnsBeacon *b, const char *config_path,
	const char *backend_name);

/** Stop the thread, multicast a goodbye, and close the socket.  Safe to call
 *  on an inert beacon or one that never started. */
void mdns_beacon_stop(MdnsBeacon *b);

#ifdef __cplusplus
}
#endif

#endif /* MDNS_BEACON_H */
