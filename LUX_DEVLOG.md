# Lux RB9704 devlog

Chronological log of design decisions, experiments, and discoveries while
porting the RockBLOCK-9704 library to the Lux node firmware and bringing up
the dev hardware. Captures *why* things were done, not just *what*. Newest
entries at the bottom.

Working notes for ongoing decisions live in `LUX_INTEGRATION_TODO.md`.

---

## 2026-05-26 — Day 1: porting

### Decision: target STM32 HAL on Nucleo-L452RE → L452VCT6

Confirmed bring-up target:
- **Dev board**: Nucleo-L452RE
- **Production**: STM32L452VCT6 (same SRAM/peripheral layout)
- **HAL layer**: STM32 HAL via CubeMX
- **Toolchain/IDE**: STM32CubeIDE
- **RTOS**: Bare-metal super-loop (defer FreeRTOS until later if needed)

Initial RAM estimate flagged as tight (<64 KB) before realising the L452
has 160 KB SRAM; budget revised mid-session.

### Library architecture survey

Reviewed the upstream library structure. Found a clean abstraction model
already in place:
- `serial.h` defines a `serialContext` struct of function pointers
  (`init`, `deinit`, `read`, `write`, `peek`)
- Existing presets under `src/serial_presets/` for Linux, Windows, Arduino
- `crossplatform.{h,c}` provides `millis()` / `delay()` per platform
- README documents `SERIAL_CONTEXT_SETUP_FUNC` macro override hook

Implication: STM32 port is mostly a new serial preset + a `crossplatform`
branch. No deep restructuring required.

### Port: STM32 serial preset

Created `src/serial_presets/serial_stm32/{serial_stm32.h, serial_stm32.c}`.

Implementation choices:
- **RX path**: HAL `HAL_UART_Receive_IT` single-byte interrupt feeds a power-of-two
  ring buffer (default 512 B). Ring buffer drained by `rbPoll()` via `peekStm32`
  / `readStm32`. Chose this over circular-DMA because it works on every STM32
  family without DMA stream allocation hassle, and 230400 baud is well within
  IT-driven byte rates for any Cortex-M.
- **TX path**: blocking `HAL_UART_Transmit` from `writeStm32`. Timeout
  initially set to 1000 ms (later reduced — see 2026-05-27 entry).
- **Wakeup**: user wires `HAL_UART_RxCpltCallback` and `HAL_UART_ErrorCallback`
  to forward to `rb9704UartRxCpltCallback` / `rb9704UartErrorCallback`. The
  error callback re-arms IT receive if HAL halted it on a UART error
  (overrun/framing).
- **Detection**: opt-in via `STM32_HAL` define. CubeMX-style include via
  `#include "main.h"` (overridable through `RB9704_STM32_HAL_HEADER`).

Power-of-two ring buffer enforced with `#error` in the header if violated —
the bitmask arithmetic in the ring would silently corrupt otherwise.

### Port: crossplatform timing

Added `STM32_HAL` branch to `crossplatform.{h,c}`:
- `millis()` → `HAL_GetTick()`
- `delay()` → `HAL_Delay()`

### Port: rbBegin overload and platform detection

- `rockblock_9704.h`: added `STM32_HAL` branch to the serial-preset include
  cascade and to `SERIAL_CONTEXT_SETUP_FUNC`.
- `rockblock_9704.h`: declared `rbBegin(UART_HandleTypeDef *)` overload
  (parallel to the existing `rbBegin(Stream &)` for Arduino and
  `rbBegin(const char *)` for desktop).
- `rockblock_9704.c`: defined the STM32 `rbBegin`; gated `#include <unistd.h>`
  off for STM32 (bare-metal has no unistd); extended the `#ifdef ARDUINO`
  branch at line 158 to also cover `STM32_HAL` so `delay()` is used instead of
  `usleep()` for the inter-API-attempt pause.

Build sanity-checked: ran existing Windows CMake/MSVC build to confirm the
new `serial_stm32.c` translates to an empty object when `STM32_HAL` isn't
defined. Library still links cleanly. Two pre-existing warnings unchanged
(`%ld` vs `size_t`, `TEXT` macro redefinition) — not our problem.

### Nucleo-L452RE bring-up example

Wrote `examples/nucleo_l452re_async_send_receive.c`. Single file structured
to mirror CubeMX-generated `main.c` with `USER CODE BEGIN/END` markers so
sections can be copy-pasted into a real CubeMX project.

Behaviour:
- USART1 = RB9704 link (230400 8N1)
- USART2 = console (115200 8N1, via ST-LINK VCP)
- `consolePrintf` wraps `HAL_UART_Transmit` with `vsnprintf` for printf-style
  output
- Sends "Hello from Lux" every 30 s via `rbSendMessageAsync(RAW_TOPIC, ...)`
- On MT-complete callback, copies payload out and prints to console with
  non-printables sanitised to `.`
- `rbPoll()` at top of loop, `HAL_Delay(5)` at bottom

Pin wiring documented in file header — PA9/PA10 for the modem link, PA2/PA3
already wired through the onboard ST-LINK VCP for the console.

### Deep dive: how the RB side of the library actually works

Walked through `rockblock_9704.c`, `jspr.c`, `jspr_command.c`, `imt_queue.c`
in detail. Key model that emerged:

- **JSPR protocol**: line-oriented text over UART. Host sends
  `VERB target {json}\r`; modem replies with `nnn target {json}\r` where
  `nnn` is a 3-digit code. `200` = success, `299` = unsolicited, `4xx/5xx` =
  errors. Includes the famous `418 Not Provisioned`.
- **Layer cake**: `serial.{h,c}` → `jspr.{h,c}` codec → `imt_queue.{h,c}`
  MO/MT queues → `rockblock_9704.{h,c}` high-level API. Everything above
  serial is fully portable C.
- **Connection bring-up** (`rbBegin`): `setApi` → `setSim` → `setState` →
  `imtQueueInit`. The `setApi` step loops twice with `delay(5)` between
  attempts (this is the line we extended for STM32).
- **MO send** is a multi-frame dance: `PUT messageOriginate` → `200` reply
  with `messageId` → wait for modem to unsolicit `messageOriginateSegment`
  prompts (chunked at `JSPR_MAX_SEGMENT_LENGTH=1447`) → respond with
  base64-encoded segments → modem buffers entire message internally → modem
  transmits over Iridium as one IMT message → modem emits final
  `messageOriginateStatus`.
- **MT receive** is symmetric in reverse: modem unsolicits `messageTerminate`
  → emits one or more `messageTerminateSegment` frames with base64 data →
  unsolicits `messageTerminateStatus { COMPLETE }`. Library base64-decodes
  segments directly into the MT slot.
- **`rbPoll()`** handles all unsolicited frames via dispatch table. Each
  call processes at most one frame. Must run at ≤50 ms cadence — modem
  aborts MO with `SEGMENT_NOT_SUPPLIED_MOS` if a segment prompt isn't
  answered within ~300 ms.

#### Correction (caught by user)

Initial walkthrough phrasing made it sound like the modem transmits each
segment over satellite before requesting the next. That's wrong: the
segments are purely a UART-layer chunking mechanism. Modem pulls the whole
message across UART into its own RAM, *then* hands it to the Iridium stack
as one IMT message. There's only one `messageOriginateStatus` per message,
not per segment.

### TODO doc created

Created `LUX_INTEGRATION_TODO.md` to capture downstream-specific work items
without polluting the upstream library. Initial sections:
- Bring-up (mostly done at this point)
- Application-layer reliability (retry wrappers, seq numbers,
  status-piggyback, SD logging)
- Super-loop integration with other I/O
- Companion computer (EO imaging) arbitration
- Library-level improvements (potential upstream patches)
- Operational reminders

### First hardware contact (evening)

User ran `receive_message.py` (Python binding installed from PyPI) against
the dev kit on COM9. Modem responded correctly:
- Board temperature: 25 °C
- HW: 0x0601
- IMEI: 300258060609970
- Serial: 1a06b7

Cloudloop shows the test MT sent at 21:28:28 stuck at `Delivery Success ✗`
with status **"Not Registered"**. Diagnosis: USB-serial path is fine, modem
is alive, but it isn't currently logged onto the Iridium network because
it's plugged into a laptop indoors with no real sky view.

Important distinction surfaced: **registration** (modem currently logged
onto Iridium network, re-acquired every power-up, takes seconds–minutes
with sky view) vs **provisioning** (modem has pulled its plan config from
the satellite into its flash, persists across reboots, takes 10–30 min
once). Both gated on sky view but for different reasons.

User decided: leave antenna at a window with "ish" sky view overnight,
take it for a walk in the morning if it still hasn't registered.

---

## 2026-05-27 — Day 2: hardware reality and design questions

### Held MTs delivered overnight (~1 hour gaps)

Two MTs queued from the Cloudloop side eventually delivered, each ~1 hour
after the initial failed-attempt timestamp. I initially hypothesised this
was an Iridium-side retry cadence of ~1 hour. User pushed back: "seems
long" — correctly.

Walked back the claim: it was a one-data-point inference. More plausible
explanation given Iridium's store-and-forward semantics: the modem caught
brief registration windows ~1 hour apart, and held MTs flush instantly on
registration. The 1-hour gaps are reception-side, not network-side.

### TX timeout tightened

Reduced `RB9704_TX_TIMEOUT_MS` from 1000 → 200 ms. Math: largest JSPR frame
is ~2 KB (PUT messageOriginateSegment with 1447-byte base64 payload), which
at 230400 baud is ~87 ms of wire time. 200 ms gives ~2× headroom and fails
fast on real UART jams. Single jam now costs us 200 ms instead of 1 s —
comfortably under the 300 ms `messageOriginateSegment` deadline.

### MT print fix

The bring-up example originally did `HAL_UART_Transmit(..., 1, 10)` per
byte for MT console printing — N+2 transmits per message, worst case
~2 s blocking for a 2 KB MT. Rewrote to sanitise in-place then do three
transmits total: prefix, body (with 500 ms timeout), CRLF. Worst-case
blocking now ~200 ms regardless of MT size.

### Deep design discussion: four follow-up questions

User asked four substantive questions; answers shaped the TODO doc:

1. **MT during MO dance.** Library handles interleaving fine in `rbPoll`
   (NULL target = take any frame). But discovered a real edge case: the
   synchronous waits in `sendMoFromQueueAsync` etc. call
   `receiveJspr(&response, "messageOriginate")` which **silently
   discards** non-matching frames. An MT arriving in the ~5–50 ms window
   between `PUT messageOriginate` and `200 messageOriginate` is lost.
   Filed as a candidate upstream patch.

2. **First-run after Cloudloop registration.** The example only works
   after the modem has had 10–30 min of clear sky to pull provisioning.
   `checkProvisioning` lazily fetches via `GET messageProvisioning` on
   first send, but the modem only replies meaningfully if it knows its
   plan from the satellite. Failure mode is silent (`rbSendMessageAsync`
   returns `false`). Recommended adding a console log of rejected sends
   so this doesn't go unnoticed.

3. **Blocking analysis with Lux's other I/O.** User's planned super-loop
   includes I2C sensors, ADCs, SD card logging (FATFS over SPI), and
   4 other UARTs. Identified blocking points: `HAL_UART_Transmit` (now
   200 ms bounded), `consolePrintf` (100 ms bounded), MT print loop
   (now fixed), and SD writes (can be 100+ ms during multi-block flush).
   Five mitigations enumerated; user picked "sprinkle `rbPoll()` between
   blocking ops" as the 80/20 bare-metal move.

4. **Leasing the link to the EO companion for thumbnails.** Recommended
   architecture: STM32 always owns the modem, companion is a client of an
   STM32-side message API over a private link. Sketched the command
   protocol (`CMD_ENQUEUE_MSG` / `STATUS_*` / `RECEIVED`). Throughput math:
   10 KB thumbnail = one IMT message = ~30–60 s sat TX time. Memory budget
   fits comfortably on L452.

### TODO doc captures the "by the way" insights

Several "by the way, while we're here" items captured:
- Idempotent commands
- Sequence numbers on telemetry
- Status piggy-back tail
- SD-logging of MO IDs and seq numbers for power-cycle resilience
- Bounded re-request window on SD
- NVIC priority discipline

### Window MT receive: end-to-end confirmed (12:45 UTC)

User's antenna placement at the window finally yielded results:
- Test MT (Cloudloop → device) sent earlier morning
- Two MO attempts from `send_message.py` runs that had reported "Sending
  failed" locally
- All three landed in Cloudloop's timeline at ~12:45 UTC simultaneously

This was a key observation: **MT + two MOs all completing at the same
instant strongly supports the "drain on registration window" model.** The
modem briefly registered, the gateway flushed held MTs, and the modem's
in-flight MOs completed their satellite handshakes — all in the same
window.

### Discovery: "No Response to Ring" state

Looking at the Pulses timeline for the successful MT, found a new state
distinct from "Not Registered":

- 12:38:43 — Cloudloop Received Message
- 12:39:25 — Delivered MT ✗ **"No Response to Ring"** (~41 s after Sent)
- 12:45:40 — Delivered MT ✓ "Delivery Success" (~6 min 15 s later)

This is genuinely new information. Iridium's actual model:
- Gateway holds MT
- Gateway actively *pages* (rings) the modem on a ~5–7 min cadence (not
  1 hour as I'd guessed)
- Three outcomes per ring: registered + good signal = success; registered
  + marginal = "No Response to Ring"; unregistered = "Not Registered"

So retry interval is ~6 min, not 1 hour. Earlier 1-hour observation was
just two reception windows happening to be ~1 hour apart. **My original
1-hour cadence hypothesis is definitively wrong.**

**Second walkback (later same day):** the "~6 min retry cadence" claim
is itself a single-data-point inference and probably also overconfident.
Two timestamps (12:39:25 fail, 12:45:40 success) tell us only that
*some* time passed; they don't tell us whether the gateway tried once
or twelve times between them. Possible alternatives include:
- Fast retry (~30 s), only the last one caught a registration window.
- Event-driven push: gateway delivers immediately when a registration
  event arrives from the network, retry cadence is just a fallback for
  when registration itself misses.
- Variable / load-dependent cadence.

The "send 3 MTs spaced 5+1 min apart" experiment will discriminate
between the cadence and event-driven hypotheses cleanly. Until then,
treat all timing claims about the Iridium retry/push behaviour as
unconfirmed. **Lesson: stop fitting models to 1–2 data points.**

### Discovery: sync API timeout footgun

User's `send_message.py` failures with the message arriving later
revealed something important about the synchronous `rbSendMessage`:

```
while (true) {
    rbPoll();
    if (moDropped)         break;  // failure flag from rbPoll
    if (moSent)            break;  // success flag from rbPoll
    if (timeout-fired)     break;
}
```

**When the timeout fires, the message is NOT cancelled** — the function
just stops waiting and returns `false`. The modem still has it queued and
continues attempting satellite transmission. `moMessageComplete` may fire
long after the script gave up. The script prints "Sending failed" but the
message can complete asynchronously, sometimes much later. The two MOs
landing at 12:45 confirmed this.

Implication: **never use the sync API in production firmware.** Always
use async + `moMessageComplete` callback. Captured in TODO doc.

### Async API: what happens on mid-flight send?

User asked: what if a new message comes in via `rbSendMessageAsync` before
the previous one completes? Walked through the code:

- Default (locked queue): new message queued at tail in FIFO order; if
  queue is full, `rbSendMessageAsync` returns `false`. Safe.
- Unlocked (`rbSendUnlockAsync`): if queue is full, the **head** of the
  queue is silently dropped — which may be the message currently
  mid-transmission to the modem. Modem then times out the orphaned segment
  exchange with `SEGMENT_NOT_SUPPLIED_MOS`, but our callback never fires
  for that message (the queue entry that owned the ID is already gone).
  Lossy and silent.
- **No public cancel API** despite the modem supporting
  `MESSAGE_CANCELLED_*` final statuses. Could be added as an upstream
  improvement.

For Lux's telemetry use case, prescribed an app-level "single-slot
pending" pattern that gives clean supersede-with-latest semantics without
relying on the library's lossy unlock-on-overflow. But user wanted to
defer the actual decision — captured in TODO doc as an open design
question rather than a TODO to implement.

### Open design questions section added to TODO

Added four open design questions to the TODO doc, framed as discussion
items rather than implementable TODOs:
1. MO queueing policy (supersede / FIFO / app-level latest-only)
2. MT acknowledgement timing (ack-on-receive vs ack-on-complete)
3. Provisioning resync triggers (ground command, periodic, on-418)
4. Modem power-state policy (always-on, cycled, hybrid)

The power-state question identified as most consequential — interacts
with GPIO interlock, brownout recovery, MT latency, registration
timing, and EO companion duty cycle.

### Production wiring decision: keep 16-pin, do the GPIO state machine

User considered an alternative: use the modem's USB-C port for power +
hardware sequencing instead of doing the 16-pin GPIO dance. Pros: avoids
the `I_EN`/`I_BTD` damage interlock entirely (USB-C sequencing is
hardware). Cons: requires a USB host on our board (L452 is USB device
only), or an FTDI bridge + load switch arrangement; adds BOM cost, PCB
area, EMC concerns; introduces "does USB-C power coexist with 16-pin
UART data?" as an unknown.

User's reflection: "you're making software effort pretty cheap." On
balance — given that the GPIO state machine is essentially
`HAL_GPIO_WritePin` plus an interlock state variable plus the existing
Linux template at `rockblock_9704.c:62` — software cost is real but
bounded. Decision: pick 2–3 more GPIOs on the production board and do
the state machine. Captured the doc-derived sequence and damage
interlock requirements in the TODO.

### GPIO sequence (per GroundControl docs)

**Startup:**
1. All MCU pins to 9704 inputs (including our UART TX) tristate or low
2. Apply power (drive `P_EN`)
3. Drive `I_EN` high
4. Wait for `I_BTD` high
5. Initialize UART pins to AF mode, call `rbBegin`

**Shutdown:**
1. Cease comms
2. Drive `I_EN` low
3. Wait for `I_BTD` low
4. Tristate inputs
5. Remove power

**Damage interlock**: once `I_EN` driven high, must wait for `I_BTD` high
before driving `I_EN` low again, and vice versa. *Failure may damage the
module.* GPIO driver must enforce this — track last commanded `I_EN`,
refuse early state changes.

### Three-message MT experiment designed

To falsify (or confirm) the "drain on registration window" model:
- Send MT at t=0
- Wait 5 min, send MT at t=5
- Wait 1 min, send MT at t=6

Predictions tabulated. Two outcomes most likely:
- All three deliver within seconds of each other → pure drain
- First two deliver together, third lags by 5–6 min → drain + gateway
  retry cadence on individual MTs

Test deferred for later in the day.

### Link monitor script created

User wanted a Python script that prints signal-strength changes live.
Discovered the existing Python binding has
`set_constellation_state_callback` already exposed — easy win.

Built `examples/python/link_monitor.py`:
- Connects, registers `constellationState` callback
- Prints timestamped colour-coded bar graph + dBm-ish level on every
  signal change (de-duped against previous state)
- Plus inbound MT capture: registers `mtMessageComplete`, calls
  `receive_message_async` on completion, prints decoded payload, then
  `acknowledge_receive_head_async` to free the slot

Renamed from `signal_monitor.py` to `link_monitor.py` to reflect the
broader scope. Important nuance: the library processes inbound MTs
unconditionally during `rbPoll`, even without registered callbacks —
which means a script that polls but doesn't ack will silently consume
MTs from the network's perspective. Now captured at the application
layer rather than dropped.

### link_monitor.py polish: Windows ANSI + clean shutdown

First run on Windows PowerShell printed literal ANSI escape codes
(`←[2m[14:55:35Z]←[0m ←[36mSIG←[0m ...`). Windows PowerShell 5.x
doesn't enable VT100 processing in its console handle by default —
Windows 10+ supports it but a process must opt in via `SetConsoleMode`
with `ENABLE_VIRTUAL_TERMINAL_PROCESSING`. Added a small ctypes
bootstrap at script start; no new dependencies. Bonuses while there:
`NO_COLOR` env var respected (no-color.org convention), and graceful
plain-text fallback when stdout is redirected to a file/pipe.

Also caught a cleanup-path crash: the Python binding's
`set_*_callback(None)` raises `TypeError: must be callable` rather
than clearing the callback. The shutdown sequence in `finally:` was
trying to unregister, hitting the type check, and surfacing an ugly
traceback on Ctrl+C. Fix: skip the unregister and rely on `rb.end()`
closing the serial connection — once serial is closed the callbacks
can't be invoked, so explicit unregister isn't needed. (The binding
behaviour is arguably a bug worth filing upstream — it would be nice
to be able to pass `None` to clear.)

After both fixes: clean colour output, clean exit, working as
intended.

### Side note: link_monitor's *real* role

link_monitor.py started as a debugging convenience for evaluating
antenna placement during bring-up. On reflection it's actually the
core of the **acceptance test & commissioning step** for incoming
9704 modules at the future Forge facility.

What makes it suitable:

- **Passive.** No commands sent, no Cloudloop credits burned per
  run, no risk of corrupting modem state. Pure observation.
- **End-to-end without ambiguity.** Captures signal-bars + dBm-ish
  level + constellation visibility + actual MT payload bytes. If
  a unit registers, sees signal, and decodes a known reference MT
  correctly, the full receive chain is verified — UART, JSPR codec,
  satellite RX, base64 decode, queue management, all of it.
- **Time-series, not snapshot.** Bars/level fluctuate continuously
  with constellation geometry. A short single-shot reading tells
  you nothing useful; a few-minute trace gives a real RF
  fingerprint of *this specific module + antenna + placement*.
- **dBm-level resolution.** The raw `signalLevel` is far more
  informative than the 5-bar indicator — two units both reading
  "3 bars" can differ by 10+ dB in actual sensitivity. The trace
  log makes that visible.

Implications for the QA pass criterion design (TODO item):

- A reference module's trace from the same test station, under the
  same antenna placement, becomes the comparison baseline.
- Pass condition is shape-of-trace, not single number: "registered
  within 90 s, maintained ≥ N bars for ≥ X seconds, decoded
  reference MT within Y seconds of first registration."
- Per-unit `signalLevel` distribution over a 5-min window is the
  RF-sensitivity fingerprint. Drift in that distribution
  over time = production-process or antenna-supply regression.
- The script's log format is already grep/awk-friendly (`SIG` and
  `MT` prefixes, ISO-style UTC timestamps) — straightforward to
  pipe into a per-unit acceptance record.

Worth noting in the QA / production-test logistics TODO entry that
the tool already exists; the work is around defining the pass
criteria and integrating into the facility test fixture (probably
a Pi or small Linux box running the script with the unit-under-test
on USB, the reference module nearby, and a structured pass/fail
emitter).

### Facility-level QA infrastructure note

Two viable approaches for the Forge to support continuous RF-quality
QA, captured for forward planning:

1. **Open-air or rooftop testing bay.** Big capital investment —
   facility design needs a bay with genuine upper-hemisphere
   visibility, weather protection, possibly a retractable cover,
   cable runs back to indoor test fixtures. Maximum fidelity (real
   bidirectional satellite link), but cost-heavy and weather/season
   constrained.
2. **Iridium L-band re-emitter on the roof.** Rooftop antenna feeds
   a building-internal repeater that rebroadcasts the downlink at
   attenuated power inside a test area. Smaller capital cost, indoor
   test bays, weather-independent. Established product category
   (similar to GNSS repeaters for indoor receiver test). Used
   widely for receiver-side QA.

**Important nuance:** L-band re-emitters are typically **receive-only**
(downlink rebroadcast inside, no uplink path). That covers most of
what link_monitor.py exercises — registration, MT reception, signal
quality, payload decode — i.e. the bulk of the acceptance test. But
**MO/round-trip testing requires real TX-to-satellite path**, which
the re-emitter doesn't provide. Three ways to handle that:

- Accept the split: re-emitter for the high-volume acceptance pass,
  separate outdoor bay (smaller, lower-investment) for round-trip
  verification on a sampled basis.
- Re-emitter + Iridium simulator / loopback gateway: synthetic but
  full bidirectional. Adds cost + the question of whether simulator
  fidelity is good enough for production sign-off.
- Pure outdoor bay: skip the re-emitter, take the bigger capex hit.

The "re-emitter for bulk + small outdoor bay for sampled round-trip"
shape is probably the right balance — high throughput on acceptance,
real-network confidence on a per-batch basis.

Also worth confirming before committing: regulatory licensing for
re-emitting Iridium spectrum (Industry Canada / FCC depending on
deployment site), and whether the modem's own TX from inside a
re-emitter bay could interfere with neighbouring units doing
acceptance.

### Architecture pivot: dedicated modem MCU

Significant scope decision: the 9704 will be managed by a dedicated
**L0-class STM32**, not by the L452 main avionics MCU. The L0 owns the
modem; the L4 talks to the L0 over a UART link as a client of an L0-
side message API.

```
   9704 ─── UART or USB ─── L0 (modem mgr) ─── UART ─── L4 (avionics)
```

Drivers for the decision:
- **Power**: L0-class chips are much lower draw than L4.
- **Isolation**: modem-subsystem faults can't take down main avionics.
- **Architectural generality**: the "STM32 owns modem, companion is a
  client" pattern we'd been sketching for the EO use case isn't an
  EO-specific thing — it's the product architecture. L4 is now the
  primary client; EO companion (when re-introduced in a later HW rev)
  becomes a second client of the same API, most likely routed through
  L4 rather than directly to L0.

EE is investigating USB-host-capable USB-UART converter chips that
would let the 9704 connect via its USB-C port (with hardware
sequencing handled by the modem's USB hardware). If a workable part
exists, **the GPIO interlock state machine goes away entirely** — no
`P_EN`/`I_EN`/`I_BTD` dance, no damage-prevention interlock, no
deferred-UART-init complexity. TBD pending part availability and
licensing/availability of L0 USB host support.

### Decisions locked this session

| Question                              | Decision                                                       |
|---------------------------------------|----------------------------------------------------------------|
| EO companion in this HW rev           | **Deferred entirely.** L4 is the only client of the L0 API.   |
| L0 ↔ L4 protocol shape                | Text-based, modelled on the existing ACTU SS protocol (see   |
|                                       | Notion: ACTU SS v0.1) — `<TYPE>,<SEQ>,<TARGET>,<CMD>,<ARGS>\n`|
|                                       | with `CMD`/`ACK`/`ERR`/`STAT`/(`EVT`) message types. Same    |
|                                       | shape already implemented & proven on actuator links.        |
| Boot sequencing                       | L0 idles waiting for first `PING` from L4. L4 starts comms.  |
| L0 power policy (this HW rev)         | **Always on.** Deep-sleep deferred until power profiling     |
|                                       | justifies the added state-machine complexity.                |
| Heartbeat / watchdog                  | `PING`/`ACK` in the protocol. Already part of ACTU SS.       |
| MO outbox location                    | **L4-side.** L0 has only enough RAM for the in-flight MO +   |
|                                       | maybe one queued. L4 holds the application outbox.           |
| `consolePrintf` retarget              | **Skip for production.** Keep for bring-up; drop if it      |
|                                       | causes blocking issues. No DMA TX work needed.               |

### Protocol design sketch (for later detailed design)

Mirroring ACTU SS, with `RB` as the target identifier for the 9704
subsystem and an additional `EVT` type for unsolicited modem events:

```
CMD,<SEQ>,RB,SEND_MO,<TOPIC>,<HEX_PAYLOAD>     -- L4 enqueues an MO
ACK,<SEQ>,RB,SEND_MO,<MO_ID>                   -- L0 accepted, assigned modem ID
ERR,<SEQ>,RB,<CODE>                            -- rejection w/ error code

CMD,<SEQ>,RB,PING                              -- L4 heartbeat
ACK,<SEQ>,RB,PING                              -- L0 alive

EVT,<SEQ>,RB,MT,<TOPIC>,<HEX_PAYLOAD>          -- unsolicited inbound MT
EVT,<SEQ>,RB,MO_COMPLETE,<MO_ID>,<STATUS>      -- MO send result
EVT,<SEQ>,RB,SIG,<BARS>,<LEVEL>,<VISIBLE>      -- signal change

STAT,<SEQ>,RB,STATE,<REGISTERED|NOT_REG|FAIL>  -- periodic
```

Binary payload encoding TBD (hex vs base64 — hex is simpler to parse,
base64 is denser by ~25%). Probably hex for v0.1 given how small most
MO payloads will be; revisit when thumbnails come back into scope.

There's a pleasing architectural symmetry here: the L0 sits between
two line-oriented text protocols (JSPR on the modem side, ACTU-style
on the L4 side) and effectively translates between them. Both are
ASCII, newline-terminated, sequence-numbered. The L0's job reduces to:
parse line → dispatch → call library function → encode result line.

### Still open

- **Exact L0 part.** TBD pending RAM budget review against library
  fixed costs (~17 KB before any user buffers).
- **9704 connection**: USB-C via host converter (if EE finds a
  workable part) vs 16-pin with the GPIO interlock state machine.
- **Protocol command set details.** Shape settled, but exact CMDs,
  EVT types for unsolicited modem events, binary encoding choice,
  error codes, and ACT_ID/TARGET allocation strategy still to
  design.

### Implication for the work already on the branch

Most of what we built still applies, with L0 substituted for L4:

- Serial preset (`serial_stm32.{c,h}`): same HAL API on L0, no change
- `crossplatform.{c,h}`: same
- `rbBegin(UART_HandleTypeDef *)`: same
- TX timeout, MT print fix: same
- Nucleo-L452RE bring-up example: keep as reference; once L0 part is
  selected, port pin assignments and clock setup (Cortex-M0+ vs M4,
  different family HAL header, lower max clock)
- GPIO state machine TODO: **conditional on USB-host outcome**
- All application-layer reliability TODOs: live on either L0 or L4,
  needs deliberate split — most likely SD logging on L4, library-
  level retry/seq on L0
- Companion arbitration TODOs: **fold into L0 ↔ L4 protocol design**

The full retargeting pass on the TODO doc waits until we know the L0
part and have the EE's USB-host findings — both shape the work
materially.

### State at end of session

- STM32 port code: bring-up branch is functional, GPIO state machine
  not yet started.
- Hardware: dev kit verified end-to-end through window placement, with
  marginal reception observed.
- Outstanding: GPIO state machine implementation (conditional),
  production-board GPIO allocation (conditional), 3-message MT
  experiment, eventual park trip for "good RF" baseline.
- Architecture: two-MCU split (L0 modem manager + L4 main avionics)
  locked in. Most other questions still open but with clearer scope.
- link_monitor.py reframed from debugging convenience to QA baseline
  tool — implications for the Forge test fixture flagged in TODO.
- Branch `lux/stm32-l452-port` pushed to Lux-Aerobot fork. Branch
  name now slightly misleading — production target is L0, not L452.
  Worth a rename on next push, or just retain as the "bring-up
  reference" branch and create a fresh `lux/l0-modem-mgr` once the
  L0 part is selected.
