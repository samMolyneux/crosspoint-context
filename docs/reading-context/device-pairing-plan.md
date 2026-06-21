# Device Pairing (Nonce Variant): No-Type Provisioning Plan

A way to get the relay URL and the write credential onto the e-reader **without typing a
long URL or token** on the button-driven e-ink keyboard. The device shows a QR (and a short
fallback code); the user finishes pairing **on their phone**. **No polling** on the device.

> **This is additive.** The existing **manual relay-URL + write-token entry**
> (`CrossPointContextSettingsActivity`) and the existing relay write/read endpoints remain
> fully available — pairing is an *alternative* provisioning path, not a replacement. See
> [Coexistence](#coexistence-with-the-existing-approach).

Companion to [`oauth-mcp-plan.md`](oauth-mcp-plan.md) (the broader read-side OAuth/MCP
migration) and [`CONTRACT.md`](CONTRACT.md) (the wire contract). This plan covers only the
**write-side provisioning** — how the device obtains its write token.

---

## Why the nonce variant

We considered three ways to remove typing (full options in
[`oauth-mcp-plan.md`](oauth-mcp-plan.md)). The nonce variant was chosen because it is the
simplest design that satisfies **all** of:

- **No polling.** The device makes **one** upfront request, then displays a QR/code and is
  done — no poll loop, no `interval`/`slow_down`/`expires_in` handling, no waiting screen.
- **The secret is never in the QR.** The QR/code carry only a short, single-use **nonce**;
  the write token `T` travels device←server over TLS in the one upfront response.
- **A no-scan fallback exists.** Because the nonce is short and human-typeable, it can be
  printed as text beside the QR — a user who can't scan types it on their phone instead.

It intentionally **departs from RFC 8628** (the Device Authorization Grant). That is fine:
the write side is not a standards-interop surface — it's our device talking to our server
with a bearer token. The **read side (Claude's connector) stays standard authorization-code
OAuth** and is unaffected. The only thing 8628 bought us was the polling return-channel,
which we are deliberately removing.

---

## Roles

| Component | Role |
|---|---|
| **E-reader** | Makes one `POST /pair/start`, stores the returned token, displays QR + nonce. Never polls. |
| **Server (Worker)** | Mints `T` + `nonce`, hosts the `/pair` verification page, commits the `writeHash → slot` binding on approval. |
| **IdP (GitHub/Google)** | Authenticates the human at the verification page → `ownerSub` (keys the slot). |
| **KV** | Pending-pairing records, the slot/credential table, the context bodies. |

**Baked into firmware:** the server **origin** only (public, not a secret) — e.g.
`https://relay.example.com`, no path. The client appends the path itself (`/pair/start`,
`/ingest`). No token baked in. The on-device URL field remains as an optional self-hoster
override, now holding just the origin.

---

## The flow

```
 E-reader                         Server                         Phone + IdP
    │                               │                                 │
    │ 1. POST /pair/start (TLS)     │                                 │
    │    client_id, device_label    │  mint T (write token)           │
    │──────────────────────────────►│  mint nonce (short, typeable)   │
    │                               │  store pair:<nonce> =           │
    │   { T, nonce,                 │   {writeHash:sha256(T),         │
    │     verification_uri,         │    slot:null, expiresAt}        │
    │     verification_uri_complete,│                                 │
    │     expires_in }              │                                 │
    │◄──────────────────────────────│                                 │
    │                               │                                 │
    │ 2. save T (CrossPoint-        │                                 │
    │    ContextStore, obfusc. SD)   │                                 │
    │ 3. show QR(_uri_complete)     │                                 │
    │    + nonce + verification_uri │                                 │
    │    "scan, or go to <url> and  │                                 │
    │     enter <nonce>, then sign in"                                │
    │ 4. DONE — no polling          │                                 │
    │   (exit screen on button)     │                                 │
    │                               │   5. GET /pair?c=<nonce>   ◄─────│ scan QR / type nonce
    │                               │      redirect → IdP login   ────►│
    │                               │   ◄── callback (ownerSub) ───────│ user signs in
    │                               │      ensure slot for ownerSub    │
    │                               │      "Pair <device_label>?"  ────►│
    │                               │   ◄── Approve ───────────────────│ user approves
    │                               │      commit writeHash → slot     │
    │                               │      consume pair:<nonce>        │
    │                               │      show "Paired ✓"        ─────►│
    │                               │                                 │
    │ ===== steady state ========== │                                 │
    │ POST /ingest  Bearer T  ──────►│  sha256(T)→slot, store ctx:<slot>
```

### Step detail

1. **`POST /pair/start`** (one request, over TLS). Body: `client_id=crosspoint-reader` and
   a non-secret `device_label` for the consent screen (e.g. `"CrossPoint • " + last 4 hex
   of MAC`).
2. **Server mints** a long random write token `T` and a short `nonce` (≈8 chars, unambiguous
   alphabet — no `0/O/1/I`). Stores `pair:<nonce> = { writeHash: sha256(T), slot: null,
   status: pending, expiresAt }` (e.g. 15-min TTL). Returns `{ T, nonce, verification_uri,
   verification_uri_complete, expires_in }`. `verification_uri_complete` = `…/pair?c=<nonce>`
   (what the QR encodes). **`T` is in this response body only — never in the QR.**
3. **Device** saves `T` immediately via `CrossPointContextStore`, then renders the QR with the
   existing `QrUtils::drawQrCode(renderer, bounds, verification_uri_complete)` plus the
   `nonce` and `verification_uri` as text.
4. **Device is done.** No polling. The screen can be dismissed with a button.
5. **Human** scans the QR (or opens the URL and types the nonce). `/pair` looks up the
   pending record, requires IdP login (standard browser auth-code redirect for the
   *person*), learns `ownerSub`, ensures that identity has a slot, and shows a per-device
   consent naming the `device_label`.
6. **On Approve**, the server commits the durable binding `writeHash → slot`, consumes/deletes
   `pair:<nonce>`, and shows "Paired ✓" on the phone.
7. **Steady state:** every push is the existing `POST /ingest` with `Bearer T`; the server
   hashes it to a slot and stores `ctx:<slot>`.

### Implicit confirmation (no polling needed)

The device never learns the binding result directly. It doesn't need to: the **first real
"Sync to CrossPoint Context" doubles as the check** — it succeeds once bound, or returns `401`,
on which the device shows *"Finish pairing on your phone, then try again."* If the nonce TTL
lapsed before approval, the saved `T` is dead and the same 401 path prompts a re-pair.

---

## No-scan fallback (very short)

Beside the QR, the device prints the **short URL** (`r.ex/pair`) and the **nonce**
(`WDJB-MJHT`). A user who can't scan opens that URL on their phone, types the nonce, signs
in — identical from there. This is the same "QR *or* short code" duality, now with no
polling. (This fallback is *why* the nonce variant was chosen over putting the token
directly in the QR: a long token can't be typed; a short nonce can.)

---

## Server: endpoints and storage

New (pairing):

- `POST /pair/start` — mint `T` + `nonce`, store the pending record, return them.
- `GET/POST /pair` — verification page: nonce lookup → IdP login → consent → commit binding.

Storage (KV):

- `pair:<nonce>` → `{ writeHash, slot, status, expiresAt }` (pending; consumed on approve,
  auto-expires otherwise).
- `cred:<writeHash>` → `{ slot }` (durable write credential, written on approve) — the
  dynamic equivalent of the legacy `TOKENS_JSON` write entries.
- `ctx:<slot>` → the stored body (**unchanged**).

**Storage model differs from the open relay on purpose.** The open relay stores tokens **raw**
(`{slot, writeToken, readToken}` in `TOKENS_JSON`, no hashing — [`CONTRACT.md`](CONTRACT.md)
explains why: the token already exists in plaintext in the firmware + skill). The pairing flow
is different — the server **mints** `T`, so its plaintext lives only on the device — so this
side stores only `sha256(T)` (`cred:<writeHash>`); a KV leak exposes no usable token. What
*does* carry over from [`CONTRACT.md`](CONTRACT.md): **never log `T`, the nonce, or bodies.**

---

## Firmware changes

Mostly wiring around code that already exists.

**Reused:**
- QR encode/render — `QrUtils::drawQrCode` ([`src/util/QrUtils.h:12`](../../src/util/QrUtils.h)).
- HTTP — the `esp_http_client` pattern in `CrossPointContextClient` (one small HTTPS POST).
- Token-at-rest — `CrossPointContextStore` (obfuscated-on-SD), used to save the returned `T`
  and URL ([`lib/CrossPointContext/CrossPointContextStore.h`](../../lib/CrossPointContext/CrossPointContextStore.h)).

**New:**
- A new dedicated activity, **`CrossPointPairingActivity`**: do `POST /pair/start`, save `T`,
  render QR + nonce + URL, exit on button. **No poll loop, no timers.**
- A **"Pair with CrossPoint Context" menu item** in `CrossPointContextSettingsActivity` that launches it via
  the same `startActivityForResult` pattern the settings menu already uses for
  `KeyboardEntryActivity` — keeping the full-screen QR/network flow out of the list-menu
  activity.
- Server **origin** as a compile-time constant (public); the client appends `/pair/start`,
  `/ingest`. Keep the URL override field (now holds an origin, not a full endpoint).
- `T` is server-minted, so the device does **not** need `esp_random()` in this variant.

No change to the streaming upload, the extract-to-SD-then-upload heap dance, or the spoiler
truncation. Re-run the firmware checks in [`testing.md`](testing.md) §2a after changes.

---

## Coexistence with the existing approach

Both provisioning paths and both wire paths stay live in this phase:

- **Manual entry kept.** `CrossPointContextSettingsActivity` continues to accept a typed relay
  URL + write token. Pairing is an additional menu option, not a replacement.
- **Both write tokens accepted.** The ingest handler resolves a presented bearer token to a
  slot by checking **either** the legacy static `TOKENS_JSON` write tokens (raw match) **or**
  the new dynamic `cred:<sha256(token)>` records (hashed lookup). A manually-provisioned token
  and a paired token are indistinguishable downstream — both just yield a slot.
- **Both write routes alive.** New pairings target `POST /ingest`, but the relay keeps the
  legacy `POST /c` route mapped to the same ingest handler, so already-configured devices keep
  pushing without a firmware update.
- **Legacy read path kept.** The existing `GET /c` + read-token skill flow keeps working. It
  isn't deleted — it becomes the self-hosting reference in the **open relay repo**; only the
  *hosted* product stops depending on it (read-side cutover in
  [`oauth-mcp-plan.md`](oauth-mcp-plan.md), Phase 3 there), **not** this plan.
- **No forced migration.** Existing users keep their typed token until they choose to
  re-pair.

Dependency note: pairing binds to a **slot via IdP login**, so it relies on the server's
slot model + IdP from [`oauth-mcp-plan.md`](oauth-mcp-plan.md) (its Phase 2 — OAuth +
`ownerSub → slot`) and is sequenced **after** that lands. Until then, the manual token +
relay path is the route — which is exactly why it stays fully functional.

---

## Security notes

- `T` travels only device←server over TLS and is stored `sha256` server-side / obfuscated
  on-device. **`T` is never in the QR.**
- `nonce` is short-lived, single-use, and bindable **only by an authenticated owner** at
  `/pair`.
- **Binding-confusion risk:** someone who shoulder-surfs the nonce off the screen could,
  before the owner, sign in as *themselves* and bind the device's `T` to *their* slot. They
  still **cannot read** the owner's context (reads are OAuth-gated on the owner's slot) and
  **cannot push** (they don't hold `T`); the only effect is the owner's pushes misdirecting
  to an empty foreign slot, which the owner notices (context never updates) and fixes by
  re-pairing. Mitigations: short TTL, single-use nonce, the `device_label` shown on the
  consent screen so the owner confirms it's their device, "first approve wins." Acceptable
  for personal use; documented here so it's a known, bounded risk.
- Rate-limit `/pair/start` and `/pair`. **Never log `T`, nonce, or bodies.**

---

## Phasing

**Prerequisite:** the IdP + `ownerSub → slot` model from [`oauth-mcp-plan.md`](oauth-mcp-plan.md)
(its Phase 2). Pairing's approval reuses that owner login and slot binding, so it lands *after*
the OAuth/slot work — not before.

1. **Server pairing endpoints** — `POST /pair/start`, `GET/POST /pair`, the KV records, and
   dual-token resolution in ingest. Verify with `curl` + a browser before touching firmware:
   start a pairing, complete `/pair` in a browser, confirm a push with the minted `T` lands.
2. **Firmware pairing activity** — the one upfront POST, save `T`, render QR + nonce, exit.
   Verify the QR scans, the short-code fallback works, and the first push round-trips.
3. **Coexistence check** — confirm a manually-entered token *and* a paired token both work
   against the same ingest endpoint; confirm the legacy `GET /c` skill flow is untouched.
4. **(Later, separate)** Full read-side cutover — retire the *hosted* read token — see
   [`oauth-mcp-plan.md`](oauth-mcp-plan.md) Phase 3.

---

## TODO mapping

| TODO item | This plan |
|-----------|-----------|
| OAuth + MCP | Provides the no-type **write-side** provisioning; read-side stays in [`oauth-mcp-plan.md`](oauth-mcp-plan.md) |
| Remove/clean up default token stuff | Replaces the need for `-DCROSSPOINT_DEFAULT_WRITE_TOKEN`; keep only the public default **URL** |
| skill refinement? | Unaffected here (read side) |
