# Reading Context → OAuth + MCP: Migration Status (Resume Here)

Living status doc for the OAuth/MCP migration — **read this first** to restart cold in a
fresh session. Sequenced by [`migration-build-order.md`](migration-build-order.md); the
deeper designs are [`oauth-mcp-plan.md`](oauth-mcp-plan.md),
[`device-pairing-plan.md`](device-pairing-plan.md), and the wire contract
[`CONTRACT.md`](CONTRACT.md).

**Snapshot:** 2026-06-08. **Stages 0–2 done + verified on the real Claude connector; Track B
Phase 1 (pairing — server) done, deployed + verified; Phase 2 (pairing — firmware) built and
compiles, on-device test pending.** Next work: flash + hardware-test pairing end-to-end, then
Stage 4 (firmware cleanup).

---

## The three repos

| Repo | Path | Remote / visibility | Branch | Latest commit |
|------|------|---------------------|--------|---------------|
| **firmware** | `~/Projects/crosspoint-context` | this repo (open) | `main` | `48e7f07` |
| **relay** (open) | `~/Projects/crosspoint-context-relay` | `github.com/samMolyneux/crosspoint-context-relay` | `main` | `83f9127` |
| **MCP** (closed) | `~/Projects/crosspoint-context-mcp` | `github.com/samMolyneux/crosspoint-context-mcp` (**private**) | `main` | `c42b9b0` |

The closed MCP repo is a fresh standalone Worker; pairing lives there only. The relay was
decoupled from this firmware repo (vendored CONTRACT + fixture) so it can be open-sourced.

## Deployed Cloudflare Workers (account `mount-payments`, id `233792cdcb32ce4bd2252d013c45d506`)

| Worker | URL | Purpose |
|--------|-----|---------|
| `reading-relay` | `https://reading-relay.mount-payments.workers.dev` | `GET /c` + `POST /c` static-token relay (device writes here today) |
| `crosspoint-context-mcp` | `https://crosspoint-context-mcp.mount-payments.workers.dev` | OAuth-gated MCP read tools + ingest + **device pairing** (`/pair/start`, `/pair`); deployed version `a65cdc1e` |

**KV namespaces:**
- `CTX` = `e6752fe976de4118a3a22a405e29bf53` — **shared** by both Workers. Holds `ctx:<slot>`
  (book bodies), `ownerSub:<sub> → slot` mappings, and now `cred:<sha256(T)> → {slot}` (durable
  paired write credentials, hashed).
- `OAUTH_KV` = `b9b0bc0517af47e89af557eda1ca8893` — MCP worker only (grants, tokens, DCR
  clients, short-lived `login:<state>` records, and now ephemeral pairing state:
  `pair:<nonce>` and `approve:<state>`, both TTL'd).

## Live runtime state (verified via Cloudflare API)

- `ctx:me` — the device's pushed book (*The Captain of Venice — Christian Cameron*, ~162 KB).
- `ownerSub:github:44400632 → me` — owner (samMolyneux) bound to the device slot (via
  `OWNER_GITHUB_LOGIN`/`OWNER_SLOT` vars in the MCP `wrangler.jsonc`).
- `ownerSub:google:104203…407462 → u_bb3ed95d…` — a second identity on a fresh empty slot.
- `OAUTH_KV`: 2 grants + 2 tokens (GitHub + Google). A few stray `client:` DCR records from
  debug runs — harmless (TTL'd).

---

## Stage status

| Stage | State | Notes |
|-------|-------|-------|
| 0 — repos | ✅ | MCP scaffolded + pushed (private); relay decoupled + deployed |
| 1 — MCP tools | ✅ | `get_progress`, `search_reading_context`, `get_recent_text` over `ctx:<slot>`; stateless Streamable HTTP `/mcp`; soft-wall in server `instructions` |
| 2 — OAuth (GitHub + Google) | ✅ | DCR + PKCE + RFC 9728 metadata; provider-pick login; `ownerSub → slot`; `/mcp` gated by `ctx.props.slot`. **Verified end-to-end on the Claude connector** (tools work; second identity isolated; no cross-user reads) |
| 3 / Track A — read cutover | ✅ by construction | Soft-wall already server-side; hosted read = OAuth + tools; nothing depends on the skill/read-token. Only a README tidy remains |
| **Track B Phase 1 — pairing (server)** | ✅ | `POST /pair/start` (mint `T` + nonce), `GET /pair?c=` (reuses IdP login + `ownerSub → slot`, then consent), `POST /pair` (commit `cred:<sha256(T)> → slot`, consume nonce). Ingest now resolves static `TOKENS_JSON` **or** hashed `cred:`. In the MCP repo (`src/pairing.ts`, `src/http.ts`); curl-reachable legs pass `./verify.sh` (10–17). IdP→approve→push leg is browser-verified |
| **Track B Phase 2 — pairing (firmware)** | 🟡 built, on-device test pending | `ClaudePairingActivity` (`src/activities/settings/`): one `POST <origin>/pair/start`, saves `<origin>/ingest` + minted `T` via `ClaudeContextStore`, renders QR (`QrUtils::drawQrCode`) of `verification_uri_complete` + nonce, exits — no poll; `silentRestart()` on exit. Menu item added to `ClaudeContextSettingsActivity`. New `ClaudeContextClient::pairStart()` does the HTTPS POST + parses the JSON. Origin baked via `-DCLAUDE_DEFAULT_PAIRING_ORIGIN` (in `claude_secrets.ini`/`platformio.local.ini`). **Compiles** (`pio run` SUCCESS, RAM 30.9%); **needs hardware**: QR scans, short-code fallback, first push round-trips |
| **Stage 4 — firmware cleanup** | ⬜ | Drop `-DCLAUDE_DEFAULT_WRITE_TOKEN`; repoint device URL to MCP `/ingest`. See [`oauth-mcp-plan.md`](oauth-mcp-plan.md) Phase 4 |

## Bugs fixed during Stage 2 (for context)

1. `deploy.sh` KV-id clobber — a blanket `sed` rewrote *both* `"id"` lines, binding
   `OAUTH_KV` to the CTX namespace. Fixed (`24564ea`): non-destructive existence check, ids
   pinned in `wrangler.jsonc`.
2. `invalid_grant` at `/token` — `userId` passed to `completeAuthorization` was the
   colon-namespaced `ownerSub` (`github:<id>`); workers-oauth-provider uses `:` as an
   internal token-format separator. Fixed (`6b86136`): sanitize the colon from the grant's
   `userId` only; keep namespaced `ownerSub` in props and the CTX key.

---

## How to resume

1. Skim this doc + [`migration-build-order.md`](migration-build-order.md).
2. **Flash + hardware-test pairing.** Both server (Phase 1, deployed) and firmware (Phase 2,
   compiles) are in place. `pio run -t upload`, then Settings → Claude Context → **Pair with
   Claude**: confirm the QR scans to the consent page, the short-code fallback works, approve
   on phone, then **Send context** should round-trip (the device now points at MCP `/ingest`
   with the minted token). The first push doubles as the pairing success check (401 ⇒ finish
   on phone and retry). Then Stage 4 (firmware cleanup) is independent and can follow.

### Verify the live system is still healthy
```bash
# deployed relay + MCP contract / OAuth wiring
( cd ~/Projects/crosspoint-context-relay && ./verify.sh https://reading-relay.mount-payments.workers.dev )
( cd ~/Projects/crosspoint-context-mcp   && ./verify-mcp.sh https://crosspoint-context-mcp.mount-payments.workers.dev )
```
Full read-side verification is via the **Claude connector** (browser login), not curl —
`/mcp` is OAuth-gated.

### Inspecting Cloudflare (logs + KV) — needs a token
A read-only Cloudflare API token lives at `~/.cf-debug-token` (scopes: Workers Observability
+ KV Storage + Scripts, all Read; **no TTL — kept intentionally for ongoing use**). Recreate
at dash.cloudflare.com/profile/api-tokens only if it's removed. Then:
```bash
TOKEN=$(cat ~/.cf-debug-token); ACC=233792cdcb32ce4bd2252d013c45d506
OAUTH=b9b0bc0517af47e89af557eda1ca8893; CTX=e6752fe976de4118a3a22a405e29bf53
api(){ curl -s -H "Authorization: Bearer $TOKEN" "$@"; }
# KV keys
api "https://api.cloudflare.com/client/v4/accounts/$ACC/storage/kv/namespaces/$OAUTH/keys" | jq -r '.result[].name'
# Observability logs (last 2h) — note queryId is required, filter by service
NOW=$(date +%s%3N); FROM=$((NOW-7200000))
api -X POST "https://api.cloudflare.com/client/v4/accounts/$ACC/workers/observability/telemetry/query" \
  -H 'Content-Type: application/json' \
  -d "{\"queryId\":\"q1\",\"view\":\"events\",\"timeframe\":{\"from\":$FROM,\"to\":$NOW},\"limit\":50,\"parameters\":{\"datasets\":[\"cloudflare-workers\"],\"filters\":[{\"key\":\"\$metadata.service\",\"type\":\"string\",\"operation\":\"eq\",\"value\":\"crosspoint-context-mcp\"}]}}" \
  | jq -r '.result.events.events | sort_by(.timestamp)[] | "\(.source.level)\t\(.["$workers"].event.response.status // "-")\t\(.source.message)"'
```

### Deploy (when changed)
`cd` into the repo, `./deploy.sh` (you're logged into wrangler; secrets come from `.dev.vars`).

---

## Housekeeping
- `~/.cf-debug-token` is a **persistent** read-only Cloudflare token (no TTL) kept for
  ongoing log/KV inspection — see the project memory. Revoke it manually if ever needed.
- Device still writes to the **relay** (`/c`, slot `me`) — unchanged until Stage 4 repoints it.
- **Pairing deployed** (version `a65cdc1e`, 2026-06-08) — Phase 1 endpoints are live and
  verified non-destructively against production (`/pair/start` mints, unapproved token →
  401 so `ctx:me` is untouched, `/pair?c=` renders, `pair:<nonce>` landed in OAUTH_KV). The
  IdP→approve→push leg still needs a browser; firmware Phase 2 can now pair against it.
- **Two known, bounded pairing-security gaps** (documented at the top of `src/pairing.ts`):
  rate-limiting `/pair/start` + `/pair` is left to the Cloudflare edge (not in code); the
  nonce rides in the verification URL (required for the QR) so platform logs may capture it —
  mitigated by short TTL + single-use + owner-IdP-gated approval. Revisit under the TODO
  "security check".
- Open TODO in firmware [`TODO.md`](../../TODO.md): include the currently-open page in sent
  context (off-by-one), give the MCP instructions latitude beyond the strict tool set, and a
  general security check.
