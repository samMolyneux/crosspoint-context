# CrossPoint Context

E-reader firmware for the ESP32-C3-based Xteink **X4** that adds a
spoiler-free **Reading Context** feature on top of the
[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)
firmware it is forked from.

Read on the device, open the reading menu, select **"Sync to CrossPoint Context"**, and an AI assistant can answer
questions about the book *up to your current page* — **never revealing anything past where
you've actually read**. Ask "who is this character again?", "what just happened?", or "draw me a map" — without
the usual risk of an AI spoiling the ending it already knows.

> Forked from [crosspoint-reader/crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader)
> (MIT). All of the base reading firmware is upstream's work; this fork adds the
> reading-context feature. See [LICENSE](./LICENSE).

![CrossPoint running on Xteink device](./docs/images/cover.jpg)

---

## Reading Context

The simplest path uses the hosted **CrossPoint Context** service — pair the device once,
connect your assistant, and there are no tokens to copy around.

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

### Hosted service

The hosted **CrossPoint Context** service ([mcp.crosspoint-context.com](https://mcp.crosspoint-context.com))
is free:

- **[Manage your data](https://mcp.crosspoint-context.com/account)** — sign in to delete your
  stored reading data or revoke a paired e-reader.
- **[Privacy Policy](https://mcp.crosspoint-context.com/privacy)** — what's stored (your current
  book's read-so-far text, title, and position, plus a pseudonymous account id)
- **[Service overview](https://mcp.crosspoint-context.com)** — what the service is and how it works.
- **MCP endpoint** — `https://mcp.crosspoint-context.com/mcp` (the connector URL from step 3).

### Self-hosting

The open-source
[**crosspoint-context-relay**](https://github.com/samMolyneux/crosspoint-context-relay) is a
small Cloudflare Worker that stores the latest push and serves it back behind a bearer token;
its README has step-by-step deploy instructions (a few minutes on a free Cloudflare account).
Then on the device choose **Settings → CrossPoint Context** and enter your relay URL and write
token manually. Read it with the bundled reference consumer (a Claude skill) — or any
tool-capable LLM/agent, since the relay is plain HTTP behind a token. Wire format:
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
- **Localization**: 26 UI languages, RTL support.

### Hardware

ESP32-C3 (single-core RISC-V, ~380KB usable RAM, no PSRAM), 800×480 monochrome
E-Ink, SD card storage. Stability under the tight RAM ceiling is the project's
primary constraint — see [SCOPE.md](./SCOPE.md).

---

## Install

Three ways to flash, easiest first. The official CrossPoint web flasher's *release list* only
carries upstream builds, but its **"Custom .bin"** upload works fine for this fork — so you can
still use it as the flashing tool with a `firmware.bin` from this repo's releases.

### Web flasher (easiest)

1. Download **`firmware.bin`** from the
   [latest release](https://github.com/samMolyneux/crosspoint-context/releases/latest).
2. On a desktop in **Chrome or Edge**, open
   [crosspointreader.com/#flash-tools](https://crosspointreader.com/#flash-tools), select **X4**,
   click **"Custom .bin"**, and upload the `firmware.bin` you downloaded.

After the first install, new versions arrive over WiFi via **OTA** (Settings → firmware update)
from this fork's own releases — no re-flash needed.

### Build and flash from source

```bash
git clone --recursive https://github.com/samMolyneux/crosspoint-context
cd crosspoint-context
pio run --target upload      # build + flash over USB-C
```

### Flash a prebuilt binary with esptool

```bash
pip install esptool
esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 \
  write_flash 0x10000 /path/to/firmware.bin   # firmware.bin from the releases page
```

Adjust the port (`dmesg` on Linux after plugging in) to match your system.

> **USB-locked devices:** some Xteink units (often third-party resellers) ship with
> USB flashing locked. Flashing *any* non-official firmware on a locked device can
> permanently brick it or strand it with no recovery path if the firmware lacks OTA.
> See the upstream [unlock tool + warnings](https://crosspointreader.com/#unlock-tool)
> before flashing a fork onto a locked unit. Units bought directly from xteink.com are
> not locked.


CrossPoint is **not affiliated with Xteink or any device manufacturer**. Base firmware
© the CrossPoint Reader contributors; reading-context additions © 2026 Sam Molyneux
(both MIT). Huge shoutout to upstream and to
[diy-esp32-epub-reader](https://github.com/atomic14/diy-esp32-epub-reader), which
inspired the original project.
