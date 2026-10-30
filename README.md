<div align="center">

<img src="payload/web/logo.png#gh-dark-mode-only" alt="PBS" width="180" />
<img src="payload/web/logo-dark.png#gh-light-mode-only" alt="PBS" width="180" />

<br/><br/>

**Broadcast Stream payload and browser viewer for PlayStation 5 — with a PS4 backend in the same tree.**

<br/>

[![C11](https://img.shields.io/badge/C-11-informational?style=flat-square&logo=c&logoColor=white)](https://en.cppreference.com/w/c/11)
[![License](https://img.shields.io/badge/license-GPL--3.0--or--later-informational?style=flat-square)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-PS5%20%7C%20PS4-blueviolet?style=flat-square)](#-supported-firmware)
[![Version](https://img.shields.io/badge/version-0.2.0-blue?style=flat-square)](VERSION)
[![Firmware](https://img.shields.io/badge/PS5%20profiles-20%20measured-brightgreen?style=flat-square)](docs/PS5_FIRMWARE_OFFSETS.txt)

<br/>

[Overview](#-overview) · [Features](#-features) · [Supported firmware](#-supported-firmware) · [Usage](#-usage) · [Build](#-build) · [HTTP API](#-http-api) · [Limits](#-known-limits) · [Docs](#-documentation)

<br/>

</div>

---

## Repository architecture

The console program is split by responsibility under `payload/src/common/` and
`payload/src/platform/{ps5,ps4}/`, with the offset tables of each firmware kept
next to the code that selects them (`platform/*/offset/`). Measurement tools
(dump payloads, generators, ledgers) live in `tools/` and are deliberately
outside the capture path. Generated assets stay in `payload/build/generated/`.

The browser viewer is embedded into the payload at build time, so a single ELF
carries capture, HTTP transport and UI. See
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the dependency rules and
[`CHANGELOG.md`](CHANGELOG.md) for the release history.

## Overview

PBS Payload runs on the console, captures the live video stream and serves it to
a browser over the local network: MJPEG frames, RTP packetized H.264, or a
recorded file written on the console itself. The same viewer starts, stops and
re-profiles the capture without reloading the payload.

Everything the payload does to firmware memory is derived from a memory dump of
that exact firmware, never from a neighbouring one. A profile is a table of
instruction addresses — the Avcap2 request geometry and the RemotePlay
constructor — and a wrong address is not a soft failure, so an unknown build is
refused by design.

```
Design rules
  ├── Truth over guesses      → a profile exists only if it was dumped and disassembled
  ├── Fail closed             → unknown request size = refused, never borrowed
  ├── Measured claims         → docs say what was verified on hardware, not what might work
  ├── Small surface           → one HTTP server, one embedded viewer, no external runtime
  └── Extras are opt-in       → branding, dumps and diagnostics are build switches
```

## Features

| Area | What it gives you |
|---|---|
| **Capture** | Hardware H.264 encoder via Avcap2; 720p / 1080p / 1440p / 2160p profiles, 60 fps mode |
| **Profiles** | Switching at runtime through `/capture/profile`; PS5 auto-selects 1080p for an observed 1080p output mode |
| **Audio** | Stereo 48 kHz float32 PCM (`/audio`) |
| **Transport** | HTTP MJPEG (`/frames`), RTP packetization for external players, capture restart and discontinuity handling |
| **Viewer** | Built-in browser UI: live stats, resolution switching, forced keyframe, recording, download |
| **Recording** | Writes an H.264 file on the console, served back from `/recordings/` |
| **Firmware** | 20 measured PS5 profiles plus audited family ranges; PS4 runtime layout discovery |
| **Diagnostics** | Offset, dependency and module dump payloads that produce the next firmware profile |
| **Reliability** | Port fallback (ten candidates), RTP backlog resync with keyframe request, periodic keyframe refresh |

The default requested encoder budget is 60 Mbit/s; the configurable range is
1–100 Mbit/s. Actual output depends on scene and encoder — a higher budget is not
a guarantee of higher measured bitrate.

## Supported firmware

### PS5 — measured profiles

Each row was disassembled from a dump of that firmware: the request geometry from
`libSceAvcap2.sprx.prefix.bin`, the RemotePlay constructor from the
`SceRemotePlay` EBOOT. The firmware ID is `0xMMMMmmmm` (major, minor, build).

| Release | Firmware ID | Request | `kind` |
| --- | --- | --- | --- |
| 3.20 | `0x03200004` | `0x218` | `0xe8` |
| 4.00 | `0x04000042` | `0x230` | `0x100` |
| 4.51 | `0x04510001` | `0x230` | `0x100` |
| 6.02 | `0x06020004` | `0x2a8` | `0x168` |
| 7.20 | `0x07200005` | `0x2a8` | `0x168` |
| 8.40 | `0x08400005` | `0x2a8` | `0x168` |
| 9.20 | `0x09200005` | `0x2b0` | `0x168` |
| 9.40 | `0x09400008` | `0x2b0` | `0x168` |
| 10.01 | `0x10010000` | `0x2b8` | `0x188` |
| 10.60 | `0x10600006` | `0x2b8` | `0x188` |
| 11.20 | `0x11200005` | `0x2e0` | `0x1a0` |
| 11.40 | `0x11400005` | `0x2e0` | `0x1a0` |
| 11.60 | `0x11600005` | `0x2e0` | `0x1a0` |
| 12.00 | `0x12000043` | `0x2e0` | `0x1a0` |
| 12.40 | `0x12400009` | `0x2e0` | `0x1a0` |
| 12.70 | `0x12700001` | `0x2e0` | `0x1a0` |
| 13.00 | `0x13000040` | `0x2e0` | `0x1a0` |
| 13.20 | `0x13200006` | `0x2e0` | `0x1a0` |
| 13.42 | `0x13420000` | `0x2e8` | `0x1a8` |
| 13.60 | `0x13600007` | `0x2e8` | `0x1a8` |

Six of them are **exact-only** (3.20, 4.00, 4.51, 11.20, 13.00, 13.20): another
build revision of the same release is refused instead of being served with a
geometry it was never measured on.

### PS5 — releases served by an audited family range

Between measured anchors, releases whose Avcap2 geometry is unchanged are
accepted without their own profile:

| Family range | Anchor profile | Releases accepted |
| --- | --- | --- |
| 6.02 – 8.40 | `602` | 6.50, 7.00, 7.01, 7.40, 7.60, 7.61, 8.00, 8.20 |
| 9.20 – 9.40 | `920` | — |
| 10.01 – 10.60 | `1001` | 10.20, 10.40 |
| 11.40 – 12.70 | `1160` | 12.02, 12.20, 12.60 |
| 13.40 – 13.60 | `1360` | 13.40 |

A range stands in only for releases inside the audited window; it never covers a
release listed as exact-only, and never covers another build of an exact-only
release.

### PS5 — not supported yet

| Release | Why it is refused | What it needs |
| --- | --- | --- |
| 9.00 | lies outside the 9.20–9.40 family | dump of `libSceAvcap2` + `SceRemotePlay` |
| 9.60 | past the end of the 9.20–9.40 family | dump |
| 11.00 | sits between the 10.01–10.60 geometry (`0x2b8`) and the measured 11.20 build (`0x2e0`) | dump |
| 14.00 | beyond the last audited family (13.40–13.60) | dump |
| 14.10 | beyond the last audited family (13.40–13.60) | dump |

If your console reports one of these, the payload refuses to patch rather than
risk a wrong request size. Run the dump build and open an issue with the
archive — that is exactly how 3.20, 4.00, 4.51, 11.20, 13.00, 13.20, 13.42 and
the other profiles were added.

### PS4

| Release | State |
| --- | --- |
| 11.00 | validated on hardware: 720p worker backend, ~59.9 fps over 180 s sessions, video read through exported `sceVshAvcapReadVideo` |
| 9.00 | kernel layout (acmgr + both gates) discovered at runtime; Avcap exports measured; capture internals not validated |
| 6.72 | offset report and diagnostics only |

PS4 kernel offsets do not transfer between releases, so a build without a
matching layout is refused before any patch is applied. PS4 audio is disabled.
Deeper detail: [`docs/PS4_FIRMWARE_OFFSETS.txt`](docs/PS4_FIRMWARE_OFFSETS.txt),
[`docs/PS4_HANDOFF.md`](docs/PS4_HANDOFF.md).

## Usage

Load `pbs.elf` with your payload loader. With no arguments the stream starts on
port 9999 at 2560×1440:

```
pbs.elf avserve [720p|1080p|1440p|2160p] [bitrate=N] [minqp=N] [norate]
                [offsetdump] [dependencydump] [moduledump] [noaudio] [port]
```

Open `http://<console-ip>:9999/` in a browser on the same network. Audio starts
after a user gesture (browser autoplay policy). If the port is already taken by
another process, the payload binds the next free port in a ten-port window and
says which one in the console notification.

Run the payload as a background service on the console; it keeps capturing while
the viewer is closed. `POST /stop` releases the capture resources.

## Build

Requires LLVM (clang + lld) and a payload SDK:

| Target | SDK | Output |
| --- | --- | --- |
| PS5 | [`ps5-payload-dev/sdk`](https://github.com/ps5-payload-dev/sdk) | `payload/build/ps5/pbs.elf` |
| PS4 | [`ps4-payload-dev/sdk`](https://github.com/ps4-payload-dev/sdk) | `payload/build/ps4/pbs-remote-stream.elf` |
| PS5 dump | same PS5 SDK | `payload/build/pbs-ps5-dump.elf` |

```bash
# Linux / WSL
export PS5_PAYLOAD_SDK=$HOME/ps5-payload-sdk
./payload/build.sh ps5
```

```powershell
# Windows (WSL-backed)
./payload/build.ps1 -Platform PS5 -SdkPath '~/ps5-payload-sdk'
```

Optional switches: `-WebLogo` (`PBS_WEB_LOGO=1` in WSL) embeds the owner banner,
`-Kind dump` builds the measurement payload, `-Ps5DefaultResolution` changes the
initial profile. Offline regression runs on the host with
`tools/test_ps4_host.sh` and in CI through `verify.yml`. Full instructions:
[`BUILDING.md`](BUILDING.md), release procedure: [`docs/RELEASING.md`](docs/RELEASING.md).

## HTTP API

| Endpoint | Method | Purpose |
| --- | --- | --- |
| `/` | GET | Embedded viewer |
| `/config.json` | GET | Active profile, advertised resolutions, decoder headers |
| `/status.json` | GET | Width, height, fps, bitrate, recording state |
| `/frames` | GET | MJPEG stream |
| `/audio` | GET | Raw PCM (stereo, 48 kHz, float32) |
| `/capture/profile` | POST | Queue a resolution change (`?height=1080`) |
| `/capture/restart` | POST | Restart the capture pipeline |
| `/video/keyframe` | POST | Force an IDR for the current viewer |
| `/record/start`, `/record/stop` | POST | Write or stop a recording on the console |
| `/recordings`, `/recordings/<name>` | GET | List and download recordings |
| `/stop` | POST | Release the capture resources |
| `/banner.png` | GET | Owner banner, present only in a build made with `PBS_WEB_LOGO=1` |

These endpoints are unauthenticated local-network services. Use a trusted LAN and
never expose the console to the Internet.

## Known limits

- **Hardware validation is uneven by design.** PS4 11.00 at 720p is validated on
  real hardware (180 s sessions, ~59.9 fps, decoded recording). PS5 profiles are
  verified by offline disassembly and bounds checks; the console smoke test is
  the last step per release and some profiles have not had it yet.
- **No native 4K, 120 fps or zero-latency claims.** 2160p is a selectable capture
  profile, not a promise about the encoder or the link.
- **PS4 audio is disabled**, and PS4 capture outside 11.00 is unvalidated.
- **Firmware outside the tables above is refused**, on purpose.
- The desktop/mobile application is a separate project and is not part of this
  repository or its license grant.

## Documentation

| Document | Content |
| --- | --- |
| [`BUILDING.md`](BUILDING.md) | Toolchain, SDK, every build switch |
| [`CHANGELOG.md`](CHANGELOG.md) | Release history |
| [`docs/PS5_FIRMWARE_OFFSETS.txt`](docs/PS5_FIRMWARE_OFFSETS.txt) | Per-release geometry, source dump and evidence |
| [`docs/PS5_FIRMWARE_RANGES.md`](docs/PS5_FIRMWARE_RANGES.md) | Why a release is served by a range, and the bounds |
| [`docs/PS5_OFFSET_PROFILES.md`](docs/PS5_OFFSET_PROFILES.md) | How a dump becomes a profile |
| [`docs/STREAM_PROFILES.md`](docs/STREAM_PROFILES.md) | Runtime resolution switching |
| [`docs/PS4_HANDOFF.md`](docs/PS4_HANDOFF.md) | PS4 hardware evidence and open points |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Module boundaries and dependency rules |
| [`docs/RELEASING.md`](docs/RELEASING.md) | Release workflow, store registration, SDK provisioning |

## Support development

If PBS Payload is useful to you and you want to support continued work:

[![Support me on Ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/seregon)

## Credits

- **John Törnblom** — PS5 payload SDK and the `ptrace` process-call technique
  this implementation references (prospero-menu/libNineS).
- **ps4-payload-dev** — PS4 payload SDK and reference tooling.
- **MemDBG** — process memory reference.
- **The PlayStation homebrew community** — testers, dump contributors and
  feedback; every new firmware profile in this repository comes from a community
  dump.

## License and provenance

Source code, including the embedded viewer, is licensed
**GPL-3.0-or-later** — see [LICENSE](LICENSE). Existing attribution is retained.

`payload/web/logo.png` (bright, for dark backgrounds) and `payload/web/logo-dark.png`
(the same mark, inverted for light backgrounds) are PBS branding, not GPL-licensed
artwork: they are included for use with this project, and unrelated reuse needs the
owner's permission. The README picks the right one with GitHub's
`#gh-dark-mode-only` / `#gh-light-mode-only` fragments.

`payload/web/banner.png` is the stream overlay served at `/banner.png`: it is
embedded only when the build asks for it (`PBS_WEB_LOGO`), it is not GPL-licensed
either, and a default build does not contain it.

<div align="center">

Released under the [GNU General Public License v3.0 or later](LICENSE)

</div>
