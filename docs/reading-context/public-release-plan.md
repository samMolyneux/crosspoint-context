# Public Release Readiness — CrossPoint Context (Resume Here)

Self-contained handoff for taking the reading-context feature from "works for the owner"
to a **multi-tenant public service** (strangers flash the firmware + connect to the hosted
MCP). Read this first in a fresh session. Companion: [`migration-status.md`](migration-status.md)
(the migration that got us here is **done** — Stages 0–2, pairing, Stage 4, rebrand, fork
sync with upstream, security check all complete and on `main`).

**Snapshot:** 2026-06-22. Migration complete; device flashed + re-paired; sync verified
live. This doc is the *next phase* (public launch), not yet started.

---

## Key facts (verified, don't re-derive)

- **Hosted MCP** (multi-tenant, deployed): `https://crosspoint-context-mcp.mount-payments.workers.dev`
  — Cloudflare account `mount-payments` (`233792cdcb32ce4bd2252d013c45d506`). Repo:
  `~/Projects/crosspoint-context-mcp` (private). Deploy: `./deploy.sh`; verify: `./verify-mcp.sh`.
- **Firmware**: `~/Projects/crosspoint-context` (open fork of `crosspoint-reader/crosspoint-reader`,
  branch `main`, synced 0-behind upstream). **Relay** (open): `~/Projects/crosspoint-context-relay`.
- **Multi-tenancy already works**: OAuth (GitHub + Google), per-identity slot isolation
  (verified — each new identity → fresh `u_<uuid>` slot, no cross-user reads), pairing flow,
  TLS verified, owner bound by **immutable** GitHub id (`OWNER_GITHUB_ID=44400632`),
  rate-limiting on `/authorize`,`/pair`,`/pair/start`,`/register` (per-IP, MINT/AUTH limiters
  in `mcp src/index.ts`).
- **Read-only Cloudflare token** at `~/.cf-debug-token` for KV/log inspection (see project memory).
- KV: `CTX` (`e6752fe976de4118a3a22a405e29bf53`) holds `ctx:<slot>`, `ownerSub:<sub>→slot`,
  `cred:<sha256(token)>→{slot}`; `OAUTH_KV` (`b9b0bc0517af47e89af557eda1ca8893`) holds OAuth
  grants/tokens/DCR + `login:`/`pair:`/`approve:` ephemeral records.

## Two gaps confirmed by inspection
1. **`gh_release` build ships with NO server origin.** `CROSSPOINT_DEFAULT_RELAY_URL` is only
   in the gitignored `platformio.local.ini`, not committed envs. A stranger flashing the public
   build has nothing to connect to.
2. **`/ingest` writes are NOT rate-limited** (only auth/pair endpoints are). Any valid token
   can write unlimited times → KV write-quota exhaustion.

---

## Work to do (priority order)

### Blockers — won't function for others / can't legally launch
1. **Bake the public origin into committed release builds.** Add
   `-DCROSSPOINT_DEFAULT_RELAY_URL="https://crosspoint-context-mcp.mount-payments.workers.dev"`
   to the committed `gh_release` / `gh_release_rc` / `slim` envs in `platformio.ini` (URL is
   public, not a secret). ~1-line change; it's what makes a flashed device reach the server.
2. **Google OAuth verification** (external lead time). Unverified Google apps cap at ~100
   users + show a scary warning. Needs homepage + privacy policy + scope justification. Start
   early. GitHub has no equivalent gate for basic scopes.
3. **Privacy policy + data-deletion path.** You'd store others' reading content. Required for
   #2 and on its own. Need a retention statement + user-facing delete of `ctx:<slot>`.

### Strongly recommended — abuse & cost control (handing strangers your CF budget)
4. **Per-user/token rate-limit on `/ingest`** (and `/mcp`). Currently unlimited.
5. **Storage caps + retention.** Each `ctx:<slot>` ≤ 5 MB; N users = unbounded KV growth.
   KV free tier (~1 GB, ~1k writes/day) will blow. Add inactivity TTL on `ctx:<slot>` and/or
   per-account cap.
6. **`cred:` revocation + expiry (security finding #5).** Never-expiring write creds with no
   revoke = liability at scale. (Note: owner currently has ~4 orphaned `cred:` records — pure
   downside, write-only-to-own-slot risk; cleanup = delete all + re-pair once, or build revoke.)
7. **Consent-phishing mitigation (security finding #4).** At scale, tricking a victim into
   approving the attacker's device token (write-poisoning their context) is realistic. Fix:
   require typing the device-shown nonce on the phone instead of the link auto-carrying it.
8. **HTTP-downgrade guard (security finding #2).** Strangers misconfigure origins; refuse to
   send bearer token / book body over `http://` unless an explicit debug flag is set.
9. **Confirm DCR `/register` records carry a TTL** (flagged needs-verification in the review).

### Cleanup / polish
10. **Retire migration cruft:** the `OWNER_GITHUB_ID → me` special-case and the legacy
    static-token `reading-relay` / `me` slot, so everyone (incl. owner) gets a uniform
    `u_<uuid>` slot.
11. **MCP tool instructions** — loosen beyond the strict 3-tool set; they're the contract
    every connector relies on (existing TODO).
12. **WiFi-connect flakiness** (existing TODO) — pairing failing on first connect wrecks
    first-run UX; fix before launch.
13. **Onboarding docs + release artifact** (CI release workflow exists; ensure it carries the
    origin from #1).

## Strategic decision underneath it all
**Hosted-free-with-caps vs. self-host-first.** Architecture already supports self-hosting
(configurable origin, open relay, vendored CONTRACT). Cheapest "public": ship firmware with
the hosted MCP as a **capped free default**, steer heavy users to self-host — bounds cost and
liability without gating access. Decide this before sizing #4/#5.

## Lowest-effort high-leverage starting points
- #1 (origin bake) — 1 line, unblocks "others can connect".
- #4 + #5 (ingest rate-limit + ctx TTL) — the server hardening that bounds your cost/risk.
