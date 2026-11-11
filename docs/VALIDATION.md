# Validation - 1.1.0

Validated on Windows 10 x64 (19045), MSVC C++17, with warnings treated as errors.

| Check | Result |
|---|---|
| Parser, scheduler and indentation | 122,091 assertions, including 100,000 trigger iterations, 10,000 parser inputs and 4,000 editor selections |
| Capture worker, isolated transport | 29 checks: original forwarding, same-mouse routing, release cancellation, ON/OFF, foreground exclusion, failure cleanup, key recording and busy-capacity cancellation |
| Actual hidden native UI | 38 checks: code editor, auto-indentation, comment preservation, single selection, ON/OFF and hit targets |
| AddressSanitizer | All three suites passed |
| Packaging | Static C++ runtime; explicit ZIP whitelist; no kernel source, PDBs, credentials or sanitizer runtime |
| Driver installer | Pinned official Keyboard & Mouse 1.0.5.0 GitHub release SHA-256 |

The scheduler has no loop-gap parameter or positive minimum interval. Tests
execute consecutive movement cycles at the same timestamp and verify that a
release cancels even a pending movement refused by a busy transport. Physical
batch retries preserve the SDK's accepted-entry tracking. No future sequence is
submitted to the driver, and OFF/foreground changes are checked before sending.

Old preferences migrate to one trigger and discard their previous loop gap.
Only the selected mouse button or keyboard key participates in the trigger.
Saved source preserves indentation and comments; invalid edits do not replace
the last valid program. Empty-work loops containing only zero-length waits are
rejected instead of spinning without producing input.

The screenshot uses the actual Direct2D renderer in explicit offline mode.
Tests use Windows events/threads and an isolated transport; they do not install
a driver, capture the host's keyboard/mouse, move its pointer, access its
clipboard or modify saved preferences. They are not hardware latency measurements.

Physical-device and application compatibility still needs target-PC testing.
This release does not claim coverage of every mouse, Windows 11 installation or
application. Zero added loop delay does not remove Windows scheduling or device
processing time. Tight movement loops can consume more CPU; explicit wait()
commands provide pacing when desired.
