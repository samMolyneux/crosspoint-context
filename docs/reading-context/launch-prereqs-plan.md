# Launch Prerequisites — Infra, Identity Verification & Privacy

Self-contained plan for the **external / legal / infra** gates that must clear before the
CrossPoint Context reading feature can be offered to strangers. Companion to
[`public-release-plan.md`](public-release-plan.md) — this doc is the deep-dive on its blocker
items **#2 (Google OAuth verification)** and **#3 (privacy policy + data-deletion path)**, plus
a newly-added foundational task: **stand the public service up on a dedicated Cloudflare account
and a real domain** (today it runs on a personal account at a `workers.dev` subdomain).

**Snapshot:** 2026-06-22. The server/firmware hardening (#1, #4, #5, #6, #9) is done and the
full pair → ingest → MCP → delete/revoke pipeline is verified live. What remains before a public
launch is mostly *not code* — it's infrastructure ownership, a third-party verification clock,
and published legal text. Treat this as the long-lead-time track and start it first.

**All blocking decisions are settled** (see [Decisions](#decisions-settled) at the bottom) and
baked into the steps. Manual Cloudflare / Google / GitHub dashboard work is called out inline as
**🖱 Dashboard** walkthroughs.

**Progress (2026-06-22):** account = **stay in `mount-payments`, free plan** (A). Domain
**`crosspoint-context.com` registered** (A.1 ✅). Fresh `CTX` namespace
`b17f345c47754833bcd3738220f41d04` created (A.2 ✅). **B code/config done & verified** (B.1 +
B.3 + #10): `wrangler.jsonc` repointed to the new `CTX` + `mcp.crosspoint-context.com` custom-domain
route, owner `vars` dropped, `ensureSlot`/`types.ts` de-owner'd, `platformio.ini` origin →
`https://mcp.crosspoint-context.com`, README/deploy.sh host strings updated — `tsc` + `wrangler
deploy --dry-run` clean. **Deployed (B.4 ✅)** — custom domain live (valid TLS), `verify-mcp.sh`
passes against `mcp.crosspoint-context.com` (DCR, both OAuth providers, RFC 9728 metadata all self-
reference the new host). OAuth callback URLs added to both apps. **Remaining in B:** real login
round-trip check + re-flash `gh_release` and verify device pairing on the new origin (hardware);
flip `workers_dev: false` once confident.

**C DONE (2026-06-22):** homepage (`GET /`) + privacy policy (`GET /privacy`) in `mcp src/pages.ts`,
wired in `default-handler.ts`, apex + `www` custom-domain routes — deployed & live (HTTP 200, valid
TLS, policy matches code: 90d TTL, 5 MB, `/account` delete/revoke, retained-pointer caveat).
Contact `privacy@crosspoint-context.com` via Cloudflare Email Routing → `sam@molyneuxs.net`
(verified delivering). C.3 `/privacy` footer link added to login + both `/account` pages
(`PRIVACY_FOOTER` in `http.ts`).

**Robustness fix (2026-06-22):** bare/malformed `/authorize` threw a Worker 1101 (unhandled
`parseAuthRequest` exception). Wrapped it → clean 400, plus a top-level try/catch in `index.ts`
fetch so no handler exception ever surfaces a raw Cloudflare 1101 (logs path, returns generic 500).

**D in progress (2026-06-22):** OAuth login now works end-to-end through Claude for **both** GitHub
(client `Ov23lisiyOdNhQsfxdf8`) and Google (`776168085215-…ij4d3tqh0bavsr586uscs255k9oa3jri`) — the
read path is live.
- **Gotcha resolved (not a deploy bug):** the Worker derives the OAuth `redirect_uri` from the
  *request host* (`url.origin`), and only `mcp.crosspoint-context.com/callback/{github,google}` is
  registered at the providers. Connecting Claude to the apex or the old `workers.dev` host made the
  Worker send a callback neither provider had → `redirect_uri_mismatch` on both. Fix: the Claude
  connector must point at **`https://mcp.crosspoint-context.com/mcp`** exactly. (Verified Google
  accepts our registered redirect_uri via a direct authorize probe.)
- **Remaining for D:** configure the OAuth consent screen (home `https://crosspoint-context.com`,
  privacy `…/privacy`, authorized domain `crosspoint-context.com`, support + developer emails,
  scopes `openid email profile`, skip logo for speed), **publish**, and **submit for verification**.
  Soft launch is possible now (owner + ~100 users, unverified warning) while the review runs.

**END-TO-END VERIFIED (2026-06-24):** device synced a real book to the new `CTX`
(`ctx:u_7543…`, 165 KB, *Tom Swan and the Last Spartans-2*, read-to section 5/p332 with spoiler
guard); device re-paired against the new namespace; Claude read it via the **GitHub**-connected MCP
respecting the boundary. Full pair → ingest → read loop works on the live stack. **Note:** GitHub
and Google are separate accounts/slots now (no owner special-casing) — device is paired under
GitHub (`u_7543…`); Google resolves to a different empty slot. Connect Claude with the **same
provider the device is paired under**.

---

## Why these are one plan

They form a strict dependency chain — you cannot meaningfully start the later ones until the
earlier ones exist:

```
A. Domain + fresh CTX (in mount-payments) (foundation: a stable, owned public URL)
        │
        ├─► B. Worker on custom domain; baked origin + OAuth callbacks repointed
        │
        ├─► C. Public homepage + privacy policy hosted ON that domain        (= #3)
        │
        └─► D. Google OAuth verification, citing the domain + homepage + policy  (= #2)
                 ↑ longest external lead time (days–weeks of Google review)
```

The domain is a hard prerequisite for **both** #2 and #3: Google's consent-screen verification
requires an *authorized domain* you own plus a homepage and privacy-policy URL on it, and the
privacy policy itself needs a permanent home. So: **A → B → C → submit D as early as possible.**

This is also the moment to land **#10 (retire the `OWNER_GITHUB_ID → me` migration cruft)** —
a fresh account starts with an empty `CTX`, so there's no legacy `me` slot to preserve and
everyone (including the owner) starts on a uniform `u_<uuid>` slot. **Decided: do #10 as part of
B.**

---

## Current state (what the move has to repoint)

Verified from config, so the migration checklist is concrete, not generic. The MCP server lives
in a **separate private repo** (`~/Projects/crosspoint-context-mcp`); the firmware origin lives
in this repo (`platformio.ini`).

| Thing | Current value | File |
|---|---|---|
| CF account | `233792cdcb32ce4bd2252d013c45d506` (mount-payments, personal) | `mcp wrangler.jsonc:account_id` |
| Public host | `crosspoint-context-mcp.mount-payments.workers.dev` (`workers_dev: true`) | `mcp wrangler.jsonc` |
| KV `CTX` | `e6752fe976de4118a3a22a405e29bf53` — **shared with the open relay** | `mcp wrangler.jsonc:kv_namespaces` |
| KV `OAUTH_KV` | `b9b0bc0517af47e89af557eda1ca8893` | `mcp wrangler.jsonc:kv_namespaces` |
| Owner binding | `OWNER_GITHUB_ID=44400632`, `OWNER_SLOT=me` | `mcp wrangler.jsonc:vars` |
| Rate-limit bindings | namespace_id 1001–1005 (arbitrary ints, account-local) | `mcp wrangler.jsonc:unsafe` |
| Secrets | `TOKENS_JSON`, `GITHUB_CLIENT_ID/SECRET`, `GOOGLE_CLIENT_ID/SECRET` | `wrangler secret` (not committed) |
| Baked firmware origin | `public_origin = …mount-payments.workers.dev` | `platformio.ini:14` (used by `gh_release`/`_rc`/`slim` at :91/:100/:109) |
| OAuth callback URLs | `<host>/callback/github`, `<host>/callback/google` | GitHub OAuth App + Google OAuth client |

KV is **account-scoped** — namespaces don't move between accounts; new ones get new ids. Rate-limit
`namespace_id`s are arbitrary per-account integers and can be reused as-is.

---

## A. Domain + fresh `CTX`  *(foundation — do first)*

**Account decision (2026-06-22): stay in the existing `mount-payments` account
(`233792cdcb32ce4bd2252d013c45d506`), free plan.** A separate account proved more friction than
it's worth here: Cloudflare has no "add account" button (account = a sign-up), and Gmail `+` aliases
are rejected as the same identity, so a second account would need a whole separate email. The public
service **already runs in this account today**, so this is the status quo — we just add an owned
domain and isolate the data with a fresh `CTX` namespace. Accepted trade-offs: the public service
shares the account (blast radius) and the **account-wide free KV quota** with `mount-payments`.

This collapses most of the original Section A — `OAUTH_KV`, the Worker secrets, and the
public-release-plan #4/#5 rate-limit bindings + guardrails **already exist in this account** and
do **not** need recreating. Only two things are genuinely new: **the domain** and **a fresh `CTX`
namespace**.

> **⚠️ The free-plan ceiling — KV writes: 1,000/day, account-wide and now *shared* with
> `mount-payments`.** Each device `/ingest` push = one `ctx:<slot>` write; each new user adds a
> one-time `ownerSub:` write and each pairing a `cred:` write. Reads are 100k/day (generous — MCP
> reads are ~1/query) and storage is 1 GB (»5 MB × users). So *writes* are the wall, ~matching the
> ~100-user Google soft-launch cap — but watch that `mount-payments`' own KV writes eat into the
> same 1,000. On free there is **no overage billing**; once a daily limit is hit, further ops of
> that type just fail until 00:00 UTC. **Upgrade to Workers Paid ($5/mo) when combined writes
> approach ~1,000/day** — one-click, no reconfiguration. Sources:
> [KV limits](https://developers.cloudflare.com/kv/platform/limits/),
> [Rate Limiting binding](https://developers.cloudflare.com/workers/runtime-apis/bindings/rate-limit/).

### A.1 — Register the domain  ✅ DONE (2026-06-22)

**`crosspoint-context.com` is registered** via Cloudflare Registrar in `mount-payments`. The two
hostnames the rest of the plan uses:
- `mcp.crosspoint-context.com` → the Worker (MCP + ingest + OAuth + pairing endpoints).
- root `crosspoint-context.com` / `www.crosspoint-context.com` → the public homepage + privacy
  policy (item C).

The zone is now under **Websites** in the account; DNS stays empty until B.1 (the `mcp` custom
domain auto-creates its record) and C (homepage records).

### A.2 — Create a fresh `CTX` namespace (isolate from the relay)  ✅ DONE (2026-06-22)

**Decided:** the public MCP gets a **fresh, empty `CTX`** — not the relay's shared
`e6752…f53`. Cleaner isolation; the owner's own data re-syncs in seconds. The relay keeps writing to
the old namespace. (If you later want the relay to feed the new one, it's now a trivial same-account
id swap — see the [relay note](#relay-note) under Decisions.) `OAUTH_KV` (`b9b0…893`) is **kept
as-is** — no reason to wipe live OAuth grants/tokens.

Created via `wrangler kv namespace create CTX_PUBLIC` (title
`crosspoint-context-mcp-CTX_PUBLIC`), **id `b17f345c47754833bcd3738220f41d04`** — bound as `CTX` in
B.1.

### A.3 — Secrets & guardrails: verify, don't recreate

Because we're staying in `mount-payments`, the deployed Worker already has its secrets
(`TOKENS_JSON`, `GITHUB_CLIENT_*`, `GOOGLE_CLIENT_*`) and the #4/#5 rate-limit bindings + any usage
notifications. Nothing to re-upload.

- **Secrets:** keep as-is. *(Optional hygiene: rotate `TOKENS_JSON` with `wrangler secret put
  TOKENS_JSON` if you want a clean pre-public value — not required.)*
- **Rate-limit bindings:** already in `wrangler.jsonc` (`INGEST_LIMITER` 12/60s per slot,
  `INGEST_IP_LIMITER` 60/60s per IP, `MCP_LIMITER` 60/60s per slot) — these now also protect the
  *shared* free KV quota, so leave them in.
- **Optional early-warning:** since the 1,000 writes/day is shared with `mount-payments`, consider a
  **Manage Account → Notifications** usage alert (or just eyeball **Storage & Databases → KV →
  [namespace] → Metrics** periodically) so the wall doesn't surprise you.

**Done when:** the domain is registered in `mount-payments` and a fresh `CTX` namespace exists with
its id noted; nothing is deployed yet.

---

## B. Worker on the custom domain; repoint everything  *(code/config)*

**Goal:** the Worker serves from `https://mcp.crosspoint-context.com` and every hard-coded reference to the old
host is updated. **Fold in #10 (retire `me`) here** (Decision 4).

### B.1 — Point `wrangler.jsonc` at the domain + fresh `CTX`

In `mcp wrangler.jsonc`:
1. `account_id` stays `233792…506` (`mount-payments`) — unchanged.
2. Set the `CTX` binding `id` to the **fresh namespace** from A.2; leave `OAUTH_KV` (`b9b0…893`)
   as-is.
3. Add a **custom-domain route**:
   ```jsonc
   "routes": [{ "pattern": "mcp.crosspoint-context.com", "custom_domain": true }]
   ```
4. **`workers_dev`:** set `false` so the canonical URL is the custom domain only. (Leaving it
   `true` is a handy fallback, but a public service should have one canonical origin — the baked
   firmware origin and OAuth callbacks all point at `mcp.crosspoint-context.com`.)
5. **#10 — retire `me`:** remove the `OWNER_GITHUB_ID` and `OWNER_SLOT` entries from `vars`. With a
   fresh empty `CTX` there's no legacy `me` slot, so the owner gets a `u_<uuid>` slot like everyone
   else on first login.
   - ✅ **Verified safe to remove (2026-06-22):** `OWNER_GITHUB_ID`/`OWNER_SLOT` are referenced
     **only** in `mcp src/idp.ts:108-117` (`ensureSlot`), purely to bind the owner's identity to the
     fixed `me` slot — no owner-only privilege or rate-limit exemption keys off them. Dropping both
     leaves the owner on a normal `u_<uuid>` slot with no behavioural loss. (Also delete the now-dead
     `OWNER_GITHUB_ID?`/`OWNER_SLOT?` fields in `src/types.ts:24-25` and the owner branch in
     `ensureSlot`.)

> **🖱 Dashboard (after first deploy):** the `custom_domain` route auto-provisions the DNS record
> and TLS cert. To confirm: **Workers & Pages → [the Worker] → Settings → Domains & Routes** — you
> should see `mcp.crosspoint-context.com` listed as a Custom Domain with an active cert. (You can also add it here
> by hand via **Add → Custom Domain** if you prefer the UI over the `routes` config.)

### B.2 — Add the new OAuth callback URLs to the **existing** apps

**Decided:** reuse the existing GitHub + Google OAuth apps (Decision 3) — just add the new
callback URLs alongside the old ones during the transition. This keeps the Google verification
baseline on the existing client; no reset.

> **🖱 GitHub:** <https://github.com/settings/developers> → **OAuth Apps** → the existing
> CrossPoint app → add `https://mcp.crosspoint-context.com/callback/github` to the Authorization callback URLs
> (GitHub allows multiple) → keep the old `workers.dev` one until the migration is verified → Update.
>
> **🖱 Google:** <https://console.cloud.google.com> → the project owning `GOOGLE_CLIENT_*` →
> **APIs & Services → Credentials** → the OAuth 2.0 Client ID → under **Authorized redirect URIs**
> add `https://mcp.crosspoint-context.com/callback/google` (keep the old one for now) → Save.

### B.3 — Firmware origin (#1) + docs

1. **`platformio.ini:14`** — update `public_origin` → `https://mcp.crosspoint-context.com`. The `gh_release` /
   `gh_release_rc` / `slim` envs already interpolate it via
   `-DCROSSPOINT_DEFAULT_RELAY_URL=\"${crosspoint.public_origin}\"` (lines 91/100/109), so a rebuild
   picks it up automatically.
2. **README + deploy.sh** in the MCP repo — mostly already placeholder-based (`<worker-url>`), so
   only a few concrete strings need touching:
   - `mcp README.md:77` — the example host `https://crosspoint-context-mcp.<subdomain>.workers.dev`
     → `https://mcp.crosspoint-context.com`.
   - `mcp README.md:88` — **stale var name bug:** says `OWNER_GITHUB_LOGIN`, but the real var is
     `OWNER_GITHUB_ID` (`wrangler.jsonc:42`, `idp.ts:108`). Since #10 removes the owner vars
     entirely, just delete this sentence rather than fix it.
   - `deploy.sh:53` — the trailing `echo "...workers.dev URL..."` hint; reword to the custom domain.
   - `verify-mcp.sh` / `verify.sh` already take the base URL as `$1`, no host baked in — nothing to
     change there beyond passing `https://mcp.crosspoint-context.com`.

### B.4 — Deploy, re-flash, verify

1. **Deploy** (same `mount-payments` account): `./deploy.sh` (from `crosspoint-context-mcp`).
2. **Verify the server:** `./verify-mcp.sh` against `https://mcp.crosspoint-context.com` — `…/mcp` reachable,
   OAuth login works on both providers from the new domain.
3. **Re-flash** a `gh_release` build and re-verify pairing end-to-end against the new host (re-run
   the §3–§7 pair → ingest → MCP flow from public-release-plan; confirm the owner lands on a
   `u_<uuid>` slot now that `me` is gone).

**Done when:** `https://mcp.crosspoint-context.com/mcp` is reachable, OAuth login works on both providers from the
new domain, a freshly-flashed `gh_release` device pairs and syncs to it, the owner is on a
`u_<uuid>` slot, and the old `workers.dev` host is no longer referenced anywhere committed.

---

## C. Privacy policy + homepage  *(= public-release-plan #3, prose/hosting half)*

**Status:** the *code* half is already done — login-gated `GET/POST /account` (`account.ts`) lets a
signed-in user delete their `ctx:<slot>` and revoke all their `cred:` tokens, verified live
2026-06-22. What remains is published text and a landing page.

### C.1 — Public homepage on `crosspoint-context.com`

Minimum for Google verification: describes what CrossPoint Context is, who runs it, and links to
the privacy policy.

> **🖱 Hosting (Cloudflare Pages — simplest for static):**
> 1. **Workers & Pages → Create → Pages** → upload the static site (or connect a repo).
> 2. After the first deploy: **[the Pages project] → Custom domains → Set up a custom domain** →
>    add `crosspoint-context.com` and `www.crosspoint-context.com`. Pages auto-creates the DNS + cert in the zone from A.2.
> *(Alternative: serve the homepage from the Worker itself. Either is fine; Pages keeps the static
> site decoupled from the MCP Worker.)*

### C.2 — Privacy policy at a stable URL

e.g. `https://crosspoint-context.com/privacy`. Must state, accurately to the implementation:
- **What's collected:** book title, reading progress (section/page), and the read-so-far text of
  the current book; the OAuth identity (`github:<id>` / `google:<sub>`) stored only as a
  pseudonymous pointer to a slot.
- **Retention:** reading context auto-deletes after **90 days of inactivity** (`CTX_TTL_SECONDS`),
  refreshed on each sync; per-account cap **5 MB**.
- **Deletion / control:** self-service at `https://mcp.crosspoint-context.com/account` — delete reading data
  and/or revoke all paired devices, on demand.
  - ⚠️ **Accuracy (Decision 5 — keep status quo):** a full self-service delete removes the book
    content (`ctx:<slot>`) and revokes all device tokens (`cred:`), but the **pseudonymous
    `ownerSub:<sub>→slot` pointer (no reading content) is retained**. State this plainly: "deleting
    your data removes your reading content and disconnects your devices; a pseudonymous account
    identifier with no reading content may be retained." Do not claim full erasure.
- **Sharing:** not sold or shared; hosted on Cloudflare; used only to answer the user's own Claude
  queries about their reading.
- **Contact** address for data requests.

### C.3 — Wire the policy URL in

Link the policy from the `/account` page and the OAuth consent copy so it's discoverable.

**Done when:** homepage + privacy policy are live on `crosspoint-context.com`, the policy matches the code's
actual retention/deletion behavior (including the retained pointer), and `/account` links to it.

---

## D. Google OAuth verification  *(= public-release-plan #2 — start the clock early)*

**Why it's a blocker:** an unverified Google OAuth app caps at ~100 users and shows a scary
"Google hasn't verified this app" warning on the consent screen. GitHub has **no** equivalent gate
for basic scopes, so this is Google-only.

### D.1 — Keep scopes minimal

The read side only needs identity — `openid email profile` (basic, non-sensitive). Staying off
*sensitive/restricted* scopes means only **brand/consent-screen verification**, not the expensive
third-party security assessment. Confirm the Google login request doesn't ask for more than these.

### D.2 — Configure the consent screen (on the existing GCP project)

Because the Google app is **reused** (Decision 3), this is done on the project that already owns
`GOOGLE_CLIENT_*`.

> **🖱 Google Cloud Console:** <https://console.cloud.google.com> → the project owning
> `GOOGLE_CLIENT_*` → **APIs & Services → OAuth consent screen**:
> 1. **User type:** External.
> 2. App name, support email, app logo.
> 3. **Authorized domain** = `crosspoint-context.com` (the apex registered in A.2).
> 4. **Application home page** = `https://crosspoint-context.com` (C.1).
> 5. **Privacy policy URL** = `https://crosspoint-context.com/privacy` (C.2).
> 6. **Scopes:** add only `openid`, `.../auth/userinfo.email`, `.../auth/userinfo.profile`.
> 7. Confirm the **Authorized redirect URI** `https://mcp.crosspoint-context.com/callback/google` is on the
>    credential (set in B.2).

### D.3 — Submit for verification

> **🖱** On the OAuth consent screen, **Publish app**, then **Prepare for verification** / submit,
> providing the scope justification (why the app needs sign-in). Expect days–weeks of back-and-forth
> — this is why A→C must move fast.

While unverified, the app still works for the owner + up to ~100 test users, so a soft/limited
launch is possible before verification completes — just with the warning screen.

**Done when:** Google shows the app as verified (no unverified warning) and the user cap is lifted.

---

## Decisions (settled)

0. **Cloudflare account → stay in `mount-payments`, free plan.** A separate account isn't worth
   the friction (no add-account button; `+` aliases rejected; would need a whole separate email).
   The service already runs in this account; we just add the domain + a fresh `CTX`. Accepted:
   shared blast radius + shared account-wide free KV write quota.
1. **Domain name → `crosspoint-context.com`** ✅ registered 2026-06-22 via Cloudflare Registrar in
   `mount-payments`. `mcp.crosspoint-context.com` = Worker; root/`www` = homepage + privacy.
2. <a id="relay-note"></a>**`CTX` sharing with the open relay → fresh, independent `CTX`.** The
   public MCP gets its own empty `CTX` namespace (A.2); the relay keeps the old `e6752…f53`; the
   owner's data re-syncs in seconds.
   - **A relay can be pointed at the new `CTX` later — and now it's trivial:** since everything
     stays in `mount-payments`, both Workers are in the same account, so KV bindings can reference
     the same namespace. To have the relay feed the public `CTX`, just set the relay's `CTX` binding
     `id` to the new namespace in its `wrangler.jsonc` and redeploy. (KV bindings are account-scoped,
     so this only works because they share the account — which they now do.)
3. **OAuth apps → reuse.** Keep the existing GitHub + Google apps; just add the new
   `mcp.crosspoint-context.com` callback URLs alongside the old ones (B.2). Secrets already live on the Worker in
   `mount-payments` — nothing to copy. Google's verification baseline stays on the existing client —
   no reset.
4. **Retire `me` now (#10) → yes, as part of B.** Fresh empty `CTX` = no legacy `me` slot to
   preserve. Drop `OWNER_GITHUB_ID` / `OWNER_SLOT` from `vars` (B.1) and put everyone, owner
   included, on `u_<uuid>`. (Verified: the owner vars are referenced only by `ensureSlot` in
   `src/idp.ts:108-117` + the type decls in `src/types.ts:24-25` — no other privilege keys off them,
   so removal is clean.)
5. **Full "close account" deletion → keep status quo.** A full delete drops reading content
   (`ctx:<slot>`) and revokes tokens (`cred:`) but **retains** the pseudonymous
   `ownerSub:<sub>→slot` pointer (no content). The privacy policy (C.2) must describe this
   accurately rather than claim total erasure.

---

## Critical path / ordering

1. **A** — *(in `mount-payments`, free plan)* register domain ✅ + create a fresh `CTX` namespace.
   Secrets, `OAUTH_KV`, and the #4/#5 guardrails already exist here — verify, don't recreate.
2. **B** — repoint `wrangler.jsonc` (account, KV ids, custom-domain route, drop `me`), add OAuth
   callbacks to the reused apps, update `platformio.ini` origin + README/deploy.sh, deploy,
   re-flash, verify owner lands on `u_<uuid>`.
3. **C** — homepage + privacy policy live on `crosspoint-context.com` (policy reflects the retained pointer).
4. **D** — submit Google verification *immediately* once C is up (longest external clock).
5. Soft-launch is possible after C with the unverified-app warning, up to ~100 users, while D is in
   review.

These are the *external/infra* gates. The remaining in-repo hardening (public-release-plan #7
consent-phishing, #8 HTTP-downgrade guard, #11 MCP tool instructions, #12 WiFi flakiness, #13
onboarding/CI artifact) proceeds in parallel and is tracked there.
