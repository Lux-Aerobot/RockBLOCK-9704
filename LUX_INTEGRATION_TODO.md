# Lux integration TODOs

Working notes for integrating the RockBLOCK-9704 library into the Lux node
firmware. Lives next to the upstream README so it travels with the submodule;
keep it Lux-specific so we can rebase the submodule cleanly.

**Architecture (locked 2026-05-27; manager MCU = L4, decided 2026-06-23):**
two-MCU split. A dedicated **second L4** (the modem manager) owns the 9704 modem —
**not an L0**. The manager will be another L4-class part (L452 baseline, matching the
Core), which comfortably absorbs the library's ~18 KB RAM floor and removes the L0
SRAM-fit constraint entirely (see "Manager MCU part — RESOLVED" below). The main
avionics L4 (STM32L452) "Node Core" talks to the manager over a UART link as a client
of a manager-side message API modelled on the ACTU SS protocol. EO companion deferred
to a future HW revision; re-enters as a second client of the same Core-side API.

**Refinements (JC sync, 2026-05-29):** the manager goes on **its own board**
this run — two boards (manager + core). Consolidating modem ownership onto
the Core later is a **future hardware revision, not a firmware toggle**; the
option is preserved (affordable/viable), decided later on power/isolation
-vs-BOM. **Power:** one 5 V bus brings up manager + modem rail together; the
**manager** gates the modem's power (load-switch IC) and owns all enable
pins + the link ("no modem without the manager"). See LUX_DEVLOG 2026-05-29.

See `LUX_DEVLOG.md` 2026-05-27 entry for full decision context and the
protocol design sketch.

Bring-up dev hardware: Nucleo-L452RE. Production split: manager L4 (part TBD,
L452-class) + STM32L452VCT6 Core. Bare-metal, HAL drivers, super-loop.

---

## Bring-up — done

- [x] STM32 HAL serial preset (`src/serial_presets/serial_stm32/`)
- [x] `crossplatform.{h,c}` STM32 branch (`HAL_GetTick`/`HAL_Delay`)
- [x] `rbBegin(UART_HandleTypeDef *)` overload
- [x] Nucleo-L452RE bring-up example
      (`examples/nucleo_l452re_async_send_receive.c`)
- [x] MT print loop: single-transmit + in-place sanitize (replaces
      N-transmit-per-byte loop)
- [x] `RB9704_TX_TIMEOUT_MS` 1000 → 200 ms

## Modem-manager firmware POC (5-step plan, started 2026-05-28)

Phased build of the dedicated modem-MCU firmware. Each step has a clean
validation gate. Laptop via ST-LINK VCP stands in for the Core/L4 in
steps 2+. See `LUX_DEVLOG.md` 2026-05-28 for the plan and rationale.

- [x] **Step 1 — button-triggered GPIO startup/shutdown + interlock.**
      `examples/nucleo_l452re_modem_manager.c`. State machine + damage
      interlock + `SIMULATE_IBTD` (jumper PC3→PC2) + TX-force-low helpers.
      Hardware-validated on Nucleo-L452RE: normal cycle, interlock holds
      mid-sequence, FAULT path + recover-on-ack, TX-low timing (see
      devlog 2026-05-28).
- [~] **Step 2** — 9704 comms + inter-MCU UART link + signal-event
      messaging (outgoing only).
      - [x] 2.1 — deferred USART1 up/down wired into the sequence +
            symmetric settle margins. Hardware-validated.
      - [x] 2.2 — first bytes out USART1 in RUNNING (`Hello Lux` every
            2 s). Clean 230400 read confirmed on a USB-UART adapter.
      - [ ] 2.3 — signal-change event as the first *shaped* outbound
            message + inter-MCU (ACTU-style) framing.
      - [x] **`__io_putchar` retarget — DONE (manager `main.c`, uncommitted).**
            Root cause of the "watchdog reset" found and fixed 2026-05-30
            (S2-part-6, devlog): the Debug build defines `DEBUG`, so jspr.c's
            `printf("SENT:/RECEIVED:")` are live; `__io_putchar` had no
            definition (weak `extern`→`0x0`), so the first library `printf` in
            `rbBegin` HardFaulted → spun → IWDG reset ~8 s later (looked exactly
            like an rbBegin hang). Verified at link level (`nm`/`objdump`:
            `_write`→`__io_putchar`, pre-fix target `0x0`). Fix:
            `int __io_putchar(int ch){ uint8_t c=(uint8_t)ch; HAL_UART_Transmit(
            &huart2,&c,1U,10U); return ch; }` in USER CODE 4. Builds clean.
            **Bonus:** this *also* turns on the JSPR `SENT:`/`RECEIVED:` trace
            (visibility) — one edit, both wins. **Next: flash + run → expect
            `rbBegin OK` + full trace = first contact end-to-end.** (Turn
            `DEBUG` off before MO segment-timing tests; the trace is per-frame.)
- [~] **Steps 3+4** — transparent Core<->modem passthrough (raw, no translation).
      Code-complete in the manager `main.c` (uncommitted; pending the CubeMX
      USART3 add + hardware validation). Dedicated Core link = **USART3**
      (separate from the USART2 debug console), single-byte IT RX into a ring.
      - MO (step 3): Core-link RX accumulated until `\n`; trailing CR/LF
        stripped (kept off-air — Iridium is billed per byte); payload handed to
        `rbSendMessageAsync(RAW_TOPIC, ...)`, drained by `rbPoll`.
      - MT (step 4): `mtMessageComplete` -> `rbReceiveMessageAsync` -> payload
        written verbatim out USART3 + `\r\n`, then `rbAcknowledgeReceiveHeadAsync`.
      - Core link is independent of the modem interlock (always up); lines that
        arrive outside RUNNING are discarded (no modem to send to).
      - **CubeMX step required before build:** add USART3 async 115200 8N1
        (e.g. PC10 TX / PC11 RX), auto-init (NOT deferred like USART1), enable
        the USART3 global interrupt. See `main.c` header comment.
      - Known bound: MT relay is a blocking `HAL_UART_Transmit` inside the
        `rbPoll` callback (fine for the tiny <50 B command/RSP payloads; revisit
        if large MTs ever interleave with an in-flight MO segment exchange).
      - See `LUX_DEVLOG.md` 2026-06-16.
- [ ] **Step 5** — wrap in ACTU-style text protocol. = POC complete.

## Bring-up — outstanding

- [ ] **GPIO startup/shutdown sequence for STM32 (16-pin connector).**
      First cut implemented in the step-1 POC file above (GPIO-interlock
      path). **Locked as the production path (2026-05-29):** 16-pin
      connector + GPIO interlock, USB-host-bridge alternative rejected.
      No longer conditional.
      See https://docs.groundcontrol.com/iot/rockblock-9704/hardware#1-using-16-pin-connector

      The Nucleo dev-kit's USB-C path handles sequencing in hardware,
      which is why our current example "works" while plugged into a
      laptop. **The 16-pin production wiring does not — the host MCU
      must do it manually, and getting it wrong can damage the
      module.**

      **Startup sequence (must be in this order):**
      1. All MCU pins connected to 9704 *inputs* — including our UART
         TX (modem's RX) and `I_EN` — must be tristate (high-Z) or
         logic low.
      2. Drive `P_EN` to apply power (active-low per upstream README
         Pi-hat config — confirm with our schematic).
      3. Drive `I_EN` high.
      4. Poll `I_BTD` until high (with timeout).
      5. *Now* initialize USART1 pins to AF and call `rbBegin`.

      **Shutdown sequence:**
      1. Cease JSPR comms (`rbEnd` to close serial cleanly).
      2. Drive `I_EN` low.
      3. Poll `I_BTD` until low.
      4. Tristate / logic-low all 9704 input pins.
      5. Remove power.

      **🚨 Damage interlock:** Once `I_EN` is driven high, the host
      must wait for `I_BTD` to transition high before driving `I_EN`
      low again. Once driven low, must wait for `I_BTD` low before
      driving high again. *Failure may damage the module* (per the
      GC docs warning). The GPIO driver must enforce this — track
      last commanded `I_EN`, refuse early state changes.

      **Implementation tasks:**
      - [ ] **Production power switch = high-side load-switch IC**
            (TPS22918 / AP22xxx class) on V_IN+, enabled from PWR_EN.
            Decided 2026-05-29 after working through the topology: the
            9704's GND/V_IN- pins (1/4/10/16) are *combined signal-ground
            + power-return*, so the switch MUST be high-side (low-side
            would switch the shared signal-ground reference → back-power).
            A high-side N-FET (e.g. IRLZ44N) needs gate-above-rail drive,
            so it's the wrong device; load-switch IC gives high-side gate
            drive + controlled inrush slew + (often) a fault flag from a
            single 3.3 V active-high enable (`PWR_GATE_ACTIVE_HIGH=1`
            still fits). Discrete P-FET + NPN level-shift is the fallback.
            See LUX_DEVLOG 2026-05-29. **Bench bring-up uses manual
            bench-supply V_IN+ control** until the IC is sourced — the
            firmware sequencing is power-source-agnostic.
      - [ ] **Power-enable lives on the MANAGER** (decided 2026-05-29):
            the load-switch IC gating the modem's V_IN+ is driven by the
            manager, off the always-on 5 V bus. Manager owns all of:
            power-enable, I_EN, I_BTD, and the JSPR link.
      - [ ] **Hardware interlock (EE/JC).** A buffer / bus-switch on the
            lines we drive into the modem, enabled by the modem's ready
            signal, so a firmware bug can't drive the modem pre-boot
            (mirrors GC's USB reference design). Defense-in-depth on top of
            the software interlock. **Design notes:** I_BTD is the natural
            enable for the **UART data lines** (matches "UART only after
            I_BTD high"); **I_EN cannot be gated by I_BTD** (I_EN precedes
            boot → circular). **Confirm:** distinct power-good output on the
            16-pin connector vs using I_BTD — JC to check the schematic pin
            naming before wiring the buffer enable.
      - [ ] Pick four L452 GPIOs: `P_EN`, `I_EN`, `I_BTD`, plus
            optionally a "modem ready" status LED. TBD on production
            board layout; on Nucleo, anything spare on the Arduino
            header.
      - [ ] CubeMX: configure USART1 *but do not let MX_USART1_UART_Init
            run from `main()` upfront* — defer to after I_BTD high.
            Either remove the auto-call or use a custom init function.
            Same for the four GPIOs (CubeMX should set them with
            startup-safe initial states: `P_EN` inactive, `I_EN` low,
            `I_BTD` input).
      - [ ] Add `rbBeginGpioStm32(huart, gpio_table, timeout)` in the
            library — mirrors the Linux `rbBeginGpio` at
            [rockblock_9704.c:62](src/rockblock_9704.c:62) but uses
            `HAL_GPIO_WritePin` / `HAL_GPIO_ReadPin` and a polling
            loop on `I_BTD` with `HAL_GetTick` for timeout.
      - [ ] Add `rbEndGpioStm32` for clean shutdown including the
            `I_BTD`-goes-low wait.
      - [ ] Add the damage-prevention interlock as static state in
            the GPIO module.
      - [ ] Update `nucleo_l452re_async_send_receive.c` to use the
            new begin/end pair and the deferred UART init pattern.

      Until this lands, the bring-up example assumes the modem is
      already powered and booted by something external (USB-C
      dev-kit, bench supply with manual jumpering, etc.).
      Fine for prototyping, NOT safe for production.

      **When not booted**, the modem's output states other than
      `I_BTD` are *undefined* — so don't trust anything on the
      modem→MCU UART line until `I_BTD` is confirmed high.

---

## Application-layer reliability

The library drops MOs on `messageOriginateStatus != mo_ack_received` and
fires `moMessageComplete(id, FAIL)`. Our policy: telemetry can always be
re-captured via ground command, so single-message loss is degraded mode,
not failure mode. But a few things to wire up to keep that promise honest.

- [ ] **Application-side MO retry wrapper** for high-value messages.
      Catch `moMessageComplete(_, FAIL)` and decide per-class whether to
      re-enqueue. Library auto-drops the slot at
      [rockblock_9704.c:766](src/rockblock_9704.c:766).
- [ ] **Use async API only — never the sync `rbSendMessage`.**
      The sync API's `timeout` parameter is a *waiting timeout*, not a
      *delivery timeout*: when it fires, the message is **not** cancelled,
      it just stops being watched. Modem keeps trying in the background;
      `moMessageComplete` may fire long after `rbSendMessage` returned
      `false`. Confirmed by observation 27 May 2026: `send_message.py`
      reported "Sending failed" on 60 s timeout, MOs actually delivered
      ~30 min later when a reception window opened. The async API plus
      `moMessageComplete` callback is the only way to track real
      delivery state without this footgun.
- [ ] **Sequence numbers on outbound telemetry.** Monotonic uint32 per
      message class. Ground side spots gaps, requests re-send by seq.
- [ ] **Status piggy-back tail.** Every MO carries last ~8 MO seqs so
      ground knows what we *think* we sent. Cheaper than per-message acks.
- [ ] **MO logging to SD before send.** Sequence:
      1. App generates seq.
      2. App writes `(seq, payload, queued_at)` to SD.
      3. App calls `rbSendMessageAsync`.
      4. On `moMessageComplete(modem_id, OK)`, app appends
         `(seq, modem_id, ack_time)` to SD log.
      Result: power-cycle mid-send leaves an unambiguous record.
- [ ] **Re-request ring on SD.** Keep last N telemetry records (start
      with 256, tune later) addressable by seq. Reject ground re-requests
      outside the window — bounds our retransmit storage commitment.
- [ ] **Idempotent commands only.** "Get current value of X", not
      "increment X". Receiver dedup by command seq, but command logic
      should tolerate replay regardless.
- [ ] **Surface send rejections** in console output. Currently
      `rbSendMessageAsync` returning `false` is silent. Add reason
      detection (provisioning? queue full? topic invalid?) and log it.

---

## Open design questions

Things we've identified but haven't decided on. Each needs a
conversation before it becomes a concrete TODO. Don't implement
without discussion.

### Near-term feature sequence (integration frontier, set 2026-06-23)

The order we're driving the Core↔manager integration, post restart-jam-fix. Each
expands in a section below; this is just the priority spine:

1. **RES/TEL queue policy + backpressure** — RES preempts TEL; TEL rolling/droppable.
   Validate MT→CMD→MO→RES under clear sky. (→ *RES vs TEL priority*.)
2. **Periodic manager→Core status feed** — manager pushes signal/modem-state; Core
   folds it into telemetry + logs it. (→ *Manager→Core status feed*.)
3. **Core→manager command surface** — power on/off/cycle modem, get-status,
   queue-and-prioritize-this-message. (→ *L0 ↔ L4 protocol design*: power-control
   commands + v0.1 set + L4-side outbox.)
4. **Per-channel telemetry config** — channel-arg SET_INTERVAL onto per-channel
   structs (LoRa-ready). (→ *Per-channel telemetry intervals*.)

Below is the fuller backlog these draw from.

### MO queueing policy: what happens when new data arrives mid-flight?

When `rbSendMessageAsync` is called while a previous MO is still in
flight (modem mid-segment-exchange, or queued behind another), the
library's current behaviour is:

- New message appended at tail of MO queue (FIFO).
- If queue is full and locked (default): new message rejected,
  `rbSendMessageAsync` returns `false`. Caller must retry later.
- If queue is full and unlocked (`rbSendUnlockAsync()` called): the
  **head** of the queue is dropped — which may be a message that's
  already partway through transmission to the modem. The modem then
  times out the orphaned segment exchange and returns
  `SEGMENT_NOT_SUPPLIED_MOS`, but our callback never fires (the queue
  entry that owned the ID is already gone). Lossy, no notification.

There is no public cancel-in-flight API, despite the modem reporting
`MESSAGE_CANCELLED_*` final statuses ([jspr.h:216](src/jspr.h:216))
that imply it supports being told to abort.

**UPDATE 2026-06-23 — upstream #68 merged into the fork (`a1f4fb9`, merge `9753c27`):**
the cancel API now exists and is **async-validated**. `sendMoFromQueueAsync` fires a new
`moMessageStarted(id)` callback once the modem accepts the MO (**async path only** — the
sync sender does not get it), and `rbCancelMessage(topic, id)` emits the
`messageOriginateStatus` "cancel" command for that id. Clean supersede is now a library
primitive: async-send TEL → stash `(topic, id)` from `moMessageStarted` →
`rbCancelMessage` when a RES must preempt → the modem's `MESSAGE_CANCELLED_*` status
returns via `rbPoll` → `moMessageComplete` frees the slot (and decrements
`moQueuedMessages`, composing with our reset fix). **This retires the
"add `rbCancelMessageAsync`?" sub-question below** — it's the intended RES/TEL supersede
mechanism. Timing caveat: cancel completes *asynchronously* — the slot frees when the
cancelled-status returns, not at the `rbCancelMessage` call; factor that into the
priority-queue design.

**The question:** for our use cases, what policy do we want per
message class?

- **Telemetry** (frequent sensor data, latest is what matters):
  - Option A: rolling supersede — drop oldest unsent, keep latest.
    Cleanest if we want freshness over completeness.
  - Option B: FIFO with reject — fixed queue depth, lose nothing in
    flight, but app sees `false` returns and has to decide what to
    do.
  - Option C: keep latest at app level, only enqueue to library when
    library queue is empty. Avoids the library's lossy unlocked-drop
    path. Pseudo-sketch in the chat history from 27 May session.

- **Critical events** (alerts, state changes, things we can't lose):
  - Almost certainly FIFO + retry on FAIL. Never supersede.

- **Companion-sourced thumbnails**:
  - Probably FIFO. Companion shouldn't be able to starve telemetry,
    which argues for a separate priority lane at the application
    arbiter level (already a TODO).

Sub-questions to resolve together:

- Do we want one unified policy or per-class policy?
- If per-class: where does the policy live — application arbiter,
  library wrapper, or message-tagged metadata?
- Does it make sense to add a `rbCancelMessageAsync(id)` to the
  library so supersede can be clean (cancel old → enqueue new with
  a proper FAIL callback for the cancelled one)?
- How does this interact with the SD-logged "what was in flight at
  power-loss" record (separate TODO under
  Application-layer reliability)?
- Implication for `IMT_QUEUE_SIZE`: rolling supersede at app level
  works with size 1 (smaller RAM); FIFO needs ≥2.

Surface this in the integration design session before writing
arbitration code — the answer shapes both the library wrapper and
the companion protocol.

### Telemetry sequence numbering: mirror vs increment on the Iridium link — open (2026-06-23)

Decided (Nick/Liam, weeks ago): telemetry uses a **single global sequence
number**, not per-link counters. Surfaced bringing the Iridium telem online —
since the companion link is always faster than Iridium, should the Iridium send
**mirror** the latest global seq (re-send the newest number, no increment) or
**increment** the shared counter on every Iridium send too?

- *Mirror* (implemented now): tags with `last_telemetry_seq`, doesn't advance →
  a satellite copy correlates to a specific companion line, no gaps in the
  companion stream. Simpler.
- *Increment*: the satellite stream becomes independently gap-detectable (ground
  can tell it missed sat-telem N without the companion stream as reference), at
  the cost of interleaving the counter across links.

Irrelevant to current bring-up (the mirror is in place and non-interfering).
Decide alongside the priority question below.

### RES vs TEL priority, and Core→manager backpressure — open (2026-06-23)

Two coupled decisions surfaced wiring the Core→manager telemetry path:

1. **RES should preempt TEL.** A command response means the ground is waiting;
   telemetry is periodic and supersedable. Likely lives in the **manager** (it
   owns the MO queue and sees the modem's real capacity — the Core shouldn't have
   to). Shape: a small priority queue, RES ahead of TEL, TEL the rolling/droppable
   class. Direct extension of the "MO queueing policy" question above (per-class:
   RES = never-drop, TEL = rolling-supersede).
2. **Backpressure.** The Core currently fires telem *blind* — every
   `iridium_telemetry_interval_s`, regardless of the manager's queue state; a full
   queue drops it (the observed `MO REJECTED`) and the Core never learns. Fine at
   the 300 s test cadence. Real fix: MO-outbox-on-Core (Core holds the backlog,
   feeds the manager only when it has room) and/or a manager→Core accepted/full
   signal — part of the Step-5 ACTU ACK/ERR framing.

**OBSERVED 2026-06-23 (no longer hypothetical):** ran the MT→CMD→MO→RES loop
(`CMD;0;1;0;2` STATUS_GET as an MT). The MT delivered and the Core generated a valid
RES (`RSP;…;2;0;0;uptime_s;9244;gnss_valid;0;telemetry_interval_s;5`), but the manager
**rejected the 71 B RES (`MO REJECTED … queue full`)** because the depth-1 slot was
occupied by a queued **TEL**, which then shipped (`MO complete id=2`). The RES — the
thing the ground was waiting on — was dropped in favour of a periodic telemetry frame.
**Structural, not a race:** with `IMT_QUEUE_SIZE = 1` + steady Iridium TEL, the slot is
almost always TEL-occupied when a RES arrives, so interactive responses are
systematically starved while telemetry flows. Confirms RES-preempts-TEL is required,
not optional.

For the Nick/Liam design session.

### RES outbox — review follow-ons (2026-06-24, outbox implemented + reviewed)

The manager-side RES-priority outbox is implemented (RES FIFO depth 4 / TEL
latest-wins / cancel-preempt via #68 `rbCancelMessage`). An adversarial review
confirmed the preemption, FIFO, and re-entrancy logic is sound, and surfaced these
follow-ons. **None block the open-sky preempt test** — loud logs were added so each is
observable on the bench rather than silent:

- **RES durability (retry on FAIL).** A RES that *starts* then FAILs on the network is
  currently **dropped** — the outbox pops it at send time, not delivery. We now log
  `** RES LOST **`, but the real fix is the **MO-retry-wrapper** above: pop a RES only
  on `moMessageComplete(OK)`, re-arm on FAIL with a bounded retry count (leave the
  cancelled-TEL FAIL path alone). Honors the "RES never lost" goal.
- **Stale-cancel + 8-bit id reuse (verify on HW first).** Modem message ids are
  `uint8_t`, modem-assigned. If a TEL's natural ACK crosses our cancel and the modem
  then reuses that id for the next RES, a stale cancel could abort the RES. Low
  probability; depends on **unverified** modem id-reuse / cancel-application timing.
  **Action: capture a real cancel trace** before engineering a guard — (a) does a
  pre-transit cancel resolve locally with no pass, or wait for one? (b) does the modem
  reuse a just-freed id immediately? The `** RES LOST **` log is the tripwire meanwhile.
- **True final status not visible.** The library collapses every non-ack final
  (`network_error` / `expired` / `cancelled_pre`/`in_transit` / CRC) to `FAIL` before
  `moMessageComplete`. To log *why* an MO failed — and the pre- vs in-transit cancel
  that drives billing — pass `jsprFinalMoStatus_t` through the callback (upstream lib
  change; candidate for the rock7 PR set).
- **Cost of cancel-in-transit.** A TEL already mid-segment when preempted still bills
  airtime, and the RES re-sends. Acceptable for rare RES; surface in/pre-transit once
  the true final status is exposed (above).
- **No-pass liveness.** If the modem holds an in-flight MO with no pass, the single slot
  stays busy until a pass or an operator modem restart — the library slot **cannot** be
  force-freed cleanly from the manager (force-clearing only our flag desyncs us and
  fails the next send). The RES is **not lost** — it waits in the FIFO. A one-shot
  `MO in-flight Ns … awaiting modem` log makes the wait visible vs a hang.

Most fold into the MO-retry-wrapper + the Step-5 ACTU ACK/ERR framing. For the
Nick/Liam session.

### Per-channel telemetry intervals: channel-id arg on SET_INTERVAL + per-channel structs — open (2026-06-23)

`CMD_TELEMETRY_SET_INTERVAL` (Core `main.c` ~l.1023) hardcodes the **companion**
scalar `telemetry_interval_s`; the command's CH_ID field is used *only* to route the
RES back, **not** to pick which stream to retune. So `iridium_telemetry_interval_s`
(and any future LoRa interval) can't be set over-air, and the two cadences live as
ad-hoc sibling globals. Surfaced while structuring the MT→CMD→MO→RES loop test.

**Proposed shape (Liam):** fold the per-link telemetry config into **per-channel
structs** — each channel owns its UART handle + interval (and a natural home for its
seq policy and priority class) — and have SET_INTERVAL carry a **channel id** that
maps the value directly onto the addressed channel's struct. Turns two special-cased
scalars into one indexable table; every link's cadence becomes independently tunable
from the ground.

Sub-decision: the command already has a CH_ID (reply-routing). Decide whether the
*target* stream reuses that same CH_ID (you retune the link you reply on) or takes a
**separate** arg (decouple "which stream to tune" from "where to send the RES").

Dovetails with the two questions above — the per-channel struct is the obvious home
for the per-link seq policy (mirror-vs-increment) and the per-class priority
(RES-never-drop / TEL-rolling). Defer; do alongside Step 5 / the Nick session.

### Manager→Core status feed: periodic state push for telemetry + logging — open (2026-06-23)

The manager owns the live modem/link state (signal bars + dBm, constellation visible,
registered/provisioned, operational state, modem up/fault, board temp), but the Core
has none of it — its TEL frames carry zeros for those fields. Feature: the manager
**periodically pushes its status to the Core** (and on significant change); the Core
folds it into its telemetry struct so those fields are both **logged** and **carried
in TEL** (companion + Iridium).

- Overlaps the v0.1 `STAT,…,STATE` (periodic) and `EVT,…,SIG` (signal-change) message
  types below — this is the concrete consumer that gives them a purpose.
- Cadence: slow periodic STAT + event-driven SIG on bar/visibility change (mirrors the
  unsolicited-driven signal logging already proven on the manager).
- Decide which fields the Core mirrors-into-TEL vs logs-only, and whether stale manager
  status (link down) blanks the fields or holds last-known + an age flag.
- Pairs with the per-channel structs — manager-sourced fields are just more
  telemetry-struct slices, like GNSS / sensor / actuator.

Feature #2 of the near-term sequence. Part of Step-5 ACTU framing / the Nick session.

### MT acknowledgement timing: when is an inbound command "done"?

`rbAcknowledgeReceiveHeadAsync()` removes the head of the MT queue,
freeing the slot for incoming messages. The library has no opinion on
*when* the app should ack — that's our policy.

The naive pattern (used in the bring-up example) is "ack immediately
on receive." That works for fire-and-forget commands but breaks down
when commands take real time to execute:

- "Capture a new thumbnail and send" — minutes of work, may itself
  fail.
- "Reboot the companion computer" — execution outlives the receive
  window; app may be down when ack is normally issued.
- "Apply config change" — needs verification before acceptance.

If we ack-on-completion instead of ack-on-receipt, the MT queue
absorbs slow command execution. If we ack-on-receipt and execution
fails, the MT itself is gone — modem doesn't replay (gateway
considers it delivered), our only recovery is "ground asks again."

Sub-questions:

- What does "command complete" mean per command class? Some have a
  natural completion event (thumbnail sent → MO complete), others
  don't (config change → ???).
- If we ack-on-completion, how do we bound queue depth? Slow
  commands could block fresh ones from arriving.
- Should we durably log received MTs to SD *before* acking, so a
  crash mid-execution leaves a recoverable record?
- Do we want a separate app-level "command journal" that's
  independent of the library's MT queue lifecycle?
- What's the right behaviour on duplicate commands (same seq from
  ground after a perceived loss)? Ties into idempotency item under
  Application-layer reliability.

### Provisioning resync: who/what triggers `rbResyncServiceConfig()`?

Resync is needed any time the Cloudloop plan changes (topics added,
removed, reconfigured). Modem can't detect this on its own — the
cached provisioning in modem flash is stale until something forces a
refresh. Resync requires a power-cycle to take effect (per upstream
README).

Possible trigger sources:

- **Ground command.** A specific MT command that, on receipt, runs
  `rbResyncServiceConfig()` and reboots the modem. Authoritative —
  the ground operator knows when they changed the plan. Adds one
  required command to the protocol; needs idempotency since the
  resync MT itself might arrive after the plan-change MT it relates
  to.
- **Periodic.** Every N hours/days, resync regardless. Catches silent
  changes; wastes power and registration time on every cycle. Bad
  fit if N is short, harmless if N is long (weekly?).
- **On 418 (not provisioned).** Trigger resync when an MO fails with
  `JSPR_RC_NOT_PROVISIONED`. Reactive — only resyncs when we
  actually have evidence we need to. Risk: cascading reboots if
  resync itself doesn't restore the topic (plan really is
  misconfigured).
- **Operator button / field service.** Pull-the-plug or a hardware
  reset triggers fresh provisioning. Brittle for unattended
  deployments.

Sub-questions:

- Which trigger(s) do we want? Probably ground command + on-418
  fallback, but worth thinking through.
- What's the right backoff on 418 to avoid reboot loops if the
  ground side is wrong?
- How do we make resync observable from ground? A "I just resynced,
  here's my current topic list" MO seems necessary.
- Does the resync need to happen *only* on a deliberate request, or
  is it safe to invoke speculatively? (Cost is one modem reboot
  cycle, ~minute of downtime.)

### Modem power-state policy: when on, when cycled?

**Resolved for this HW rev (2026-05-27): always on.** Deep-sleep
cycling deferred until power-budget profiling on real hardware shows
it's worth the state-machine complexity. Keeping the discussion below
for the future revisit.

The 9704 has non-trivial idle power (especially with the receiver
active for MT delivery). Three rough policies:

- **Always on.** Simplest firmware, immediate MT delivery, no
  registration latency penalty per cycle. Worst power draw.
- **Aggressive cycling.** Modem on during scheduled comms windows,
  off between. Low power. Adds 30 s – few min of registration time
  to every comms window. MTs that arrive while modem is off are
  held by the gateway (good!), but only delivered during the next
  on-window (latency penalty).
- **Hybrid / event-driven.** On in response to ground command,
  scheduled telemetry cadence, or local event. Off otherwise.
  Best power/latency trade-off but most state machine complexity.

The choice interacts with:

- **GPIO interlock complexity.** Aggressive cycling means many
  `I_EN` transitions, each constrained by the `I_BTD` damage
  interlock. Lots of opportunities to get it wrong.
- **Brownout recovery.** Modem state at MCU reset depends on
  whether we were mid-cycle when the reset happened. Hybrid policy
  has more transient states to recover from.
- **Provisioning durability.** Provisioning is in modem flash, so
  power-cycling doesn't lose it. (Worth confirming with a deliberate
  power-cycle test.)
- **Registration after wake.** Confirm that re-registration after
  warm-boot is fast (~tens of seconds) vs. cold-boot (longer).
- **MT delivery semantics.** Iridium holds MTs for a configurable
  store window — need to find out what Cloudloop's setting is.
  Worst case: an MT issued while modem is off is lost if the
  off-window exceeds the hold time.

Sub-questions:

- What's our power budget for the modem subsystem? Drives the
  whole choice.
- What's the worst-acceptable MT latency for command response?
  Drives on-window cadence in policy #2/#3.
- How does the EO companion's duty cycle interact? If companion is
  also intermittent, align the wake schedules.
- Is there value in a "low-power but registered" mode — modem
  receiver active but transmitter idle? (Need to check 9704
  datasheet for whether that's a real state.)
- How do we report power-state transitions to ground for
  observability?

### Manager MCU part — RESOLVED (2026-06-23): another L4, not an L0

**Decision: the modem manager will be a second L4-class STM32 (L452 baseline, to match
the Core), not an L0.** The ~18 KB library RAM floor that drove the entire L0-fit
analysis below is trivial on an L4 (L452 = 160 KB SRAM), so the SRAM constraint, the
`IMT_PAYLOAD_SIZE`/queue-size RAM-tuning pressure, and the USB-host-vs-channel-count
part juggling all fall away — and the manager reuses the Core's L452 HAL/CubeMX
toolchain. The L0 analysis below is retained for the record only.

---

#### (historical) L0 part-selection analysis

The library's fixed buffer cost is ~17 KB before any user buffers:

- `jsprRxBuffer` — 8 KB
- `JSPR_MAX_JSON_LENGTH` (response.json) — 3.5 KB
- `BASE64_TEMP_BUFFER` — 2 KB
- `COMMAND_MAX_LEN` (jsprCommandBuffer) — 2 KB
- Misc state, RX ring buffer, etc. — ~1.5 KB

Plus per-message:
- `IMT_QUEUE_SIZE × IMT_PAYLOAD_SIZE × 2` (MO + MT) — variable.
  At queue size 1 and payload 342 B (one Iridium segment + CRC):
  ~700 B.

So ~18 KB floor on L0. Plus protocol parser, app code, stack, etc.
Realistic minimum: ~24 KB SRAM, comfortable: ~32 KB+.

STM32L0 candidates:

| Part      | Flash | SRAM | Fit? |
|-----------|-------|------|------|
| L011/L021 | 16 KB | 2 KB | No — way too small |
| L031/L041 | 32 KB | 8 KB | No |
| L051/L052 | 64 KB | 8 KB | No |
| L053      | 64 KB | 8 KB | No |
| L062/L063 | 32 KB | 8 KB | No |
| L071/L072 | 192 KB| 20 KB| Tight but feasible (with `IMT_PAYLOAD_SIZE=342`, queue size 1) |
| L073/L083 | 192 KB| 20 KB| Same as L07x |

If the library's fixed JSPR buffers were made configurable (upstream
improvement TODO), the floor could drop to ~10 KB, making smaller L0
parts viable. **Action**: probably L073/L083 baseline; revisit if
upstream patches land.

Sub-questions:

- Package preference (LQFP / BGA / WLCSP) per the production board?
- Do we want any peripherals beyond UART × 2 + GPIO? (e.g. ADC for
  modem temperature monitoring, I2C for an EEPROM with the L0's
  configuration?)
- Does keeping a common HAL/CubeMX vendor for L0 + L4 simplify the
  toolchain enough to be a deciding factor vs (e.g.) an Ambiq Apollo
  or NXP equivalent? (Probably yes — staying in STM32 land is
  cheap.)

### 9704 connection: USB-C vs 16-pin — RESOLVED (2026-05-29)

**Decision: 16-pin connector + GPIO interlock. USB-host bridge rejected.**
The dedicated manager MCU drives the 9704's power/enable pins directly and
enforces the I_EN/I_BTD damage interlock in firmware. No USB-host bridge
part to source, no USB-host stack on the manager MCU, and USB-host
capability drops off the L0 part-selection criteria.

Original investigation kept below for the record.

EE was investigating USB-host-capable USB-UART converter chips. If a
viable part existed, 9704 would connect via its USB-C port, modem-side
hardware would handle startup/shutdown sequencing, and the GPIO interlock
state machine would go away entirely. Not pursued.

Sub-questions / TBD:

- What part(s) does the EE find viable? Candidates likely include
  FT311D, FT312D, FT4222H, MAX3421E (with SPI host driver on L0),
  VNC2.
- Does the L0 part itself have any USB host capability? (STM32L4
  Plus has it; classic L0 typically does not.)
- Cost/BOM impact vs the GPIO solution's software cost.
- Power budget impact — USB-host bridges have their own idle draw.
- Licensing/regulatory: any restrictions on the USB host approach
  for satellite-comms hardware?

**Resolved (2026-05-29): 16-pin + GPIO interlock chosen, USB-host
rejected.** The GPIO state machine is the production path, no longer a
conditional fallback. Sub-questions above retained for the record.

---

## Super-loop integration with other I/O

Other subsystems sharing the MCU: I2C sensor polling, ADC reads, SD card
logging (FATFS over SPI), 4 additional UART ports.

`rbPoll` must run at ≤50 ms cadence — modem aborts MO with
`SEGMENT_NOT_SUPPLIED_MOS` if a `messageOriginateSegment` prompt isn't
answered within ~300 ms. Bare-metal for now; revisit FreeRTOS later if
loop budget gets tight.

- [ ] **Sprinkle `rbPoll()` between every blocking op in the super-loop.**
      Empty-ring case is ~µs, free to over-call.
- [ ] **Chunk SD card writes.** Multi-block flushes can block 100+ ms.
      Break at FAT cluster boundaries with explicit `disk_ioctl(CTRL_SYNC)`
      in between; `rbPoll()` after each chunk.
- [ ] **DMA-TX retarget for UART2 (console).** Currently
      `consolePrintf` does blocking `HAL_UART_Transmit` with 100 ms
      timeout. Replace with small TX ring + `HAL_UART_Transmit_DMA` +
      ring-advance on TC interrupt. ~50 LoC.
- [ ] **NVIC priority discipline.** Document and enforce:
      - Highest: time-critical sensor sample timing
      - Mid: all UART RX IRQs (including `huart1` for modem)
      - Lower: SPI/DMA completion (SD)
      - Lowest: SysTick
      No IRQ at priority 0 unless we really mean it.

---

## L0 ↔ L4 protocol design

(Was: companion computer / EO imaging arbitration. EO companion deferred
in this HW rev — re-enters in a future revision as a second client of
the same L4-side API. Most of the original arbitration thinking now
applies to the L4 ↔ L0 link instead.)

Decision (2026-05-27): text-based protocol modelled on the existing
ACTU SS spec. Format `<TYPE>,<SEQ>,<TARGET>,<CMD>,<ARGS...>\n`. Types
include `CMD`/`ACK`/`ERR`/`STAT` plus a new `EVT` type for unsolicited
modem events. Same shape already in production on actuator links.

Sketch in `LUX_DEVLOG.md` 2026-05-27. To turn into a spec:

- [ ] **Decide L4 ↔ L0 link parameters**: baud (probably 230400 to
      match modem-side, or 115200), parity, hardware flow control
      yes/no.
      - [ ] **Serial transport mode (JC sync 2026-05-29).** Modem link
            (manager): **RX = DMA** (≤300 ms segment deadline,
            un-backpressurable modem), **TX = interrupt** (non-blocking;
            we schedule sends — DMA-TX only if a channel is spare). Core↔
            manager link: **DMA-capable on the L4 (Core) side** to keep the
            move-to-Core option open, but **interrupt near-term** (tiny
            frames). Current bring-up firmware is blocking-TX + per-byte-IT
            -RX; migrate to IT-TX + circular-DMA-RX
            (`HAL_UARTEx_ReceiveToIdle_DMA`) when the manager part is
            chosen — the M4 bring-up build can stay as-is meanwhile.
            Channel budget (L0 ≈ 7, L452 = 14) feeds L0 part selection.
- [ ] **Convenience GPIO/IRQ line between Core and manager (JC sync
      2026-05-29).** A dedicated signal pin (+ interrupt) separate from the
      UART. No committed use yet — manager→Core attention/IRQ, power-good
      relay, etc. Cheap optionality; add the pin now, decide use later.
- [ ] **Provision dedicated topics** in Cloudloop:
      - 315 (RED) — telemetry
      - 316 (ORANGE) — imagery (deferred in this HW rev; provision
        anyway so it's ready when EO returns)
      - TBD — commands inbound
- [ ] **Define the v0.1 command set.** Minimum viable list:
      - `CMD,SEQ,RB,PING` / `ACK,SEQ,RB,PING` (heartbeat)
      - `CMD,SEQ,RB,SEND_MO,TOPIC,HEX_PAYLOAD` /
        `ACK,SEQ,RB,SEND_MO,MO_ID` /
        `EVT,SEQ,RB,MO_COMPLETE,MO_ID,STATUS`
      - `EVT,SEQ,RB,MT,TOPIC,HEX_PAYLOAD` (unsolicited)
      - `EVT,SEQ,RB,SIG,BARS,LEVEL,VISIBLE` (signal change)
      - `STAT,SEQ,RB,STATE,REGISTERED|NOT_REG|FAIL` (periodic)
      - `CMD,SEQ,RB,GET_STATUS` / status reply
      Error code set TBD; at minimum `QUEUE_FULL`, `NOT_PROVISIONED`,
      `INVALID_TOPIC`, `INVALID_ARGS`, `MODEM_FAULT`.
- [ ] **Modem power-control commands (future, noted 2026-05-28).**
      The Core should be able to instruct the manager to power the
      modem on/off and to recover from a FAULT (i.e. trigger the
      shutdown→startup interlock cycle remotely). Candidate commands:
      `CMD,SEQ,RB,MODEM_ON` / `MODEM_OFF` / `RESET`. Not needed for the
      POC (the dev-board user button triggers the sequence), but the
      GPIO state machine should be built with a Core-triggerable
      recovery path out of FAULT so this drops in cleanly later.
      The button and the future RESET command should funnel into the
      same state-machine entry point.
- [ ] **Binary-payload encoding choice.** Hex (simple, ~100% overhead)
      vs base64 (denser, ~33% overhead, slightly fiercer parser).
      Recommendation: hex for v0.1 since MO/MT payloads are typically
      <300 B. Revisit when thumbnails return.
- [ ] **L4-side outbox design.** Multi-tier priority queue (telemetry
      > thumbnail > opportunistic, even though only telemetry exists
      in this HW rev). L4 drains into L0 via `SEND_MO` when L0 has
      capacity (signalled by L0 acking previous sends).
- [ ] **Sequence-number space and dedup policy.** Per-direction
      monotonic; receiver dedups by SEQ. ACTU SS already has this
      pattern; carry it over.
- [ ] **Heartbeat policy.** L4 pings L0 every N seconds (TBD, probably
      1–5 s). L0 resets if it misses M consecutive (TBD, probably
      3–5). L4 logs if L0 reset events repeat — symptomatic of
      modem-subsystem instability.
- [ ] **Heartbeat MO as MT-puller** ⭐. Field test 2026-05-27
      confirmed: when an MO completes successfully, the Iridium
      gateway opportunistically drains any held MTs in the same
      session. This is a clean active-polling mechanism for inbound
      commands. Design implication:
      - L4 schedules small heartbeat MOs at cadence N (TBD; 5–30 min
        depending on power/cost tolerance) on a low-priority topic.
      - L0 forwards them via `rbSendMessageAsync`.
      - Successful MO → gateway pushes held MTs → L0 forwards each
        to L4 as `EVT,SEQ,RB,MT,...`.
      - Tunable knob: heartbeat cadence trades MT-pull latency
        bound vs. sat-byte cost.
      - Distinct from the L4 ↔ L0 link `PING` command (which is the
        local inter-MCU heartbeat); the heartbeat MO is the
        radio-side periodic ping. Probably want a separate
        `CMD,SEQ,RB,HEARTBEAT` from L4 to L0 that fires this so the
        cadence policy lives on L4.
      - Topic choice: cheapest provisioned topic (probably RAW or
        a dedicated heartbeat topic if Cloudloop billing supports
        it).
      - See `LUX_DEVLOG.md` 2026-05-27 field-test entry for the
        observation that prompted this.

### Deferred (re-enters with EO companion)

- [ ] **EO companion routing.** Through L4 (preferred), or direct to
      L0 via additional UART. Picks up when EO comes back into scope.
- [ ] **Multi-tier priority queue activation.** Telemetry vs thumbnail
      vs opportunistic ordering becomes meaningful when there are
      multiple clients.
- [ ] **Thumbnail size budget.** Decide max thumbnail size — drives
      `IMT_PAYLOAD_SIZE` on L0. Working assumption: 10 KB (one IMT
      message). Bump if companion can't compress that hard. May
      force a larger L0 part than the bare-minimum 20 KB option.

---

## Library-level improvements (potential upstream patches)

Things worth fixing in the library itself, not just our application:

- [ ] **`receiveJspr` drops unsolicited frames during sync waits.**
      ([jspr.c:118](src/jspr.c:118)). When `expectedTarget != NULL`,
      non-matching frames are `memset`-ed and discarded. Means an MT
      arriving in the ~5–50 ms window between
      `PUT messageOriginate` and the `200 messageOriginate` reply is
      lost silently — subsequent `messageTerminateSegment` frames then
      have no MT queue entry to bind to. Patch: dispatch unsolicited
      frames through the same path as `rbPoll` while waiting.
      ~20 LoC.
- [ ] **Provisioning-failure visibility.** `rbSendMessageAsync` returns
      `false` if `checkProvisioning` fails, with no diagnostic. Add an
      out-parameter or last-error global so callers can distinguish
      "not provisioned" from "queue full" from "topic invalid".
- [ ] **Make JSPR scratch buffers configurable.** `RX_BUFFER_SIZE` /
      `TX_BUFFER_SIZE` / `JSPR_MAX_JSON_LENGTH` are fixed at 8 KB / 8 KB
      / 3.5 KB. They dominate RAM on tight MCUs. Should be overridable
      like `IMT_PAYLOAD_SIZE` already is. (Note: `IMT_PAYLOAD_SIZE` got an
      `STM32_HAL` default of 2048+CRC in the fork — commit 40363fd — since the
      non-Arduino default is 100 kB and overflows MCU RAM.)
- [ ] **`setApi` reply window too tight** ([rockblock_9704.c:155]). Sends
      `GET apiVersion` then checks with a *non-blocking* `receiveJspr`, only 2×
      with `delay(5)` between (~10 ms total). A ready modem that replies slower
      — or is still booting — is missed, so `rbBegin` returns false. **Fixed in fork a2f1197 (2026-05-30):** swapped to
      `waitForJsprMessage(&response, target, JSPR_RC_NO_ERROR, 1)` across
      setApi/setSim/setState. But it was **not** the S1 first-contact blocker —
      even patient, the modem still doesn't answer our 16-pin commands (manual
      GET probe pending; see LUX_DEVLOG 2026-05-30). Real bug + good upstream
      patch regardless.
- [ ] **`clearLeftoverData` / `receiveJspr` can infinite-loop on continuous RX**
      ([rockblock_9704.c:137], [jspr.c:55]). No bound/timeout on the drain/read
      loops: if the line streams continuously (modem boot burst, or a TXD-low
      0x00 flood) they never exit → hung supervisor (caught 2026-05-30; the
      manager's IWDG now backstops it). Patch: cap the drain (byte-budget or
      time bound).
- [ ] **(HELD — pending evidence) Feed the IWDG from inside the `delay()`
      shim** (crossplatform STM32 branch). Was proposed (S2-part-5) for a
      *legitimately slow* `rbBegin`, but **S2-part-6 found the resets were a
      HardFault on the first DEBUG `printf`, not a slow handshake** — and the
      `delay()`-feed would NOT have helped that (the fault is a tight
      `_write`→`__io_putchar` call, and `HardFault_Handler`'s `while(1)` calls
      no `delay()` either). So this is on hold until the trace run shows whether
      a genuinely slow handshake even exists. *If* it does: `waitForJsprMessage`
      calls `delay(10)` and `setApi` `delay(5)`, so feeding the dog there keeps
      it fed through real waiting, while `clearLeftoverData`'s *tight* spin (no
      `delay()`) is still caught. Don't pet inside `serialRead`. Don't add the
      complexity for a problem we may not have.
- [ ] **`rbSendMessageAny` bool-compare bug** ([rockblock_9704.c:430]).
      `if(queued >= 0)` where `queued` is `bool` → always true; defeats the
      queue-add success check. Sync-API-only (we use async) and bounded by a
      downstream NULL guard, but a real bug. Fix: `if(queued)`. Also remove the
      dead locals in `sendMoFromQueue` (initCrc/segmentStart/segmentLength/
      encodedBytes). (Found via build warnings 2026-05-30.)
- [ ] **`hwInfo`/`simStatus` accessors each do their own round-trip — add a
      cached/batch fetch.** `rbGetImei`/`rbGetHwVersion`/`rbGetSerialNumber`/
      `rbGetBoardTemp` each call `getHwInfo` → a separate `GET hwInfo`, so logging
      three fields fires three round-trips (seen at first contact: 3× `GET hwInfo`
      back-to-back). Same shape for `getSimStatus`. Options: cache the last-fetched
      struct (note `board_temp` goes stale — bound the cache age), or expose a
      public `rbGetHwInfo(jsprHwInfo_t*)` so a caller fetches once and reads all
      fields. **Deferred (Liam, 2026-05-31):** not worth risking the upstream
      examples right now — future improvement. NB: the *correctness* bug (impatient
      single read → sentinels) IS fixed — see `LUX_LIBRARY_CHANGES.md` §3; this
      item is only the round-trip redundancy.

---

## Operational reminders (not code)

Two distinct sky-view-gated states often confused:

| State              | What it means                                                                 | Persists?                       | Typical time to acquire             |
|--------------------|-------------------------------------------------------------------------------|---------------------------------|-------------------------------------|
| **Registered**     | Modem is currently logged onto the Iridium network and reachable for MTs/MOs. | No — re-acquired every power-up | 30 s – few minutes with clear sky   |
| **Provisioned**    | Modem has pulled its topic/plan config from the satellite into its flash.     | Yes — survives reboots          | 10 – 30 minutes with clear sky once |

Cloudloop's "Pulses" timeline distinguishes these. A held MT
showing **Delivered MT ✗ "Not Registered"** means the gateway has the
message but the device isn't currently on the network. As soon as the
device registers, queued MTs drop down automatically.

- [ ] **First-run provisioning warm-up.** After Cloudloop registration,
      power the modem with clear sky view for **30+ minutes** before
      first STM32 run. Modem pulls topic provisioning over Iridium and
      persists to its flash. Subsequent boots only need to re-register
      (seconds–minutes), not re-provision.
- [ ] **Indoor / window placement usually doesn't cut it.** Iridium
      satellites need most of the upper hemisphere visible. Test
      stations need genuine open sky access.
- [ ] **If Cloudloop plan changes**, call `rbResyncServiceConfig()` and
      power-cycle the modem (in that order, per README).
- [ ] **Don't skip the 100 ms `HAL_Delay` after `rbBegin`.** Without it,
      first MO returns `RC 418` (modem still settling internal state).

---

## QA / production-test logistics

Sky view is a hard physical requirement and likely a throughput
bottleneck for unit QA. Worth deciding up front.

**Baseline tool already exists:** `examples/python/link_monitor.py` is
the basis of the Forge acceptance/commissioning step. Passive
observation, captures signal-bars + dBm + visibility + decoded MT
payload with timestamps. See LUX_DEVLOG 2026-05-27 for the reframing
discussion. Remaining work is around pass criteria definition and
test-fixture integration.

- [ ] **Facility-level RF infrastructure decision.** Two paths for the
      Forge to support continuous RF QA (see LUX_DEVLOG 2026-05-27):
      - **Open-air / rooftop test bay** — high capital cost, real
        bidirectional link, weather/season dependent.
      - **Rooftop L-band re-emitter** feeding indoor test bays —
        lower capital cost, weather-independent, but typically
        receive-only (covers acceptance pass but not MO/round-trip).
      Most likely shape: re-emitter for bulk acceptance + small
      outdoor bay for sampled round-trip verification. Confirm
      regulatory licensing before committing.
- [ ] **Decide on a bring-up QA test station** in the meantime.
      For pre-Forge work, somewhere with genuine sky view (roof,
      outdoor enclosure, ground-floor bay with open ceiling hatch,
      etc.). Window-mounted antennas have been unreliable in
      bring-up.
- [ ] **Define a QA pass criterion** that doesn't require waiting for
      the worst-case 30-minute provisioning window on every unit.
      Working shape (refine against reference module traces):
      "registers within 90 s, maintains ≥ N bars for ≥ X seconds in a
      5-min window, decodes reference MT within Y seconds of first
      registration." N/X/Y to be determined from reference-module
      `link_monitor` traces. Assumes modem batch arrived pre-
      provisioned from supplier, which is usually the case once one
      unit per IMEI block has been activated.
- [ ] **Capture reference-module traces.** Pick a known-good 9704,
      run link_monitor for a 5–10 min window under standard test-
      station antenna placement, archive the log as the comparison
      baseline. Re-capture seasonally or after any antenna-supply
      change.
- [ ] **Per-unit RF fingerprint logging.** Pipe link_monitor output
      to a structured per-unit log file (one per IMEI). dBm-level
      distribution over the 5-min window is the actual sensitivity
      fingerprint — far more informative than the 5-bar reading.
      Drift in this distribution over time = production-process or
      antenna-supply regression worth catching.
- [ ] **Forge test fixture integration.** Probably a Pi or small
      Linux box running link_monitor with the unit-under-test on USB,
      reference module nearby for cross-check, structured pass/fail
      emitter to whatever traveller / MES system tracks units. Log
      format (`SIG`/`MT` prefixes, ISO-style UTC timestamps) is
      already grep/awk-friendly.
- [ ] **Loopback / dry-run mode** for benchtop firmware iteration.
      Stub the JSPR layer so we can develop arbitration logic, command
      handlers, SD logging, etc. without burning Iridium credits or
      needing sky view. Probably a `serialMock` preset that replays
      canned JSPR responses.
- [ ] **Track per-unit registration time** during QA. If it creeps up
      it's an antenna-placement or RF-shielding issue worth catching
      before deployment.
