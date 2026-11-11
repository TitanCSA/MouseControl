# Validation — 1.0.0

Validated on Windows 10 x64 (19045), MSVC C++17, with warnings treated as errors.

| Check | Result |
|---|---|
| Parser and deterministic scheduler | 110,072 assertions, including 100,000 trigger/timing iterations and 10,000 bounded parser inputs |
| Capture worker with isolated transport | 23 checks: original input forwarding, same-mouse routing, release cancellation, pause/resume, foreground exclusion, send failure cleanup and key recording |
| Actual hidden native UI | 39 checks: visual/code round trip, invalid code, fields, trigger toggles, step ordering and hit targets |
| AddressSanitizer | All three suites passed |
| Packaging | Static C++ runtime; explicit ZIP whitelist; no kernel source, PDBs, credentials or sanitizer runtime |
| Driver installer | Exact SHA-256 match with the official Keyboard & Mouse 1.0.5.0 GitHub release |

The app's UI image is rendered from its actual Direct2D drawing code, using an
explicit offline preview. Offline tests do not install a driver, open a capture
on the host, move the desktop pointer, access the clipboard or modify saved user
preferences. The worker test uses actual Windows events and threads with an
isolated transport; its success is not a physical hardware latency measurement.

The native driver boundary forwards the received batch unchanged, checks its
accepted count, then considers one additional relative movement. No future
motion queue is created. Closing/pausing joins the worker and closes its capture.

Physical-device and application compatibility follows the existing driver and
needs testing on the target PC. This release was not tested against every mouse,
Windows 11 installation or application. Windows scheduling and device processing
can affect millisecond timing; no zero-latency or hard real-time claim is made.
