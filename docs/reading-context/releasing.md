# Releasing CrossPoint Context (fork)

How to cut a public firmware release for this fork. The CI workflow is kept **identical to
upstream** (`.github/workflows/release.yml` builds the binaries and uploads them as a workflow
**artifact** on any tag push). Like upstream, the **GitHub Release is created manually** — there is
no auto-publish step, so that release.yml merges cleanly from upstream.

The two fork-specific things to remember are baked into the steps below: **OTA points at this
repo**, and the **version carries a `-ctx` suffix**.

## Versioning

- Scheme: `MAJOR.MINOR.PATCH-ctx`, e.g. `1.3.1-ctx`. Same numeric base as upstream, **no `v`
  prefix**; the `-ctx` suffix marks it as this fork on the About/boot screen.
- The **numeric part must increase** every release — the on-device OTA check compares only the
  numbers (`sscanf("%d.%d.%d")`); the `-ctx` suffix is a constant label it ignores. A lower number
  will not be offered as an update.
- The **git tag must equal `crosspoint.version`** in `platformio.ini` **exactly** — OTA's
  "you're up to date" check is a literal string compare of the release tag against the running
  `CROSSPOINT_VERSION`.

## OTA (why the Release matters)

`src/network/OtaUpdater.cpp` fetches
`https://api.github.com/repos/samMolyneux/crosspoint-context/releases/latest` and updates the device
to the `firmware.bin` attached there. So:

- The repo must stay **public** (devices download anonymously).
- Every Release **must** have a **`firmware.bin`** asset, or OTA finds nothing to install.
- Do **not** repoint OTA back at upstream — upstream builds have no CrossPoint Context feature and
  would migrate devices off the fork.

## Steps

1. **Bump the version** in the `[crosspoint]` section of `platformio.ini`
   (e.g. `1.3.1-ctx` -> `1.3.2-ctx`).
2. **Commit and push** to `main`.
3. **Tag with the exact same string** and push the tag (this triggers the build):
   ```bash
   git tag 1.3.2-ctx
   git push origin 1.3.2-ctx
   ```
4. Wait for the **Compile Release** workflow to finish (Actions tab). It builds `gh_release` —
   which bakes `public_origin = https://mcp.crosspoint-context.com` — and uploads a
   `CrossPoint-<tag>` artifact containing `firmware.bin`, `bootloader.bin`, `partitions.bin`
   (+ `.elf`/`.map`).
5. **Create the GitHub Release manually** (mirrors upstream). Easiest via the CLI from the repo:
   ```bash
   # download this tag's build artifact from the workflow run
   gh run download -n CrossPoint-1.3.2-ctx -D /tmp/cp-1.3.2-ctx

   gh release create 1.3.2-ctx \
     --title "CrossPoint Context 1.3.2-ctx" \
     --generate-notes \
     /tmp/cp-1.3.2-ctx/firmware.bin \
     /tmp/cp-1.3.2-ctx/bootloader.bin \
     /tmp/cp-1.3.2-ctx/partitions.bin
   ```
   (Or the web UI: **Releases -> Draft a new release -> pick the tag -> attach the three `.bin`
   files**.)
6. **Verify** the published Release:
   - has **`firmware.bin`** attached, named exactly that (OTA matches the asset name literally);
   - is a **full release — not a draft or pre-release** (the OTA endpoint `releases/latest` only
     returns full releases; a draft/pre-release is invisible to devices). `gh release create`
     without `--draft`/`--prerelease` is correct; in the web UI, leave "Set as a pre-release"
     unticked;
   - has a **tag name equal to `crosspoint.version`** from step 1.

## How users install

- **New install / different firmware:** download `firmware.bin` from the
  [Releases page](https://github.com/samMolyneux/crosspoint-context/releases), then flash via
  <https://crosspointreader.com/#flash-tools> -> select **X4** -> **"Custom .bin"** -> upload it.
  (Desktop Chrome/Edge only; USB-flash-locked units need the unlock tool first.)
- **Existing CrossPoint Context devices:** auto-update over WiFi via OTA once a higher numeric
  version is published (Settings -> firmware update).
