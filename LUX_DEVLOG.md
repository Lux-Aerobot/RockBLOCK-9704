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

### Field test (evening): genuine outdoor capture with multiple confirmed deliveries

Took the dev kit outdoors to `45.4012841, -75.6301899` (Ottawa,
partial sky view). Captured ~50 min of `link_monitor.py` output
with multiple MT deliveries and MO sends. Raw logs archived in
`examples/sample-traces/2026-05-27-*.log`. User-annotated notes in
`2026-05-27-field-test-notes.md` alongside.

This single field test produced three substantive findings that
update our mental model. The Iridium MT delivery model in
particular is now significantly clearer than before.

### Finding 1: MO completions drain the MT queue ⭐

The standout finding. When the modem successfully completes an MO
send, **the Iridium gateway opportunistically pushes any held MTs
to the modem in the same session.** Directly observed multiple
times during the field test.

Not documented anywhere I've found, but architecturally obvious in
hindsight: the modem is already in active comms for the MO, the
gateway has the modem on the line, pushing held MTs is free
piggyback rather than waiting for the next retry timer.

**Architectural implication**: this is a clean *active polling*
mechanism for inbound commands. Application can periodically send
a small "heartbeat" MO to actively pull held MTs, rather than
waiting passively for the gateway to ring. Becomes a tunable
latency-vs-cost knob:

| Heartbeat cadence | MT-pull latency bound | Sat-bytes/day cost     |
|-------------------|-----------------------|------------------------|
| Every 5 min       | ≤ 5 min               | ~288 heartbeats × ~few bytes |
| Every 30 min      | ≤ 30 min              | ~48 heartbeats         |
| Only-when-needed  | Unbounded             | Free (rides on real telemetry) |

Folded into TODO as a new design pattern under the L0 ↔ L4
protocol design — see "Heartbeat MO as MT-puller" item.

The scheduled-telemetry cadence we'd already been thinking about
for power-budget reasons now serves double duty as the MT-pull
mechanism. Cleaner architecture, fewer moving parts.

### Finding 2: Cluster delivery directly confirmed

Two distinct cluster-delivery events in this trace, in the same
session:

- `21:29:15Z` — MT id=8 `"Hello Lux!"` and MT id=9 `"Hello from Lux 2!"`
  arrive in the same second
- `21:45:12Z` — MT id=14 `"swamp the system"` and MT id=15
  `"how about now?"` arrive in the same second

Plus the user's field notes: "4 messages delivered in <1 s on a
retry today" — an even larger cluster outside the captured window.

The "drain on registration / drain on session" model is now
empirically locked. Multiple pending MTs flush together whenever a
viable session opens, whether triggered by registration or by an
MO send.

### Finding 3: Signal climbs *during* a session — interesting selection-vs-mechanism question

Every MT delivery in the trace is preceded by a signal climb,
often from 1–2 bars to 4–5 bars within ~5 seconds before the MT
event. Two plausible explanations the trace alone can't
distinguish:

1. **Selection effect**: the modem only attempts the full delivery
   handshake when a satellite is genuinely accessible. We see
   "successful MTs preceded by good signal" because failed
   attempts at bad signal don't show up as MT events at all. The
   boring statistical explanation.
2. **Active link optimisation**: the modem actively selects the
   best-visible satellite once a session begins (beam-steering,
   frequency hopping, satellite hand-off). The elevated signal is
   a *consequence* of an active session rather than its
   precondition.

If (2), the implication is: signal *during* an attempted send is
the relevant metric, not signal *before*. Worth probing later
with deliberate sends at known-low-signal moments. MT id=18 at
21:48:00 was delivered with 2/5 signal at the moment of receipt
— hints at "marginal signal can still complete delivery once the
session is established."

### Updated mental model: Iridium MT delivery has three drain triggers

Previous (post-yesterday's walkback): gateway retries on some
cadence; the cadence was unclear.

New (after field test): the gateway delivers held MTs in response
to **any** of these events:

1. **Modem registers** with the network (re-acquired event every
   power-up, every emergence from coverage gap)
2. **Modem completes an MO send** (the newly-confirmed trigger)
3. **Fallback retry timer** (cadence still TBD, but no longer the
   primary mechanism)

In good conditions, **end-to-end MT delivery latency observed at
~30–60 s** under this model.

### Walked back: the "~6 min retry cadence" claim was off

User's field-test notes confirm "Successful deliveries in ~30–60 s"
under good signal — much faster than my yesterday "~6 min retry"
claim. The 12:39:25 → 12:45:40 gap I'd seen yesterday was just
reception-window-driven, not retry-cadence-driven. Same single-data-
point inference error I keep making.

The fallback retry timer probably still exists but is not the
binding constraint when reception is OK and the application is
actively sending. Logged as a residual mystery; not worth probing
unless we hit a case where it matters.

### Implications for the L0 ↔ L4 protocol design

- **Heartbeat MO as MT-puller**: new design pattern, captured in
  TODO. The L4 schedules small heartbeat MOs; L0 forwards them as
  IMT messages; gateway responds by draining any held MTs in the
  same session.
- **MT acknowledgement timing question**: the previous concern
  about slow command execution blocking the MT queue is less
  acute given the cluster-drain behaviour — the queue empties
  fast when sessions open. But it still matters if the next
  session is far off (deep-sleep policy or extended coverage
  gaps).
- **Signal-as-MO-trigger**: probably *don't* gate MO sends on
  high signal at this layer. The selection-vs-mechanism question
  on signal climb means a "wait for good signal" rule may
  artificially suppress sends that would have succeeded.

### Trace data archived for reference

Three new files in `examples/sample-traces/`:

- `2026-05-27-field-test-good-rf.log` — ~50 min outdoor capture
  with 12 MT deliveries and 2 MO sends
- `2026-05-27-walk-home-balcony.log` — mobility / hand-off trace
  during the walk back
- `2026-05-27-field-test-notes.md` — user-annotated observations

These now form the "known partial-sky-view reference" against
which future runs (true rooftop, marginal indoor, etc.) can be
compared. Not a *clean* good-RF baseline — that's still
outstanding — but a much better dataset than yesterday's window
placement.

### State at end of session

- STM32 port code: bring-up branch is functional, GPIO state machine
  not yet started.
- Hardware: dev kit verified end-to-end with multiple MT/MO round-
  trips under genuine outdoor partial-sky conditions.
- Outstanding: GPIO state machine implementation (conditional on
  USB-host investigation), production-board GPIO allocation
  (conditional), true unobstructed-sky baseline trace.
- Architecture: two-MCU split (L0 modem manager + L4 main avionics)
  locked in.
- Iridium delivery model: now substantially clearer — three drain
  triggers (registration, MO completion, retry timer), with the
  MO-completion trigger discovered today as the basis for
  active-polling MT-pull pattern.
- link_monitor.py confirmed as the QA baseline tool; first
  reference traces archived.
- Branch `lux/stm32-l452-port` pushed to Lux-Aerobot fork. Branch
  name now slightly misleading — production target is L0, not L452.
  Worth a rename on next push, or just retain as the "bring-up
  reference" branch and create a fresh `lux/l0-modem-mgr` once the
  L0 part is selected.

---

## 2026-05-28 — modem-manager POC begun (state as of ~noon)

### Notion sync

Connected the Notion workspace. The "Iridium 9704 bring-up & testing"
task (Engineering 2026 → Innovation) is the project hub; it links to
the GitHub `LUX_DEVLOG.md` / `LUX_INTEGRATION_TODO.md` as the canonical
detailed docs. Wrote the locked architecture decisions into the
tracker's status block, and resolved two stale questions on the master
"Iridium SATCOM" page using field-test evidence:
- "Is 2 kB really the minimum message size?" → struck. Not a minimum;
  10–61 B payloads delivered fine. The 2 kB is the Arduino-default
  `IMT_PAYLOAD_SIZE` buffer (max), not a floor.
- "L452 may have insufficient RAM to manage the 9704" → struck.
  Superseded by the two-MCU decision (dedicated modem MCU).

Useful context pulled from the master page, now feeding our decisions:
- **Message sizes (current scope)**: telemetry ~150 B, commands /
  responses <50 B, thumbnails deferred. So `IMT_PAYLOAD_SIZE` can be
  tiny — no payload-driven RAM pressure on the L0.
- **Cost/power**: ~$0.5 per transmission; heartbeat-MO cadence is
  therefore cost-bound, not just latency-bound (5 min ≈ $150/day/unit,
  15 min ≈ $50/day/unit). Folded into the heartbeat-MO TODO.

### Implementation plan agreed (5 steps)

Phased build of the modem-manager firmware, each step with a clean
validation gate (tracked as tasks):
1. Button-triggered GPIO startup/shutdown sequence + interlock
   (UART configured, not yet talking). ← **current**
2. 9704 comms + inter-MCU UART link + signal-event messaging (outgoing
   only as proof of life). Laptop via ST-LINK VCP stands in for the
   Core/L4.
3. MO pipeline — raw passthrough (text in over the Core link → straight
   to modem, no translation yet).
4. MT pipeline — 1:1 text passthrough modem → Core link.
5. Wrap everything in the ACTU-style text protocol (TYPE/SEQ/TARGET/
   CMD, variable-length payload). That = POC complete.

### Step 1 written (not yet hardware-validated)

`examples/nucleo_l452re_modem_manager.c` on the branch:
- State machine IDLE → STARTUP → RUNNING → SHUTDOWN → IDLE, plus FAULT.
- Startup: power gate on → settle → I_EN high → wait I_BTD high → host
  TX (PA9) switches from forced-low to USART1 AF. Shutdown mirrors it.
- Damage interlock enforced structurally (button only honoured from a
  stable state) + explicit pre-boot I_BTD check. FAULT escape removes
  power (valid even when the interlock would otherwise block an I_EN
  change), recovers to IDLE on ack. Future Core `RESET` funnels into
  the same path.
- `SIMULATE_IBTD` (default on): MCU drives I_BTD_SIM (PC3), jumpered to
  the real I_BTD input (PC2), modelling the modem's boot/shutdown
  timing — exercises the real GPIO read path with zero modem risk.
- TX force-low / AF-switch helpers validate the "host TX low until
  booted" requirement.

Two corrections caught during/after writing:
- `LOG` macro had the timestamp arg out of order (would've swapped the
  tick with the first real arg) — fixed to `##__VA_ARGS__` form.
- **Power model corrected**: our `PWR_EN` drives a load-switch gate that
  applies/removes the modem's V_IN+ rail entirely — it is **not** the
  9704's pin 6 P_EN (internal cap-charge enable, tie to GND). Gate
  polarity now configurable via `PWR_GATE_ACTIVE_HIGH`. This also
  dissolved the earlier back-power caveat (with a real gate, "off" =
  V_IN+ removed, and the existing input-safe-low sequencing covers
  back-power).

### State as of noon 2026-05-28

- **Architecture**: two-MCU split locked. Dedicated modem MCU
  (class TBD — L0 baseline, may step up for headroom) + L4 Node Core.
- **9704 comms**: Send MO / Receive MT / signal monitoring all proven
  end-to-end via the Python binding + field test. C library port built
  but not yet run on STM32 hardware.
- **Modem-manager firmware**: step 1 of 5 written and pushed, awaiting
  bench bring-up (SIMULATE_IBTD + PC3→PC2 jumper). Steps 2–5 pending,
  deliberately held until step 1 is hardware-validated.
- **Open hardware unknowns**: exact modem MCU part; 9704 connection
  (USB-C-via-host-bridge vs 16-pin GPIO); power-gate polarity.
- **Docs**: devlog + TODO current; Notion tracker synced.
- **Branch**: `lux/stm32-l452-port` @ the step-1 + power-gate-fix
  commits.

### Step 1 hardware-validated (2026-05-28, afternoon)

Flashed to the Nucleo-L452RE and ran on the bench (SIMULATE_IBTD on,
PC2/PC3 + control pins jumpered to a common probe node). All four
checks pass — **step 1 complete, firmware unchanged** (validated build
is `fb28652`, no code edits needed from bring-up):

1. **Normal cycle** — startup/shutdown timestamps match the sim delays
   (~2000 / ~1000 ms). Sequencing + I_BTD read path confirmed.
2. **Interlock holds** — button presses during STARTUP/SHUTDOWN are
   ignored; I_EN never changes mid-sequence.
3. **FAULT path** — I_BTD never going high drives the 30 s timeout →
   FAULT, power gate forced off, frantic LED, recover-on-ack.
4. **TX-low / I_BTD timing** — confirmed once probed correctly.

Two bench-side gotchas worth recording (firmware was correct
throughout — both were instrumentation/pinout mistakes):
- **`-P` vs `RE` pinout**: an early FAULT came from jumpering the
  Arduino A2/A3 silk (PA4/PB0 on the L452RE), not PC2/PC3 — traced to
  using the `NUCLEO_L4xxRx-P` diagram instead of the clean L452RE one.
  Briefly relocated the firmware to the A-header to compensate, then
  reverted once the real cause (wrong diagram) was found; PC0–PC3
  assignments stand.
- **Probe on the wrong pin**: the "I_BTD goes high before I_EN"
  anomaly was the I_BTD LED probed on PC0 (= P_EN), so it was
  displaying P_EN the whole time. The MCU's own I_BTD read (console,
  2007 ms) was correct all along. Re-probing the PC2↔PC3 node showed
  the expected timing.

Lesson reinforced: when an LED/probe disagrees with the console
timestamp, trust the console (MCU truth) and suspect the probe.

Step 2 (USART1 brought up for real, inter-MCU link to the
laptop-as-Core, signal-change events as the first message) is now
unblocked.

### Step 2.1 hardware-validated (2026-05-28, evening)

Deferred USART1 init/deinit wired into the sequence and validated on
the bench. Branch tip `2fcd2cf`.

What 2.1 added:
- `uart1_up()` = `MX_USART1_UART_Init()` at boot (startup step 5), so
  PA9 now idles at a *real* UART high in RUNNING — the thing step 1
  couldn't show (step 1 parked PA9 in AF with the peripheral disabled,
  no defined idle level).
- `uart1_down()` = `HAL_UART_DeInit()` + re-assert TX low at shutdown
  ("cease serial communications").
- **Symmetric settle margins** (all 100 ms, individually tunable):
  ```
  startup:  PWR_ON -[PWR_SETTLE]- I_EN^ -wait I_BTD^- -[UART_UP_DELAY]- USART1 up -> RUNNING
  shutdown: USART1 down -[UART_DOWN_DELAY]- I_EN_ -wait I_BTD_- -[PWR_OFF_DELAY]- power off -> IDLE
  ```
  STARTUP and SHUTDOWN each became three sub-phases (flags
  `g_ien_committed`, `g_ibtd_high_seen`, `g_ibtd_low_seen`).

Bench result: all four 100 ms gaps appear in the console; PA9 goes
low -> UART idle-high (RUNNING) -> low; interlock and FAULT behaviour
unchanged.

CubeMX setup that makes this work (everything-in-main.c layout):
- Configure USART1 (async 230400 8N1).
- Advanced Settings -> Generated Function Calls: tick **Do Not
  Generate Function Call** for `MX_USART1_UART_Init` (so it isn't
  auto-called — we call it at boot), and untick **Visibility (Static)**
  for USART1 (so our prototype/call links cleanly). Leave USART2 as-is.

Gotcha recorded: re-pasting only the *changed middle* of the config
block dropped the `#define SIMULATE_IBTD` line (it sits at the top of
the block, above the delay defines). Builds clean, but the sim that
drives I_BTD is then `#ifdef`'d out -> I_BTD never comes up -> boot
timeout. The reset banner reports `SIMULATE_IBTD: ON/OFF`, which is
the fast tell. Lesson: re-paste whole USER CODE regions start-to-end,
not the visibly-changed middle — a dropped `#define` at a block
boundary compiles fine but changes behaviour.

Next: 2.2 — first actual bytes out USART1 in RUNNING (fixed test
string, provable on scope/loopback), as the stepping stone to the
signal-event message and the inter-MCU link framing.

### Step 2.2 — first bytes out USART1 (2026-05-28, late)

Branch tip `9116a3a`. While RUNNING, the firmware now transmits
`"Hello Lux\r\n"` out USART1 every `TEST_TX_INTERVAL_MS` (2 s) via
blocking `HAL_UART_Transmit`, with a `USART1 TX: "Hello Lux"` console
line each time. Gated to RUNNING (USART1 only up there). No protocol —
purely "does the link move bytes."

Also added a comment documenting why PA10 (USART1 RX) is deliberately
NOT forced low: it connects to the 9704's TXD (an *output*), so it's
read-only on our side; driving it would contend with the modem's
driver. Only PA9 (-> 9704 RXD, an *input*) is forced low pre-boot. The
asymmetry is correct — protect the line you drive, never drive a line
the modem drives.

Bench: bytes confirmed leaving PA9 (read via a USB-UART adapter +
PuTTY). Hit a baud chase first — saw garbage at 230400. Root cause was
a **CubeMX gotcha, not firmware**: the USART1 baud field was typed as
230400 but never committed (clicked away without pressing Enter), so it
reverted to the 115200 default; the device ran 115200 against PuTTY's
230400 → exact 2x mismatch → garbage. Confirmed our deferred init is
innocent: `uart1_up()` -> `MX_USART1_UART_Init()` recomputes BRR from
the live clock at call time, so calling it late doesn't affect baud.
Fix: re-enter 230400, **press Enter**, regenerate, reflash. (No
USER-CODE re-paste needed — baud lives in CubeMX's generated init.)

Lesson filed: CubeMX numeric fields (baud especially) commit on
Enter/Tab only — clicking away silently reverts. Eyeball that values
stick after typing. This is exactly the class of error the Hello-Lux
bench test exists to catch before the real 9704 (hard-locked 230400)
is on the line.

Session end (2026-05-28): step 1 + 2.1 hardware-validated; 2.2 code
complete and bytes confirmed flowing (final clean `Hello Lux` read
pending the 230400 re-commit + reflash). Next session: confirm clean
230400 read, then 2.3 — the signal-change event as the first shaped
outbound message, and the inter-MCU (ACTU-style) framing. All work on
branch `lux/stm32-l452-port` (Lux-Aerobot fork).
