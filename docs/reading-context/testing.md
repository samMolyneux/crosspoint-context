# Testing — Reading Context → Claude

How to test each section of the feature. There are three independently-verifiable pieces
(relay, firmware, skill) plus an end-to-end integration pass. Build and test them in that
order — each is checkable on its own before the next exists.

`CONTRACT.md` is the seam all three share; when in doubt about a status code, header, or the
body shape, that file is authoritative. `fake-book.md` (same folder) is the shared fixture —
firmware output should diff cleanly against its shape.

## Prerequisites

- **Tokens** — the master copy of `relayUrl`, `writeToken`, and `readToken` lives in
  `secrets.local.json` at the repo root (gitignored; matched by `*.local*`). The relay
  compares a presented bearer token **directly** against the raw `writeToken` / `readToken`
  (no hashing); those values are pasted verbatim into the relay's `.dev.vars`, the
  firmware's `claude_secrets.ini`, and the skill's `SKILL.md`.
- **Relay running locally** — `npx wrangler dev` (local KV; identical code to a deployed
  Worker). Use `--ip 0.0.0.0` when a device on the LAN needs to reach it.
- **Fixture** — `fake-book.md`, already truncated at section 2, page 3, with the two
  header lines. Used as the relay/skill test payload.

---

## 1. Relay (Cloudflare Worker)

No device involved — drive the whole contract with `curl`. Start `npx wrangler dev`, then:

```bash
RELAY=http://127.0.0.1:8787/c          # or https://reading-relay.<you>.workers.dev/c when deployed
WRITE=$(jq -r .writeToken ../../secrets.local.json)   # repo-root master reference
READ=$(jq -r .readToken  ../../secrets.local.json)

# write (authorised) — store the fixture
curl -X POST -H "Authorization: Bearer $WRITE" -H "Content-Type: text/markdown" \
     --data-binary @fake-book.md "$RELAY"           # -> 200 OK

# read (authorised) — should round-trip the fixture verbatim
curl -H "Authorization: Bearer $READ" "$RELAY"      # -> 200 + fake-book.md contents
```

Full contract matrix (each line is the expected status — see `CONTRACT.md`):

| Request | Expected |
|---------|----------|
| `POST /c` with a valid write token | `200 OK` (body stored) |
| `POST /c` with a missing/unknown token | `401 Unauthorized` |
| `POST /c` with body > 5,000,000 bytes | `413 Too large` |
| `GET /c` with a valid read token, after a write | `200` + body verbatim, `text/markdown; charset=utf-8` |
| `GET /c` with a missing/unknown token | `401 Unauthorized` |
| `GET /c` before anything has been written | `404 No context yet` |
| `PUT` / `DELETE` / other method on `/c` | `405 Method not allowed` |
| Any path other than `/c` | `404` |

```bash
# a few of the negative cases
curl "$RELAY"                                        # no token            -> 401
curl -X PUT  -H "Authorization: Bearer $WRITE" "$RELAY"   # wrong method   -> 405
curl "${RELAY%/c}/nope"                              # wrong path          -> 404
# read-before-write: restart wrangler (clears KV) then GET first          -> 404
```

The relay is "done" once write → read → 401 round-trips and the negative cases match.
**Never log raw tokens or body contents** — confirm the Worker doesn't.

---

## 2. Firmware (CrossPoint e-reader)

### 2a. Static / build checks (no hardware)

The three pre-PR checks (run from the firmware repo root):

```bash
git submodule update --init open-x4-sdk    # the SDK is a submodule; a shallow clone won't have it
./bin/clang-format-fix                      # must be clean — zero reflow of the ClaudeContext sources
pio check -e default                        # cppcheck --enable=all — expect "No defects found", exit 0
pio run   -e default                        # must build
python scripts/gen_i18n.py lib/I18n/translations lib/I18n/   # generator clean, every new StrId exists
```

Reference figures from the verified `pio run -e default` build (a baseline to compare
against, not a hard promise — they drift with other changes):

- **RAM:** 30.9% (101,252 / 327,680 B)
- **Flash:** 78.6% (5,152,185 / 6,553,600 B)

A large jump in either after a change is worth investigating against the 380 KB ceiling.

### 2b. On-device send

1. **Configure** — Settings → System → "Claude Context": set Relay URL to
   `http://<LAN-IP>:<port>/c` (the machine's **LAN IP**, not `localhost` — the ESP32 must
   reach it over WiFi; `wrangler dev` is plain HTTP locally) and paste the write token.
   (Alternatively a build can bake defaults in via `-DCLAUDE_DEFAULT_RELAY_URL` /
   `-DCLAUDE_DEFAULT_WRITE_TOKEN`. The literal URL + token live in one gitignored file,
   `claude_secrets.ini` (a `[claude]` section); `platformio.local.ini` references them with
   `${claude.relay_url}` / `${claude.write_token}` so the token isn't hand-copied.)
2. **Read** a few pages into a book so there's a non-trivial position to truncate at.
3. **Tap** the reader menu → "Send context to Claude". Watch for the on-screen success
   result (and the serial log if monitoring).
4. **Verify** from the host: `curl -H "Authorization: Bearer $READ" "$RELAY"` and confirm
   the stored body is the real book-so-far — the two header lines present, prose truncated
   at exactly the current page, nothing past it.

> **Heap note:** the send releases the EPUB before networking (extract-to-SD then upload),
> so the one-Section + TLS peak is the main heap risk to watch on real hardware. The
> baseline build leaves headroom, but monitor `ESP.getFreeHeap()` if you see resets during
> the WiFi/upload phase.

---

## 3. Skill (reading-context)

Test against a local copy of `fake-book.md` (the skill reads from the file the relay
serves). Confirm all three behaviours:

| Test | Question | Expected |
|------|----------|----------|
| **Answerable** | a character or term that appears in the read text (e.g. "Who is Mira? What is Pitch?") | answered **from the file** |
| **Unread (hard wall)** | one of the story's still-unresolved mysteries — the locked cellar, Old Tom's fate | reported as **not yet read**, not guessed |
| **No outside knowledge (soft wall)** | the same unread question | the skill does **not** fill the gap from training data or a web search, even if it recognises the title |

The two spoiler walls: the **hard wall** is truncation — unread text physically never left
the device, so it can't be revealed regardless of how the skill reads the file. The **soft
wall** is the instruction telling Claude not to draw on outside knowledge of the book. Both
must hold. Save a short transcript as the skill's test record.

---

## 4. End-to-end integration

Once all three pieces are green, the full phone-style flow:

1. `npx wrangler dev --ip 0.0.0.0` so the ESP32 can reach the relay over WiFi.
2. Firmware: Settings → Claude Context → `relayUrl = http://<LAN-IP>:<port>/c`, write token.
3. On the device: open a book, read a few pages, tap "Send context to Claude".
4. `curl -H "Authorization: Bearer $READ" "$RELAY"` → confirm the body is the real
   book-so-far, truncated at the right page, with the two header lines.
5. In Claude with the skill installed: ask a read question (answers) and an unread question
   (refused).

---

## Verification status

| Item | Status |
|------|--------|
| Relay contract (write/read/401/404/413/405) via `curl` | ✅ Verified |
| `clang-format-fix` / `pio check` / `pio run` | ✅ Verified (clean / no defects / builds) |
| i18n generator + every new `StrId` present | ✅ Verified |
| **End-to-end send on ESP32-C3 hardware** | ✅ **Verified on hardware** — send works; body reaches the relay, correctly truncated, header lines present |
| Skill: answerable / unread / no-outside-knowledge | ✅ Verified against `fake-book.md` |

### Edge cases still worth a glance

These aren't known failures — the core path is confirmed on hardware — but they're the
places a regression would most likely show up:

- **Section cache-key match.** Extraction replicates the reader's viewport (orientation +
  `screenMargin` + status-bar height) so it reads the existing section cache. If the user
  reads with **auto-page-turn** active the reader's viewport differs, so a section may miss
  cache → it is **skipped (logged), not rebuilt** (a rebuild would need CSS that isn't
  loaded on this path). Worth re-checking if a book sends with a gap.
- **Page-truncation off-by-one** at the exact current page, and the `UINT16_MAX` page
  sentinel (handled upstream by the reader before the value reaches the send code).
- **TLS path.** The local relay is plain HTTP (`wrangler dev`), so the HTTPS heap behaviour
  is only exercised once the Worker is actually deployed to `https://…workers.dev`. Re-check
  the heap during the handshake the first time you point the device at a deployed relay.

---

## Gotchas (diff against these)

- **Body-format drift** — diff firmware output against `fake-book.md`'s exact shape: line 1
  `# <Title> — <Author>` (em-dash), line 2 the `Read up to: section <s>, page <p>…` marker,
  a blank line, then prose. Only `TAG_PageLine` text (images/rules skipped).
- **Page-truncation off-by-one** — nothing past page `p` of section `s` may be present.
- **`UINT16_MAX` sentinel** — guard it the way the reader's `onEnter` does.
- **Section cache-key mismatch** — see the edge-case note above.
