# Fable 5 masterprompt — Maruko inject-mode AE

Paste the block below to Fable 5 to continue this work.

---

You are continuing an in-flight investigation on the Waybeam SigmaStar video
encoder. Work in `/home/snokvist/dev/waybeam-coordination/waybeam_venc` on branch
`feature/maruko-cus3a-apply-cost`. **First, read
`documentation/MARUKO_CUS3A_INJECT_HANDOFF.md` in full** — it is the source of
truth for state, measured numbers, code map, SDK references, and bench rules. Do
not re-derive what it already establishes.

## Mission
Make the Maruko (I6C, SSC378QE) camera run 1080p100 with a **good image**
(correct exposure, correct white balance, low noise) at **~43% CPU or less** —
i.e. match majestic. The CPU mechanism is already solved (inject-mode removes the
`RegisterIspApiAgent` cost → 34–39%, proven). The open problem is getting the
**AE+AWB to actually converge** in inject-mode without the agent.

## Do this, in order
1. **Path A first (cheap, decisive).** In `maruko_pipeline.c` inject branch,
   change `CUS3A_SetRunMode(0,0, 2 /*INJECT*/)` to `0 /*NORMAL*/`, keep the
   `maruko_inject_ae_driver` RunOnce thread. Rebuild, deploy to `/tmp`, and do
   **one** cold-start on the device. Check three things: (a) does the AE reach
   NORMAL and exposure move off `300us/1x`? (b) is the image well-exposed + white
   balanced (pull `/api/v1/snapshot.jpg` and look)? (c) is `IspMidThreadWq` still
   ~0 and total ~40% (`/tmp/samp.sh 8`)? This one test resolves whether the
   native algo can run cheaply. Report the outcome before proceeding.
2. **If Path A fails**, pursue Path B: register a *custom* AE+AWB algo via
   `CUS3A_RegInterfaceEX(E_ALGO_ADAPTOR_1, …)` in inject-mode (majestic's likely
   design). This requires first **fixing the `uAvgY≈4` metering sub-bug** (see
   handoff — the stat scale/source is wrong, not the divisor) and **adding an AWB
   control law** (the yellow/green cast is because our custom path has no AWB).
   Before building, consider disassembling majestic's `Cus3A_ProcAE` to see which
   interface it registers (native `Sigma3AGetAeInterface` vs its own).

## Non-negotiable bench rules (device is fragile — violating these wastes an hour)
- Device is **192.168.2.12** (Maruko, tmpfs). `ssh -o ConnectTimeout=12`.
- `aeEngine`/enable-path/`noiseLevel` changes force a respawn that **wedges the
  SCL fence into D-state** after 2–3 restart cycles. **Measure each variant via
  `reboot -f` + a single cold-start.** Never live-`/api/v1/set` these, never
  rapid restart-cycle. `reboot -f` recovers (ssh hangs → background it + poll
  reachability).
- Deploy order: **teardown (SIGTERM, poll `pidof` empty) → `cat > /tmp/waybeam_inject`
  → start**. Writing a running binary = "Text file busy". Never SIGKILL.
- Flash `/usr/bin/waybeam` is the shipped good image — leave it; test from
  `/tmp/waybeam_inject`. Restore the user's stream with a flash-native cold-start
  when you pause.
- Env gate: `MARUKO_AE_INJECT=1`. Config via `json_cli -i /etc/waybeam.json -s`.
- Build: `make build SOC_BUILD=maruko` (+ `star6e`). clang LSP errors are false
  positives — trust the gcc build. Keep default behavior unchanged (gate off).

## Working style
Verify every claim on the device with **both** an image (`/api/v1/snapshot.jpg`,
view it) **and** a CPU sample — this problem has fooled confident hypotheses
twice. Commit incrementally with honest messages. Update the handoff doc and the
memory note `venc_maruko_cpu_profile_highfps` as you learn. If you hit a fork you
can't resolve from the SDK, disassemble the relevant `libcus3a.so` /
`libmi_isp.so` path rather than guessing. Ask the user to eyeball the live RTP
stream for final AE/AWB quality — the snapshot is a proxy.

## Definition of done
`aeEngine=inject` (or the chosen selector) gives, on a single cold-start at
1080p100: exposure that converges to a well-lit image, neutral white balance
(no green/yellow cast beyond the bench's warm lighting), low noise, solid 100fps,
`IspMidThreadWq`≈0, total ≤~43%. Then wire it as a real config field (mind the
venc 7-touch schema checklist) and propose it as the high-fps default.
