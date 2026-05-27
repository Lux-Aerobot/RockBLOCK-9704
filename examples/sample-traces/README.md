# Sample link traces

Reference `link_monitor.py` output captured under known RF conditions.
Used as comparison baselines for:

- QA acceptance-test pass criteria (eventually, per the Forge facility
  test fixture work — see `LUX_INTEGRATION_TODO.md`)
- Field-deployment placement evaluation
- Library / protocol behaviour diagnostics

Format: raw `link_monitor.py` output with `NO_COLOR=1` (no ANSI
escapes). Lines either start with `[HH:MM:SSZ] SIG` (signal change)
or `[HH:MM:SSZ] MT` (inbound MT event). All timestamps are UTC.

## Inventory

| File                                       | Conditions                                                            | Notes                                                          |
|--------------------------------------------|-----------------------------------------------------------------------|----------------------------------------------------------------|
| `2026-05-27-field-test-good-rf.log`        | Stationary outdoor, ~partial sky view, dev kit on USB to laptop      | First confirmed end-to-end MO + MT round-trips. ~50 min run.   |
| `2026-05-27-walk-home-balcony.log`         | Walking, transitioning into apartment balcony placement              | Mobility / hand-off behaviour observable.                       |
| `2026-05-27-field-test-notes.md`           | Field notes accompanying the above two logs                          | Human-annotated observations during capture.                    |

## Reproducing a capture

```powershell
$env:NO_COLOR = 1
py examples\python\link_monitor.py --device COM5 |
    Tee-Object -FilePath ".\examples\sample-traces\YYYY-MM-DD-description.log"
```

Drop `$env:NO_COLOR = 1` if you want colour in the live terminal —
the log file will then contain ANSI escape codes (still parseable but
needs a strip step).

## Test location for 2026-05-27 traces

Coordinates `45.4012841, -75.6301899` (Ottawa, partial sky view —
specifically described as "not *quite* full sky view"). This is *not*
a fully-clear-sky reference; it's better than indoor-window placement
but worse than a clean rooftop. A true "good RF" baseline from an
unobstructed site is still outstanding.
