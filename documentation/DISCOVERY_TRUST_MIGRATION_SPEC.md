# Discovery & Trust Migration Spec

**Status:** Phase 1 — Spec (no code yet)
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
- **Drop the complex hub_ip_trust mDNS-seeded trust layer** on the vehicle in
  favor of **intent-based trust-on-subscribe**.
- Ground / Android **discover venc, then probe independently** for hub
  capabilities at a known port.

---

## 2. Core idea: device discovery vs. capability probing

The current `mod_mdns` does **two** jobs. Only one of them belongs on venc:

| Job | What it is | Where it goes |
|---|---|---|
| **Announce** | Build PTR/SRV/TXT/A, answer queries (RFC 6762/6763 wire engine) | **venc** (always-on beacon) + ground hub (its own service) |
| **Discover + consume** | Parse peers, feed `mod_sync` / `hub_ip_trust` | **ground hub only** (vehicle becomes passive) |

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
| **vehicle hub (air)** | *removed* | *removed* | **trust-on-subscribe** |
| **ground hub** | `_waybeam-hub._tcp` (ground↔Android) | venc beacon **+** hub beacons | keeps its discover/trust layer |
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
                 │  4. hub present? → talk to hub (/subscribe_video, sync)  │
                 │     hub absent?  → talk to venc directly (/api/v1, sidecar│
                 │  5. SUBSCRIBE = explicit intent → vehicle trusts caller  │
                 └─────────────────────────────────────────────────────────┘
```

Key invariant: **the vehicle never initiates discovery.** It answers mDNS
(venc), answers REST probes (hub, if up), and trusts whoever *explicitly
subscribes* (hub `/subscribe_video` or venc sidecar `MSG_SUBSCRIBE`).

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

## 5. Trust model migration

### 5.1 What goes away (vehicle)

- `mod_mdns` on the vehicle hub: announce **and** discover paths.
- The `hub_ip_trust_add_peer()` call that mod_mdns makes on peer discovery
  (`mod_mdns.c:1075`) — the ambient-trust seed.
- `mod_sync_notify_peer_discovered()` from mDNS on the vehicle
  (`mod_mdns.c:1081`).

### 5.2 What replaces it: trust-on-subscribe

The vehicle gains trust from **explicit inbound intent**, which it already
receives:

- Hub path: `POST /subscribe_video` (and `/subscribe` heartbeat) — requester IP
  becomes trusted for the duration of the subscription/lock.
- venc-direct path: sidecar `MSG_SUBSCRIBE` (UDP 5602) — probe IP becomes the
  receiver/trusted peer.

This dovetails with the existing **video subscription lock** (identity-based
ownership, 409 on conflict, `subscriber.timeout_s` expiry) — trust and lock now
share one lifecycle anchored on the subscribe action.

> **Decision to lock (D2):** confirm that trust-on-subscribe covers *all* paths
> that `hub_ip_trust` previously gated on the vehicle — sync hello acceptance,
> telemetry target, video. If the ground's unicast `mod_sync` hello is gated by
> `hub_ip_trust`, we need a seed *before* the first subscribe, or we relax sync
> to accept-then-trust-on-subscribe. **Must be resolved before Phase 4.**

### 5.3 What stays (ground)

The ground hub keeps its full `mod_mdns` (announce + discover + trust). It is
the active discoverer now: it consumes venc beacons **and** hub beacons and
keeps feeding `mod_sync`.

---

## 6. Capability probing contract (ground / Android)

After discovering a venc beacon (→ vehicle IP), the consumer determines what
else is on the box by **probing known ports**, not by reading mDNS TXT.

**Phase-1 mechanism (chosen): pure REST probe, zero coupling.**

1. Hit `http://<vehicle>:8060/status` and `/modules` (well-known hub port).
2. `200` → hub present; parse the real capability list from those endpoints
   (they already serve exactly this).
3. Refused / timeout (short, ~300 ms) → no hub; drive venc directly via
   `:80/api/v1/*` + sidecar `:5602`.

Rejected alternatives:

- **Static TXT hint (`hub_port=`):** still probes, still times out when absent —
  marginal gain. Skip unless the hub port ever becomes non-standard.
- **venc-hosted hub registry (`/api/v1/peers`):** the hub POSTs its presence to
  venc on startup; venc reflects it so the consumer gets one-stop discovery and
  no blind timeout. **Most literally "venc = layer of truth,"** but adds a venc
  endpoint + hub registration + stale-entry expiry. **Deferred to a later phase
  (D3)** — adopt only if probe timeout churn proves to matter.

> **Decision to lock (D3):** start pure-probe; graduate to the venc registry
> only on evidence. Documented so we don't oscillate.

---

## 7. Repo-by-repo change list

### 7.1 `waybeam_venc` (anchor)

- `vendor/mdns_wire/` — new vendored wire codec (extracted from hub mod_mdns).
- `src/mdns_beacon.c` + `include/mdns_beacon.h` — announce-only responder
  (`mdns_beacon_init/fd/poll/close`, `mdns_beacon_goodbye`). Reads `VencConfig`
  for TXT fields; getifaddrs for A records.
- `src/main.c` / backend runtime (`star6e_runtime.c`, `maruko_runtime.c`) —
  init beacon after backend up, poll its fd in the loop, goodbye on teardown.
  Must be pause/teardown-aware (mirror sidecar handling).
- `include/venc_config.h` + `src/venc_config.c` — new `discovery` config block
  (`enabled`, `service_type`, `name`/hostname override). Defaults make it on.
- `Makefile` — link `vendor/mdns_wire`, both backends, strict `-Werror`.
- `tests/` — `test_mdns_beacon.c` (TXT build, record encode, goodbye), wire
  codec round-trip tests. `documentation/HTTP_API_CONTRACT.md` — note the
  beacon + TXT schema (and that live state stays on `/api/v1/config`).

### 7.2 `waybeam-hub`

See `docs/discovery-trust-migration-plan.md` for detail. Summary:

- Ground: teach `mod_mdns` discover path to also match `_waybeam-venc._tcp`
  and map it to "vehicle discovered" → existing one-shot auto-subscribe.
  venc TXT vocabulary differs (no `has_subscribe_video` etc.), so
  `mod_sync_notify_peer_discovered()` needs a venc-shaped argument path.
- Ground: adopt `vendor/mdns_wire` (replace the in-module wire helpers).
- Vehicle: gate `mod_mdns` off (config `mdns.enabled=false` on vehicle config
  first; later compile-out of `HUB_MOD_MDNS` from the `vehicle` profile).
- Vehicle: implement **trust-on-subscribe** seed in the `/subscribe*` +
  sidecar paths to replace the removed `hub_ip_trust` mDNS seeding.
- Add capability-probe client used after a venc beacon is discovered.

### 7.3 `waybeam-android` (later)

- mDNS browse for `_waybeam-venc._tcp` (device list) instead of relying on hub.
- After discovery, REST-probe `:8060` for hub; fall back to venc `:80` direct.
- Subscribe via whichever path is present (hub `/subscribe_video` or venc
  sidecar/HTTP). No code in this repo; tracked here for protocol alignment.

---

## 8. Phased rollout (each phase independently shippable & revertible)

| Phase | Repo(s) | Change | Coexistence guarantee |
|---|---|---|---|
| **0** | all | This spec | — |
| **1** | venc | Add `_waybeam-venc._tcp` beacon (additive). Hub mDNS untouched. | Old discovery fully intact; new beacon is extra. |
| **1.5** ✅ | venc | Serial-suffix naming (`waybeam-<die-id>.local`), drop `sidecar_port` from TXT, expose `device.serial` via `GET /api/v1/config` (§4.1, §4.5, D6). **Done** — host tests pass and `make verify` cross-builds both backends (star6e/glibc + maruko/musl) clean. | Beacon stays additive; only the name/TXT shape changes before any consumer depends on it. |
| **1.6** ✅ | venc | Bare `waybeam.local` convenience alias with IP-tiebreak conflict resolution; `discovery.bareAlias` default true (§4.6, D7). **Done** — host-tested + `make verify` clean. | Alias is additive; the unique suffixed name always works regardless. |
| **2** | hub (ground) | Ground learns `_waybeam-venc._tcp`; adopt `vendor/mdns_wire`. | Ground still consumes `_waybeam-hub._tcp` too. |
| **3** | hub (vehicle) | Add trust-on-subscribe seed alongside existing mDNS trust. | Both trust paths active; no regression. |
| **4** | hub (vehicle) | Disable, then compile out vehicle `mod_mdns`. | Only after D2 confirmed; ground+venc cover discovery. |
| **5** | android | Browse venc beacon + probe hub. | Android keeps hub-mDNS fallback until parity proven. |

Phases 1–3 are **purely additive** (no behavior removed), so they can ship and
soak before the removal in Phase 4. Phase 4 is the only "burn the bridge" step
and is gated on D2.

---

## 9. Open decisions (must be locked before the phase that needs them)

- **D1 — TXT schema freeze (LOCKED).** Target key set: `proto`, `version`
  (§4.2). `sidecar_port` is being dropped in Phase 1.5 (D6); everything else is
  probe-discoverable / via `GET /api/v1/config`.
- **D2 — Trust-on-subscribe completeness** (before Phase 4). Prove it covers
  sync-hello acceptance, telemetry target, and video on the vehicle once mDNS
  trust seeding is gone. §5.2.
- **D3 — Probe vs venc-registry** (before Phase 5/optimization). Default
  pure-probe; venc `/api/v1/peers` registry only on evidence. §6.
- **D4 — `vendor/mdns_wire` ownership** (before Phase 2). Which repo is the
  source of truth for the vendored codec + the version bump/sync ritual.
- **D5 — Vehicle hub mDNS removal mechanism** (Phase 4). Config-gate first
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
- **Trust gap** if D2 is wrong → ground/Android silently can't reach the
  vehicle after Phase 4. Mitigated by additive Phases 1–3 soak and keeping a
  config rollback (`mdns.enabled=true`) until proven.
- **Probe timeout churn** on hub-less vehicles → bounded by short timeout;
  escalation path is D3.
- **Slower hub-down detection** (probe failure vs mDNS goodbye) → acceptable;
  the *device* (venc) still has a real goodbye.
- **Strict venc build** (`-Werror -Wextra`, dual backend) → wire codec must be
  warning-clean on both `star6e` and `maruko`.

---

## 11. Test plan (high level)

- venc: unit tests for TXT build + DNS record encode + goodbye; `make verify`
  (both backends); on-device `avahi-browse -r _waybeam-venc._tcp` smoke test.
- hub: extend `make test` (keep `HUB_MOD_MDNS`) with `_waybeam-venc._tcp`
  parse + venc-shaped `notify_peer_discovered`; trust-on-subscribe unit tests;
  capability-probe client tests (200 / refused / timeout).
- Cross: ground discovers venc beacon → auto-subscribe one-shot still fires
  exactly once; Android-direct (no hub) subscribe via venc sidecar works.

---

## 12. Out of scope (this spec)

- Any code. This is Phase 1 only.
- Android implementation (tracked, not built here).
- Changing the sidecar wire format or the video-lock semantics (reused as-is).
```
