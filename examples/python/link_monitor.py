"""
Live link monitor for the RockBLOCK 9704.

Passively observes the satellite link and prints a timestamped event log of:
  - Signal-strength changes (bars, level, constellation visibility) via the
    modem's unsolicited `constellationState` frames.
  - Inbound MT messages (id, status, decoded payload) via the
    `mtMessageComplete` callback and `receive_message_async`.

Intended for antenna/placement evaluation, QA reception-quality fingerprinting,
and verifying the "drain on registration window" model: leave this running and
correlate MT arrivals against the moments when signal bars came up.

Usage:
    python link_monitor.py --device COM9          (Windows)
    python link_monitor.py --device /dev/ttyUSB2  (Linux/macOS)

Press Ctrl+C to stop.
"""

import argparse
import os
import signal
import sys
from datetime import datetime, timezone
from time import sleep

from rockblock9704 import RockBlock9704


def _enable_ansi_on_windows() -> bool:
    """
    Enable ANSI escape sequence processing on Windows consoles.

    Windows PowerShell 5.x and legacy cmd.exe don't interpret ANSI by default
    — colour codes print as literal `←[2m` etc. Windows 10+ supports it via
    the SetConsoleMode VT flag, but the bit has to be flipped manually.
    Windows Terminal and PowerShell 7 (pwsh) already enable it.

    Returns True if colour output should work, False to fall back to plain.
    """
    if sys.platform != "win32":
        return True
    try:
        import ctypes
        kernel32 = ctypes.windll.kernel32
        STDOUT_HANDLE = -11
        ENABLE_VIRTUAL_TERMINAL_PROCESSING = 0x0004
        handle = kernel32.GetStdHandle(STDOUT_HANDLE)
        mode = ctypes.c_ulong()
        if not kernel32.GetConsoleMode(handle, ctypes.byref(mode)):
            return False
        new_mode = mode.value | ENABLE_VIRTUAL_TERMINAL_PROCESSING
        return bool(kernel32.SetConsoleMode(handle, new_mode))
    except (OSError, AttributeError):
        return False


_USE_COLOUR = _enable_ansi_on_windows() and os.environ.get("NO_COLOR") is None

# ANSI colours. Resolved to empty strings if the terminal can't render them
# so the output stays readable instead of being noisy with literal escapes.
def _c(code: str) -> str:
    return code if _USE_COLOUR else ""


RESET = _c("\033[0m")
DIM = _c("\033[2m")
BOLD = _c("\033[1m")
GREEN = _c("\033[32m")
YELLOW = _c("\033[33m")
RED = _c("\033[31m")
CYAN = _c("\033[36m")
MAGENTA = _c("\033[35m")

# Track previous signal state so we only print on actual change.
_last_bars = None
_last_level = None
_last_visible = None

# MT receive flag. The library callback fires on the rb.poll() thread, so we
# just set a flag and drain in the main loop where it's safer to do I/O.
_mt_pending = False

_running = True


def _ts() -> str:
    return datetime.now(timezone.utc).strftime("%H:%M:%S")


def _bar_colour(bars: int) -> str:
    if bars >= 4:
        return GREEN
    if bars >= 2:
        return YELLOW
    return RED


def _bar_glyph(bars: int) -> str:
    return _bar_colour(bars) + ("█" * bars) + RESET + DIM + ("░" * (5 - bars)) + RESET


def on_constellation_state(state: dict) -> None:
    """Modem-emitted signal-strength update. Only print on real change."""
    global _last_bars, _last_level, _last_visible

    bars = int(state.get("signalBars", 0))
    level = int(state.get("signalLevel", 0))
    visible = bool(state.get("constellationVisible", False))

    if bars == _last_bars and level == _last_level and visible == _last_visible:
        return

    _last_bars = bars
    _last_level = level
    _last_visible = visible

    vis = f"{GREEN}visible{RESET}" if visible else f"{RED}not visible{RESET}"
    print(
        f"{DIM}[{_ts()}Z]{RESET} {CYAN}SIG{RESET} "
        f"{_bar_glyph(bars)} "
        f"{BOLD}{bars}/5{RESET}  "
        f"level={CYAN}{level:>5d}{RESET}  "
        f"constellation: {vis}",
        flush=True,
    )


def on_mt_complete(msg_id: int, status: int) -> None:
    """
    Fires when the library has fully received an MT (or detected a failure).
    Defer the actual receive_message_async() call to the main loop to avoid
    re-entering library code from the callback context.
    """
    global _mt_pending

    if status == 1:  # RB_MSG_STATUS_OK
        print(
            f"{DIM}[{_ts()}Z]{RESET} {MAGENTA}MT{RESET}  "
            f"id={msg_id} complete -- draining...",
            flush=True,
        )
        _mt_pending = True
    else:
        print(
            f"{DIM}[{_ts()}Z]{RESET} {RED}MT{RESET}  "
            f"id={msg_id} FAILED (status={status})",
            flush=True,
        )


def _drain_mt(rb: RockBlock9704) -> None:
    """Pull any ready MT out of the library queue and print it."""
    global _mt_pending
    _mt_pending = False

    payload = rb.receive_message_async()
    if not payload:
        # Spurious wake or already drained; nothing to do.
        return

    # Try UTF-8 first; fall back to hex if it isn't text.
    try:
        text = payload.decode("utf-8")
        printable = "".join(c if (32 <= ord(c) <= 126) else "." for c in text)
        body = f"{GREEN}\"{printable}\"{RESET}"
    except UnicodeDecodeError:
        body = f"{DIM}<binary, {len(payload)} bytes>{RESET} {payload.hex()}"

    print(
        f"{DIM}[{_ts()}Z]{RESET} {MAGENTA}MT{RESET}  "
        f"{len(payload):>4d} B  {body}",
        flush=True,
    )

    rb.acknowledge_receive_head_async()


def _handle_sigint(_sig, _frame):
    global _running
    _running = False


def main() -> int:
    parser = argparse.ArgumentParser(
        prog="link_monitor",
        description="Live RockBLOCK 9704 link monitor (signal + inbound MT).",
    )
    parser.add_argument("--device", required=True, help="Serial port of the RB9704")
    args = parser.parse_args()

    signal.signal(signal.SIGINT, _handle_sigint)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, _handle_sigint)

    rb = RockBlock9704()
    print(f"Connecting on {args.device}...", flush=True)
    if not rb.begin(args.device):
        print(f"{RED}Failed to open serial port / negotiate JSPR.{RESET}", file=sys.stderr)
        return 1

    print(
        f"{GREEN}Connected.{RESET}  "
        f"IMEI={rb.get_imei()}  "
        f"HW={rb.get_hardware_version()}  "
        f"FW={rb.get_firmware_version()}",
        flush=True,
    )
    print("Watching for signal changes and inbound MTs. Press Ctrl+C to stop.", flush=True)
    print()

    # Register both callbacks BEFORE we start polling so we don't miss frames.
    rb.set_constellation_state_callback(on_constellation_state)
    rb.set_mt_message_complete_callback(on_mt_complete)

    # Print current signal once at startup so the screen isn't blank.
    initial = rb.get_signal()
    if initial is not None and initial >= 0:
        on_constellation_state({
            "signalBars": initial,
            "signalLevel": 0,
            "constellationVisible": initial > 0,
        })

    try:
        while _running:
            rb.poll()
            if _mt_pending:
                _drain_mt(rb)
            sleep(0.05)
    finally:
        # Note: the Python binding rejects None for callback un-registration
        # (raises TypeError "must be callable"). Easiest workaround is to
        # leave the callbacks set and just rely on rb.end() closing the serial
        # connection — once serial is closed, rb.poll() reads nothing and the
        # callbacks won't be invoked. Safe to skip the explicit unregister.
        rb.end()
        print(f"\n{DIM}Disconnected.{RESET}", flush=True)

    return 0


if __name__ == "__main__":
    sys.exit(main())
