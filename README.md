# CrossPoint Context

E-reader firmware for the ESP32-C3-based Xteink **X4**/**X3** that adds a
spoiler-free **Reading Context** feature on top of the
[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)
firmware it is forked from.

Read on the device, tap **"Sync to CrossPoint Context"**, and an AI assistant can answer
questions about the book *up to your current page* — **never revealing anything past where
you've actually read**, and never drawing on outside knowledge of the book. Ask "who is
this character again?", "what just happened?", or "remind me what that place is" — without
the usual risk of an AI spoiling the ending it already knows.

> Forked from [crosspoint-reader/crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader)
> (MIT). All of the base reading firmware is upstream's work; this fork adds the
> reading-context feature. See [LICENSE](./LICENSE).

![CrossPoint running on Xteink device](./docs/images/cover.jpg)

---

## Reading Context

The simplest path uses the hosted **CrossPoint Context** service — pair the device once,
connect your assistant, and there are no tokens to copy around. Prefer to run your own
server? See [Self-hosting](#self-hosting) below.

### Quick start (hosted)

1. **Flash** the firmware (see [Install](#install)). Release builds point at the hosted
   service by default.
2. **Pair the device.** On the reader: **Settings → CrossPoint Context → Pair**. It shows a
   QR code and a short code — scan it with your phone (or open the shown URL and type the
   code), sign in with GitHub or Google, and approve. The device is now bound to your
   account; nothing to copy by hand.
3. **Connect your assistant.** In Claude (or any [MCP](https://modelcontextprotocol.io)
   client), add a custom connector for `https://mcp.crosspoint-context.com/mcp` and sign in
   with the same account. It exposes read tools that return only the passages your assistant
   needs — never anything past your current page.
4. **Read and ask.** Read on the device, tap **"Sync to CrossPoint Context"**, then ask your
   assistant about the book.

The hosted server handles identity (OAuth) and serves your reading position over MCP; it only
ever exposes text up to the page you've read.

### Self-hosting

Don't want to use the hosted service? Run your own server instead. The open-source
[**crosspoint-context-relay**](https://github.com/samMolyneux/crosspoint-context-relay) is a
small Cloudflare Worker that stores the latest push and serves it back behind a bearer token;
its README has step-by-step deploy instructions (a few minutes on a free Cloudflare account).
Then on the device choose **Settings → CrossPoint Context** and enter your relay URL and write
token manually. Read it with the bundled reference consumer (a Claude skill) — or any
tool-capable LLM/agent, since the relay is plain HTTP behind a token. Over a trusted LAN a
plain `http://` relay works; otherwise use `https://`. Wire format:
[`CONTRACT.md`](./docs/reading-context/CONTRACT.md).

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
