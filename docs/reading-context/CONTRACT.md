# Wire Contract — Reading Context → Claude

**This file is the single source of truth for the seam between the three pieces (relay,
firmware, skill). Do not change it without updating all three sessions.** Single-user:
exactly one `{slot, writeToken, readToken}` record.

## Endpoints

Base path: `/c` (the relay serves only this path; anything else → 404).

### `POST /c` — write (the e-reader pushes)
- Header: `Authorization: Bearer <WRITE_TOKEN>` (required)
- Header: `Content-Type: text/markdown`
- Transfer-Encoding: `chunked` is allowed (firmware streams page-by-page). The relay
  assembles the full body regardless (`await req.text()`).
- Body: the markdown document defined under **Body format** below.
- Responses:
  - `200` `OK` — stored.
  - `401` `Unauthorized` — missing/unknown token (not the known `writeToken`).
  - `413` `Too large` — body > 5,000,000 bytes.

### `GET /c` — read (Claude's skill pulls)
- Header: `Authorization: Bearer <READ_TOKEN>` (required)
- Responses:
  - `200` — returns the stored body verbatim, `Content-Type: text/markdown; charset=utf-8`.
  - `401` `Unauthorized` — missing/unknown token (not the known `readToken`).
  - `404` `No context yet` — nothing has been POSTed for this slot.

### Other methods → `405 Method not allowed`.

## Auth model
- Tokens are long random base64url strings (32 bytes). Two distinct tokens: **write**
  (held by the device) and **read** (held by the skill).
- The relay compares the presented bearer token **directly** against `writeToken` /
  `readToken` in `TOKENS_JSON` (single-user, manual token management — no hashing). The
  raw tokens already live in the firmware and skill; the relay's copy sits in a Cloudflare
  Worker secret (encrypted at rest), so a stored hash would only protect one of several
  plaintext copies. `TOKENS_JSON` is never logged.
- `TOKENS_JSON` = `[{ "slot": "me", "writeToken": "<token>", "readToken": "<token>" }]`.
- KV key for the body: `ctx:<slot>` (here `ctx:me`).
- **Never log raw tokens or body contents.**
- Master copy of the tokens: `crosspoint-context/secrets.local.json` (gitignored) — the one
  place values are edited, then pasted verbatim into the consumers (`.dev.vars`,
  `claude_secrets.ini`, `SKILL.md`).

## Body format

The body the firmware writes and the skill reads is exactly:

```
# <Title> — <Author>
# Read up to: section <s>, page <p>. Do not reveal anything beyond this point.

<book text: sections 0..s in reading order, page-by-page, the last section (s)
truncated after page p; one page's prose per emitted chunk, blank-line separated>
```

- Line 1: title and author, em-dash separated.
- Line 2: the truncation marker + spoiler instruction. `<s>` = spineIndex, `<p>` =
  pageNumber (0-based) of the current/last-read page.
- Then a blank line, then the prose. Only `TAG_PageLine` text is included (images/rules
  skipped). Truncation is at page granularity — nothing past page `p` of section `s` is
  present.

See `fake-book.md` for a concrete example in this exact shape; firmware output should diff
cleanly against that shape (header lines present, truncated at the stated page).

## Local-run note
For now the relay runs via `npx wrangler dev` (local KV) — identical code to a deployed
Worker. The skill and firmware point at the local URL; firmware must use the machine's
**LAN IP** (not `localhost`) to be reachable from the ESP32 over WiFi. `wrangler dev` is
plain HTTP locally, so the firmware TLS path is only exercised once actually deployed.
