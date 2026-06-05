# Reading Context → OAuth + MCP: Migration Status (Resume Here)

Living status doc for the OAuth/MCP migration — **read this first** to restart cold in a
fresh session. Sequenced by [`migration-build-order.md`](migration-build-order.md); the
deeper designs are [`oauth-mcp-plan.md`](oauth-mcp-plan.md),
[`device-pairing-plan.md`](device-pairing-plan.md), and the wire contract
[`CONTRACT.md`](CONTRACT.md).

**Snapshot:** 2026-06-05. **Stages 0–2 done and verified on the real Claude connector.**
Next work: Track B (device pairing) and Stage 4 (firmware cleanup).

---

## The three repos

| Repo | Path | Remote / visibility | Branch | Latest commit |
|------|------|---------------------|--------|---------------|
| **firmware** | `~/Projects/crosspoint-context` | this repo (open) | `main` | `48e7f07` |
| **relay** (open) | `~/Projects/crosspoint-context-relay` | `github.com/samMolyneux/crosspoint-context-relay` | `main` | `83f9127` |
| **MCP** (closed) | `~/Projects/crosspoint-context-mcp` | `github.com/samMolyneux/crosspoint-context-mcp` (**private**) | `main` | `6b86136` |

The closed MCP repo is a fresh standalone Worker; pairing lives there only. The relay was
decoupled from this firmware repo (vendored CONTRACT + fixture) so it can be open-sourced.

## Deployed Cloudflare Workers (account `mount-payments`, id `233792cdcb32ce4bd2252d013c45d506`)

| Worker | URL | Purpose |
|--------|-----|---------|
| `reading-relay` | `https://reading-relay.mount-payments.workers.dev` | `GET /c` + `POST /c` static-token relay (device writes here today) |
| `crosspoint-context-mcp` | `https://crosspoint-context-mcp.mount-payments.workers.dev` | OAuth-gated MCP read tools + ingest; deployed version `90a5f150` |

**KV namespaces:**
- `CTX` = `e6752fe976de4118a3a22a405e29bf53` — **shared** by both Workers. Holds `ctx:<slot>`
  (book bodies) and `ownerSub:<sub> → slot` mappings.
- `OAUTH_KV` = `b9b0bc0517af47e89af557eda1ca8893` — MCP worker only (grants, tokens, DCR
  clients, short-lived `login:<state>` records).

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
| **Track B — device pairing** | ⬜ **next** | QR/nonce, `/pair/start` + `/pair`, dynamic `cred:<sha256(T)>`; prerequisite (OAuth/slot model) now in place. See [`device-pairing-plan.md`](device-pairing-plan.md) |
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
2. Pick up at **Track B (device pairing)** — read [`device-pairing-plan.md`](device-pairing-plan.md).
   The firmware side adds a `ClaudePairingActivity`; the server side adds `/pair/start` +
   `/pair` + dual-token resolution in ingest, in the **MCP repo**.

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
- Open TODO in firmware [`TODO.md`](../../TODO.md): include the currently-open page in sent
  context (off-by-one), and give the MCP instructions latitude beyond the strict tool set.
