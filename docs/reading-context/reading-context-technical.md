# Reading Context → Claude: Technical Companion

Companion to `reading-context-plan.md`. This document resolves the **open items to
confirm before coding** against the actual CrossPoint source, and gives concrete
integration points (file paths, function signatures, structs) for the firmware push.

> Source inspected: `crosspoint-reader/crosspoint-reader` at `master` (shallow clone).
> Line numbers are indicative and will drift; treat the symbol names as the stable anchor.

---

## Open item 1 — `progress.bin` layout (RESOLVED)

`progress.bin` is a flat little-endian record, **not** documented in `file-formats.md`
because it's written/read inline in the reader activity rather than via a versioned
struct. It is 6 bytes (older files may be 4):

| Offset | Size | Field          | Notes |
|--------|------|----------------|-------|
| 0      | u16 LE | `spineIndex`   | current chapter = index into the spine |
| 2      | u16 LE | `pageNumber`   | current page within that section |
| 4      | u16 LE | `pageCount`    | total pages in the current section (optional; absent in 4-byte files) |

**Write side** — `src/activities/reader/EpubReaderUtils.h`, `saveProgress()`:

```cpp
uint8_t data[6];
data[0] = spineIndex & 0xFF;   data[1] = (spineIndex >> 8) & 0xFF;
data[2] = pageNumber & 0xFF;   data[3] = (pageNumber >> 8) & 0xFF;
data[4] = pageCount & 0xFF;    data[5] = (pageCount >> 8) & 0xFF;
```

**Read side** — `src/activities/reader/EpubReaderActivity.cpp` (`onEnter`):

```cpp
uint8_t data[6];
int dataSize = f.read(data, 6);
if (dataSize == 4 || dataSize == 6) {
  currentSpineIndex = data[0] + (data[1] << 8);
  nextPageNumber    = data[2] + (data[3] << 8);
  if (nextPageNumber == UINT16_MAX) nextPageNumber = 0;  // in-memory sentinel; never persisted state
}
if (dataSize == 6) cachedChapterTotalPageCount = data[4] + (data[5] << 8);
```

Implications for the push:

- The two fields we need — **`spineIndex`** (chapter) and **`pageNumber`** (page within
  chapter) — are bytes 0–1 and 2–3. This is exactly the cut point.
- **Handle both 4- and 6-byte files.** Don't assume 6.
- **Guard the `UINT16_MAX` sentinel** for `pageNumber` the same way the reader does, or a
  freshly-navigated "last page of previous chapter" state would mis-truncate.
- Reading from a *file* means the push reflects the **last saved** position. If the live
  reader holds a newer unsaved position in RAM, prefer reading the in-memory
  `currentSpineIndex` / `nextPageNumber` if the push runs inside the reader activity
  (it does — see Open item 4). Reading the file is the fallback if the push ever runs
  outside the activity.

---

## Open item 2 — `sections/<n>.bin` indexing (RESOLVED)

> Unlike `progress.bin`, the `book.bin` (v5) and `section.bin` (v24) on-disk structures
> **are** documented in `docs/file-formats.md` (ImHex patterns). Use that as the
> authoritative byte-layout reference; this section covers only what the push needs.

Confirmed: section cache files are named **by spine index**. From
`lib/Epub/Epub/Section.h`, the constructor builds:

```cpp
filePath = epub->getCachePath() + "/sections/" + std::to_string(spineIndex) + ".bin";
```

So `sections/0.bin` … `sections/<spineIndex>.bin` correspond 1:1 to spine entries in
reading order. To extract "book so far" you iterate `s = 0 .. progress.spineIndex`.

### Major simplification: text extraction already exists

The plan proposed hand-rolling word-joining from the `PageLine` structs. **Not needed** —
`Section` already exposes a text API. From `lib/Epub/Epub/Section.cpp`:

```cpp
std::string Section::getTextFromSectionFile() {
  std::string fullText;
  auto p = this->loadPageFromSectionFile();   // loads the page at section.currentPage (does NOT advance)
  if (p) {
    for (const auto& el : p->elements) {
      if (el->getTag() == TAG_PageLine) {
        const auto& line = static_cast<const PageLine&>(*el);
        if (line.getBlock()) {
          for (const auto& w : line.getBlock()->getWords()) {
            if (!fullText.empty()) fullText += " ";
            fullText += w;
          }
        }
      }
    }
  }
  return fullText;
}
```

Two things to note:

- **It returns the text of a single page** — the page at `section.currentPage`.
  **Correction (confirmed during implementation):** `loadPageFromSectionFile()` seeks to
  `currentPage` but does **not** increment it (`lib/Epub/Epub/Section.cpp:313-330`), so
  calling `getTextFromSectionFile()` in a loop returns the *same* page repeatedly. To walk
  a section you must set `section.currentPage = p` before each call (the existing
  `DISPLAY_QR` handler does exactly this); to truncate, stop after the current page.
- It only emits `TAG_PageLine` content. Images (`TAG_PageImage`) and rules
  (`TAG_PageHorizontalRule`) are skipped — correct for our purposes (we want prose).

This means the push loop is page-by-page and naturally streaming (one page in RAM at a
time), which matches the ~380 KB constraint without extra effort.

### Loading a section before reading pages

`Section::loadSectionFile(...)` takes the layout/cache-busting parameters (fontId,
lineCompression, viewport dims, hyphenation, embeddedStyle, imageRendering,
focusReading). The section cache is keyed on these — they must match what the reader used
or the cache is treated as stale. The safe source for these values is the live reader /
`SETTINGS`, which is another reason to run the push **inside the reader activity** where
those values are already in hand (see Open item 4). The screenshot and SYNC handlers in
`EpubReaderActivity.cpp` show the pattern for getting at `section` and settings.

---

## Open item 3 — HTTP client + TLS (RESOLVED, with one caveat)

There are two relevant clients:

1. **`HttpDownloader`** (`src/network/HttpDownloader.{h,cpp}`) — built on
   `esp_http_client`, HTTPS verified against the CA bundle, scheme chosen from the URL.
   **But it is fetch/GET-only** (`fetchUrl`, `downloadToFile`). No POST, no custom
   request body, no arbitrary headers. Not usable for the push as-is.

2. **`KOReaderSyncClient`** (`lib/KOReaderSync/KOReaderSyncClient.cpp`) — uses
   `esp_http_client` directly and **does POST/PUT with custom headers and a body**. This
   is the template to copy.

The KOReader PUT path (the shape our push needs):

```cpp
esp_http_client_handle_t client = createClient(url.c_str(), &buf, HTTP_METHOD_PUT);
esp_http_client_set_header(client, "Content-Type", "application/json");
esp_http_client_set_post_field(client, body.c_str(), body.length());
esp_err_t err = esp_http_client_perform(client);
int httpCode = esp_http_client_get_status_code(client);
esp_http_client_cleanup(client);
```

`createClient()` (same file) sets `config.method`, small TLS buffers, and adds headers via
`esp_http_client_set_header` — including auth headers (`x-auth-user`, `x-auth-key`). Our
push would set `Authorization: Bearer <write_token>` and
`Content-Type: text/markdown` the same way, with `config.method = HTTP_METHOD_POST`.

### Caveat: `set_post_field` buffers the whole body

`esp_http_client_set_post_field(client, body, len)` hands the client a complete in-memory
buffer. That **contradicts the streaming requirement** — deep in a long book the body is
tens of thousands of words, which we must not hold in RAM all at once.

Two ways to resolve, in order of preference:

**(a) Chunked streaming upload.** Open the connection, then write the body incrementally:

```cpp
esp_http_client_set_header(client, "Transfer-Encoding", "chunked");
esp_http_client_open(client, -1);            // -1 = unknown content length
// for each page text chunk:
esp_http_client_write(client, chunk.data(), chunk.size());
esp_http_client_fetch_headers(client);       // after all writes
int code = esp_http_client_get_status_code(client);
```

This keeps only one page's worth of text resident. The Cloudflare Worker reads the body
with `await req.text()` regardless of chunking, so no relay change is needed. **This is
the recommended approach** and the one to build.

**(b) Bounded buffer + length cap.** If chunked upload proves fiddly, build the body into
a `std::string` but cap it (e.g. refuse / truncate above a sane size) and rely on the
fact that PSRAM-less C3 RAM is the hard limit. This is simpler but reintroduces the
"whole book in RAM" problem the design exists to avoid — only acceptable as a stopgap for
testing with short books.

> The "smarter accumulation" optimisation in the plan (device sends only newly-read pages,
> relay appends) sidesteps this entirely later, since each push body becomes tiny. For the
> first version, chunked streaming (a) is the clean answer.

---

## Open item 4 — settings storage + menu entry (RESOLVED)

### Where the config lives

Copy the **`KOReaderCredentialStore`** pattern
(`lib/KOReaderSync/KOReaderCredentialStore.{h,cpp}`): a singleton persisted to the SD card
as JSON, with secrets **XOR-obfuscated against the device MAC and base64-encoded** (its
own words: "not cryptographically secure, but prevents casual reading and ties credentials
to the specific device"). It already stores a **custom server URL** plus credentials —
structurally identical to what we need (`relay_url`, `write_token`).

Proposed `CrossPointContextStore` (mirroring the KOReader store):

```cpp
class CrossPointContextStore {
  std::string relayUrl;     // e.g. https://reading-relay.<you>.workers.dev/c
  std::string writeToken;   // bearer token; obfuscated at rest like KOReader password
 public:
  static CrossPointContextStore& getInstance();
  bool saveToFile() const;
  bool loadFromFile();
  void setConfig(const std::string& url, const std::string& token);
  const std::string& getRelayUrl() const;
  const std::string& getWriteToken() const;   // de-obfuscated for use
  bool isConfigured() const;                   // both fields non-empty
};
```

**As built:** the fields are entered through an on-device settings activity
(`CrossPointContextSettingsActivity`, reached via Settings → System → "CrossPoint Context"), using
`KeyboardEntryActivity` with the token masked — matching the codebase's existing
KOReader/OPDS credential convention rather than the browser web-settings path (which is a
larger, separate lift, reasonable as a future enhancement). For testing convenience a
build may also bake in defaults via `-DCROSSPOINT_DEFAULT_RELAY_URL` /
`-DCROSSPOINT_DEFAULT_WRITE_TOKEN`. The literal URL + token live in a single gitignored
`crosspoint_context_secrets.ini` (`[crosspoint_context]` section); `platformio.local.ini` references them via
`${crosspoint_context.relay_url}` / `${crosspoint_context.write_token}` interpolation, so the token exists in one
place and isn't hand-copied into a build flag. `CrossPointContextStore` fills these only for
fields not already configured on-device, and no token is hardcoded in committed source.

### The menu entry

Reader menu items are declared in
`src/activities/reader/EpubReaderMenuActivity.{h,cpp}`:

- Add an enum value to `MenuAction` (in the `.h`):
  ```cpp
  enum class MenuAction {
    SELECT_CHAPTER, FOOTNOTES, GO_TO_PERCENT, AUTO_PAGE_TURN, ROTATE_SCREEN,
    BOOKMARKS, SCREENSHOT, DISPLAY_QR, GO_HOME, SYNC, DELETE_CACHE,
    SEND_CONTEXT,   // new
  };
  ```
- Register it in `buildMenuItems()` (bump the `items.reserve(...)`), with a new `StrId`
  string id for the label (the project has 22 UI languages; add the string id and at
  least the English string):
  ```cpp
  items.push_back({MenuAction::SEND_CONTEXT, StrId::STR_CPCONTEXT_SYNC});
  ```
- Handle it where the other actions are dispatched, in
  `EpubReaderActivity.cpp` (the same `switch` that has `case ... ::SCREENSHOT:` and
  `case ... ::SYNC:`). The **`SYNC` handler is the closest template**: it checks the
  store is configured, reads the live `section` / page state, does network IO, and shows
  a result. Model `SEND_CONTEXT` on it:
  ```cpp
  case EpubReaderMenuActivity::MenuAction::SEND_CONTEXT: {
    if (CROSSPOINT_CONTEXT_STORE.isConfigured()) {
      sendReadingContext();   // see push routine below
    } else {
      // show "configure relay URL/token in Settings → CrossPoint Context" message
    }
    break;
  }
  ```

Running here is ideal because the reader activity already holds the live `epub`,
`section`, `currentSpineIndex`, `nextPageNumber`, and the layout settings needed to load
sections — so we avoid re-deriving them and get the freshest position (not just the last
file save).

There is also a **`DISPLAY_QR`** action already present — useful later if you want the
menu to show the relay/skill URL as a QR for first-time setup, reusing existing QR
helpers.

---

## Concrete push routine

Bringing the above together (pseudo-C++, error handling elided):

```cpp
void EpubReaderActivity::sendReadingContext() {
  // 1. Position: prefer live in-memory state (freshest); fall back to progress.bin.
  const int upToSpine = currentSpineIndex;
  const int upToPage  = nextPageNumber;        // guard UINT16_MAX sentinel as in onEnter

  // 2. Open chunked POST to the relay.
  auto& store = CrossPointContextStore::getInstance();
  esp_http_client_config_t cfg = {};
  cfg.url = store.getRelayUrl().c_str();
  cfg.method = HTTP_METHOD_POST;
  // small TLS buffers as in KOReaderSync createClient()
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  esp_http_client_set_header(client, "Authorization",
                             ("Bearer " + store.getWriteToken()).c_str());
  esp_http_client_set_header(client, "Content-Type", "text/markdown");
  esp_http_client_set_header(client, "Transfer-Encoding", "chunked");
  esp_http_client_open(client, -1);

  auto writeChunk = [&](const std::string& s) {
    esp_http_client_write(client, s.data(), s.size());
  };

  // 3. Header lines (title/author/progress + spoiler instruction).
  writeChunk("# " + epub->getTitle() + " — " + epub->getAuthor() + "\n");
  writeChunk("# Read up to: section " + std::to_string(upToSpine) +
             ", page " + std::to_string(upToPage) +
             ". Do not reveal anything beyond this point.\n\n");

  // 4. Walk sections 0..upToSpine, page by page, truncating the last section.
  for (int s = 0; s <= upToSpine; ++s) {
    Section sec(epubShared, s, renderer);
    sec.loadSectionFile(/* fontId, lineCompression, ... from SETTINGS */);
    const int lastPage = (s == upToSpine) ? upToPage
                                          : (sec.getCachedPageCount().value_or(0) - 1);
    for (int p = 0; p <= lastPage; ++p) {
      sec.currentPage = p;                                  // required: the call does NOT advance
      std::string pageText = sec.getTextFromSectionFile();
      if (!pageText.empty()) writeChunk(pageText + "\n");
    }
  }

  // 5. Finish and check status.
  esp_http_client_fetch_headers(client);
  int code = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  // show brief on-screen success/failure (200 = ok, 401 = bad token, etc.)
}
```

Open detail to verify when implementing: the exact arguments to `loadSectionFile()` /
how the live reader currently constructs its `Section` (copy that call verbatim from the
reader's normal page-load path so the cache key matches and you read the existing cache
rather than triggering a rebuild). **Resolved:** `getTextFromSectionFile()` does not seek
on its own — it reads `section.currentPage` and leaves it unchanged — so the loop must set
`sec.currentPage = p` before each call (shown above), not rely on an internal cursor.

---

## Relay-side note (unchanged, for completeness)

The Worker from the plan needs **no change** to support chunked uploads — `await
req.text()` assembles the body regardless of transfer encoding. The only relay-side
consideration is the body-size guard (the plan caps at 5 MB), which comfortably exceeds a
full novel's plain text (a 200k-word book is ~1.2 MB UTF-8), so no adjustment needed for
the first version.

---

## Summary of what changed vs. the plan

- **`progress.bin`** is a 6-byte (or legacy 4-byte) LE record; cut point = bytes 0–1
  (`spineIndex`) and 2–3 (`pageNumber`). Sentinel `UINT16_MAX` must be guarded.
- **Sections are spine-indexed**, and **text extraction already exists**
  (`Section::getTextFromSectionFile()`), one page per call — no hand-rolled word joining.
  It reads `section.currentPage` and does not advance, so set `currentPage = p` per page.
- **HTTP:** reuse the `esp_http_client` pattern from `KOReaderSyncClient` (POST + headers),
  **not** `HttpDownloader` (GET-only). Use **chunked upload** to preserve streaming, since
  `set_post_field` would buffer the whole body.
- **Config:** clone `KOReaderCredentialStore` (MAC-XOR obfuscation, JSON on SD, custom
  URL field) as `CrossPointContextStore`; set via an on-device settings activity (as built),
  with optional `-DCROSSPOINT_DEFAULT_*` compile-time defaults for testing.
- **Menu:** add `MenuAction::SEND_CONTEXT` + a `StrId`, register in `buildMenuItems()`,
  dispatch in `EpubReaderActivity.cpp` modelled on the existing `SYNC` handler; run the
  push inside the reader activity to use live position + layout settings.

---

## Coding standards

All changes must follow the upstream repo's conventions, so the fork stays buildable and
remains one accepted PR away from being upstreamable:

- Honour the hard constraints in `CLAUDE.md` — the **380 KB RAM ceiling**, justifying any
  new heap allocation, and not assuming an ESP-IDF/SDK API exists without checking.
- Run the pre-PR checks before committing: `./bin/clang-format-fix`, `pio check -e
  default`, `pio run -e default`.
- Match existing patterns rather than inventing new ones (e.g. the `KOReaderCredentialStore`
  store shape, the `esp_http_client` usage in `KOReaderSyncClient`, the `MenuAction` /
  `StrId` conventions) — see `docs/contributing/`.
- Add the new UI string id with at least the English string; the project ships 22 UI
  languages, so follow its localisation pattern for the label.
