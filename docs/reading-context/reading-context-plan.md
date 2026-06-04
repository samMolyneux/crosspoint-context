# Reading Context → Claude: Implementation Plan

A tool to send the contents of a book *up to the reader's current position* from a
CrossPoint e-reader to Claude, so questions like "who is X" or "draw me a map of this
battle" can be answered with confidence that nothing past the reading point is in
context.

This document covers the **skill + token** approach using a **Cloudflare Worker** relay.
A later section sketches how to migrate to an MCP connector.

---

## Goals and constraints

- **Phone-first.** The whole flow must work from the Claude mobile app with no file
  download/upload step. (Laptop works automatically if the phone path works.)
- **Spoiler-safe.** Text past the current reading position must never leave the device.
- **Small context.** Claude should search a fetched file for relevant passages, not
  swallow the whole book into the conversation on every question.
- **Low device cost.** The e-reader is an ESP32-C3 with ~380 KB usable RAM. No buffering
  whole books in memory; no constant radio use.
- **Fresh when it matters.** Context is updated by a **manual menu action** on the
  device — tapped at the moment the user turns to ask a question, so the relay is as
  fresh as possible exactly when needed.

### Why this shape

A custom connector / remote MCP server is reached from **Anthropic's cloud**, not from
the user's phone or laptop. A device on home WiFi is therefore not directly reachable.
The way to get "Claude fetches it itself" is to have the reader **push** its truncated
context out to a small public endpoint, and have Claude **pull** from that endpoint. The
endpoint is reachable by Anthropic's cloud but kept private via unguessable URLs + bearer
tokens.

---

## Architecture overview

```
┌──────────────┐   (1) manual menu tap        ┌──────────────────┐
│  CrossPoint  │ ───────────────────────────► │ Cloudflare Worker│
│   e-reader   │   POST /c  (write token)      │   relay + KV     │
│              │   body = book-so-far (md)     │                  │
└──────────────┘                               └──────────────────┘
                                                        ▲
                                                        │ (2) GET /c (read token)
                                                        │     fetched by skill
                                               ┌────────┴─────────┐
                                               │  Claude (app/web)│
                                               │  reading-context │
                                               │      skill       │
                                               └──────────────────┘
```

Three independent, separately-testable pieces:

1. **Relay** — Cloudflare Worker with KV storage, two routes, per-token slots.
2. **Firmware** — a menu action that extracts the truncated book-so-far and POSTs it.
3. **Skill** — instructions telling Claude to fetch the slot and search it.

Build and test in that order; each is verifiable with `curl` before the next exists.

---

## Piece 1 — Cloudflare Worker relay

### Responsibilities

- Accept writes from the reader (`POST /c`) authenticated by a **write token**.
- Serve reads to Claude (`GET /c`) authenticated by a **read token**.
- Store the latest pushed body per user, keyed so users never cross over.
- Reject anything unauthenticated.

### Storage model

Use **Workers KV**. One key per user, value = the latest markdown body.

- Key: `ctx:<slot>` where `slot = sha256(read_token)` (or a stable user id).
- Separate **read** and **write** tokens per user so a leaked device (write-only)
  token cannot be used to *read* reading history.
- Map both tokens → the same slot via a small config (KV or Worker secret/JSON).

For a single user you can start with one token pair hard-configured. Multi-user is just
"more entries in the token table" (see Scaling).

### Token handling

> **Superseded (kept as the original design rationale).** The shipped relay drops the
> hashing for a single-user, manually-managed setup: it compares the presented bearer token
> **directly** against the raw `writeToken` / `readToken` in `TOKENS_JSON`. See
> `CONTRACT.md` (Auth model) for the current behaviour and `secrets.local.json` for the
> single token source. The hash-table design below is the original plan, not what runs.

- Tokens are long random strings (e.g. 32 bytes, base64url).
- The Worker hashes the presented token and looks it up in a table of
  `{ write_hash, read_hash, slot }` records.
- Never log raw tokens or bodies.

### Worker code (starting point)

```js
// wrangler.toml binding: KV namespace "CTX"
// Secret: TOKENS_JSON  (JSON array of {slot, writeHash, readHash})
//   where *Hash = sha256 hex of the token.

async function sha256Hex(s) {
  const buf = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(s));
  return [...new Uint8Array(buf)].map(b => b.toString(16).padStart(2, "0")).join("");
}

function bearer(req) {
  const h = req.headers.get("Authorization") || "";
  return h.startsWith("Bearer ") ? h.slice(7) : null;
}

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    if (url.pathname !== "/c") return new Response("Not found", { status: 404 });

    const token = bearer(req);
    if (!token) return new Response("Unauthorized", { status: 401 });

    const tokens = JSON.parse(env.TOKENS_JSON);
    const hash = await sha256Hex(token);

    if (req.method === "POST") {
      const rec = tokens.find(t => t.writeHash === hash);
      if (!rec) return new Response("Unauthorized", { status: 401 });
      const body = await req.text();
      if (body.length > 5_000_000) return new Response("Too large", { status: 413 });
      await env.CTX.put(`ctx:${rec.slot}`, body);
      return new Response("OK", { status: 200 });
    }

    if (req.method === "GET") {
      const rec = tokens.find(t => t.readHash === hash);
      if (!rec) return new Response("Unauthorized", { status: 401 });
      const body = await env.CTX.get(`ctx:${rec.slot}`);
      if (body === null) return new Response("No context yet", { status: 404 });
      return new Response(body, {
        status: 200,
        headers: { "Content-Type": "text/markdown; charset=utf-8" },
      });
    }

    return new Response("Method not allowed", { status: 405 });
  },
};
```

### Deploy

```bash
npm create cloudflare@latest reading-relay
cd reading-relay
# create a KV namespace and bind it as CTX in wrangler.toml
npx wrangler kv namespace create CTX
# set the token table as a secret
echo '[{"slot":"me","writeHash":"<sha256 of write token>","readHash":"<sha256 of read token>"}]' \
  | npx wrangler secret put TOKENS_JSON
npx wrangler deploy
```

### Test (do this before any firmware work)

```bash
RELAY=https://reading-relay.<you>.workers.dev/c

# write
curl -X POST -H "Authorization: Bearer WRITE_TOKEN" \
     --data-binary @fake-book.md "$RELAY"            # -> OK

# read
curl -H "Authorization: Bearer READ_TOKEN" "$RELAY"  # -> fake-book.md contents

# unauthenticated
curl "$RELAY"                                         # -> 401
```

Once this round-trips, the relay is done.

---

## Piece 2 — Firmware: menu action + push

### Trigger

A new entry in the reading menu (same menu as screenshot / go-to-chapter), e.g.
**"Send context to Claude"**. Tapping it runs the push routine once. No timer, no
page-turn hook — the tap happens when the user turns to ask a question, so freshness is
maximal exactly when it's needed.

### What it sends

A single streamed `POST /c` with:

```
Authorization: Bearer <WRITE_TOKEN>
Content-Type: text/markdown

# <Title> — <Author>
# Read up to: section <spineIndex>, page <page>. Do not reveal anything beyond this point.

<book text, chapter 0 .. current, current chapter truncated at current page>
```

The `WRITE_TOKEN` and relay URL are stored in a persisted device config, not hard-coded.
(As built, they're entered through an **on-device settings activity** — Settings → System
→ "Claude Context" — matching the codebase's KOReader/OPDS credential convention, with
optional `-DCLAUDE_DEFAULT_*` compile-time defaults for testing.)

### Extraction

The book's parsed content already lives on the SD card under `.crosspoint/epub_<hash>/`:
`book.bin` (metadata + spine + TOC) and `sections/<n>.bin`, one per spine entry in reading
order. Current position is in `progress.bin` (`spineIndex` + `pageNumber`).

Two points that make extraction straightforward (full details — struct layouts, exact
fields, the existing text API — are in the technical companion):

- **A text-extraction API already exists** (`Section::getTextFromSectionFile()`), so there
  is no need to hand-roll word-joining from the page structures. It returns one page's
  text per call, which fits the page-by-page streaming model.
- **Page-level truncation is free.** Walk sections `0 .. spineIndex-1` fully, then in
  section `spineIndex` emit only pages `0 .. pageNumber` and stop. The page is the unit.
  (Guard the `UINT16_MAX` page sentinel — see companion.)

### Push routine (shape)

Run inside the reader activity, so the live position and layout settings are already in
hand. High level (concrete C++ — chunked-upload mechanics, the section-load call, status
handling — is in the technical companion):

```
on SEND_CONTEXT:
    if not relay configured: show "configure in Settings → Claude Context"; stop
    open chunked POST to relay_url
        headers: Authorization: Bearer <write_token>, Content-Type: text/markdown
    write header lines: "# <title> — <author>"
                        "# Read up to: section <s>, page <p>. Do not reveal beyond this."
    for s in 0 .. spineIndex:
        load section s
        last_page = (s == spineIndex) ? pageNumber : (section page count - 1)
        for p in 0 .. last_page:
            write getTextFromSectionFile()   // one page per call; streamed, not buffered
    finish; show brief success/failure
```

### Implementation notes

- **HTTP client.** Reuse the `esp_http_client` POST pattern from `KOReaderSyncClient`
  (custom headers + body) — **not** `HttpDownloader`, which is GET-only. TLS to a single
  known host is the heaviest part; test the POST in isolation first. See companion.
- **Stream, never buffer.** Emit each page as it's read; hold at most one page in RAM. Use
  a chunked upload — a plain buffered POST would hold the whole book in RAM, which the
  RAM ceiling forbids. See companion for the chunked-upload mechanics.
- **WiFi state.** The push only works with WiFi up. Because the trigger is manual and
  coincides with turning to the phone, connectivity is normally available. If the POST
  fails, show a short on-screen error so the user knows to retry.
- **Config.** Store `relay_url` and `write_token` in a persisted device config. (As built:
  an on-device settings activity, with optional `-DCLAUDE_DEFAULT_*` compile-time defaults.)

### Test

Tap the menu item, then re-run the relay `GET` — the stored body should be the real
book-so-far, truncated at the correct page, with the header lines present.

---

## Piece 3 — The skill

A small skill that fetches the slot and answers only from it, searching rather than
loading the whole file.

```markdown
---
name: reading-context
description: Answer questions about the book the user is currently reading on their
  e-reader, using only what they have read so far.
---

When the user asks about their current book:

1. If reading-context.md is not in the working directory, fetch it:
     curl -sH "Authorization: Bearer <READ_TOKEN>" https://reading-relay.<you>.workers.dev/c > reading-context.md
2. The first lines are a header giving the title, author, and how far the user has read.
3. Answer from reading-context.md. Read the file however best fits the question — grep
   for a name or term, read a chapter or passage in full, skim the header/structure
   first, or follow a thread across several sections. Prefer reading the parts you need
   rather than loading the entire file at once, but use your judgement: if a question
   genuinely needs broad context (e.g. tracing a relationship or summarising events so
   far), read as widely as it requires.
4. Do not use outside knowledge of this book, including training data or web search, even
   if you recognise the title. Do not search the web for the book, its plot, or its
   characters. If the answer isn't in the file, say it hasn't been read yet rather than
   filling it in from elsewhere.
```

### Why this keeps context manageable

The fetched file lives on disk in Claude's environment, so Claude reads from it rather
than holding the whole book in the conversation. The default leans towards reading only
the parts a question needs — a name lookup pulls in a few hundred tokens, not the whole
novel — but the model has latitude to read more widely when a question calls for it
(tracing a relationship, summarising events so far). That trades some context economy for
better answers on exactly the questions that motivated the project; a few broad questions
will pull in a lot, and a long book late on could approach limits within one conversation.
This loosening is purely about how freely the model traverses the file — the spoiler walls
below are unaffected, since there is nothing past the reading point in the file to find no
matter how widely it reads.

### Two layers of spoiler protection

- **Hard wall (truncation):** unread text physically never leaves the device, so it
  cannot be revealed.
- **Soft wall (instruction):** step 4 stops Claude drawing on its own knowledge of a
  famous book or web-searching the plot. This is an instruction, not a guarantee — for
  very well-known books a stray detail could slip through — but combined with the hard
  wall it covers both leak paths.

---

## End-to-end flow (phone)

1. Finish a passage on the reader.
2. Tap **"Send context to Claude"** → fresh push to your slot.
3. Open the Claude app, ask your question.
4. The skill fetches your slot, reads it as the question needs — a name lookup, a passage
   in full, or a thread across chapters — and answers from it.

No download, no upload. Fresh as of the tap. Context small.

---

## Privacy

"Public" means *reachable by Anthropic's cloud*, not *discoverable*. Two layers:

- **Unguessable URL** (the Worker subdomain) — not enough alone, since URLs leak into
  logs.
- **Bearer tokens** — every read and write must present a valid token; anything else gets
  401. Separate read/write tokens per user; the device only holds the write token.

The soft spot is the **read token sitting in the skill file**. Fine for personal use;
it's the thing the MCP migration replaces with OAuth.

---

## Build order / milestones

1. **Relay up and `curl`-tested** (write → read → 401). No device involved.
2. **Firmware push** to the relay; verify the stored body is correct & truncated.
   - Sub-step: test the chunked POST in isolation (auth header + a short body).
   - Sub-step: confirm the `loadSectionFile()` call matches the reader's own (cache key).
3. **Skill** added; ask a question from the phone end to end.
4. (Optional) iterate on extraction quality (paragraph joins, header wording).

You may find the single tap is fine and stop here. If it ever annoys you, a debounced
page-turn push + incremental append (relay stores accumulated text, device sends only the
newly-read pages) makes an automatic trigger affordable — but that's a later option, not
needed now.

---

## Later: moving to an MCP connector

The skill + token approach is the low-infrastructure version. An MCP connector is the
more robust answer to **privacy** (OAuth instead of a token in a file) and **multi-user
isolation** (each person connects with their own login). It also lets context stay small
*by construction* rather than by instruction.

### What changes

- The relay grows from "dumb store" into a small **remote MCP server** (still deployable
  as a Cloudflare Worker). Instead of one `GET /c`, it exposes **tools**, e.g.:
  - `search_reading_context(query)` → returns only passages matching the query.
  - `get_recent_chapters(n)` → returns the last *n* chapters read.
  - `get_progress()` → title, author, how far read.
- Because the tools return only matching/recent passages, the whole book never enters the
  conversation — small context is guaranteed by the tool design, not by asking Claude to
  grep nicely.
- **Auth becomes OAuth.** The connector holds the credential via the standard OAuth flow;
  no read token lives in a skill file. Each user signs in and is isolated by the auth
  layer.
- The reader's push is unchanged (still `POST` the truncated book-so-far). The MCP server
  indexes/serves it.

### Why not start here

More server code (MCP protocol handling, OAuth, a search index) for a single-user setup
that the skill already satisfies. Migrate when you want proper privacy without
hand-managed tokens, or when sharing with friends.

---

## Scaling to friends (optional)

The design is already keyed on a per-user token, so multi-tenancy is nearly free:

- Each person = one `{slot, writeHash, readHash}` record in the token table.
- Their reader writes to their slot (write token in device settings); their skill reads
  their slot (read token in their skill). No crossover — neither knows the other's token.
- Setup per friend: mint a token pair, put the write token in their device settings, hand
  them a skill pre-filled with their read token.

**Honest cost:** once others depend on it, it stops being "a thing I run for myself" and
becomes a small service you operate — uptime, the (small) bill, and you holding their
reading data on your host. All modest for book text and a handful of people. The MCP +
OAuth route is the cleaner long-term answer for friends, since isolation and credentials
are handled by the auth layer rather than by you distributing tokens.

---

## Implementation checkpoints (resolved — see technical companion)

These were the unknowns before inspecting the source; all are now resolved in
`reading-context-technical.md`. Kept here as a checklist:

- **`progress.bin` layout** — resolved: a 6-byte (legacy 4-byte) little-endian record;
  cut point is `spineIndex` (bytes 0–1) and `pageNumber` (bytes 2–3). Guard the
  `UINT16_MAX` page sentinel.
- **`sections/<n>.bin` indexing** — resolved: files are named by spine index
  (`sections/<spineIndex>.bin`), so iterate `0 .. spineIndex`.
- **Settings storage** — resolved: clone the `KOReaderCredentialStore` pattern (JSON on
  SD, MAC-XOR-obfuscated secret, custom-URL field) as a `ClaudeContextStore`; set via an
  on-device settings activity (as built), with optional `-DCLAUDE_DEFAULT_*` defaults.
- **HTTP client + TLS** — resolved: use the `esp_http_client` POST pattern from
  `KOReaderSyncClient`; `HttpDownloader` is GET-only. Use chunked upload to stay
  streaming.
- **Menu entry** — resolved: add `MenuAction::SEND_CONTEXT` + a `StrId`, register in
  `buildMenuItems()`, dispatch in `EpubReaderActivity.cpp` modelled on the `SYNC` handler.

---

## Potential optimisations (future)

Not needed for the manual-tap version, noted for later:

- **Smarter accumulation.** Instead of re-sending the whole book-so-far on every push,
  the relay keeps the accumulated text and the device sends only the newly-read pages
  since the last push (incremental append). Per-push payload drops to almost nothing
  regardless of book length, and the radio barely wakes — which is what makes a frequent
  or automatic trigger affordable. Truncation and reconstruction stay on the device
  (cheap, and required on-device for the spoiler guarantee); only the running history
  moves relay-side.
