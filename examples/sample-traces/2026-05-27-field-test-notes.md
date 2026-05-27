# 2026-05-27 field test — observation notes

Companion notes captured during the trace runs at
`45.4012841, -75.6301899` (Ottawa, partial sky view — "not *quite*
full sky view").

## Observations during the run

- MT messages are definitely on a retry cadence. If a message is not
  immediately responded to it gets stored.
- A new message being received drains as much of the queue as it can.
- 4 messages delivered in <1 s on a retry today.
- 1 message was pending until a second delivered successfully —
  wondered if a successful received MO also drains the MT queue.
- Successful deliveries in ~30–60 s.
- Signal level consistently rises from low to high when message sent
  and system connected.
- Periods of very low signal lead to delivery failure on 16 B
  payload.

## Confirmed during the run

- **Yes — MT queue is drained from Iridium when an MO successfully
  goes.** This is the key new finding. Could be worth adding
  behaviour that takes advantage of that: schedule MO attempts for
  when constellation is visible to pull commands at that time.
- Periods of constellation loss at this location ~2 minutes.

## Confirmed MT delivery events in the trace

From `2026-05-27-field-test-good-rf.log`:

| Time (Z)    | id | Bytes | Payload                                          | Notes |
|-------------|-----|-------|--------------------------------------------------|-------|
| 21:29:15    | 8  | 10    | `Hello Lux!`                                     | Cluster of 2 — drained on same window |
| 21:29:15    | 9  | 17    | `Hello from Lux 2!`                              | |
| 21:35:07    | 10 | 16    | `17h34 local test`                               | |
| 21:35:58    | 11 | 16    | `17:35 local test`                               | |
| 21:38:06    | 12 | 20    | `Test test test 17:37`                           | |
| 21:39:27    | 13 | 14    | `Are you there?`                                 | |
| 21:45:12    | 14 | 16    | `swamp the system`                               | Cluster of 2 — drained on same window |
| 21:45:12    | 15 | 14    | `how about now?`                                 | |
| 21:46:17    | 16 | 21    | `try at ish low signal`                          | |
| 21:47:21    | 17 | 48    | `Intentional failure attempt 1 (signal 0 on send)` | Tried to provoke failure; succeeded anyway |
| 21:48:00    | 18 | 61    | `try that again with a longer message...`        | 2/5 signal at receive |
| 21:48:38    | 19 | 38    | `opportunistic no constellation visible`         | Delivered during peak signal |

## Signal-precedes-delivery pattern

Every MT delivery in this trace is preceded by a climb in signal,
typically from 1–2 bars to 4–5 bars within ~5 seconds before the MT
event. Two explanations to disambiguate later:

1. **Selection effect**: the modem only completes delivery when a
   satellite is actually accessible. Failed attempts at bad signal
   don't surface as MT events.
2. **Active link optimisation**: the modem may select the
   best-visible satellite once a session begins, with the elevated
   signal being a *consequence* of an active session rather than its
   precondition.

The trace alone cannot distinguish these. Worth probing.
