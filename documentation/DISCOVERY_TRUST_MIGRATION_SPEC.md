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

### 4.1 Service type

New, distinct from the hub's: **`_waybeam-venc._tcp.local`**. Distinct type
avoids record collisions when both venc and a hub run on the same host, and
lets consumers query specifically for "devices" vs "hubs."

### 4.2 TXT schema — stable identity/endpoints ONLY

venc's headline fields (bitrate, fps, mode) are **live-mutable**. Putting them
in TXT means re-announcing on every `/api/v1/set` — noisy and racy against the
encode loop. **Rule: TXT carries only stable identity + endpoints. Live state
is pulled via `GET /api/v1/config`.**

| TXT key | Source | Example | Mutable at runtime? |
|---|---|---|---|
| `proto` | constant | `1` | no (schema version) |
| `name` | config / hostname | `waybeam-0` | no |
| `backend` | build | `star6e` \| `maruko` | no |
| `model` | sensor/SoC | `ssc338q` | no |
| `sensor` | config | `imx415` | rarely (restart) |
| `codec` | constant | `h265` | no |
| `web_port` | `system.web_port` | `80` | no |
| `sidecar_port` | `outgoing.sidecar_port` | `5602` | no (restart) |
| `version` | `VENC_VERSION` | `0.17.1` | no |
| `api_contract` | build | `0.2.0` | no |

> **Phase 1 status (implemented):** the beacon emits `proto`, `name`,
> `backend`, `model`, `codec`, `version`, `web_port`, and `sidecar_port`
> (optional keys `model`/`sidecar_port` omitted when empty/zero). `sensor`
> and `api_contract` are reserved for a later phase (live sensor name needs
> backend pipeline state; the HTTP contract version constant isn't wired
> yet). See `src/mdns_wire.c` + `src/mdns_beacon.c`.

> Fields that *can* change but only across a pipeline restart (sensor,
> sidecar_port) are acceptable because a restart re-announces anyway. Anything
> that changes live (bitrate/fps/mode/stream enable) is **forbidden** in TXT.

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
- Integrated into the backend runtime loop (`star6e_runtime.c` /
  `maruko_runtime.c`) via an fd + poll callback, mirroring `rtp_sidecar`.

### 4.4 Shared wire codec (avoid a two-repo fork)

The DNS encode/parse core would otherwise exist twice (venc + ground hub).
**Extract it into a vendored, version-stamped source** shared by both repos —
same discipline as `vendor/venc_ring/` (`VENC_RING_VERSION`, "keep in sync").

- Proposed: `vendor/mdns_wire/{mdns_wire.c,mdns_wire.h}` with
  `MDNS_WIRE_VERSION`, pure functions (build PTR/SRV/TXT/A, parse records,
  TXT get/put). No socket or platform code — caller owns the socket.
- venc links the encode path; ground hub links encode+parse. Hub keeps its
  socket/lifecycle/`mod_sync` glue on top.

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
| **2** | hub (ground) | Ground learns `_waybeam-venc._tcp`; adopt `vendor/mdns_wire`. | Ground still consumes `_waybeam-hub._tcp` too. |
| **3** | hub (vehicle) | Add trust-on-subscribe seed alongside existing mDNS trust. | Both trust paths active; no regression. |
| **4** | hub (vehicle) | Disable, then compile out vehicle `mod_mdns`. | Only after D2 confirmed; ground+venc cover discovery. |
| **5** | android | Browse venc beacon + probe hub. | Android keeps hub-mDNS fallback until parity proven. |

Phases 1–3 are **purely additive** (no behavior removed), so they can ship and
soak before the removal in Phase 4. Phase 4 is the only "burn the bridge" step
and is gated on D2.

---

## 9. Open decisions (must be locked before the phase that needs them)

- **D1 — TXT schema freeze** (before Phase 1). Exact key set in §4.2. Stable
  fields only; `proto=1`.
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
