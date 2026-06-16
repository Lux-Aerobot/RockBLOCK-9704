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

### Step 2.2 closed — clean 230400 read confirmed (2026-05-28)

Re-committed the USART1 baud field in CubeMX (typed 230400 *and pressed
Enter* this time), regenerated, reflashed. `Hello Lux\r\n` now reads
clean on the USB-UART adapter at 230400 — no garbage. This confirms the
earlier garbage was purely the uncommitted-baud CubeMX gotcha, not the
deferred `uart1_up()` init (which recomputes BRR from the live clock at
call time, so late init is baud-innocent).

**Step 2.2 complete.** Next: 2.3 — the signal-change event as the first
*shaped* outbound message + the inter-MCU (ACTU-style) framing.

---

## 2026-05-29 — Day 4: 16-pin connection locked; prepping first real-modem power-up

### Decision: 16-pin connector + GPIO interlock (USB-host bridge rejected)

The long-open "USB-C via a USB-host bridge vs the 16-pin connector"
question is **resolved in favour of the 16-pin connector** with the
software GPIO startup/shutdown interlock, driven by the dedicated
manager MCU. The USB-host-bridge approach is dropped. Consequences:

- The GPIO interlock state machine is now **unconditionally** the
  production path — it was previously hedged as "conditional on the EE's
  USB-host findings." The step-1 POC sequencing *is* the real path, not a
  fallback. Good news: that code is already written and bench-validated
  (against `SIMULATE_IBTD`).
- No USB-host bridge part to source; no USB-host stack on the manager MCU.
- USB-host capability drops off the L0 part-selection criteria — widens
  the candidate list.

### Wiring note caught before first power-up: GND / V_IN- pins

From the GroundControl pin descriptions: **pins 1, 4, 10, 16 are GND /
V_IN-** — signal grounds that must *all* be connected to the host circuit
*unless* the module is used in USB-only mode. We've just rejected
USB-only, so **all four must be tied to host ground.** These are also the
power-return path (V_IN-), so for the production PCB they want
low-impedance ground (the modem's TX current bursts return through here).
Easy to under-connect on a first bench hookup if you assume one ground
pin is enough.

### Plan for today: power-only bring-up before any serial

Validate the real 9704 powers up, asserts I_BTD ("booted"), and shuts
down cleanly through our interlock — *before* touching serial protocols.
Moves us off the `SIMULATE_IBTD` harness onto the real module's I_BTD
timing. (Results to follow in a later entry today.)

### First real-modem power-up — plan & checklist (the "step 2.x → real HW" gate)

First time the real, expensive 9704 sees *our* interlock. Deliberately
power/enable-only — no UART data lines connected — so any problem reads as
"didn't boot / didn't shut down," not tangled with protocol bugs. This is
the gate between the `SIMULATE_IBTD` bench work and live-modem serial work.

**Wiring map (logical → 9704 pin → MCU pin).** Full table on the Notion
page. Confirmation status matters:
- ✓ confirmed from RB9704 pin description: GND/V_IN- (pins 1/4/10/16, all
  to common host ground in non-USB mode); P_EN (pin 6) **left OPEN** (no
  tie, no pull — supersedes our earlier "tie to GND" assumption).
- ⚠ from our working firmware map, to be confirmed against the connector
  pinout before wiring: V_IN+ (15), I_EN (3), I_BTD (7), RXD (14),
  TXD (13). [Liam cross-checking the pin numbers and dropping the real
  MCU pin numbers into the map.]

**Logic levels — confirmed compatible, no shifting** (RB9704 Signal
Thresholds): modem inputs accept 3.3 V drive (logic-in HIGH 2.0–3.6 V,
LOW ≤0.4 V); modem outputs swing 2.9–3.4 V HIGH / 0–0.4 V LOW, read
directly by the 3.3 V L452. Modem outputs are **weak (≤2 mA)** — must not
be contended (drives the PC3 warning below; also why the PC2 pull-down,
~tens of µA, won't fight a real I_BTD high).

**Power.** DC-power path: V set within 4.0–5.3 V, **bench current limit
400 mA** (≤500 mA DC-path max). Watch for false CC-trip on cap-charge
inrush — if the modem won't boot at 400 mA, nudge toward 500 before
suspecting wiring. (Separate battery path 3.6–4.5 V / ≤1100 mA exists for
later TX-burst headroom — not needed today, no TX.)

**Antenna ON for all tests** (operator habit; protects the PA from driving
into an open if the modem ever keys up unexpectedly). Boot/shutdown needs
no sky view — I_BTD is just "booted," no RF.

**SIMULATE_IBTD with a real modem.** Detection/timing already read the
I_BTD *input* pin (PC2); the sim only drives PC3. So the define can be
left ON (keeps the live boot/shutdown reference numbers in the banner) OR
off. **Either way the PC3→PC2 jumper MUST be removed** — PC3 stays an
actively-driven push-pull output and would contend with the modem's ≤2 mA
I_BTD driver. Lean: flip it OFF for real runs so PC3 is never even
configured as an output (the contention foot-gun disappears entirely).

**Phased test:**

1. **USB boot-time probe (no firmware sequencing).** Power the modem over
   its USB-C port; scope/meter I_BTD (at PC2 / the I_BTD pin) and record
   the real boot time to I_BTD-high. Validates the I_BTD→PC2 wiring and
   gives the real boot figure *before* our sequence is in the loop. Also
   answers a key question: **is I_BTD asserted on power alone, or only
   after I_EN?** (USB sequences enable in hardware, so this tells us
   whether boot is power-gated or enable-gated.)
2. **Our startup sequence.** Bench supply on V_IN+ via the load switch,
   button-triggered: PWR_EN on → [PWR_SETTLE] → I_EN high → wait real
   I_BTD high → [UART_UP_DELAY] → (USART1 up, harmless w/ data lines off)
   → RUNNING. Record real I_BTD-high latency from the console
   ("I_BTD HIGH after %lu ms").
3. **Our shutdown sequence.** Button: → I_EN low → wait real I_BTD low →
   [PWR_OFF_DELAY] → power off → IDLE. Record real I_BTD-low latency.
4. **Interlock spot-check.** Confirm button presses mid-STARTUP/SHUTDOWN
   are ignored (I_EN never moves mid-sequence) — same as the sim runs.

**Success criteria (today's DoD):**
- Real modem asserts I_BTD high within `IBTD_BOOT_TIMEOUT_MS` (30 s) →
  reaches RUNNING.
- Clean shutdown: I_BTD low → power off → IDLE.
- No spurious FAULT on a clean cycle; interlock holds.
- Real boot/shutdown times recorded (compare against the sim's modelled
  2000 / 1000 ms — and **retune `IBTD_*_TIMEOUT_MS` / settle margins** to
  the real figures afterward).
- "Ready" today = I_BTD high (hardware booted). JSPR-level ready (modem
  answering over UART) is the serial step — out of scope.

**Watch-items / known failure modes:**
- **`FAULT: I_BTD already HIGH before boot requested`** (sm_step pre-boot
  check). Fires if the real modem asserts I_BTD on power *before* we drive
  I_EN high (i.e. boot is power-gated and faster than `PWR_SETTLE_MS`).
  Real boot looks like seconds, so at a 100 ms settle we're likely still
  low — but if this trips, it's the cause, **not** a wiring fault. The USB
  probe (step 1) tells us in advance which gating applies. If power-gated
  & fast, options: lengthen `PWR_SETTLE_MS` past boot, or relax the
  pre-boot check for the real-modem case.
- **CC-trip on inrush** (see Power) — bump limit toward 500 mA.
- **PC3 contention** if the sim jumper is left in with a real modem — see
  above; remove the jumper.
- Trust the console/MCU timestamps over any single LED/probe (standing
  lesson — several past "bugs" were probe-on-wrong-pin).

### Power-gate topology: IRLZ44N dead-end → load-switch IC; manual V_IN+ for the bench

Going to wire the PWR_EN load-switch gate with an **IRLZ44N** (on hand).
Worked through the topology before energizing — it's the wrong device for
this job, and the reasoning is worth recording.

**The 9704's ground pins (1/4/10/16) are combined signal-ground *and*
V_IN-.** That single fact decides the switch topology:

- **Low-side** (the natural use of an N-channel FET — switch in the ground
  return): here it would switch the modem's GND/V_IN- — i.e. the
  *signal-ground reference* the MCU shares with the modem for I_EN, I_BTD,
  and UART. In "off," the modem's V_IN+ is still tied to the supply and its
  ground floats up; our I_EN/TX, driven to 0 V system-ground, then sit
  *below* the modem's floating ground and forward-bias the pin-to-ground
  ESD diodes → current injection / back-power, recurring every power-off.
  **Rejected** — you can't switch a ground that's also the signal
  reference.
- **High-side** (switch V_IN+, leave GND/V_IN- permanently common — the
  correct topology for this module): but a high-side **N**-FET needs its
  gate driven *above the rail*. V_GS = V_gate − V_source; when it conducts
  the source follows up to ≈V_rail (4–5 V), so a 3.3 V (even a 5 V) gate
  gives V_GS ≤ 0 → device off. Needs a charge pump / bootstrap. Not worth
  it.

**The logic-level threshold does NOT rescue high-side** — the trap I
nearly fell into. IRLZ44N V_GS(th) ≈ 2 V *looks* like "3.3 V turns it on,"
but that spec is measured at **V_DS = V_GS** (onset of conduction, 250 µA)
— it's the threshold, not "fully enhanced," and more importantly it says
nothing about the high-side geometry where the source rises to the rail.
Logic-level only helps **low-side** (source at ground, V_GS = full gate
swing). *Lesson: "logic-level" ≠ "high-side-capable." An N-FET is a
low-side switch; high-side wants a P-FET (+ NPN level-shift from the GPIO)
or a load-switch IC. The `V_DS = V_GS` condition on the threshold spec is
the tell.*

**Decisions:**
- **Production power switch = load-switch IC** (TPS22918 / AP22xxx class):
  built-in high-side gate drive, controlled slew to tame the cap-charge
  inrush, often a fault flag, single 3.3 V enable from PWR_EN/PC0. Enable
  is active-high, so `PWR_GATE_ACTIVE_HIGH=1` still fits. (Discrete P-FET +
  NPN is the parts-bin fallback.)
- **Bench test today = no switch at all.** Control V_IN+ manually via the
  bench-supply output-enable; PWR_EN/PC0 drives nothing this run.

**Why manual power is safe (Liam's catch):** the GC sequence brackets the
whole run — *apply power is the FIRST startup step, remove power is the
LAST shutdown step.* So as long as we (1) power the Nucleo first, letting
`modem_pins_init` drive I_EN/TX safe-low *before* any modem power, then
enable V_IN+, and (2) cut V_IN+ only after the shutdown sequence has driven
I_EN low, the ordering the modem cares about is satisfied. The
power↔I_EN/I_BTD timing isn't tight on the modem's side — all of our settle
margins live *after* power-on — so a manual supply toggle is fine. The
worry about "manual timing" was unfounded once the sequence ordering is
remembered.

This also absorbs the dropped USB boot-probe: the boot-gating question
(power-gated vs I_EN-gated) falls straight out of the manual run — button
proceeds to RUNNING ⇒ I_EN-gated; `FAULT: I_BTD already HIGH before boot
requested` ⇒ power-gated. No scope, no second power source.

### JC design sync — power architecture, two-board split, hardware interlock, DMA confirmed

Sync with JC (board/EE owner). The DMA + optionality decisions from
earlier today were confirmed; several power/board-architecture points were
settled and two new hardware ideas came up. RB9704-relevant outcomes:

**DMA (confirmed, sharpened):**
- **Modem link (manager side): RX = DMA** — the no-drop-bytes play,
  justified by the ≤300 ms segment deadline + un-backpressurable modem.
  **TX = interrupt, not blocking** — we choose when to send, so a transmit
  interrupt is enough; DMA-TX only if a channel is genuinely spare. (Note:
  the current bring-up firmware has *blocking* TX + per-byte IT RX — these
  become IT-TX and DMA-RX when we build out the manager / pick the part;
  the M4 bring-up build can stay as-is meanwhile.)
- **Core↔manager link: DMA-capable on the Core (L4) side** to preserve the
  move-to-Core option, but **interrupt is fine near-term** — inter-MCU
  frames are tiny (~50 B, one frame per full message), an IT drain keeps up
  easily.
- DMA **channels** are the real constraint, not pins: classic L0 ≈ 7,
  L452 = 14. Both still building DMA fluency; agreed to mark the lines
  DMA-capable and "get there as we understand it." → folds into L0 part
  selection (favour channel headroom).

**Power architecture (settled):**
- **No modem without the manager** — there is no scenario where the modem
  is powered but the manager isn't. So a single 5 V bus brings up the
  manager and the modem rail *together*; the **manager** then gates the
  modem's actual power via the power-enable (the load-switch IC specced
  earlier today) and owns *all* the modem's enable pins + the JSPR link.
  "Manager handles the modem entirely."
- **Resolves the open TODO question** (*where does PWR_EN live?*) → **on the
  manager.** The load-switch IC is the manager-controlled gate on the
  modem's V_IN+ off the always-on bus.
- **Manager goes on its own board this run** (JC's call): cleaner power
  sequencing, fault isolation, and — critically — revisability (swap modem
  / move to a 9603 / reflash the manager without touching the Core). A
  power daughterboard handles the switching (also hedges BQ25750 charger
  stock risk).
- Modem-power-cut capability stays (manager *can* disable modem power,
  e.g. future sleep), but the always-on-this-rev policy is unchanged.

**Two boards this run; consolidation = future HW rev (Liam clarification).**
Moving modem ownership onto the Core later is a *hardware revision*, not a
firmware toggle — first run is explicitly two boards (manager board + core
board). The option is preserved (affordable, viable), decided later on
power/isolation-vs-BOM; the ACTU-style message API stays the boundary that
keeps the choice deferrable.

**NEW — hardware interlock (JC, EE-side; direction agreed):** beyond our
software interlock, add a *hardware* lockout so a firmware bug can't drive
the modem before it's ready — mirroring GroundControl's USB reference
design, which holds the data lines off until the modem is good. Approach:
a buffer / bus-switch on the lines we drive into the modem, enabled by the
modem's ready signal. Defense-in-depth on the damage-class concern; details
TBD by JC.
- *Design note:* I_BTD (booted) is the natural enable for gating the
  **UART data lines** (matches our software "UART only after I_BTD high"
  step). But **I_EN cannot be gated by I_BTD** — I_EN must be driven
  *before* boot, so I_BTD doesn't exist yet (circular dependency). **Open
  question:** is there a distinct *power-good* output on the 16-pin
  connector, or is I_BTD the signal to use? JC noted the schematic names
  things differently — confirm which pin feeds the buffer enable before
  wiring it.

**NEW — convenience GPIO/IRQ line between Core and manager:** a dedicated
signal pin (+ interrupt) separate from the UART. No committed use yet —
gives options (manager→Core attention/IRQ, power-good relay, etc.). Cheap
to add now, decide use later.

**Minor — modem thermal:** not a current concern; we don't monitor 9704
die temp. GroundControl rolled a revision to fix low-temp (−20 °C)
failures; our unit is post-revision.

*(The sync also covered non-RB9704 board work — IMU 6- vs 9-axis + an
external camera-mounted IMU, magnetometer sourcing, Qwiic/SparkFun
quick-connects, solar-board consolidation, and milestone-chain schedule
risk. Out of scope for these docs; parked for Liam to route.)*

### ⭐ First real-modem power-up — CLEAN; boot is I_EN-gated; real timing captured (2026-05-29)

First time the real 9704 ran through our interlock (manual bench-supply
V_IN+, antenna on, power/enable-only — the modem UART is not yet in the
loop; the RUNNING "Hello Lux" transmits out PA9 regardless of whether it's
wired to the modem). **Two full startup/shutdown cycles, both clean, the
sequence exactly as designed.**

**Boot is I_EN-gated, not power-gated.** With the modem continuously
powered (manual supply, no enable button), I_BTD stayed LOW until I_EN was
driven high — the pre-boot check passed, no FAULT. Confirms the firmware's
core assumption, and is why the missing supply-enable button was a
non-issue: power can sit on continuously; I_EN is the gate.

**Real timing (both cycles, room temp, n=2):**
- Boot (I_EN high → I_BTD high): **930 ms**, identical both cycles —
  deterministic hardware boot.
- Shutdown (I_EN low → I_BTD low): **16–19 ms** — near-instant.
- vs the sim's modelled 2000 / 1000 ms: sim boot was conservative; real
  shutdown is ~60× faster than modelled. The SIMULATE_IBTD comparison
  (define left ON, jumper OUT — sim drove PC3 into open air while PC2 read
  the real modem) worked exactly as intended: banner showed 2000/1000 while
  the live I_BTD read 930/17.

Full sequence confirmed in order: power → [100 ms] → I_EN↑ → [930 ms boot]
→ [100 ms] → USART1 up → RUNNING → … → USART1 down → [100 ms] → I_EN↓ →
[17 ms] → [100 ms] → power off → IDLE. All four 100 ms settle margins
present and correct.

**Retuned to the real figures:**
- `IBTD_BOOT_TIMEOUT_MS` 30 s → **10 s** (10× over real 930 ms; deliberately
  generous — n=2 at room temp, and this modem family has cold-temperature
  history, so leave headroom for a slower cold boot; re-confirm cold before
  tightening).
- `IBTD_SHUTDOWN_TIMEOUT_MS` 30 s → **5 s** (~300× over real ~17 ms).
- Settle margins (100 ms each) left as-is — our choice, not modem-dictated,
  and the 930 ms boot dominates the cycle anyway.

**`SIMULATE_IBTD` kept ON (Liam's call), jumper OUT.** Left defined to
retain the modelled reference times (2000/1000 ms) in the banner for
side-by-side comparison with the live modem; the PC3→PC2 jumper stays
removed so the push-pull PC3 sim output can't contend with the modem's
weak I_BTD driver. Caveat: the runtime banner still prints the "jumper
PC3 -> PC2" hint, which is misleading with a real modem on the line —
flagged to soften.

**Open for the comms step:** I_BTD-high = *hardware* booted, NOT
*JSPR-ready*. The 100 ms `UART_UP_DELAY` before we talk is still an
unvalidated guess — verify/revisit when we first exchange JSPR; the modem
may need more settle before it answers.

**Task #10 (first real-modem power-up) complete.** Next: real JSPR comms
over USART1 to the modem — the library's first contact on STM32 hardware —
then 2.3 (signal-event as the first shaped outbound message + ACTU framing).

---

## 2026-05-30 — Day 5 (Sat): S1 — library integrated; first-contact blocker diagnosed

Weekend plan: 4 × ~2 h sessions. S1 goal = manager talking to the RB module,
signal status + MT echo on the console, DMA RX. Outcome: library integrated
and a lot surfaced — first contact not yet achieved, but we now know exactly
why, and it isn't the link.

### Setup

- New CubeIDE project `lux-modem-manager-bringup` put under git as its own
  Lux-Aerobot repo (matches the per-node convention). RB9704 library added as a
  **git submodule** of the fork (`Lib/RockBLOCK-9704` @ `lux/stm32-l452-port`),
  so library edits stay in the fork (with this devlog) and are reusable for the
  L0/core later.
- Library wired into the CubeIDE build (source folder + include paths +
  `STM32_HAL`; non-STM32 presets / arduino .cpp / wingetopt / ekermit excluded).
  In RUNNING: `rbBegin(&huart1)`, `rbPoll()`, constellationState→signal log,
  mtMessageComplete→MT echo. IT RX path first (DMA RX to follow).

### RAM gotcha (fixed): non-Arduino IMT_PAYLOAD_SIZE default is 100 kB

First build overflowed RAM by ~88 kB. The library's non-Arduino default is
`IMT_PAYLOAD_SIZE = 100000 + CRC`, and the MO+MT queues are
`[IMT_QUEUE_SIZE][IMT_PAYLOAD_SIZE]` → ~200 kB on a 160 kB part. Added an
`#elif defined(STM32_HAL)` branch in `imt_queue.h` defaulting to 2048+CRC
(fork commit `40363fd`), `#ifndef` guard preserved for per-project override.
Confirms the override is **mandatory** on any MCU — feeds L0 part selection.

### Hang found → independent watchdog added (validated)

A blocking library call hung the supervisor: `rbBegin` → `clearLeftoverData`'s
`while(peek>0)` spun forever on continuous RX bytes, so the button / interlock /
FAULT couldn't run — the modem couldn't be safed. A state-machine timeout can't
recover a *blocked callee*; an IWDG can. Added IWDG (~3 s, refreshed each loop);
on a hang → reset → `modem_pins_init` drives I_EN low + power off → IDLE; boot
logs `RCC_FLAG_IWDGRST`. Hardware-validated: the hang now auto-recovers safely
instead of wedging. (commit `1aeec22`)

### FAULT-recovery bug found → fixed (validated)

On a clean `rbBegin`-fail FAULT, the button couldn't recover. `enter_fault`'s
safe action is `PWR_OFF` — fine in production (load switch cuts power → I_BTD
drops) but a **no-op on the bench's manual supply**, so I_BTD stayed high and
the FAULT-ack (gated on `!IBTD_HIGH`) deadlocked. Fix: in `ST_FAULT`, drive I_EN
low when I_BTD is **high** (interlock-allowed, since I_BTD went high after I_EN
high) — shuts the modem down → I_BTD drops → recovery proceeds. Works on bench +
production, and strictly safer than the old `if(!IBTD_HIGH()) IEN_LOW()` (which
drove I_EN low in the unsafe never-booted direction). Validated:
button-out-of-FAULT now works. (commit `9d44b9c`)

### ⭐ Root cause of "no first contact": I_BTD-high ≠ JSPR-ready

A temporary raw-RX probe (dump 32 bytes before `rbBegin`) cracked it. Attempt 1
caught clean **`299 bootInfo {"image_type":"prod..."}`** — valid JSPR at 230400.
So **RX / TX / baud / wiring are all proven good; the link was never the
problem.** (Bonus: confirms the modem is running production firmware.)

The real issue: after I_BTD, the modem emits an unsolicited `299 bootInfo`
burst, and during early boot its TXD sits low (reads as continuous `0x00`
framing errors — the `'2'/'g'` + `00 00 …` floods in probe attempts 2 & 3). We
call `rbBegin` ~200 ms after I_BTD, **mid-boot**, producing the two failures
seen across runs:
- **Hang**: `clearLeftoverData` never empties while boot bytes keep arriving.
- **Clean FAULT**: boot bytes done, but the modem isn't command-ready yet, so
  `setApi` gets no `apiVersion` reply within its tight **2 × 5 ms non-blocking**
  window → `rbBegin` returns false.

The hang-vs-fail intermittency was just *where in the boot sequence* we sampled.
Confirms the "I_BTD ≠ JSPR-ready" flag from the power-up entry.

### Proposed fix (converge next session)

Firmware (no library change): after USART1 up, **drain RX until quiet for
~500 ms** (modem done booting) before `rbBegin` — kills the `clearLeftoverData`
hang and guarantees the modem is past its boot chatter; the drain logging also
reveals the real boot-chatter duration. Then `rbBegin` (with a couple retries).

Open question / limit of "firmware handles it": even with a ready modem,
`setApi`'s 2 × 5 ms non-blocking window may be too tight for the reply latency
(and each `rbBegin` retry resets the RX ring, so retries are a weak hedge). If
`rbBegin` still clean-fails after the quiet-wait, the clean fix is a small
**library** change — make `setApi` wait for the reply with a real timeout
(`waitForJsprMessage`) instead of 2 impatient checks. Deferred per "don't touch
the library yet," but flagged as likely-necessary and a good upstream patch.

### Status at end of S1 (time-boxed)

- **Validated wins:** library integrated + builds; RAM fixed; **watchdog**
  safety net; **FAULT recovery** on the bench; **link proven good** (clean
  JSPR RX). All the safety/robustness scaffolding now exists.
- **Not done:** first contact (`rbBegin` success), signal/MT echo, DMA RX. S1
  carries over — but the blocker is understood and a fix is staged.
- Committed/pushed: `lux-modem-manager-bringup` @ `9d44b9c`; fork @ `40363fd`.
- Modem powered down safely at session end.

### S2 (part 1, same day): patient-handshake fix — decisive negative

Applied the firmware RX-quiet wait (drain boot chatter before `rbBegin`) +
the library patience fix (`setApi`/`setSim`/`setState` now use
`waitForJsprMessage` instead of a single non-blocking `receiveJspr`; fork
`a2f1197`). Result: **`rbBegin` still gets no reply** — and because it now
waits patiently (~1 s/step) instead of failing fast, it blocks past the 3 s
IWDG, so all three attempts end in a watchdog reset (expected side-effect of
patience, not a regression). So `setApi`'s tight window was **not** the (sole)
blocker.

State now firmly established:
- Modem healthy (Python over USB ✓); RX works (we receive `299 bootInfo` over
  the 16-pin UART ✓); TX wire continuous to the modem's **RXD / pin 14** ✓;
  **USB always unplugged** on the bench (so not a USB-vs-16-pin mode issue).
- The quiet-wait drains ~168 bytes of boot chatter (~870 ms) then the line is
  quiet; we send `GET apiVersion`; nothing comes back.
- So: **the modem transmits to us over the 16-pin but does not answer our
  commands.**

Remaining candidates: (a) our TX bytes aren't landing correctly at the modem
despite continuity (framing / level / wrong-pin-despite-continuity); (b) our
RX-handling path (IT ring / library) is missing the reply; (c) Liam's thought
— the modem emits a boot frame it expects a response to and we miss it in the
UART-up gap, leaving it not command-ready. (c) is less likely (the Python
binding runs the same `setApi` handshake with no special boot handling) but
the probe below informs it.

The patience fix is **kept** regardless (real bug; fork `a2f1197`).

Next (staged in the manager firmware): a decisive **manual `GET apiVersion`
+ raw blocking RX dump** — send `"GET apiVersion {}\r"` out PA9, then
`HAL_UART_Receive` a chunk (~1.5 s timeout) and hex/char-dump it, bypassing
the library, the IT ring, and `setApi`. Bytes back (`200 apiVersion {...}`) =>
modem replies and our RX path missed it; silence => modem isn't getting/
answering our TX; an unexpected frame => modem in a waiting/odd state. Settles
(a)/(b)/(c) in one run. Modem-manager build for this run stays uncommitted.

### S2 (part 2): manual GET probe — the modem DOES answer; picture refined

Ran the manual `GET apiVersion {}\r` + raw blocking RX dump. Results:
- Fresh start, run A: **`200 apiVersion {"supported_versions":[{"major":1,
  "minor":7,...}]}`** — clean reply (modem API v1.7).
- Fresh start, run B: **`405 MALFORMED {}.`** — modem received and *parsed* our
  command but found it malformed.
- After FAULT → I_EN re-enable (manager still running, no reset/power-cycle):
  **`0 bytes`** (deaf).

Three findings:

1. **TX-to-modem works.** The modem replies to our commands (200 clean, or 405
   when garbled). The full path — TX, RX, baud, wiring, library — is good;
   first contact is essentially proven.

2. **Intermittent TX corruption.** The *same* `GET apiVersion {}\r` gives 200
   (clean) one run and 405 MALFORMED another → bytes occasionally garbled in
   transit, or residual bytes in the modem's RX concatenating with our command.
   A signal-integrity / framing / residual-byte issue to chase (matters for
   reliable MO sends). NOT fatal for bring-up: `setApi`'s `waitForJsprMessage`
   ignores a 405 (waits for a 200 apiVersion) and `rbBegin` retries, so a clean
   GET should get through.

3. **The "deafness" is firmware, not the modem — the earlier "needs power
   cycle" theory was WRONG (corrected by Liam's manager-reset test).** A manager
   *reset* (no modem power cycle) gets a reply; only the FAULT → I_EN-recycle
   path (MCU keeps running) goes deaf. A reset/power-cycle gives a clean USART1
   re-init (gState RESET → HAL_UART_Init runs MspInit); `enter_fault` only
   forces PA9 low and never `HAL_UART_DeInit`s USART1, so the next `uart1_up`
   re-init is stale → deaf. **Fix (next session): `enter_fault` should
   `uart1_down` (DeInit USART1)** like the graceful shutdown, so every re-enable
   gets a fresh UART. (Stale "fresh power-up" comment in main.c to fix too.)

Net: comms path proven (modem answers). Remaining for S1: (1) clean UART
re-init on FAULT, (2) chase the intermittent 405 / TX integrity, (3) DMA RX
swap, (4) MT echo (needs sky view). Restored `rbBegin` (patient + retry) should
reach RUNNING on a fresh start by retrying past an occasional 405 — test
pending.

### S2 (part 3): the patient handshake outruns the 3 s watchdog ⭐ (Liam's catch)

Ran the restored `rbBegin` on a fresh flash: boot-settle OK, then watchdog
reset — with **no `rbBegin` output at all** (not even a retry log). Liam
diagnosed it on sight: the now-patient handshake blocks longer than the 3 s
IWDG.

Math — a single `rbBegin`, when the modem doesn't reply cleanly (405 / silent),
blocks ~4 s: `setApi` (2-iteration loop × `waitForJsprMessage` 1 s ≈ 2 s) +
`setSim` (1 s) + `setState` (1 s). The IWDG can't be petted inside a library
call, so a single `rbBegin` exceeds the 3 s window → reset mid-handshake,
before the firmware's retry loop can even log attempt 2. (The manual GET probe
survived because it was one ~1.5 s op under 3 s.) So we never see `rbBegin`
finish — success or clean-fail — it gets guillotined every time.

**Fix (top of next session): lengthen the IWDG to ~8 s** — CubeMX: prescaler
64, reload 1500 → 4000 (500 Hz → 8 s), regenerate. The firmware already pets
the IWDG before each `rbBegin` attempt, so the longest un-petted span is ONE
`rbBegin` (~4 s); 8 s gives 2× margin. Safety property holds: a true wedge
still self-recovers (8 s vs 3 s), and a stuck-but-powered modem is idle, not a
damage state. Then `rbBegin` runs its full patient+retry and we see the real
outcome. (Longer-term, the clean answer is non-blocking comms / supervisor on
a timer — the production direction — not an ever-longer watchdog.)

**Revised resume order:** (1) IWDG → ~8 s; (2) `enter_fault` → `uart1_down`
(fixes FAULT-recycle deafness so we can iterate without reflashing); (3) chase
intermittent 405 / TX integrity (matters for MO); (4) fix stale "fresh
power-up" comment in main.c; (5) DMA RX swap; (6) MT echo (sky view). The comms
path itself is proven (modem answered the manual GET).

### S2 (part 4): applying both fixes (end of session)

Liam applied **`uart1_down()` in ST_FAULT** — when I_BTD is high: DeInit USART1
+ force PA9 low, then drive I_EN low. Proper cease-comms-then-disable order, and
the `HAL_UART_DeInit` gives the next `uart1_up()` a clean re-init (gState→RESET →
MspInit runs), fixing the FAULT-recycle deafness so we can iterate without
reflashing. Confirmed correct (harmless that the I_BTD-high block re-runs each
~2 ms loop during the ~17 ms shutdown window — idempotent; optional tidy: guard
or move to `enter_fault`). Plus the **IWDG 1500→4000 (~8 s)** CubeMX step.
Testing this combo before session end — expecting `rbBegin` to finally run to
completion: `rbBegin OK` (first contact) if the modem answers cleanly, or a
clean retryable FAULT if it 405s/goes silent. Manager-side build still
uncommitted (validate first); fork library fixes are committed (`40363fd`,
`a2f1197`).

### S2 (part 5): 8 s IWDG STILL trips — the watchdog is catching a slow handshake, not a hang ⭐

Ran the combo (IWDG ~8 s + `uart1_down` in FAULT). Console: clean
power→I_EN→I_BTD@930 ms→USART1 up→boot-settle (drained 168 B, RX quiet @871 ms)
→ **then watchdog reset during `rbBegin`**, then a clean force-safe to IDLE on
the reset. So the recovery path works now, but `rbBegin` still doesn't finish.

**Re-derived the exact `rbBegin` time budget from source (not estimate):**
`waitForJsprMessage` (jspr.c:146) loops `receiveJspr` + `delay(10)` until the
expected `target`+`code` arrives **or** `(millis()-start) > timeoutSeconds*1000`
≈ **1.00 s**. Every step that doesn't get its expected reply burns its *full*
~1 s. On the path where the GETs succeed (proven — first contact) but the
PUT/unsolicited confirmations don't:
- `setApi` — GET ok, PUT `apiVersion` + wait, ×2-iteration loop → **~1–2 s**
- `setSim` — GET ok, PUT `simConfig` + wait, then wait for **unsolicited**
  `simStatus` → **~2 s**
- `setState` — GET ok, PUT `operationalState` + wait, possibly INACTIVE→ACTIVE
  (two more puts+waits) → **~3–4 s**

Sum ≈ **7–9 s of pure timeout — right at the 8 s IWDG boundary.** Add **one**
`405 MALFORMED` (never matches `JSPR_RC_NO_ERROR`, so it burns the whole 1 s)
and it sails past 8 s → reset. This exactly matches the console.

**Two things this clarifies:**
1. **Bumping the watchdog is a band-aid** — it masks that *confirmation* replies
   aren't arriving. The first-contact manual test only exercised a **GET**; the
   handshake also does **PUTs** + waits for **unsolicited** frames
   (`simStatus`, op-state transitions) — exactly the parts exposed to the
   intermittent `405`/TX-integrity issue.
2. The watchdog is doing its job; it's just guillotining a slow-but-progressing
   handshake, not a true wedge.

**Decisive next move — turn on the library's own trace (one run answers it).**
`jspr.c` already has `#ifdef DEBUG` → `printf("SENT: …")` (l.27–31) and
`printf("RECEIVED: …")` (l.76–78) for *every* JSPR frame. Build the library
with `DEBUG` defined and we see exactly which step stalls and whether it's a
`405`. **Gotcha (verified):** the manager's `consolePrintf` bypasses `printf`
(`vsnprintf`→`HAL_UART_Transmit(&huart2)`), and `_write`/`__io_putchar` are
still the **weak ST stubs** in `syscalls.c` — so library `printf` currently goes
*nowhere*. Add the retarget first:
```c
int __io_putchar(int ch){ HAL_UART_Transmit(&huart2,(uint8_t*)&ch,1,10); return ch; }
```
then `-DDEBUG` the library → full SENT/RECEIVED trace on the console.

**Robustness fix (so a legit-slow handshake survives):** feed the IWDG from
inside the **`delay()` shim** (crossplatform layer) — it's called by
`waitForJsprMessage`'s `delay(10)` and `setApi`'s `delay(5)`, so the dog stays
fed through legitimate waiting. Critically, `clearLeftoverData` is a *tight*
`serialPeek/serialRead` spin with **no `delay()`**, so the original hang we
guarded against is **still** caught. Surgical: keeps the safety net, removes the
false trip. (Do NOT pet inside `serialRead` — that would defeat the
`clearLeftoverData` hang-catch.)

**Likely outcome once TX is clean:** every command gets its `200/299` in tens of
ms, `waitForJsprMessage` returns immediately, whole `rbBegin` finishes in <1 s —
the 8 s watchdog stops mattering. So priority order: **(1) `__io_putchar`
retarget + `-DDEBUG` trace** (see the 405s); **(2) fix TX integrity** (root
cause the trace reveals); **(3) IWDG-in-`delay()`** hardening. No code touched
this session (docs only); diagnosis logged for a clean resume.

### S2 (part 6): it was NEVER a watchdog timeout — it's a HardFault on the first DEBUG `printf` ⭐⭐ (corrects part 5)

Going to wire up the `-DDEBUG` trace, I checked two things and they collided
into the actual root cause — **verified at the link/disassembly level, not
guessed:**

1. **`DEBUG` is already defined project-wide.** The CubeIDE *Debug* build config
   lists `DEBUG` in the C-compiler symbols (`.cproject`: `DEBUG`, `USE_HAL_DRIVER`,
   `STM32L452xx`, `STM32_HAL`). So jspr.c's `#ifdef DEBUG` →
   `printf("SENT: …")` / `printf("RECEIVED: …")` (jspr.c:27, 76) were **compiled
   in and live the whole time.**
2. **`__io_putchar` has no implementation.** Project-wide it appears exactly
   once — the weak `extern` stub in `syscalls.c:35`. No strong definition →
   the symbol resolves to address `0x0`. (Confirmed `consolePrintf` never used
   `printf`; it does `vsnprintf` + `HAL_UART_Transmit(&huart2)` directly, so the
   broken `printf` path was simply never exercised — until a library `printf` fired.)

**The mechanism (the "watchdog reset" was a HardFault all along):**
`rbBegin` → `setApi` → `jsprGetApiVersion` → `sendJspr`, and `sendJspr` does
`context.serialWrite(cmd)` to the modem **first**, *then* `printf("SENT: …")`.
So the command reaches the modem (which is why first contact "worked" — the GET
goes out and the modem even replies), and *immediately after*, `printf` →
newlib (`_isatty`==1 → line-buffered → flushes on `\n`) → `_write`
(syscalls.o, weak, **is** the linked one — nosys didn't override it) →
`__io_putchar(0x0)` → `bl 0x0` → branch to a non-Thumb even address →
**HardFault** → `HardFault_Handler`'s default `while(1){}` → IWDG never
refreshed → ~8 s later the watchdog resets us and prints `! WATCHDOG RESET`.

Link-level proof (Debug build, bundled arm-none-eabi 14.3):
- `nm`: `_write` = weak `T/W` at `0x080020a8` (the syscalls.o one, used);
  `_write_r` from `libc_nano`; pre-fix `__io_putchar` undefined-weak → `0x0`.
- `objdump` of `<_write>`: the loop body is literally `__io_putchar(*ptr++)`.
- So pre-fix the `bl __io_putchar` targeted `0x0`. Post-fix `__io_putchar` is a
  real function at `0x08001d78`.

**This supersedes the part-5 "slow handshake sums to ~8 s" explanation.** That
timing math is real (and would bite eventually), but it was *not* the trigger —
the fault fires on the very first JSPR `printf`, within milliseconds of
`rbBegin` starting, long before any timeout could accumulate. The 8 s gap we
saw was just the IWDG period counting down while stuck in the fault loop. It
*looked* like an rbBegin hang because the symptom (8 s → reset, no `rbBegin`
output) is identical.

**Fix applied (manager `main.c`, USER CODE 4 — builds clean, text 58128 /
bss 23028):**
```c
int __io_putchar(int ch){ uint8_t c=(uint8_t)ch; HAL_UART_Transmit(&huart2,&c,1U,10U); return ch; }
```
This is a **no-regret** change: it removes the fault *and* lights up the JSPR
`SENT:`/`RECEIVED:` trace we wanted for visibility — one edit does both. Very
likely this alone gets `rbBegin` to complete (first contact end-to-end). Left
**uncommitted** per practice (validate on hardware first); the change compiles
and links with the bundled toolchain, no errors (only pre-existing benign
"static declared but never defined" header warnings).

**On the IWDG-in-`delay()` hardening (part-5 item #3): held — it would NOT have
fixed this.** The fault is a tight `_write`→`__io_putchar` call with no
`delay()`, and once in `HardFault_Handler`'s `while(1)` nothing calls `delay()`
either, so feeding the dog there changes nothing for this failure. It's still a
reasonable hardening *if* a genuinely slow handshake turns out to exist — but
that's now unproven, so it waits until the trace run shows whether `rbBegin`
ever legitimately runs long. Don't add complexity for a problem we may not have.

**Two real latent issues this exposed (worth fixing regardless):**
- The Debug build ships `DEBUG` on but no `__io_putchar` — *any* library
  `printf` (incl. the unconditional kermit ones on the firmware-update path)
  would HardFault. The retarget closes that for good.
- `HardFault_Handler` is a silent `while(1)`. Consider a minimal busy-wait UART
  print (`"!! HARDFAULT"`) before the spin so a future fault is unambiguous
  instead of masquerading as a watchdog timeout. (Not done now — the retarget
  removes the only fault we know about; revisit as defensive instrumentation.)

**Resume (post-dinner): build → flash → run startup.** Expected: `rbBegin` runs
to completion with a full `SENT:`/`RECEIVED:` JSPR trace on the console →
first contact end-to-end → commit the manager chunk. If it *still* resets, then
a fault is NOT the (only) cause and we read the trace to see how far `rbBegin`
gets — but the link-level evidence says the retarget should clear it. NB: with
`DEBUG` on, the trace also prints every `rbPoll` in RUNNING (great for S1; turn
`DEBUG` off before MO segment-timing tests where the ~300 ms prompt deadline
matters).

### S2 (part 7): trace is ALIVE — two real, separate bugs now visible ⭐

The `__io_putchar` retarget worked exactly as predicted: **no more reset-on-
`rbBegin`-start; we now get a live `SENT:`/`RECEIVED:` trace.** HardFault theory
confirmed on hardware. What the trace shows (reproducible, two identical runs):
```
SENT:     GET apiVersion {}
RECEIVED: 405 MALFORMED {}                          <- clue 1
RECEIVED:
SENT:     GET apiVersion {}
RECEIVED: ,"patch":0},{"major":1,"minor":6,...}]}   <- clue 2
```
(Then setApi fails to match → patient 1 s waits × loop × retries stack toward
8 s → the *residual* watchdog reset. So part-5's timing math is finally real —
but only as a **symptom** of the two bugs below, not a cause.)

- **Clue 1 — `405 MALFORMED` on the FIRST command only.** Command bytes are
  provably correct (`"GET apiVersion {}\r"`, `JSPR_GET_API_LEN`==18, no padding/
  nulls). First-cmd-garbage / rest-clean is the fingerprint of a **TXD glitch
  byte at USART1 bring-up**: PA9 is held *low* during boot (interlock), and
  `uart1_up()` flips it to USART-AF (idle *high*); a receiver watching a
  held-low line (a break/framing condition) can latch a phantom `0x00` as the
  line returns to idle, which prefixes our first command → `[0x00]GET…` →
  malformed. (Inference; fits evidence + the existing "TXD-low 0x00 noise" note.)
- **Clue 2 — the good reply comes back HEAD-CHOPPED** (`,"patch":0},…` is the
  tail of `200 apiVersion {"supported_versions":[{…`). We're **dropping RX
  bytes**: single-byte `HAL_UART_Receive_IT` at 230400 has a re-arm gap; a fast
  burst overruns it → lost bytes → `\r` framing desyncs → fragment. This is the
  hard evidence that **DMA RX is required**, not optional.

**On the patient-handshake change (a2f1197):** Liam's instinct was right — it was
a fix for a problem we don't have. We added it believing rbBegin failed from a
too-short reply window; the trace proves the modem replies promptly — we were
HardFaulting before we could read it. Not *wrong* (the 10 ms window is a real
latent bug, keep it for prod), but currently a **confound**: because the
handshake genuinely fails, each patient wait burns its full 1 s, which is what
produces the residual watchdog resets. Fix the two real bugs → modem replies
clean+fast → patient waits return instantly → patience becomes invisible. Don't
add/remove patience to chase this.

**Experiment applied (manager `main.c`, uncommitted, builds clean — text
58336):** parser-sync — after the boot-settle quiet-wait (modem booted &
listening), send a lone `\r` to terminate any latched partial line, then a
bounded drain-to-quiet (reuses the existing IWDG-safe loop) before `rbBegin`.
New log: `parser sync: sent CR, drained N reply bytes`. **Decision tree on
next run:** (a) 405 gone + whole `200 apiVersion` → first contact; (b) 405 gone
but reply still fragments → clue 2 is the gate, do DMA RX next; (c) 405 persists
→ not a latched partial, pivot. Targets clue 1 only — clue 2 (DMA RX) is the
likely follow-on regardless.

### S2 (part 8): 🎉 FIRST CONTACT — full JSPR handshake, rbBegin OK → RUNNING ⭐⭐⭐

Both fixes landed it. CR-flush confirmed (clue 1): `parser sync: sent CR, drained
17 reply bytes` every run — and **17 == `"405 MALFORMED {}\r"`**, i.e. the modem
405s the *throwaway* CR line now instead of our real command. The 405 is gone
from all three runs (fresh power-up / direct watchdog-recovery / manager-reset).
Then `receiveJspr` inter-byte patience (clue 2) made the reply arrive **whole**,
and the entire handshake ran to completion:
```
GET/PUT apiVersion   -> 200, negotiated v1.7
GET/PUT simConfig    -> 200 internal; 299 simStatus card_present, ICCID 8988169771001181297
GET/PUT operationalState -> inactive -> active
GET hwInfo           -> 200 hw=0x0601 serial=1a06b7 IMEI=300258060609970 temp=21C
GET constellationState -> 200 visible:false bars:0
rbBegin OK -> RUNNING ; then unsolicited 299s + "SIG bars=0 level=2048 visible=no"
```
**Liam's observation:** the inter-byte patience did **not** stall `rbPoll` in
RUNNING (the risk with that change) — unsolicited frames process smoothly. Good
confirmation the bound is small enough.

**Diagnostic arc that got us here (whole S2):** HardFault-masquerading-as-
watchdog (null `__io_putchar` under the Debug build's `DEBUG`) → trace lit up →
two real bugs visible: (1) TXD glitch byte at USART1 bring-up → first command
`405` (fixed manager-side with a parser-sync `\r` + bounded drain), (2)
`receiveJspr` discarding in-flight frames → head-chopped replies (fixed in the
lib with inter-byte patience). The patient-handshake change (a2f1197) was a fix
for a problem we didn't have — kept it (real latent bug) but it was never the
blocker. Lesson logged: **get visibility before theorizing about timing.**

**Committed:** manager `c212769` on `main` (first-contact chunk: `__io_putchar`
+ parser-sync `\r` + submodule `40363fd→c50118c` + IWDG ~8s via `.ioc`); library
`c50118c` on the fork (receiveJspr patience + `LUX_LIBRARY_CHANGES.md` ledger).
S1 first-contact gate: **DONE.**

**Follow-ups (not blockers, in priority-ish order):**
1. **hw/imei/temp accessors read impatiently.** `rbBegin OK` line shows
   `hw=? imei=? temp=-100C` though the `200 hwInfo` data (IMEI 300258060609970,
   temp 21 C) is right there in the trace. The `rbGetHwVersion/Imei/BoardTemp`
   accessors each fire their own `GET hwInfo` and read before the reply lands
   (the 3× `GET hwInfo` + empty `RECEIVED`s). Same family as the patience bug,
   different call path — likely they don't use `waitForJsprMessage`. Cosmetic;
   easy follow-up (cache hwInfo during rbBegin, or make the accessors patient).
2. **DMA-to-IDLE RX** still the planned proper RX path (throughput + robustness
   for MO segments); patience is a correct stopgap, not a substitute.
3. **Real signal / MT echo** need **sky view** (bench shows bars=0 visible=no,
   which is correct — the pipeline fires, the antenna just can't see birds).
4. Stale "fresh power-up" comment in `main.c` (the deafness was the FAULT-recycle
   path, since fixed by `uart1_down`) — tidy when convenient.

### S2 (part 9): hwInfo fix validated; a SIG-line regression, an active-poll, and a deliberate revert

Reflashed with the hwInfo/simStatus patient-read fix → `rbBegin OK: hw=0x0601
imei=300258060609970 temp=22C` (real values, accessor reads now interleave
cleanly). hwInfo accessor fix **validated** (task done).

**Side effect Liam caught:** no more `SIG` line in RUNNING. Root cause (a nice
second-order one): the patient hwInfo reads make `rbBegin` *longer*, so the
modem's **one-shot** unsolicited `299 constellationState` (fired right after it
goes active) now lands *inside* `getHwInfo`'s `waitForJsprMessage` loop and is
**discarded** (the known "sync waits drop unsolicited frames" issue — already in
TODO/upstream-patch list). With signal flat at 0 indoors the modem never
re-sends it, so `rbPoll` in RUNNING has nothing → no `SIG` line. Old build's
shorter `rbBegin` finished before that frame arrived, so it landed in RUNNING.
Pure timing-boundary shift.

**Tried then reverted:** added an active `rbGetSignal()` poll in RUNNING
(2 s cadence). Built/worked, but **Liam reverted it** — wants the terminal clean,
and "updated on unsolicited messages" is the right shape for the application; the
proper fix (dispatch unsolicited frames during sync waits) is already a TODO
item. Decision: **let the outdoor test prove the unsolicited path** rather than
mask it with a poll. `main.c` reverted byte-identical to `c212769`. (Active-poll
kept only as a possible production option, not now.)

**State:** manager working tree = just the validated hwInfo submodule bump
(`c50118c→96abe76`), to be committed with the outdoor-test result. Next data
point: bring the modem to open sky and confirm `SIG bars=…/visible=yes` fires
via the unsolicited `onConstellationState` callback as signal changes.

### S2 (part 10): unsolicited signal path CONFIRMED — live satellite transit, antenna flat on the desk ⭐

Didn't even need to go outside. With the (no-poll) build sitting on the bench,
the modem volunteered unsolicited `299 constellationState` frames as a bird
transited:
```
299 constellationState {"constellation_visible":true,"signal_bars":0,"signal_level":-117}
[703541] SIG bars=0 level=-117 visible=yes      <- satellite in view
299 constellationState {"constellation_visible":false,"signal_bars":0}
[721348] SIG bars=0 level=-117 visible=no        <- ~18 s later, passed
```
**Both halves of part 9's theory proven:** (1) the unsolicited path works with
zero polling — frame → `rbPoll` → `onConstellationState` → SIG line; (2) the
earlier RUNNING silence was simply "no change to report" (the one-shot at-active
frame having been consumed in `rbBegin`). The revert was correct; event-driven is
the right shape. Interpretation: `visible=yes` + `bars=0` + `level=-117 dBm` =
detected an Iridium downlink but link too weak for a bar (flat desk antenna,
indoors). Iridium's 66-bird LEO swarm means even a bad antenna catches brief
zenith transits. Open-sky view → stronger `level` → real `bars`. Tracking
pipeline end-to-end alive.

**Committed the validated S1 baseline:** manager submodule bump to the fork
(hwInfo/simStatus patient reads + receiveJspr patience + parser-sync + first
contact). S1 signal-status sub-goal: **proven.** Remaining S1: MT echo (needs an
actual inbound message — sky view + ground send) and the DMA-to-IDLE RX swap.

### S2 (part 11): 🛰️ S1 FUNCTIONALLY COMPLETE — real signal bars + MT "Hello Lux" round-trip + clean shutdown (open-sky test) ⭐⭐⭐

Took the rig outside (deck, open sky, racing a thunderstorm) on the committed
baseline (`dcd5e54` / `a0a1114`) — **no code change**, just sky. Everything S1
needs lit up at once:

- **Real signal, unsolicited.** Bars climbed live as Iridium birds passed:
  `bars = 2 / 3 / 5 / 3 / 2 / 1`, `level` −113…−118 dBm, `visible=yes` — all
  from unsolicited `299 constellationState` → `onConstellationState`, **peak 5
  bars.** The event-driven signal path (the one we kept after reverting the
  active poll) is fully validated in open sky.
- **MT receive + decode + echo — "Hello Lux" round-tripped ground→device.** Sent
  a 9-byte MT from Cloudloop (topic 244, msg 21). Full IMT receive path ran:
  `messageTerminate` → `messageTerminateSegment` (base64) →
  `messageTerminateStatus: complete` → `onMtComplete(id=21, OK)` → echo
  `MT 9 bytes: Hello Lux`. **Cloudloop console confirms the other end:** Encoded
  → Sent (17:18:11 UTC) → Delivered (17:18:44) → Delivery Success, 9 B, Dir MT.
  Segment reassembly + base64 decode + completion callback all working.
- **Clean shutdown on the live modem.** `button → USART1 down → I_EN low →
  I_BTD LOW after 40 ms → power gate OFF → IDLE`. Damage-interlock shutdown
  sequence validated against the real 9704 (startup was already proven). Both
  ends of the power lifecycle now real-hardware-clean.

**S1 functional set complete:** first contact + identity + real signal + MT echo
+ clean power-down, all on real hardware over the hand-wired 16-pin link. The
only remaining S1 item is the **DMA-to-IDLE RX** swap — an architecture/robustness
improvement, not a functional gap (the `receiveJspr` inter-byte patience is the
correct stopgap). **Next: S2 — MO (outbound) pipeline → ACTU-style inter-MCU
protocol.** Validated the committed baseline; nothing to commit for this run.

---

## 2026-06-16 — Steps 3+4: transparent Core<->modem passthrough (code-complete)

Implemented the Core <-> modem passthrough in the manager `main.c` — POC steps
3 (MO pipeline) and 4 (MT pipeline) in one pass, since they share the link
plumbing. **Deliberately raw / no translation**: the manager is a dumb relay at
this layer. Per the CORE MESSAGE DEFINITIONS page, Core traffic is line-oriented
ASCII (`TYPE;SEQ;PAYLOAD\r\n`) and RSP messages are explicitly "forwarded as-is
via … modem manager" — so a line in = one MO, one MT = one line out. The
`TYPE/SEQ`-aware routing and ACTU framing stay deferred to Step 5.

**NOT yet hardware-validated** — written away from the bench (Liam in MTL this
week, no bench supply). Code-complete and committed so it's not lost; the
validation run waits until bench access is back. The S1 set above remains the
last thing proven on real hardware.

### Design

- **Dedicated Core link = USART3**, kept separate from the USART2 debug console
  so passthrough data never mixes with logs or the JSPR `DEBUG` trace. This also
  matches production (the Core<->manager link is a distinct UART from the modem
  link). In bring-up a USB-UART adapter on USART3 stands in for the Core/L4.
- **RX transport = single-byte IT into a power-of-two ring** (`CORE_RX_RING_SIZE`
  512), drained in the super-loop by `core_link_service()`. Matches the JC-sync
  decision (Core link interrupt near-term, tiny frames; DMA on the L4 side later).
  `HAL_UART_RxCpltCallback` now branches: `huart3` -> our ring, `huart1` -> the
  library (it filters by handle anyway, but the branch is explicit).
- **MO (step 3):** accumulate Core RX until `\n`, strip a trailing CR so the
  CRLF stays **off-air** (Iridium is billed per byte), and hand the payload to
  `rbSendMessageAsync(RAW_TOPIC, …)` — async only, never the sync API (the
  timeout-isn't-cancel footgun). `rbPoll` does the segment exchange. Empty lines
  ignored; oversize lines (> `CORE_LINE_MAX` 1024) drained-and-dropped with a log.
- **MT (step 4):** rewrote the old `echo_mt` (console sanitize) into
  `relay_mt_to_core` — writes the MT payload **verbatim** out USART3, appends
  `\r\n` (mirrors the terminator stripped on the MO side), keeps a sanitized
  preview on the debug console only, then acks the queue head.
- **Lifecycle:** the Core link is independent of the modem interlock — brought
  up once at boot and never torn down. Lines arriving outside RUNNING are
  discarded (no modem to send to, and so no stale backlog queues for next boot).

### Decisions (Liam, this session)

Confirmed up front: dedicated UART (not VCP reuse); strip-and-re-add the
terminator (off-air payload only); `RAW_TOPIC` (244) for all passthrough MOs
(single topic — per-type routing is translation = Step 5).

### Required CubeMX step before this builds

`main.c` now references `huart3`. Add USART3 in CubeMX: Asynchronous, 115200
8N1, free pins (suggest **PC10 TX / PC11 RX** — PC0–3 are the modem control
pins), **auto-init** (do NOT defer like USART1 — the Core link has no
boot-sequencing constraint), and **enable the USART3 global interrupt** (NVIC)
for IT RX. Regenerate; `MX_USART3_UART_Init()` is then auto-called and
`core_link_start()` (USER CODE 2) arms the receive.

### Known bounds / follow-ups

- MT relay is a blocking `HAL_UART_Transmit` inside the `rbPoll` callback. Fine
  for the <50 B command/RSP payloads in scope; revisit if a large MT ever
  interleaves with an in-flight MO segment exchange (the ~300 ms prompt deadline).
- **Turn the build `DEBUG` symbol OFF for MO segment-timing tests** — the
  per-frame JSPR `printf` trace on USART2 can eat into the segment deadline.
- Validation plan: USART3 adapter in a terminal; type a `\n`-terminated line ->
  expect `MO queued … <- core` then (sky view) a real send; ground-send an MT ->
  expect the verbatim payload + CRLF back on the adapter. Needs sky view for a
  live round-trip; the queue/relay plumbing can be exercised on the bench
  (rejections, framing) without it.

State: **code-complete, committed, not yet hardware-validated** (pending the
CubeMX USART3 add). No library changes this session — manager `main.c` only.
