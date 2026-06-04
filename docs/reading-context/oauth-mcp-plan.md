# Reading Context → Claude: OAuth + MCP Migration Plan

Migrating the reading-context feature from the shipped **skill + bearer-token + dumb
relay** design to a **remote MCP server with OAuth** for the read side.

This is the plan referenced as "Later: moving to an MCP connector" in
[`reading-context-plan.md`](reading-context-plan.md) (§"Later"), now promoted to its own
document and made concrete. It supersedes that section.

> **Status of what exists today** (all hardware-verified — see [`testing.md`](testing.md)):
> - **Relay**: Cloudflare Worker, two routes on `/c` — `POST /c` (write token) and
>   `GET /c` (read token), KV key `ctx:<slot>`, sha256 token matching against
>   `TOKENS_JSON`. Contract in [`CONTRACT.md`](CONTRACT.md).
> - **Firmware**: `lib/ClaudeContext/` (`ClaudeContextStore`, `ClaudeContextClient`),
>   `ClaudeContextSendActivity`, `ClaudeContextSettingsActivity`. Pushes the truncated
>   book-so-far. Holds relay URL + obfuscated write token; optional compile-time defaults
>   via `-DCLAUDE_DEFAULT_RELAY_URL` / `-DCLAUDE_DEFAULT_WRITE_TOKEN`.
> - **Skill**: fetches `GET /c` with a read token baked into the skill file, then answers
>   only from the fetched text, with hard-wall (truncation) + soft-wall (instruction)
>   spoiler protection.

---

## Why move at all

The shipped design works but has two known soft spots, both called out in the original
plan:

1. **The read token lives in plaintext in the skill file.** Anyone with the skill file
   can read the user's full reading history. The plan itself flags this as "the thing the
   MCP migration replaces with OAuth" ([`reading-context-plan.md`](reading-context-plan.md)
   §Privacy).
2. **Context economy is by instruction, not by construction.** The skill *asks* Claude to
   grep rather than swallow the whole file; a broad question can still pull the entire
   book into the conversation. MCP tools that return only matching/recent passages make
   small context structural.

Multi-user ("friends") also gets cleaner: identity-based isolation via login instead of
hand-minting and distributing token pairs.

### What does *not* change

- **The spoiler hard wall.** Truncation stays on the device — unread text never leaves the
  ESP32. MCP changes only how the *already-uploaded* text is queried. The hard wall is
  unaffected (and the soft wall gets stronger, since tools stop whole-book context).
- **The firmware push, in shape.** The device still pushes the truncated book-so-far over
  an authenticated HTTP POST. See "Firmware impact" — the change there is small and mostly
  cleanup.

---

## Scope of the migration (the asymmetry that drives the design)

The two sides of the relay have very different clients, so they migrate differently:

| Side | Client | Today | After migration |
|------|--------|-------|-----------------|
| **Read** (pull) | Claude, on behalf of a human in a browser/app | bearer **read token** in skill file | **OAuth 2.1** (custom connector) |
| **Write** (push) | ESP32-C3 firmware, headless, no browser | bearer **write token** (device config) | **bearer write token, kept** — see below |

**The read side moves to OAuth. The write side stays on a bearer token.** Rationale:

- The read client is Claude acting for a *person* who is already in an interactive
  browser/app context — exactly what an OAuth authorization-code flow needs. This is the
  side where a token-in-a-file is the actual privacy weakness.
- The write client is a **headless microcontroller**. It has no browser, ~380 KB RAM, and
  a slow e-ink UI. An interactive OAuth flow is the wrong tool: it would mean either an
  awkward device-authorization (RFC 8628) flow on a device with no good text entry, or
  embedding client secrets in firmware. A long-lived, **revocable, per-device bearer
  token** is the right credential for a machine identity, and it's already built and
  working.

So: keep the device's bearer write token; replace the human's read token with OAuth. This
also means the firmware change is small (Phase 4 is mostly cleanup, not a rewrite).

---

## Target architecture

```
┌──────────────┐  POST (Bearer write token)   ┌──────────────────────────────┐
│  CrossPoint  │ ───────────────────────────► │  Remote MCP server (Worker)  │
│   e-reader   │  body = truncated book-so-far │                              │
└──────────────┘                              │  • ingest endpoint (write)   │
                                              │  • MCP transport (read)      │
                                              │  • OAuth 2.1 (read auth)     │
                                              │  • KV store ctx:<slot>       │
                                              └──────────────────────────────┘
                                                        ▲   tools over MCP
                                                        │   (OAuth bearer, per-user)
                                               ┌────────┴─────────┐
                                               │   Claude (app/   │
                                               │  web) — custom   │
                                               │   connector      │
                                               └──────────────────┘
```

The Worker grows from a dumb key/value store into a small **remote MCP server** that also
keeps the write-ingest endpoint. Three concerns inside one Worker:

1. **Ingest** — authenticated write from the device (unchanged in spirit).
2. **MCP transport + tools** — what Claude calls (new).
3. **OAuth** — authorizes the MCP/read side, maps a logged-in identity → a slot (new).

### Tools (the read API, replacing `GET /c`)

Designed so the whole book never enters the conversation:

- **`get_progress()`** → `{ title, author, section, page }`. Parsed from the two header
  lines already defined in [`CONTRACT.md`](CONTRACT.md) §Body format. **No contract
  change needed.**
- **`search_reading_context(query, maxMatches?)`** → the matching passages plus a window
  of surrounding text, not the whole body. For one book of a few hundred KB, a substring /
  regex scan inside the Worker is sufficient — no vector index required initially. **No
  contract change needed.**
- **`get_recent_text(chars?)`** → the tail of the read text (what was just read), for
  "what just happened / remind me where I am." **No contract change needed.**

Optional, **requires a contract change** (see below):

- **`get_recent_chapters(n)`** / chapter-scoped search — needs section boundaries in the
  body. The current body is page-by-page prose with **no chapter delimiters**
  ([`CONTRACT.md`](CONTRACT.md): "sections 0..s ... page-by-page, blank-line separated"),
  so this can't be built without the firmware emitting section markers. Treat as a
  follow-on, not part of the MVP.

### Server-side instructions (where the skill's guidance goes)

MCP lets the server ship an `instructions` field and per-tool descriptions. The skill's
**soft wall** — "answer only from this content; do not use outside knowledge of the book
or web-search its plot, even if you recognise the title" — moves into the server
instructions and the tool descriptions, so it travels with the connector instead of
living in a skill file. This is the bulk of the "skill refinement?" TODO item: most of the
skill's value becomes server-side, and the skill either disappears or shrinks to a thin
optional companion (decision D4).

---

## Auth design (read side)

Follow the **MCP authorization spec** (the 2025-06-18 revision): the MCP server acts as an
**OAuth 2.1 Resource Server**, advertises Protected Resource Metadata (RFC 9728), supports
PKCE, and the audience of issued tokens is bound to this resource (RFC 8707 resource
indicators) to avoid token-passthrough / confused-deputy problems. Claude's custom-
connector flow discovers the metadata, performs **Dynamic Client Registration** (RFC 7591),
and runs the authorization-code-with-PKCE flow in the user's browser.

**Identity / where login happens** — decision D1 below. Two realistic options on
Cloudflare:

- **Delegated IdP** (recommended): use Cloudflare's
  [`workers-oauth-provider`](https://github.com/cloudflare/workers-oauth-provider) as the
  OAuth server, delegating the actual login to an existing IdP (GitHub or Google). This is
  the canonical Cloudflare "remote MCP server with OAuth" pattern; the provider handles
  DCR, PKCE, and token issuance, and the user logs in with an account they already have.
  The OAuth subject (e.g. GitHub user id / verified email) becomes the slot key.
- **Self-issued**: the Worker is its own authorization server with a minimal credential.
  More code, more to get wrong on the security-sensitive path; only worth it to avoid an
  external IdP dependency.

**Slot binding after migration.** The token table record changes from
`{ slot, writeHash, readHash }` to roughly:

```jsonc
{
  "slot": "me",
  "writeHash": "<sha256 of the device write token>",  // device → slot (unchanged mechanism)
  "ownerSub": "<oauth subject>"                         // human identity → slot (replaces readHash)
}
```

The read path is authorized by matching the OAuth token's subject/audience to `ownerSub`;
`readHash` is retired.

---

## Contract changes

[`CONTRACT.md`](CONTRACT.md) is the single source of truth across relay/firmware/skill and
must be updated in lockstep. This migration touches it as follows:

1. **`GET /c` retired** on the read side, replaced by MCP tools + OAuth. (Keep it during
   the transition — see phasing — then remove.)
2. **Write path**: keep `POST` with `Authorization: Bearer <write token>`. The path may
   move from `/c` to a clearer `/ingest` (decision D3); body format unchanged for the MVP.
3. **(Optional, deferred)** Add section markers to the body to enable chapter-scoped tools.
   This is a real three-sided change (firmware emits markers, server parses them, tools use
   them) and a body-format version bump — **out of scope for the MVP**, noted so the tool
   list above is honest about what's free vs not.

Everything the MVP tools need (`get_progress`, `search_reading_context`, `get_recent_text`)
is derivable from the **current** body format, so the firmware body need not change to ship
the migration.

---

## Firmware impact (small — mostly the cleanup TODO)

The device keeps pushing a truncated markdown body over an authenticated POST, so
`lib/ClaudeContext/` and the two activities stay structurally as-is. Concrete changes:

1. **Remove the compile-time default tokens** (TODO item 2 — "Remove/clean up default
   token stuff"). Drop `-DCLAUDE_DEFAULT_RELAY_URL` / `-DCLAUDE_DEFAULT_WRITE_TOKEN` and
   `ClaudeContextStore::applyCompileTimeDefaults()`
   ([`ClaudeContextStore.h:20-22`](../../lib/ClaudeContext/ClaudeContextStore.h)). These
   were a testing convenience; with a deployed server the device is configured on-device
   via `ClaudeContextSettingsActivity` (or a provisioning step), and a baked-in default
   token is a credential-handling smell. Verify nothing in `platformio.ini` /
   `platformio.local.ini` still sets them after removal.
2. **Point the relay URL at the new server** and, if D3 moves the write path, update the
   endpoint (settings field already supports an arbitrary URL — `getNormalisedUrl()` in
   [`ClaudeContextStore.h:42-43`](../../lib/ClaudeContext/ClaudeContextStore.h)).
3. **Write-token provisioning** (decision D2): the write token becomes a per-device,
   server-revocable credential. The on-device entry path
   (`ClaudeContextSettingsActivity`) already covers manual entry; no new firmware needed
   unless we want server-issued tokens.

No change to the streaming/chunked upload, the extract-to-SD-then-upload heap dance, or the
spoiler truncation logic. Re-run the firmware checks from [`testing.md`](testing.md) §2a
after the default-token removal (clang-format, `pio check`, `pio run`, i18n gen) and a
single on-hardware send to confirm the deployed-HTTPS path still round-trips.

---

## Phasing (each phase independently verifiable, like the original three-piece build)

**Phase 0 — Decisions.** Resolve D1–D4 below. No code.

**Phase 1 — Stand up the MCP server next to the existing relay.** Keep `POST` (ingest) and
`GET /c` working so the shipped skill+token path keeps functioning. Add the MCP transport +
the three MVP tools, reading the same KV `ctx:<slot>`. Verify tools with an MCP client /
inspector against a manually-seeded slot, before any auth.

**Phase 2 — Add OAuth on the read side.** Wire `workers-oauth-provider` (or the chosen
auth), advertise protected-resource metadata, map `ownerSub → slot`. Connect from Claude
as a custom connector, log in, and confirm: tools work; a second identity gets its own
(empty) slot; one user can't read another's context.

**Phase 3 — Cut over the read side.** Move the soft-wall guidance into server instructions
/ tool descriptions. Retire the read token and `readHash`; slim or remove the skill (D4).
Remove `GET /c` once nothing depends on it.

**Phase 4 — Firmware cleanup.** Remove the default-token macros, repoint/rename the write
endpoint, settle write-token provisioning. Re-verify on hardware.

**Phase 5 — Multi-user (optional).** "Friends" becomes: each person logs in (own identity →
own slot) and gets a per-device write token. No token-pair distribution.

Build order keeps the working system live throughout: the old skill+token read path is only
removed in Phase 3, *after* the MCP read path is proven in Phase 2.

---

## Decisions to make (Phase 0)

- **D1 — Identity provider.** Delegated IdP (GitHub/Google via `workers-oauth-provider`,
  recommended) vs self-issued auth. Affects how a user "logs in" to the connector and what
  `ownerSub` is.
- **D2 — Write-token lifecycle.** Keep manually-entered device tokens, or have the server
  issue/revoke per-device tokens (e.g. a small authed page that mints a token for a slot).
  Revocability is the main argument for server-issued.
- **D3 — Write endpoint path.** Keep `POST /c`, or rename to `/ingest` for clarity now that
  read is no longer a sibling `GET /c`. Cheap to do during this migration; a settings-field
  URL change on the device.
- **D4 — Fate of the skill.** Fold all guidance into MCP server instructions and delete the
  skill, or keep a thin skill as an optional companion (e.g. for users who can't add a
  connector). Recommended: server instructions primary; skill optional/deprecated.

---

## Risks and notes

- **OAuth + MCP is the security-sensitive part.** Lean on `workers-oauth-provider` rather
  than hand-rolling token issuance/PKCE. Bind token audience to this resource (RFC 8707) to
  prevent confused-deputy; never log tokens or body (carried over from
  [`CONTRACT.md`](CONTRACT.md) §Auth model).
- **Custom-connector availability.** Remote MCP connectors with OAuth are usable from the
  Claude app/web; confirm the exact "add connector" UX and any plan requirements at build
  time, since this surface moves quickly.
- **Spoiler guarantee is preserved by construction**, not weakened: truncation stays on the
  device; tools return only matching/recent passages; server instructions carry the
  no-outside-knowledge wall. Both walls from [`testing.md`](testing.md) §3 still apply and
  should be re-tested against the connector.
- **Don't regress the working path.** The shipped skill+token flow is hardware-verified;
  keep it operational until Phase 3 cutover is proven.

---

## TODO mapping

From the repo-root `TODO.md`:

| TODO item | Covered by |
|-----------|-----------|
| OAuth + MCP | This whole document (Phases 1–3, 5) |
| Remove/clean up default token stuff | Phase 4, item 1 (drop `-DCLAUDE_DEFAULT_*`) |
| skill refinement? | Phase 3 + D4 (guidance → server instructions; skill slimmed/retired) |
