# Lux modifications to the upstream RockBLOCK-9704 library

**Purpose:** a single, clear ledger of *every* change we've made to the upstream
`rock7/RockBLOCK-9704` source, so we can review, revert, or rebase cleanly and
never bury a footgun by accident. If you change library code, **add it here in
the same commit.**

- Upstream remote: `upstream` = `rock7/RockBLOCK-9704`; our fork branch:
  `lux/stm32-l452-port`.
- Authoritative diff at any time: `git log --oneline upstream/main..HEAD` and
  `git diff upstream/main..HEAD -- src/`. This file is the *human* summary of
  that diff — the git history is the source of truth.
- This ledger covers **library code only**. Manager-firmware changes (the
  `__io_putchar` retarget, the parser-sync `\r`, the GPIO/interlock state
  machine, etc.) live in the modem-manager project and `LUX_DEVLOG.md`.

### Scope legend (the footgun indicator)

- 🟢 **STM32-only** — gated behind `#if defined(STM32_HAL)`. Cannot change
  behavior on Arduino/Linux/Windows or alter upstream logic. Low revert risk.
- 🟡 **Shared path** — modifies code that runs on **all** platforms. These are
  the ones to scrutinize first if anything regresses, and the first candidates
  to revert when isolating a problem.

---

## 🟢 STM32 platform port (additive — new platform branch)

The bulk of our work is a new platform preset, not edits to existing logic. It
only compiles/runs when `STM32_HAL` is defined.

| Area | Files | What |
|------|-------|------|
| Serial preset | `src/serial_presets/serial_stm32/serial_stm32.{c,h}` (new) | HAL-UART serial context: IT single-byte RX into a ring buffer, blocking TX, peek/read. Implements the `serialContext` function pointers. |
| Cross-platform shims | `src/crossplatform.{c,h}` | Added `#elif defined(STM32_HAL)` branch: `millis()`→`HAL_GetTick()`, `delay()`→`HAL_Delay()`, header include via `RB9704_STM32_HAL_HEADER` (default `"main.h"`). |
| Begin overload | `src/rockblock_9704.{c,h}` | Added `bool rbBegin(UART_HandleTypeDef *)` under `#if defined(STM32_HAL)`, mirroring the non-Arduino `rbBegin(const char*)`. |
| IMT queue sizing | `src/imt_queue.h` | Added `#elif defined(STM32_HAL)` → `IMT_PAYLOAD_SIZE 2048U + IMT_CRC_SIZE`. **Why:** the non-Arduino default is 100 kB, which overflows MCU RAM (~200 KB of queues). Commit `40363fd`. |

**Revert:** these are isolated behind `STM32_HAL`; to disable, simply don't
define `STM32_HAL` (the build falls back to upstream platform branches). To
remove entirely: `git revert` the port commits or delete the `serial_stm32/`
dir + the `STM32_HAL` branches.

---

## 🟡 Shared-path behavior changes (review these first if anything regresses)

These touch functions used on **every** platform. Each is a real fix, but each
changes upstream behavior — hence this section.

### 1. Patient handshake in `setApi` / `setSim` / `setState` — commit `a2f1197`

- **Files:** `src/rockblock_9704.c` (`setApi` ~l.155, `setSim` ~l.190,
  `setState` ~l.228).
- **What:** replaced the 8 post-send non-blocking `receiveJspr(&response, "X")`
  calls with `waitForJsprMessage(&response, "X", JSPR_RC_NO_ERROR, 1)` (1 s
  bounded wait). Also fixed a typo: target `"operationState"` → `"operationalState"`.
- **Why:** upstream's reply window was ~10 ms (2× `receiveJspr` with `delay(5)`),
  which misses a modem that replies slightly slower. The typo meant `setState`
  never matched its own reply target.
- **Footgun watch:** because each wait is bounded at 1 s, a handshake step that
  *fails* now burns a full second instead of returning instantly. With a
  retrying caller this can stack toward a watchdog window. (Observed: this is
  what produced the residual watchdog resets while the real bugs — TXD glitch
  and head-chopped reads — were still unfixed. See `LUX_DEVLOG.md` S2 parts
  5–7.) The typo fix is a pure correctness fix; keep it regardless.
- **Revert:** `git revert a2f1197`, or restore the `receiveJspr` calls (but
  **keep** the `operationalState` spelling fix).

### 2. `receiveJspr` inter-byte patience — `src/jspr.c`

- **What:** in `receiveJspr`'s read loop, when `context.serialRead` returns 0
  (ring empty) but we are **mid-frame** (`pos > 0`, no terminating `\r` yet),
  wait up to `JSPR_MAX_INTERBYTE_WAITS` ticks of `delay(1)` for the next byte
  and keep assembling the *same* frame, instead of bailing and discarding it.
  The wait budget resets on each successful byte. New tunable
  `#define JSPR_MAX_INTERBYTE_WAITS 5U` (override-able).
- **Why:** upstream `receiveJspr` is non-blocking and stateless across calls. At
  230400 a reply often straddles two poll iterations: the first call reads the
  *head*, the ring runs dry before the `\r`, upstream throws the head away, and
  the next call returns only the *tail* — a consistently head-chopped frame that
  never parses. (Observed on every `GET apiVersion` after the TXD-glitch fix —
  see `LUX_DEVLOG.md` S2 part 7.) This is the same class of issue noted in
  `LUX_INTEGRATION_TODO.md` under "receiveJspr drops frames during sync waits."
- **Footgun watch:** `receiveJspr` is no longer strictly non-blocking — it can
  now block up to `JSPR_MAX_INTERBYTE_WAITS` ms (default 5) *per dry-spell while
  mid-frame*. Bounded and small relative to the 50 ms `rbPoll` cadence, and it
  makes `rbPoll`/MT reads more robust too — but if a future hot path can't
  tolerate even that, lower the define or revert. The outer 8 KB
  `RX_BUFFER_SIZE` cap still bounds total reads.
- **Revert:** remove the `JSPR_MAX_INTERBYTE_WAITS` block, the
  `interByteWaits` local, and the mid-frame retry branch in `receiveJspr`
  (restoring the original `if (bytesRead <= 0) { reading = false; break; }`).
  Committed alongside this ledger — `git log --oneline -- src/jspr.c LUX_LIBRARY_CHANGES.md`.

### 3. Patient reads in `getHwInfo` / `getSimStatus` — `src/rockblock_9704.c`

- **What:** replaced the bare single `receiveJspr(&response, "X")` with
  `waitForJsprMessage(&response, "X", JSPR_RC_NO_ERROR, 1)` in `getHwInfo`
  (~l.940) and `getSimStatus` (~l.996).
- **Why:** both fired the `GET` then read the ring **once, immediately** —
  before the modem had replied — so the accessors (`rbGetImei/HwVersion/
  SerialNumber/BoardTemp`, `rbGetSimStatus...`) returned sentinels (`?`,
  `-100 °C`) even though the `200 hwInfo`/`200 simStatus` reply arrived intact a
  moment later (observed at first contact — `rbBegin OK` line showed
  `hw=? imei=? temp=-100C` while the trace held the real IMEI/temp). This is a
  *synchronization* bug, not delivery — same root as `a2f1197`/`rbGetSignal`,
  which already used the patient call. DMA RX would **not** fix it (the data was
  never lost, just read too early).
- **Footgun watch:** same tradeoff as any `waitForJsprMessage` accessor — it can
  block up to 1 s if the modem never answers, and (existing upstream behavior)
  it discards non-matching *unsolicited* frames seen while waiting. No worse than
  `rbGetSignal`, which already does this. Note these accessors still each do
  their own round-trip; calling three of them (hw+imei+temp) sends three
  `GET hwInfo`s — harmless but redundant (see manager-side log; a single-fetch
  tidy is optional).
- **Revert:** restore the two `receiveJspr(&response, "X")` calls.

---

## Known upstream issues we have NOT changed (tracked, not yet patched)

See `LUX_INTEGRATION_TODO.md` → "Library-level improvements" for the full list
(unbounded `clearLeftoverData` drain, `rbSendMessageAny` bool-compare bug,
fixed JSPR scratch-buffer sizes, etc.). Listed there so we don't silently depend
on un-patched behavior.
