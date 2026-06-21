# Reading Context → OAuth + MCP: Consolidated Build Order

The decision record that sequences the two migration plans into one build order:

- [`oauth-mcp-plan.md`](oauth-mcp-plan.md) — the **spine**: relay → remote MCP server with
  OAuth on the read side.
- [`device-pairing-plan.md`](device-pairing-plan.md) — a **dependent branch**: no-type
  write-side provisioning (QR/nonce), which grafts on *after* the OAuth/slot model lands.

It does not restate those plans; it decides **order, repos, and parallelism**, and points
back to them for the detail. [`CONTRACT.md`](CONTRACT.md) remains the single source of truth
for the wire seam.

---

## Decisions

1. **Ordered, not in conjunction — around one shared milestone.** `device-pairing` is a
   dependent sub-plan of `oauth-mcp`, not a peer: pairing's approval reuses the IdP login +
   `ownerSub → slot` binding from `oauth-mcp` Phase 2 (device-pairing-plan.md:204-207, 231).
   So `oauth-mcp` is the spine; pairing branches off **after** Phase 2. Once Phase 2 lands,
   the read side and the write/pairing side fan out into two parallel tracks because they
   touch different surfaces.

2. **Three repos, decided roles:**

   | Repo | Visibility | Status | Contains |
   |------|-----------|--------|----------|
   | `crosspoint-context` (this) | open | exists | firmware, `lib/CrossPointContext/`, the skill, docs |
   | `crosspoint-context-relay` | open | exists | dumb Worker: `GET /c`, `POST /c`, `/ingest`, static raw `TOKENS_JSON`, bundled skill — the **self-hosting reference** |
   | `crosspoint-context-mcp` | **closed** | **NEW** | hosted product: MCP tools, OAuth, `/pair`, dynamic hashed `cred:<sha256(T)>`, `/ingest` |

3. **New MCP repo is a fresh standalone repo**, not a fork or shared-core package. It copies
   the small ingest handler (~30 lines) it needs. Shared between the two server repos is the
   **`CONTRACT.md` spec, not code** — keeping a clean open/closed license boundary, accepting
   minor duplication of the ingest/KV glue.

4. **Pairing lives in the closed MCP repo only.** Pairing needs IdP/OAuth, which is the
   hosted side. The open relay keeps static raw tokens; it gets no `/pair`. The firmware
   `CrossPointPairingActivity` is open (in this repo) but talks to the closed MCP origin.

---

## Build order

```
Stage 0  Repo setup (prereq)
         • Create crosspoint-context-mcp (closed, fresh standalone)
         • Decouple crosspoint-context-relay from ../crosspoint-context
           relative paths so it can go public (oauth-mcp-plan.md:72-76):
           vendor CONTRACT.md / fake-book.md, drop secrets.local.json refs

Stage 1  MCP server + 3 tools, NO auth              ← oauth-mcp Phase 1
         get_progress, search_reading_context, get_recent_text over the
         existing KV ctx:<slot>; add /ingest; keep GET /c live.
         Verify with an MCP inspector against a manually-seeded slot.

Stage 2  OAuth + ownerSub→slot   ★ LINCHPIN          ← oauth-mcp Phase 2
         workers-oauth-provider, GitHub/Google login, custom connector.
         Confirm: tools work; a 2nd identity gets its own empty slot;
         no cross-user reads. Everything below depends on this.
         │
         ├─ Track A (read side)                       ← oauth-mcp Phase 3
         │    soft-wall guidance → server instructions / tool descriptions;
         │    hosted read = OAuth + tools (read token no longer in the path).
         │    GET /c + skill NOT deleted — they live on in the open relay.
         │
         └─ Track B (write side / pairing)            ← device-pairing 1→2→3
              1. server: /pair/start, /pair, KV pending records,
                 dual-token resolution in ingest (raw TOKENS_JSON OR
                 hashed cred:<sha256(T)>)
              2. firmware: CrossPointPairingActivity (one POST, save T,
                 render QR + nonce, exit — no poll loop)
              3. coexistence check: manual token AND paired token both
                 push to /ingest; legacy GET /c skill flow untouched

Stage 4  Firmware cleanup                            ← oauth-mcp Phase 4
         CAN START AT STAGE 1 (depends only on /ingest existing, not auth):
         drop -DCROSSPOINT_DEFAULT_WRITE_TOKEN + its store branch; KEEP the
         baked-in URL; store holds origin only, client appends /ingest.
         Pull forward to de-risk the firmware side early.

Stage 5  Multi-user (optional)                       ← oauth-mcp Phase 5
         Falls out of Stage 2+3: each person logs in (own identity → own
         slot) + per-device write token. No token-pair distribution.
```

---

## Invariants held throughout

- **Nothing working gets deleted.** `GET /c` + skill + static raw tokens stay live and
  *become* the open-relay reference. "Retire" means only that the hosted product stops
  depending on them (Stage 2 / Track A) — not removal from the codebase.
- **Spoiler hard wall is untouched.** Truncation stays on the device; unread text never
  leaves the ESP32. The migration only changes how *already-uploaded* text is queried.
- **The firmware body format does not change for the MVP.** All three MVP tools are
  derivable from the current body (CONTRACT.md §Body format). Section markers for
  chapter-scoped tools are a deferred, contract-bumping follow-on.

---

## TODO mapping (repo-root `TODO.md`)

| TODO item | Covered by |
|-----------|-----------|
| OAuth + MCP | Stages 1–2 (+ Track A, Stage 5) |
| Remove/clean up default token stuff | Stage 4 (drop `-DCROSSPOINT_DEFAULT_WRITE_TOKEN`, keep URL) |
| skill refinement? | Track A (guidance → server instructions; skill retained as open-relay example) |
| include past reading somehow | Out of scope here — needs section markers (deferred contract change) |
