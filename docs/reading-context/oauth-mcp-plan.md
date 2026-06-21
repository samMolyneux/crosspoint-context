  # Reading Context → Claude: OAuth + MCP Migration Plan

Migrating the reading-context feature from the shipped **skill + bearer-token + dumb
relay** design to a **remote MCP server with OAuth** for the read side.

This is the plan referenced as "Later: moving to an MCP connector" in
[`reading-context-plan.md`](reading-context-plan.md) (§"Later"), now promoted to its own
document and made concrete. It supersedes that section.

> **Status of what exists today** (all hardware-verified — see [`testing.md`](testing.md)):
> - **Relay**: Cloudflare Worker, two routes on `/c` — `POST /c` (write token) and
>   `GET /c` (read token), KV key `ctx:<slot>`, **raw** token comparison against
>   `TOKENS_JSON = [{slot, writeToken, readToken}]` (no hashing — see `CONTRACT.md` for the
>   rationale). Contract in [`CONTRACT.md`](CONTRACT.md).
> - **Firmware**: `lib/CrossPointContext/` (`CrossPointContextStore`, `CrossPointContextClient`),
>   `CrossPointContextSendActivity`, `CrossPointContextSettingsActivity`. Pushes the truncated
>   book-so-far. Holds relay URL + obfuscated write token; optional compile-time defaults
>   via `-DCROSSPOINT_DEFAULT_RELAY_URL` / `-DCROSSPOINT_DEFAULT_WRITE_TOKEN`.
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

## Repository split (open relay / closed MCP)

The work lands in **two repos with different licenses**, which is what fixes the fate of the
static-token model and the skill:

| Repo | Visibility | Contains | Auth model |
|------|-----------|----------|------------|
| **Relay** | **Open source** | The simple Cloudflare Worker — `POST /ingest` (+ legacy `POST /c`), `GET /c`, KV `ctx:<slot>`, **static `TOKENS_JSON`** write tokens, and the reading-context **skill** as a worked example | bearer write token + bearer read token (manual) |
| **MCP** | **Closed source** | The hosted product — OAuth (delegated GitHub/Google login), the MCP server + read tools, dynamic **pairing** (`/pair/start`, `/pair`, `cred:<sha256(T)>`), `ownerSub → slot` identity binding | OAuth 2.1 (read) + server-minted, revocable write tokens |

So the **open relay is the self-hosting reference**: minimal, no OAuth, no MCP, no pairing —
clone it, set a token pair, point the device at it, use the bundled skill. The **closed MCP
repo is the hosted offering** that adds identity, queryable tools, and typing-free pairing on
top of the same KV/`ctx:<slot>` storage shape.

The **firmware** (open, in this e-reader repo) talks to **both**: manual token entry targets a
self-hosted open relay; pairing targets the hosted closed MCP. The two provisioning paths
already coexist in [`device-pairing-plan.md`](device-pairing-plan.md).

> **Note — to resolve before the relay goes public.** The relay repo currently reaches into
> the firmware repo by relative path (its source/README/scripts reference
> `../crosspoint-context/CONTRACT.md`, `secrets.local.json`, and `fake-book.md`). That coupling
> won't hold once the relay is a standalone public repo — it needs sorting out as part of the
> open-sourcing, but the approach is left open here.

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
skill's value becomes server-side. The skill is **not** the hosted read path — Claude reads
via the connector with no skill required — but it lives on in the **open relay repo** as a
worked example, demonstrating the `GET /c` + read-token contract and the hard-wall/soft-wall
spoiler discipline end-to-end for self-hosters.

---

## Auth design (read side)

Follow the **MCP authorization spec** (the 2025-06-18 revision): the MCP server acts as an
**OAuth 2.1 Resource Server**, advertises Protected Resource Metadata (RFC 9728), supports
PKCE, and the audience of issued tokens is bound to this resource (RFC 8707 resource
indicators) to avoid token-passthrough / confused-deputy problems. Claude's custom-
connector flow discovers the metadata, performs **Dynamic Client Registration** (RFC 7591),
and runs the authorization-code-with-PKCE flow in the user's browser.

**Identity.** Login is delegated to an existing IdP via Cloudflare's
[`workers-oauth-provider`](https://github.com/cloudflare/workers-oauth-provider) — the
canonical Cloudflare "remote MCP server with OAuth" pattern. The provider fronts the OAuth
surface Claude connects to and handles DCR, PKCE, and token issuance; the human login itself
is delegated upstream to **GitHub or Google** (both offered; the user picks one at the login
screen). No passwords are stored and there is no bespoke authorization server to secure. The
OAuth subject becomes the slot key, **namespaced by provider** (`github:<id>` / `google:<sub>`)
so the two can't collide. One consequence to surface in the UX: a user must log in with the
*same* provider on the pairing-approval page and on the Claude connector to land on the same
slot.

**Slot binding.** A slot is keyed by the logged-in identity — `ownerSub → slot` — which
replaces the read token entirely; the hosted side issues no read token. The *device* binds
separately via its write credential. The three KV relationships:

```jsonc
ownerSub:<oauth subject>  → slot     // human identity → slot (replaces the read token)
cred:<sha256(T)>          → slot     // device credential → slot (server-minted at pairing)
ctx:<slot>                → body     // the stored book-so-far (unchanged)
```

The read path is authorized by matching the OAuth token's subject/audience to `ownerSub`. The
device side is detailed in [Write-token model](#write-token-model) below.

---

## Write-token model

The device authenticates its push with a long-lived bearer token. Two provisioning paths
coexist, mapping onto the repo split:

- **Self-hosted (open relay): static, raw.** `TOKENS_JSON = [{slot, writeToken, readToken}]`
  baked into the Worker config; a presented token is compared directly, no hashing — the token
  already lives in plaintext in the firmware and skill, so a stored hash would protect only one
  of several copies. Provisioned by typing the token into `CrossPointContextSettingsActivity`;
  revoked by editing the config and redeploying. Deliberately minimal — no minting endpoint, no
  runtime management UI.
- **Hosted (closed MCP): dynamic, hashed.** The server mints the token `T` during pairing (see
  [`device-pairing-plan.md`](device-pairing-plan.md)) and stores only `cred:<sha256(T)> → {slot}`.
  Because the server mints `T`, its plaintext lives only on the device, so hashing means a store
  leak exposes no usable token. Each device gets its own record, individually revocable at
  runtime with a single `KV.delete` — which makes lost-device and re-pair trivial and touches
  nothing else (not the read side, not `ctx:<slot>`, not other devices).

The ingest handler resolves a presented bearer token to a slot via **either** a raw `TOKENS_JSON`
match **or** a hashed `cred:<sha256(token)>` lookup, so a manually-configured device and a paired
device are indistinguishable downstream.

---

## Contract changes

[`CONTRACT.md`](CONTRACT.md) is the single source of truth across relay/firmware/skill and
must be updated in lockstep. This migration touches it as follows:

1. **`GET /c` retired** on the read side, replaced by MCP tools + OAuth. (Keep it during
   the transition — see phasing — then remove.)
2. **Write path**: keep `POST` with `Authorization: Bearer <write token>`. The path is
   `/ingest`; the device stores the origin only and the client appends the path. Body format
   unchanged for the MVP. `CONTRACT.md` is updated to `POST /ingest` when this route lands
   (Phase 1).
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
`lib/CrossPointContext/` and the two activities stay structurally as-is. Concrete changes:

1. **Remove the compile-time default _token_** (TODO item 2 — "Remove/clean up default
   token stuff"). Drop `-DCROSSPOINT_DEFAULT_WRITE_TOKEN` and the token branch of
   `CrossPointContextStore::applyCompileTimeDefaults()`
   ([`CrossPointContextStore.h:20-22`](../../lib/CrossPointContext/CrossPointContextStore.h)) — a
   baked-in default token is a credential-handling smell. **Keep `-DCROSSPOINT_DEFAULT_RELAY_URL`**:
   the base URL is public (not a secret), and the new pairing flow relies on it being baked in
   so the user never types it (see [`device-pairing-plan.md`](device-pairing-plan.md)). Verify
   nothing in `platformio.ini` / `platformio.local.ini` still sets the token default after removal.
2. **Point the baked-in URL at the new server.** The store holds the **origin only** and the
   client appends `POST /ingest`, so `getNormalisedUrl()`
   ([`CrossPointContextStore.h:42-43`](../../lib/CrossPointContext/CrossPointContextStore.h)) returns the
   scheme-normalised origin without a path, and `CrossPointContextClient::postFile` appends `/ingest`
   before calling `esp_http_client_set_url`. The on-device URL field stays as an optional
   self-hoster override — now a short origin, not a full endpoint.
3. **Write-token provisioning.** The device supports both paths from the
   [Write-token model](#write-token-model): manual entry via `CrossPointContextSettingsActivity`
   (for a self-hosted relay) and the typing-free pairing flow that receives a server-minted
   token (for the hosted product, see [`device-pairing-plan.md`](device-pairing-plan.md)).
   Manual entry already exists; pairing is the one genuinely new firmware activity.

No change to the streaming/chunked upload, the extract-to-SD-then-upload heap dance, or the
spoiler truncation logic. Re-run the firmware checks from [`testing.md`](testing.md) §2a
after the default-token removal (clang-format, `pio check`, `pio run`, i18n gen) and a
single on-hardware send to confirm the deployed-HTTPS path still round-trips.

---

## Phasing (each phase independently verifiable, like the original three-piece build)

**Phase 1 — Stand up the MCP server next to the existing relay.** Keep `POST` (ingest) and
`GET /c` working so the shipped skill+token path keeps functioning. Add the MCP transport +
the three MVP tools, reading the same KV `ctx:<slot>`. Verify tools with an MCP client /
inspector against a manually-seeded slot, before any auth.

**Phase 2 — Add OAuth on the read side.** Wire `workers-oauth-provider` (or the chosen
auth), advertise protected-resource metadata, map `ownerSub → slot`. Connect from Claude
as a custom connector, log in, and confirm: tools work; a second identity gets its own
(empty) slot; one user can't read another's context.

**Phase 3 — Cut over the *hosted* read side.** In the closed MCP product, move the soft-wall
guidance into server instructions / tool descriptions; the hosted read path is OAuth + tools,
so the read token is no longer part of it. **`GET /c`, the read token, and the skill are not
deleted — they stay in the open relay repo** as the self-hosting reference. "Retire" here means
*the hosted product stops depending on them*, not removal from the codebase.

**Phase 4 — Firmware cleanup.** Remove the default write-token macro (keep the baked-in URL),
repoint/rename the write endpoint via that compile-time constant, settle write-token
provisioning. Re-verify on hardware.

**Phase 5 — Multi-user (optional).** "Friends" becomes: each person logs in (own identity →
own slot) and gets a per-device write token. No token-pair distribution.

Build order keeps the working system live throughout: the hosted product stops depending on
the skill+token read path only in Phase 3, *after* the MCP read path is proven in Phase 2 —
and even then that path lives on in the open relay repo for self-hosters.

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
| Remove/clean up default token stuff | Phase 4, item 1 (drop `-DCROSSPOINT_DEFAULT_*`) |
| skill refinement? | Phase 3 (guidance → server instructions; skill retained as the open-relay example) |
