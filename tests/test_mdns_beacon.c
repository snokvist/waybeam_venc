#include "mdns_beacon.h"

#include "mdns_wire.h"
#include "venc_config.h"
#include "test_helpers.h"

#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

/* ── Wire codec round-trip ───────────────────────────────────────────── */

static int test_wire_name_roundtrip(void)
{
	int failures = 0;
	uint8_t buf[128];
	int len = mdns_encode_name(buf, (int)sizeof(buf),
		"cam0._waybeam-venc._tcp.local");
	CHECK("wire_encode_name_ok", len > 0);

	char out[MDNS_MAX_NAME];
	int consumed = mdns_decode_name(buf, len, 0, out, (int)sizeof(out));
	CHECK("wire_decode_name_consumed", consumed == len);
	CHECK("wire_decode_name_match",
		strcmp(out, "cam0._waybeam-venc._tcp.local") == 0);

	/* Oversized label rejected */
	char big[80];
	memset(big, 'a', sizeof(big));
	big[70] = '\0';
	CHECK("wire_encode_long_label_rejected",
		mdns_encode_name(buf, (int)sizeof(buf), big) < 0);
	return failures;
}

static int test_wire_txt_get(void)
{
	int failures = 0;
	/* rdata: [7]"a=hello"[3]"b=1" */
	uint8_t rd[64];
	int n = 0;
	const char *p1 = "a=hello";
	const char *p2 = "b=1";
	rd[n++] = (uint8_t)strlen(p1);
	memcpy(rd + n, p1, strlen(p1)); n += (int)strlen(p1);
	rd[n++] = (uint8_t)strlen(p2);
	memcpy(rd + n, p2, strlen(p2)); n += (int)strlen(p2);

	char val[16] = "";
	CHECK("txt_get_a", mdns_txt_get_value(rd, n, "a", val, sizeof(val)) &&
		strcmp(val, "hello") == 0);
	CHECK("txt_get_b", mdns_txt_get_value(rd, n, "b", val, sizeof(val)) &&
		strcmp(val, "1") == 0);
	CHECK("txt_get_missing",
		!mdns_txt_get_value(rd, n, "c", val, sizeof(val)));
	return failures;
}

/* ── Beacon packet builder ───────────────────────────────────────────── */

static MdnsBeaconParams sample_params(void)
{
	MdnsBeaconParams p;
	memset(&p, 0, sizeof(p));
	p.enabled = true;
	snprintf(p.service_type, sizeof(p.service_type), "_waybeam-venc._tcp");
	snprintf(p.version, sizeof(p.version), "0.17.1");
	p.web_port = 80;
	p.sidecar_port = 5602;
	return p;
}

static int test_build_packet_records(void)
{
	int failures = 0;
	MdnsBeaconParams p = sample_params();
	struct in_addr ip;
	CHECK("build_ip_parse", inet_pton(AF_INET, "192.168.1.42", &ip) == 1);

	uint8_t pkt[MDNS_BEACON_BUF_SIZE];
	int len = mdns_beacon_build_packet(pkt, (int)sizeof(pkt), &p, "cam0",
		&ip, 1, 120);
	CHECK("build_len_ok", len > 12);

	uint16_t qd = mdns_get16(pkt + 4);
	uint16_t an = mdns_get16(pkt + 6);
	CHECK("build_qd_zero", qd == 0);
	CHECK("build_answer_count", an == 4);  /* PTR + SRV + TXT + 1 A */

	bool ptr_ok = false, srv_ok = false, a_ok = false;
	bool txt_proto = false, txt_version = false, txt_sidecar = false;

	int pos = 12;
	for (uint16_t i = 0; i < an; i++) {
		MdnsRr rr;
		int next = mdns_parse_rr(pkt, len, pos, &rr);
		CHECK("build_parse_rr", next > pos);
		if (next <= pos) break;

		if (rr.type == MDNS_TYPE_PTR &&
			strcmp(rr.name, "_waybeam-venc._tcp.local") == 0)
			ptr_ok = true;

		if (rr.type == MDNS_TYPE_SRV && rr.rdlength >= 6) {
			uint16_t port = mdns_get16(pkt + rr.rdata_offset + 4);
			if (port == 80)
				srv_ok = true;
		}

		if (rr.type == MDNS_TYPE_A && rr.rdlength == 4) {
			if (memcmp(pkt + rr.rdata_offset, &ip.s_addr, 4) == 0)
				a_ok = true;
		}

		if (rr.type == MDNS_TYPE_TXT && rr.rdlength > 0) {
			const uint8_t *rd = pkt + rr.rdata_offset;
			int rl = rr.rdlength;
			char v[64] = "";
			txt_proto = mdns_txt_get_value(rd, rl, "proto", v, sizeof(v)) &&
				strcmp(v, "1") == 0;
			txt_version = mdns_txt_get_value(rd, rl, "version", v, sizeof(v)) &&
				strcmp(v, "0.17.1") == 0;
			txt_sidecar = mdns_txt_get_value(rd, rl, "sidecar_port", v,
				sizeof(v)) && strcmp(v, "5602") == 0;
		}
		pos = next;
	}

	CHECK("build_has_ptr", ptr_ok);
	CHECK("build_has_srv_port", srv_ok);
	CHECK("build_has_a_ip", a_ok);
	CHECK("build_txt_proto", txt_proto);
	CHECK("build_txt_version", txt_version);
	CHECK("build_txt_sidecar_port", txt_sidecar);

	/* Trimmed schema: backend/model/codec/web_port/name are NOT advertised */
	pos = 12;
	bool saw_dropped = false;
	for (uint16_t i = 0; i < an; i++) {
		MdnsRr rr;
		int next = mdns_parse_rr(pkt, len, pos, &rr);
		if (next <= pos) break;
		if (rr.type == MDNS_TYPE_TXT && rr.rdlength > 0) {
			char v[64] = "";
			const uint8_t *rd = pkt + rr.rdata_offset;
			int rl = rr.rdlength;
			saw_dropped =
				mdns_txt_get_value(rd, rl, "backend", v, sizeof(v)) ||
				mdns_txt_get_value(rd, rl, "model", v, sizeof(v)) ||
				mdns_txt_get_value(rd, rl, "codec", v, sizeof(v)) ||
				mdns_txt_get_value(rd, rl, "web_port", v, sizeof(v)) ||
				mdns_txt_get_value(rd, rl, "name", v, sizeof(v));
		}
		pos = next;
	}
	CHECK("build_txt_no_dropped_keys", !saw_dropped);
	return failures;
}

static int test_build_packet_guards(void)
{
	int failures = 0;
	MdnsBeaconParams p = sample_params();
	struct in_addr ip;
	inet_pton(AF_INET, "10.0.0.1", &ip);
	uint8_t pkt[MDNS_BEACON_BUF_SIZE];

	CHECK("build_reject_no_ip",
		mdns_beacon_build_packet(pkt, sizeof(pkt), &p, "cam0", &ip, 0, 120) < 0);
	CHECK("build_reject_empty_host",
		mdns_beacon_build_packet(pkt, sizeof(pkt), &p, "", &ip, 1, 120) < 0);

	/* Goodbye packet (ttl 0): first record carries TTL 0 */
	int len = mdns_beacon_build_packet(pkt, sizeof(pkt), &p, "cam0", &ip, 1, 0);
	CHECK("build_goodbye_len", len > 12);
	MdnsRr rr;
	int next = mdns_parse_rr(pkt, len, 12, &rr);
	CHECK("build_goodbye_ttl_zero", next > 12 && rr.ttl == 0);

	/* sidecar_port omitted when zero */
	MdnsBeaconParams q = sample_params();
	q.sidecar_port = 0;
	len = mdns_beacon_build_packet(pkt, sizeof(pkt), &q, "cam0", &ip, 1, 120);
	CHECK("build_optional_omit_len", len > 12);
	int pos = 12;
	uint16_t an = mdns_get16(pkt + 6);
	bool saw_sidecar = false;
	for (uint16_t i = 0; i < an; i++) {
		MdnsRr r;
		int nx = mdns_parse_rr(pkt, len, pos, &r);
		if (nx <= pos) break;
		if (r.type == MDNS_TYPE_TXT && r.rdlength > 0) {
			char v[32] = "";
			saw_sidecar = mdns_txt_get_value(pkt + r.rdata_offset, r.rdlength,
				"sidecar_port", v, sizeof(v));
		}
		pos = nx;
	}
	CHECK("build_omit_sidecar", !saw_sidecar);
	return failures;
}

/* ── Config defaults ─────────────────────────────────────────────────── */

static int test_discovery_defaults(void)
{
	int failures = 0;
	VencConfig cfg;
	venc_config_defaults(&cfg);
	CHECK("disc_default_enabled", cfg.discovery.enabled == true);
	CHECK("disc_default_service",
		strcmp(cfg.discovery.service_type, "_waybeam-venc._tcp") == 0);
	CHECK("disc_default_name_empty", cfg.discovery.name[0] == '\0');
	return failures;
}

int test_mdns_beacon(void)
{
	int failures = 0;
	failures += test_wire_name_roundtrip();
	failures += test_wire_txt_get();
	failures += test_build_packet_records();
	failures += test_build_packet_guards();
	failures += test_discovery_defaults();
	return failures;
}
