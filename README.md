# PBS Payload

Open-source PS5 Broadcast Stream payload and browser viewer. This repository
contains the console-side program only, with a fresh source history.

## Features

- H.264 capture with 720p, 1080p and 1440p profiles in the verified 60 fps mode.
- Stereo 48 kHz float32 PCM audio.
- Built-in browser viewer with the owner's banner.
- HTTP streaming and RTP video packetization.
- Capture restart and discontinuity handling.

The default requested encoder budget is 60 Mbit/s; the configurable range is
1-100 Mbit/s. Actual output depends on the scene and encoder. A higher budget
is not a guarantee of higher measured bitrate. Native 4K, 120 fps and zero
latency are not supported claims.

## Build and use

See [BUILDING.md](BUILDING.md). Build with LLVM and a PS5 Payload SDK, then
load `payload/build/pbs.elf` with your existing payload loader. No arguments
starts the stream on port 9999. Open `http://<console-ip>:9999/` in a compatible
browser and enable audio with a user gesture.

A console environment able to load homebrew payloads is required. No jailbreak,
SDK, console firmware, credentials or console keys are included. The cleaned
snapshot compiles but has not yet been redeployed and tested on a console.
Research-only USB, kernel scan, dumping and encoder experiments are excluded.

The endpoints are unauthenticated local-network services. Use a trusted LAN;
do not expose them to the Internet.

## License and provenance

Source code, including the browser viewer and transport test, is licensed
GPL-3.0-or-later; see [LICENSE](LICENSE). Existing attribution is retained.
The process-call implementation documents its reference to the ptrace
technique used by prospero-menu/libNineS (John Tornblom) and MemDBG.

`payload/web/banner.png` is Seregonwar's branding, not GPL-licensed artwork.
It is included for use with this project; unrelated reuse requires the owner's
permission. It is embedded by the build script and served by the viewer.

The separately developed desktop/mobile application is not part of this
repository or its license grant.
