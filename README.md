# CrossPoint Context

E-reader firmware for the ESP32-C3-based Xteink **X4**/**X3** that adds a
spoiler-free **Reading Context** feature on top of the
[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)
firmware it is forked from.

Tap **"Sync to CrossPoint Context"** on the device and it pushes the book *up to your
current page* to a small relay. An AI assistant then reads that text and answers
questions about what you're reading — **never revealing anything past where you've
actually read**, and never drawing on outside knowledge of the book. Ask "who is
this character again?", "what just happened?", or "remind me what that place is" —
without the usual risk of an AI spoiling the ending it already knows. The transport
is plain HTTP, so any tool-capable LLM or agent can consume it.

> Forked from [crosspoint-reader/crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader)
> (MIT). All of the base reading firmware is upstream's work; this fork adds the
> reading-context feature. See [LICENSE](./LICENSE).

![CrossPoint running on Xteink device](./docs/images/cover.jpg)

---

## Reading Context

The feature has three pieces:

1. **The device** (this repo) — extracts the book text up to your current page and
   pushes it over HTTPS to a server with a write token.
2. **A relay** — stores the latest push and serves it back to a reader with a read
   token. Self-host the open-source reference:
   [**crosspoint-context-relay**](https://github.com/samMolyneux/crosspoint-context-relay)
   (clone it, set a token pair, point your device at it, use the bundled skill).
3. **An AI assistant** — fetches the stored text and answers strictly from it. The
   relay ships a reference consumer implemented as a Claude skill (`skill/`), but any
   LLM or agent that can fetch a URL with a bearer token works just as well.

The exact wire format between the three is documented in
[`docs/reading-context/CONTRACT.md`](./docs/reading-context/CONTRACT.md).

On-device, configure the server origin and write token under
**Settings → Reading Context**. Self-hosters point this at their own relay; over a
trusted LAN a plain `http://` relay works, otherwise use `https://`.

---

## What CrossPoint does (base firmware)

Underneath the context feature this is the full CrossPoint reader — see the
[upstream README](https://github.com/crosspoint-reader/crosspoint-reader) for the
complete tour. In brief:

- **Reader engine**: EPUB 2/3 rendering, images, hyphenation, kerning, chapter
  navigation, footnotes, bookmarks, go-to-percent, auto page turn, orientation
  control, focus reading, KOReader progress sync.
- **Formats**: `.epub`, `.xtc/.xtch`, `.txt`, `.bmp`.
- **Library**: folder browser, recent books, SD-cache management, custom SD-card fonts.
- **Wireless**: file-transfer web UI, web settings UI/API, WebDAV, OPDS browser,
  Calibre connect, OTA updates.
- **Customization**: themes, sleep screens, button remapping, refresh cadence.
- **Localization**: 24 UI languages, RTL support.

### Hardware

ESP32-C3 (single-core RISC-V, ~380KB usable RAM, no PSRAM), 800×480 monochrome
E-Ink, SD card storage. Stability under the tight RAM ceiling is the project's
primary constraint — see [SCOPE.md](./SCOPE.md).

---

## Install

This fork is distributed as source / a built `firmware.bin`; it is **not** served by
the official CrossPoint web flasher (that only carries official upstream releases).

### Build and flash from source (recommended)

```bash
git clone --recursive https://github.com/samMolyneux/crosspoint-context
cd crosspoint-context
pio run --target upload      # build + flash over USB-C
```

### Flash a prebuilt binary

```bash
pip install esptool
esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 \
  write_flash 0x10000 /path/to/firmware.bin
```

Adjust the port (`dmesg` on Linux after plugging in) to match your system.

> **USB-locked devices:** some Xteink units (often third-party resellers) ship with
> USB flashing locked. Flashing *any* non-official firmware on a locked device can
> permanently brick it or strand it with no recovery path if the firmware lacks OTA.
> See the upstream [unlock tool + warnings](https://crosspointreader.com/#unlock-tool)
> before flashing a fork onto a locked unit. Units bought directly from xteink.com are
> not locked.

---

## Development

### Prerequisites

- [pioarduino](https://github.com/pioarduino/pioarduino) (or VS Code + pioarduino plugin)
- Python 3.8+, `clang-format` 21, a data-capable USB-C cable

### Build / check / monitor

```bash
pio run --target upload          # build + flash
./bin/clang-format-fix           # format
pio check -e default             # static analysis
pio run -e default               # build check

python3 scripts/debugging_monitor.py   # serial monitor (pyserial colorama matplotlib)
```

See [CLAUDE.md](./CLAUDE.md) for the engineering rules (memory discipline, HAL usage,
i18n) and [docs/contributing/](./docs/contributing/README.md) for contribution docs.

---

## Documentation

- [User Guide](./USER_GUIDE.md)
- [Reading Context wire contract](./docs/reading-context/CONTRACT.md)
- [Web server usage](./docs/webserver.md) · [endpoints](./docs/webserver-endpoints.md)
- [File formats](./docs/file-formats.md) · [Project scope](./SCOPE.md) · [Governance](./GOVERNANCE.md)

### Internals — SD caching

CrossPoint caches aggressively to the SD card to stay within the ~380KB RAM ceiling.
Chapter layouts, metadata, covers, and CSS are cached under `.crosspoint/` on the card
(one `epub_<hash>/` directory per book). Removing `/.crosspoint` forces a clean
regeneration on next open. Details in [file-formats.md](./docs/file-formats.md).

---

## Contributing

Contributions welcome — start with the [contributing docs](./docs/contributing/README.md)
and [GOVERNANCE.md](./GOVERNANCE.md). The reading-context additions live in
`lib/CrossPointContext/`, `lib/KOReaderSync/`, and `src/activities/` (settings + reader).

For the base reader, consider contributing upstream to
[crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader) so the whole
community benefits.

---

CrossPoint is **not affiliated with Xteink or any device manufacturer**. Base firmware
© the CrossPoint Reader contributors; reading-context additions © 2026 Sam Molyneux
(both MIT). Huge shoutout to upstream and to
[diy-esp32-epub-reader](https://github.com/atomic14/diy-esp32-epub-reader), which
inspired the original project.
