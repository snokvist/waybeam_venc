# Discovery & Trust Migration Spec

**Status:** venc Phases 1/1.5/1.6 shipped (PR #147). Phases 2–7 **implemented** on branch `claude/mod-mdns-waybeam-venc-uy7y9z` (waybeam-hub commit `5960634`, Waybeam-android commit `afd4df9`) — **pending PR review + hardware soak**. Plan revised 2026-06-16 — trust deleted outright, subscribe = live `outgoing.server` config-set (no new venc endpoint).
**Branch:** `claude/mod-mdns-waybeam-venc-uy7y9z` (all repos)
**Scope:** 3 repos — `waybeam_venc` (anchor), `waybeam-hub`, `waybeam-android` (later)
**Owner doc:** this file is the master. Hub-side companion:
`waybeam-hub/docs/discovery-trust-migration-plan.md`.

---

## 1. Motivation

Discovery today is **hub-centric**. `mod_mdns` lives in both the vehicle (air)
hub and the ground hub. The vehicle hub announces `_waybeam-hub._tcp.local`,
discovers peers via multicast, and **passively seeds trust** through
`hub_ip_trust_add_peer()` + `mod_sync_notify_peer_discovered()`.

Two facts break this model:

1. **venc is always present; the hub is only sometimes present.** venc owns the
   camera/ISP/encoder and runs on every vehicle. The hub is optional (minimal
   builds, `vehicle_wfb_ng`, diagnostics images, Android-direct topologies).
   Anchoring discovery on the *sometimes-present* node means the
   *always-present* device can be invisible.
2. **Ambient mDNS trust is more than we need.** The vehicle trusts whoever it
   *discovers* on multicast. But the vehicle is fundamentally **passive** — it
   never needs to find the ground; the ground subscribes to it. Trust can ride
   on that explicit action instead of an ambient multicast packet.

### Goal

- Make **venc the always-on mDNS device beacon** — the layer of truth for
  *device* discovery.
- **Remove mDNS from the vehicle (air-side) hub.** Keep mDNS only on the
  ground hub (for ground↔Android peering).
- **Delete the `hub_ip_trust` layer outright** (both hubs) — the wfb RF link is
  the security boundary; accept-all on the private link.
- Ground / Android **discover venc, then probe independently** for hub
  capabilities at a known port.

---

## 2. Core idea: device discovery vs. capability probing

The current `mod_mdns` does **two** jobs. Only one of them belongs on venc:

| Job | What it is | Where it goes |
|---|---|---|
| **Announce** | Build PTR/SRV/TXT/A, answer queries (RFC 6762/6763 wire engine) | **venc** (always-on beacon) + ground hub (its own service) |
| **Discover + consume** | Parse peers into the `mod_mdns` browse cache (no `mod_sync`, no `hub_ip_trust` — both deleted) | **ground hub only** (vehicle becomes passive) |

The new mental model:

- **Device discovery** = "find the vehicle on the network." Answered by venc's
  beacon (`_waybeam-venc._tcp`). Always available.
- **Capability probing** = "what services ride along on that vehicle?"
  Answered by *probing known ports* over REST, not by enumerating everything in
  mDNS TXT. The hub is just "does `:8060/status` answer."

This cleanly separates an always-true fact (the device exists, here is its IP
and stable endpoints) from a sometimes-true fact (a hub is running, here are
its live capabilities).

---

## 3. Target architecture

### 3.1 Per-node responsibilities

| Node | Announces | Discovers | Trust seeding |
|---|---|---|---|
| **venc (air)** | `_waybeam-venc._tcp` — always | — | n/a (stateless beacon) |
| **vehicle hub (air)** | *removed* | *removed* | **removed (accept-all)** |
| **ground hub** | `_waybeam-hub._tcp` (ground↔Android) | venc beacon **+** hub beacons | **removed** (mDNS browse only) |
| **Android (later)** | optional | venc beacon **+** hub beacons | n/a (client) |

### 3.2 Discovery + subscribe flow (target state)

```
                 ┌────────────────────────── AIR ──────────────────────────┐
                 │  venc  (always up)            vehicle hub (maybe up)     │
                 │  ┌──────────────┐             ┌──────────────────────┐   │
   mDNS query    │  │ mDNS beacon  │             │ HTTP :8060           │   │
  ───────────────┼─▶│ _waybeam-    │             │ /status /modules     │   │
   (ground/andr) │  │  venc._tcp   │             │ (capabilities)       │   │
                 │  │  HTTP :80    │             └──────────▲───────────┘   │
                 │  │  sidecar:5602│                        │               │
                 │  └──────┬───────┘                        │ REST probe    │
                 └─────────┼────────────────────────────────┼───────────────┘
                           │ 1. mDNS response                │ 3. probe :8060
                           ▼ (vehicle IP + stable endpoints) │    after discovery
                 ┌──────────────────────────────────────────┴──────────────┐
                 │  GROUND hub / Android                                    │
                 │  2. learn vehicle IP from venc beacon                    │
                 │  4. SUBSCRIBE = POST outgoing.server to venc :80 (live)  │
                 │     GS present → GS owns stream + restreams to others    │
                 │  5. no trust layer — the wfb link is the boundary        │
                 └─────────────────────────────────────────────────────────┘
```

Key invariant: **the vehicle never initiates discovery.** It answers mDNS
(venc), answers REST probes (hub, if up), and accepts a live `outgoing.server`
retarget from whoever subscribes. No trust layer — the wfb link is the boundary.

---

## 4. mDNS service design (venc beacon)

### 4.1 Service type and naming

New service type, distinct from the hub's: **`_waybeam-venc._tcp.local`**.
A distinct type avoids record collisions when both venc and a hub run on the
same host, and lets consumers query specifically for "devices" vs "hubs."

**Naming — `waybeam` only, serial-suffixed.** We do **not** announce the
legacy `openipc.local` Majestic alias; venc owns a single `waybeam`-family
name. To keep that name unique and stable across a multi-camera fleet, the
host name and the service instance are suffixed with a short slice of the
SoC die ID (§4.5):

| Record | Value | Example |
|---|---|---|
| Service instance | `waybeam-<suffix>` | `waybeam-f5cb1d` |
| Host name (SRV target / A) | `waybeam-<suffix>.local` | `waybeam-f5cb1d.local` |
| Service type (PTR) | `_waybeam-venc._tcp.local` | — |

Because the suffix derives from a hardware-unique value, every camera gets a
collision-free, deterministic name with **no RFC 6762 conflict-renaming
churn** (no boot-order-dependent `waybeam-2.local`). One camera on the wire
still reads cleanly as `waybeam-<suffix>.local`; N cameras are N distinct
DNS-SD instances under the one service type. The bare `waybeam.local` is
**not** claimed (it can't be unique across a fleet); consumers address by the
suffixed name and key by the full serial (§4.5).

### 4.2 TXT schema — stable identity/endpoints ONLY

venc's headline fields (bitrate, fps, mode) are **live-mutable**. Putting them
in TXT means re-announcing on every `/api/v1/set` — noisy and racy against the
encode loop. **Rule: TXT carries only the few stable facts a consumer can't
already get from the standard DNS records. Everything else is probed.**

The **service type `_waybeam-venc._tcp` is the recognition signal** — it tells
a consumer "this is an OpenIPC camera running the waybeam encoder." The
hostname, IP, and primary port are already carried by the standard SRV / A /
instance-name records. So TXT only needs to add what those records can't:

| TXT key | Source | Example | Why it's here |
|---|---|---|---|
| `proto` | constant | `1` | wire-schema version (forward-compat) |
| `version` | `VENC_VERSION` | `0.18.0` | waybeam version — the one volatile-but-stable identity bit |

> **D1 — TXT schema freeze (LOCKED):** exactly the two keys above — `proto`
> and `version`.
>
> **Implementation status: DONE (Phase 1.5).** The beacon emits `proto` and
> `version` only; `sidecar_port` was removed. Naming is serial-suffixed
> (§4.1, §4.5) via `src/device_id.c`, and the full die ID is exposed at
> `GET /api/v1/config` → `data.device.serial`. See `src/mdns_beacon.c`,
> `src/device_id.c`, `src/venc_api.c::handle_config`,
> `tests/test_mdns_beacon.c`.

**Deliberately NOT in TXT — probe instead:** `sidecar_port`, `backend`/`model`/
SoC, `sensor`, `codec`, `web_port` (it's the SRV port), `name` (it's the
instance label), the full serial (it's exposed via the API, §4.5), and
`api_contract`. These are either redundant with the DNS records or are
*facts a consumer fetches with one `GET /api/v1/config` after device
discovery*. In particular `sidecar_port` was intentionally **removed** from
the beacon: it is a core venc mechanic, but a subscriber has already resolved
the vehicle IP from the beacon and can read the live port (and everything else
it needs) straight from the config endpoint — no reason to duplicate a
restart-stable port into the announce. This keeps the discovery layer to a
single always-true fact ("a waybeam encoder lives here, at this name/IP") and
pushes everything else to capability probing (§6). Anything that changes live
(bitrate/fps/mode/stream enable) is **forbidden** in TXT.

DNS records: PTR (type→instance), SRV (instance→`<host>.local:web_port`),
TXT (above), A (one per local IPv4, wlan first).

### 4.3 Announce-only responder

venc needs the **announce half** of `mod_mdns` only — no peer parsing, no
`mod_sync`, no `hub_ip_trust`. Port the hub's hand-rolled raw-socket engine
(zero external deps, RFC 6762/6763), **not** the dead `tinysvcmdns` in
`sdk/ssc338q/lib/` (unused, do not resurrect).

- Multicast UDP socket on `224.0.0.251:5353`, non-blocking.
- 3-packet startup announce (0/250/500 ms), respond to PTR queries with jitter.
- **Goodbye (TTL=0) on exit** → beacon liveness == camera liveness.
- **Implemented as a self-contained thread** (`src/mdns_beacon.c`) started
  from `main()` around `backend_execute()`, rather than folded into the
  per-backend runtime loop. This keeps the fragile `star6e_runtime.c` /
  `maruko_runtime.c` reinit paths untouched and the beacon fully isolated from
  the encode path; it is stopped (goodbye) before any SIGHUP-respawn exec.

### 4.4 Shared wire codec (avoid a two-repo fork)

The DNS encode/parse core would otherwise exist twice (venc + ground hub).
**It is a version-stamped source** (`MDNS_WIRE_VERSION`) shared by both repos —
same discipline as `vendor/venc_ring/` (`VENC_RING_VERSION`, "keep in sync").

> **Attribution:** the wire codec and multicast socket handling derive from
> [OpenIPC herald](https://github.com/OpenIPC/firmware/tree/master/general/package/herald)
> (MIT) — the compact mDNS/DNS-SD stack for the OpenIPC project — via
> waybeam-hub's `mod_mdns`, which inlined herald's wire helpers. The credit
> chain is preserved in `src/mdns_wire.{c,h}` and `src/mdns_beacon.c`.

- **Phase 1 location:** the codec ships as `src/mdns_wire.{c,h}` +
  `include/mdns_wire.h` with `MDNS_WIRE_VERSION 1` — pure functions (build
  PTR/SRV/TXT/A, parse records, TXT get). No socket or platform code; the
  caller owns the socket.
- venc links the encode path; ground hub links encode+parse. Hub keeps its
  socket/lifecycle/`mod_sync` glue on top.
- **D4 (open):** before Phase 2, decide whether to promote this to a
  dedicated `vendor/mdns_wire/` directory (matching `vendor/venc_ring/`) and
  which repo owns the version bump/sync ritual.

### 4.5 Device identity — SoC die ID (serial-suffix naming)

The serial suffix in `waybeam-<suffix>.local` comes from the **SigmaStar SoC
die ID** — the same value `ipcinfo -i` prints. It is hardware-unique,
read-only, and stable across reboots/reflashes, which is exactly what a fleet
key needs.

#### How `ipcinfo` reads it (verified on Star6E / ssc338q @ 192.168.1.13)

`ipcinfo` is the OpenIPC [`ipctool`](https://github.com/OpenIPC/ipctool)
binary. Its SigmaStar HAL (`src/hal/sstar.c` → `sstar_get_die_id`,
`src/hal/sstar.h`) reads the die ID natively from RIU registers via a
`/dev/mem` mmap — **no SDK call, no shell-out, no extra dependency**:

1. **Detect chip generation:** read `0x1F003C00` (low 16 bits). On the test
   vehicle this returns `0xF1` → `INFINITY6E` (matches
   `/sys/class/mstar/msys/CHIP_ID`).
2. **Pick the die-ID base** by generation:
   - `INFINITY6E` (ssc338q — our Star6E): base `0x1F203150` (`CHIP_ADDR1`)
   - `INFINITY6` (non-E): base `0x1F004058` (`CHIP_ADDR2`)
   - `INFINITY6C` (ssc30kq — **Maruko**): **die ID is NOT exposed**;
     `sstar_get_die_id` returns false. See the fallback below.
3. **Assemble 48 bits** as the low 16 bits of three consecutive registers,
   MSW-first (`base+8`, `base+4`, `base+0`), each formatted `%04X`.

Device-verified on `192.168.1.13` (`ipcinfo -i` = `47D1CEF5CB1D`):

```
0x1F203158 = 0x000047D1   →  "47D1"   (base+8, MSW)
0x1F203154 = 0x0000CEF5   →  "CEF5"   (base+4)
0x1F203150 = 0x0000CB1D   →  "CB1D"   (base+0, LSW)
                              ───────
                              47D1CEF5CB1D   == ipcinfo -i  ✓
```

#### Native extraction in venc (no fork)

venc reads the same three registers directly: `open("/dev/mem")`,
`mmap` the page containing the die-ID base (page-align the physical address,
add the in-page offset), read the three 16-bit values, format the 12-hex
string. Done **once at startup**, cached — never on the encode path, no
`popen("ipcinfo -i")` dependency. Re-uses the existing `/dev/mem` access the
SDK already needs. (Implementation may instead lift the value from a SigmaStar
SDK chip-ID API if one is cleaner than raw `/dev/mem`; the register addresses
above are the ground truth either way.)

#### Suffix derivation

- **Suffix** = last 6 hex chars of the 12-hex die ID, lowercased →
  `47D1CEF5CB1D` → `f5cb1d`. 24 bits ≈ 16.7 M space; ample for a LAN, and the
  tail visibly matches `ipcinfo -i` for field debugging.
- **Host / instance** = `waybeam-f5cb1d` / `waybeam-f5cb1d.local`.
- The **full 12-hex die ID** is the authoritative fleet key. It is **not** in
  TXT (§4.2) — a consumer fetches it from `GET /api/v1/config` after discovery
  (add a read-only `device.serial` field there). The 6-hex suffix is for
  display/addressing; on the ~1-in-16M chance two suffixes collide, the full
  serial from the API disambiguates.

#### Maruko (ssc37x / INFINITY6C-class) fallback

**Device-verified on 192.168.2.12 (`ssc37x`, gen reg `0xF9`):** `ipcinfo -i`
returns **empty**, the die-ID register region reads all-zero (both the
INFINITY6E base `0x1F203150` and the `0x1F004058` base), there is **no serial
in the flash env**, and the EMAC MAC is the **same dummy `00:00:23:34:45:66`**
seen on the Star6E. So neither the die ID nor the MAC is a usable per-unit
identifier on this hardware.

Fallback chain (first that applies):

1. `discovery.name` config override, if set (operator-assigned) → used as-is,
   no suffix.
2. Bare `waybeam` with **no suffix**, accepting RFC 6762 conflict-renaming
   (`waybeam-2.local`) in the rare multi-Maruko-on-one-LAN case.

The EMAC MAC is explicitly **not** used (verified non-unique). This asymmetry
(Star6E gets a stable hardware suffix, Maruko does not) is acceptable: the
dominant fleet hardware is Star6E, and a Maruko operator can always pin a
unique name via `discovery.name`.

#### Availability detection (implementation rule)

Don't enumerate every chip generation — **read and validate**. Attempt the
die-ID read at the gen-appropriate base, then reject the result if it is
all-zero or all-`0xFFFF`. A valid 48-bit non-degenerate value → use the
suffix; anything else → fall back. This is what distinguishes Star6E (valid)
from Maruko (all-zero) without hardcoding the full SigmaStar gen enum.

#### Consumer flow (hub / Android)

1. Browse `_waybeam-venc._tcp` → N instances (`waybeam-<suffix>`).
2. Resolve SRV→A for each → vehicle IP + web port.
3. `GET /api/v1/config` → read `device.serial` (full die ID) as the stable
   fleet key, plus `sidecar_port` and any capabilities needed.
4. Track/de-dupe by full serial, never by IP or friendly name.

### 4.6 Bare `waybeam.local` convenience alias (Phase 1.6)

The suffixed name is unambiguous but not memorable. Since **most deployments
run a single vehicle**, the beacon also claims the bare host name
**`waybeam.local`** so a human can just `ping waybeam.local` / open
`http://waybeam.local`. Config: `discovery.bareAlias` (default **true**).

**Mechanics.** A single responder can own multiple host names, so the beacon
publishes A records for **both** `waybeam-<suffix>.local` (always; the SRV
target) and `waybeam.local` (the alias) → same IPs. The alias is **only**
claimed when the primary name is *not* already `waybeam` (i.e. a suffixed
device); on the Maruko bare-`waybeam` fallback there is nothing extra to add.
The beacon also answers direct `A`/`ANY` queries for both names.

**Multi-device safety (the only real risk).** A-records carry the cache-flush
bit, so two devices both announcing `waybeam.local` → different IPs would flap
in resolver caches. The beacon resolves this by **conflict detection on its
existing RX socket** (no new socket, no formal probing):

- It scans incoming mDNS for an `A waybeam.local` from another address.
- Tiebreak is **RFC 6762 §8.2 lexicographic** on the A rdata → in practice
  the **higher IP keeps `waybeam.local`, the lower IP yields** it (multicasts
  a TTL=0 goodbye for the alias only and stops announcing it). The unique
  suffixed name is never touched, so the yielding device stays fully
  reachable. The winner re-asserts immediately to defend.

Result: **one device → `waybeam.local` always resolves**; N devices → exactly
one holds it (deterministically by IP), all reachable by `waybeam-<suffix>`.

> **Known simplification:** suppression is sticky until restart — if the
> winner later leaves, the yielder does not re-claim `waybeam.local` until it
> restarts. Acceptable for the single-device-dominant use case; revisit only
> if multi-device churn proves to matter. Tiebreak is by current IP, so it is
> stable only as far as the IPs are (static / DHCP-reserved).

### 4.7 RFC 6762 / 6763 conformance posture

The beacon is an **announce-focused responder**, not a full zeroconf stack.
What it does to the letter, and where it deliberately simplifies:

| Area | Posture |
|---|---|
| Multicast endpoint | `224.0.0.251:5353`, `IP_MULTICAST_TTL=1` (link-local) ✓ |
| Cache-flush bit | Set on **unique** records (SRV/TXT/A); **never** on the shared PTR (RFC 6762 §10.2) ✓ — interop-critical, and correct |
| Goodbye | TTL=0 multicast on stop (full set) and on alias yield (alias-only) (§10.1) ✓ |
| Query handling | Answers PTR/ANY for the service type and A/ANY for both host names; full record set in each response ✓ |
| **Probing (§8.1)** | **Not implemented.** Unique names are not probed before claiming. The suffixed name is hardware-unique by construction; the bare alias uses *optimistic announce + reactive §8.2 conflict resolution* instead. Trade-off: a transient double-claim is possible at boot until the loser yields (sub-second in practice). |
| Announce cadence (§8.3) | 3 packets at 0/250/500 ms, then re-announce every 60 s (RFC suggests ≥2 packets ≥1 s apart; ours is faster — more aggressive, still valid). |
| Record TTL | 120 s for all records (RFC suggests 4500 s for PTR/TXT). Paired with the 60 s refresh, caches never expire; chosen for faster staleness recovery on a mobile FPV link. |
| Unicast-response (QU) bit | Ignored — always responds via multicast. Acceptable; resolvers still receive answers. |
| `_services._dns-sd._udp` meta-query (§9) | Not answered — direct `_waybeam-venc._tcp` browse works (verified against avahi); whole-network service enumeration won't list us. Minor, additive later if needed. |

Validated end-to-end against **avahi** (the de-facto reference responder/
resolver) on both SoC families — see §8 Phase 1.5/1.6 rows.

---

## 5. Trust model — removed, not migrated

> **Supersedes earlier drafts.** Prior versions of this section replaced
> ambient mDNS trust with "trust-on-subscribe." That intermediate step is
> **dropped**: `hub_ip_trust` is deleted outright. The wfb RF link is the
> security boundary — no internet exposure, no shared L2 segment to defend.
> (Decisions locked 2026-06-16.)

### 5.1 What goes away (both hubs)

`hub_ip_trust` gated exactly two inbound paths and seeded from three:

| Path | file:line | Was | Becomes |
|---|---|---|---|
| Sync hello accept | `mod_sync.c:424` | DENY if untrusted | moot — `mod_sync` deleted (§5.2) |
| Telemetry UDP ingest | `mod_telemetry.c:399` | DROP + `udp_rx_rejected++` | accept any source IP |
| Seed on sync hello | `mod_sync.c:346` | `hub_ip_trust_add_peer()` | deleted with `mod_sync` |
| Seed on mDNS discover | `mod_sync.c:617`, `mod_mdns.c:1075` | `hub_ip_trust_add_peer()` | deleted |

After removal there is no IP allowlist anywhere. Telemetry, video subscribe, and
all REST endpoints accept any source on the link.

### 5.2 `mod_sync` is deleted entirely

`mod_sync` (the 8060 unicast hello/state gossip protocol) has **four** consumers
— removing the menu-mirror alone does *not* kill it. Discovery now comes from
the venc beacon, so all four are repointed or removed first:

| Consumer | file:line | Replacement |
|---|---|---|
| Remote menu-mirror (`state` msgs) | `mod_sync.c` state path | **deleted** (feature retired) |
| Auto-subscribe one-shot trigger | `mod_webui.c:4153` | "venc beacon seen" event from `mod_mdns` |
| Peer/capability list in `/status` | `mod_webui.c:1956, 2227` | `mod_mdns` browse cache |
| Link-log targeting | `mod_link_log.c` | vehicle IP from the venc beacon |

Once those three non-menu consumers read from the `mod_mdns` browse cache, the
whole module is removed: `src/mod_sync.{c,h}`, its `CORE_SRCS`/`TEST_LIB_SRCS`
entries, the `hub_modules_register(&mod_sync)` call, and every `mod_sync_*`
caller.

### 5.3 What stays

- **Ground hub:** `mod_mdns` survives but now **browses `_waybeam-venc._tcp`**
  as the vehicle anchor (alongside its own ground service). It no longer seeds
  trust or notifies `mod_sync` (both gone).
- **Vehicle hub:** telemetry, PWM, OSD only. No `mod_mdns`, no `mod_sync`, no
  `hub_ip_trust`, no `/subscribe_video*` — subscribe moves to venc (§6).

---

## 6. Subscribe, arbitration & capability probing

### 6.1 Subscribe = a live `outgoing.server` config-set (no new endpoint)

venc already owns the only field that controls its RTP destination, and it is
already classified `MUT_LIVE`:

```c
FIELD(outgoing, server, FT_STRING, MUT_LIVE),   /* src/venc_api.c:400 */
```

Setting it calls `apply_server()` (`src/venc_api.c:1492/1504/1510`), which
retargets the running stream via the output seqlock — **no restart, no
respawn.** venc exposes config-set as a **dot-notation `GET /api/v1/set`** (the
same setter used for `video0.fps`, `outgoing.enabled`, …); there is **no**
`POST`/JSON `/api/v1/config` route (`/api/v1/config` is GET-only). So the
canonical "subscribe" is:

```
GET /api/v1/set?outgoing.server=udp://<my-ip>:<my-port>
```

> **Device-verified 2026-06-17 (Star6E .13).** `/api/v1/set` is the live
> setter; an early draft of this spec (and the first hub/Android cut) used a
> non-existent `POST /api/v1/config` — corrected in both repos. The live
> **udp→udp** retarget was proven end-to-end: with venc in UDP mode, a
> `set?outgoing.server=` moved the running RTP stream to a new dst (captured
> 5k pkts/3s before and after) with **no respawn** (PID unchanged) and the old
> dst went silent.

> **⚠ Topology constraint — live retarget needs UDP mode.**
> `star6e_output_apply_server()` (`src/star6e_output.c:676`) **rejects any live
> server change while the output is `shm://`** (the wfb SHM ring): *"live switch
> away from shm:// is not supported (requires restart)"*, and likewise refuses a
> live switch *to* `shm://`. So this subscribe works only when venc is already
> in a **UDP** topology (udp→udp destination change). In a **wfb-RF** deployment
> venc stays `shm://` feeding wfb-air, the ground is on a different L2 and never
> sees the venc beacon, and delivery is via wfb-gs — the retarget is simply not
> used. mDNS-discovery + live-retarget is therefore a **direct-WiFi/LAN**
> topology feature (Android-direct, shared-WiFi ground).

> **Decision (D2, resolved):** there is **no** `/api/v1/subscribe` endpoint and
> **no** subscription lock. A dedicated verb would only wrap a field that
> already does the job; `GET /api/v1/config` already reports the current
> destination and each `set` overwrites it (last-writer-wins). venc earns
> "anchor" by *owning the field*, not by exposing a new verb.

Consumers already compute their own IP (ground `derive_local_ip`, Android
`getDeviceIp`), so socket-IP inference buys nothing and is not implemented.

### 6.2 Stream arbitration = topology, not a lock

The vehicle streams to exactly one destination. Contention is **designed out**
rather than locked:

- **Ground station present:** the GS owns the vehicle's destination (sets
  `outgoing.server` to itself) and **restreams** to any other viewer via its
  existing `/restream/subscribe` (pixelpilot forward path — already independent
  of `mod_sync`/`hub_ip_trust`). Android becomes a restream copy-recipient and
  never touches the vehicle's destination.
- **No ground station:** the sole decoder (e.g. Android) sets `outgoing.server`
  to itself directly. "One decoder at a time" makes last-writer-wins correct.

This is why the 409 lock, the one-shot auto-subscribe guard, and the
ground↔Android coexistence dance all disappear — they existed to manage a
contention the restream topology removes.

> **Restream is single-subscriber today** (`restream_activate` takes one dst).
> Fine for one-decoder-at-a-time; multi-recipient fan-out is a later option.

### 6.3 Capability probing (retained, unchanged)

After learning the vehicle IP from the venc beacon, a consumer discovers what
*else* rides on that box by probing known ports — not by reading mDNS TXT:

1. `GET http://<vehicle>:8060/status` + `/modules` (well-known hub port).
2. `200` → hub present; parse its live capability list.
3. Refused / ~300 ms timeout → no hub; drive venc directly at `:80/api/v1/*`
   (+ sidecar `:5602` for frame metadata).

> **Decision (D3, resolved):** pure REST probe, zero coupling. The venc-hosted
> hub registry (`/api/v1/peers`) is **not** adopted — the restream topology and
> a single well-known hub port make blind-timeout churn a non-issue.

---

## 7. Repo-by-repo change list

### 7.1 `waybeam_venc` (anchor) — DONE, no further change

The beacon shipped in PR #147 (Phases 1/1.5/1.6: `src/mdns_wire.{c,h}`,
`src/mdns_beacon.{c,h}`, `src/device_id.{c,h}`, the `discovery` config block,
serial-suffix naming + `waybeam.local` alias). **No new endpoint is needed:**
subscribe is the existing `MUT_LIVE` `outgoing.server` config-set (§6.1). venc
is feature-complete for this migration. The wire codec lives at
`src/mdns_wire.{c,h}` today; promoting it to a shared `vendor/mdns_wire` is the
only open venc-touching item, gated on **D4** (before hub Phase 2).

### 7.2 `waybeam-hub`

- **Ground:** teach `mod_mdns` to browse `_waybeam-venc._tcp` as the vehicle
  anchor; drive the one-shot auto-subscribe from a "venc beacon seen" event;
  subscribe by POSTing `outgoing.server` to venc `:80/api/v1/config` (not the
  vehicle hub). Keep / extend `/restream/subscribe` for fan-out to Android.
- **Ground:** repoint `/status` peer list + link-log targeting to the
  `mod_mdns` browse cache; remove menu-mirror; then **delete `mod_sync`**.
- **Both:** **delete `hub_ip_trust`** (accept-all; §5.1) — drop the check at
  `mod_telemetry.c:399` and the seed calls in `mod_mdns.c`/`mod_sync.c`.
- **Vehicle:** remove `mod_mdns`, `mod_sync`, `hub_ip_trust`, and the
  `/subscribe_video*` endpoints from the vehicle profile. Vehicle hub =
  telemetry/PWM/OSD only; venc is the anchor and owns subscribe.
- **Follow-up (cleanup):** `mod_mdns`'s `iq_ct` TXT field + `mod_mdns_set_iq_ct()`
  seed/toggle (`/etc/iq_settings.json`, WebUI) is now dead — the only consumer
  (Android auto-colortrans) was removed (§7.3) and the vehicle no longer
  advertises mDNS, so this only ever compiled on the **ground** hub, which has
  no camera. Safe to strip from `mod_mdns.c`/`mod_webui.c` in a later pass;
  flagged, not deleted here.

### 7.3 `waybeam-android`

- Browse `_waybeam-venc._tcp` for the vehicle device list (alongside the hub
  service); merge into the existing discovered-hubs map.
- **Prefer the GS restream when a ground hub is present** (`POST
  /restream/subscribe` to the GS); fall back to a **direct venc subscribe**
  (`POST /api/v1/config {outgoing.server}`) when no GS exists.
- Drop the self-registration as a discoverable decoder service — restream is
  pull-based, so nothing needs to discover Android.
- Identity stays an informational label; no token/auth (private link).
- **Drop discovery-driven auto-colortrans (0.7.1).** The app previously read the
  vehicle hub's `iq_ct` TXT to auto-apply the Color Transform shader. With the
  vehicle hub no longer advertising mDNS (Phase 6), that signal is gone and venc
  exposes no `iq_ct` enable flag (colortrans is a structured ISP/IQ matrix via
  `GET /api/v1/iq`, not a boolean). Resolution: Color Transform is a **manual
  filter** like the other shaders; the `auto_colortrans` setting + mDNS-change
  plumbing were removed. This was the app's last dependency on the vehicle-hub
  beacon, so removing it completes the venc-as-sole-vehicle-anchor model.

---

## 8. Phased rollout (each phase independently shippable & revertible)

> **Revised 2026-06-16.** venc needs **no** new code (subscribe = live
> `outgoing.server` config-set, §6.1). The remaining work is hub/Android
> repointing + deletions. Additive phases (repoint to the new path) land and
> soak before any deletion.

| Phase | Repo(s) | Change | Coexistence guarantee |
|---|---|---|---|
| **0** | all | This spec | — |
| **1 / 1.5 / 1.6** ✅ | venc | mDNS beacon + serial-suffix naming + `waybeam.local` alias (PR #147). **Done** — host tests pass; `make verify` cross-builds both backends clean. | Additive; old discovery fully intact. |
| **2** ✅ | hub (ground) | `mod_mdns` also browses `_waybeam-venc._tcp`; auto-subscribe fires on "venc beacon seen" and POSTs `outgoing.server` to venc. **Done** (commit `5960634`) — folded with Phase 4: `mod_sync` deleted outright rather than left idle. | Old sync path still present; new path proven first. |
| **3** ✅ | android | Browse venc beacon; restream-preference (GS present → `/restream/subscribe`; else direct venc subscribe). **Done** (commit `afd4df9`). | Hub-mDNS fallback kept until parity proven. |
| **4** ✅ | hub (ground) | Repoint `/status` + link-log to mDNS cache; remove menu-mirror; **delete `mod_sync`**. **Done** (commit `5960634`). | Build green; peers come from mDNS. |
| **5** ✅ | hub (both) | **Delete `hub_ip_trust`** (accept-all); drop the telemetry check + mDNS seeds. **Done** (commit `5960634`). | Telemetry from any IP; build green. |
| **6** ✅ | hub (vehicle) | Remove `mod_mdns` + `mod_sync` from the vehicle profile (`-DHUB_MOD_MDNS` dropped, `mdns.enabled=false`). **Done** (commit `5960634`). Vehicle `/subscribe_video*` route handlers left in place (now unused/harmless) — flagged for a follow-up endpoint cleanup. | venc beacon is sole discovery; vehicle hub = telemetry/PWM/OSD. |
| **7** ✅ | all + coord | Android drops self-registration; purge dead config (`trusted_peers`, `reject_unknown` removed; `sync_bind`/`sync_port` retained — still drive the hub's own `_waybeam-hub._tcp` announce, NOT dead); rewrite/retire `protocols/hub-sync.md`; update this spec, SNAPSHOT, roadmaps. **Done** (commits `5960634`/`afd4df9` + coord-repo docs). | `/audit-protocols` clean. |

Phases 2–3 are **purely additive** (new path by config, fully revertible). The
deletions in 4–6 land only after the replacement runs on hardware. The one
security-relevant consequence — telemetry accepting any source IP — is accepted
(private wfb link is the boundary).

---

## 9. Open decisions (must be locked before the phase that needs them)

- **D1 — TXT schema freeze (LOCKED).** Target key set: `proto`, `version`
  (§4.2). `sidecar_port` is being dropped in Phase 1.5 (D6); everything else is
  probe-discoverable / via `GET /api/v1/config`.
- **D2 — Trust model (RESOLVED 2026-06-16).** `hub_ip_trust` is **deleted
  outright** (not migrated to trust-on-subscribe); accept-all on the private
  link. `mod_sync` is deleted entirely. Subscribe = live `outgoing.server`
  config-set with last-writer-wins, no lock. §5, §6.1.
- **D3 — Probe vs venc-registry (RESOLVED 2026-06-16).** Pure REST probe, zero
  coupling; no venc `/api/v1/peers` registry. §6.3.
- **D4 — `vendor/mdns_wire` ownership** (before Phase 2). Which repo is the
  source of truth for the vendored codec + the version bump/sync ritual.
- **D5 — Vehicle hub mDNS removal mechanism** (Phase 6). Config-gate first
  (`mdns.enabled=false`), then drop `HUB_MOD_MDNS` from the `vehicle`/
  `vehicle_wfb_ng` profiles? Confirm `make test` keeps MDNS enabled.
- **D7 — Bare `waybeam.local` alias (Phase 1.6, DONE).** §4.6. Default-on
  (`discovery.bareAlias`); single responder publishes both the suffixed name
  and the bare alias; multi-device contention resolved by RFC 6762 §8.2 IP
  tiebreak (higher IP keeps it), suppression sticky until restart.
- **D6 — Device identity & serial-suffix naming (Phase 1.5).** §4.1 + §4.5.
  Decisions folded in: `waybeam`-only (no `openipc.local`); host/instance
  suffixed with the last 6 hex of the SoC die ID; full die ID is the fleet key
  via `GET /api/v1/config` (new `device.serial` field), not TXT; `sidecar_port`
  removed from TXT. Open sub-points: (a) raw `/dev/mem` read vs an SDK chip-ID
  call; (b) the Maruko/INFINITY6C fallback (no hardware die ID).

---

## 10. Risks

- **Two-repo wire fork** if D4 is skipped → diverging RFC implementations.
  Mitigated by `vendor/mdns_wire` + version stamp.
- **Discovery gap** after the vehicle-hub mDNS removal (Phase 6) → ground/
  Android can't find the vehicle if the venc beacon is down. Mitigated by the
  additive Phases 2–3 soak and a config rollback (`mdns.enabled=true` on the
  vehicle hub) until the venc beacon is proven as the sole anchor.
- **Open telemetry ingest** after `hub_ip_trust` removal (Phase 5) → any host
  on the link can inject telemetry UDP. Accepted: the wfb RF link is the
  security boundary (no internet, no shared L2).
- **Probe timeout churn** on hub-less vehicles → bounded by short timeout.
- **Slower hub-down detection** (probe failure vs mDNS goodbye) → acceptable;
  the *device* (venc) still has a real goodbye.
- **Strict venc build** (`-Werror -Wextra`, dual backend) → wire codec must be
  warning-clean on both `star6e` and `maruko`.

---

## 11. Test plan (high level)

- venc: unit tests for TXT build + DNS record encode + goodbye; `make verify`
  (both backends); on-device `avahi-browse -r _waybeam-venc._tcp` smoke test.
- hub: extend `make test` with `_waybeam-venc._tcp` parse + mDNS browse-cache
  peer registry (replacing `mod_sync`); capability-probe client tests (200 /
  refused / timeout); confirm builds stay green after `mod_sync` + `hub_ip_trust`
  deletion.
- Cross: ground discovers venc beacon → auto-subscribe one-shot fires exactly
  once and POSTs `outgoing.server` to venc; Android-direct (no hub) subscribe
  via venc config-set works; GS-present → Android receives the GS restream and
  never retargets the vehicle.

---

## 12. Out of scope (this spec)

- venc code — Phases 1/1.5/1.6 already shipped (PR #147); nothing further needed.
- Hub/Android implementation (tracked here for protocol alignment; built in
  their own repos under Phases 2–7).
- Changing the sidecar wire format or the video-lock semantics (reused as-is).
```
