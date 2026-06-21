#include "ClaudePairingActivity.h"

#include <ClaudeContextClient.h>
#include <ClaudeContextStore.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

namespace {
#ifdef CLAUDE_DEFAULT_PAIRING_ORIGIN
constexpr const char* PAIRING_ORIGIN = CLAUDE_DEFAULT_PAIRING_ORIGIN;
#else
constexpr const char* PAIRING_ORIGIN = "";
#endif

// Non-secret label shown on the phone's consent screen, e.g. "CrossPoint A1B2" (last 2 MAC
// bytes) — lets the owner confirm it's their device before approving.
std::string deviceLabel() {
  uint8_t mac[6] = {0};
  WiFi.macAddress(mac);
  char buf[24];
  snprintf(buf, sizeof(buf), "CrossPoint %02X%02X", mac[4], mac[5]);
  return std::string(buf);
}

// Origin with any trailing slashes removed, so paths can be appended cleanly.
std::string normalisedOrigin() {
  std::string base = PAIRING_ORIGIN;
  while (!base.empty() && base.back() == '/') {
    base.pop_back();
  }
  return base;
}

// Greedily pack characters into lines each <= maxWidth px, UTF-8 safe (never splits a
// multi-byte codepoint). Unlike GfxRenderer::wrappedText (word-wrap only), this breaks a long
// spaceless token such as a URL across lines instead of truncating it with an ellipsis.
std::vector<std::string> wrapToWidth(const GfxRenderer& renderer, const int fontId, const std::string& text,
                                     const int maxWidth) {
  std::vector<std::string> lines;
  if (text.empty()) {
    return lines;
  }
  if (maxWidth <= 0) {
    lines.push_back(text);
    return lines;
  }

  std::string current;
  size_t i = 0;
  while (i < text.size()) {
    // Advance over one whole UTF-8 codepoint (skip 0x80..0xBF continuation bytes).
    size_t next = i + 1;
    while (next < text.size() && (static_cast<unsigned char>(text[next]) & 0xC0) == 0x80) {
      ++next;
    }
    const std::string glyph = text.substr(i, next - i);
    if (!current.empty() && renderer.getTextWidth(fontId, (current + glyph).c_str()) > maxWidth) {
      lines.push_back(current);
      current = glyph;
    } else {
      current += glyph;
    }
    i = next;
  }
  if (!current.empty()) {
    lines.push_back(current);
  }
  return lines;
}
}  // namespace

void ClaudePairingActivity::onEnter() {
  Activity::onEnter();

  if (PAIRING_ORIGIN[0] == '\0') {
    state = UNAVAILABLE;
    requestUpdate();
    return;
  }

  // Compute the label once: the same string is sent in the POST and shown on screen, so the
  // user can confirm it matches the phone's consent screen.
  deviceLabelText = deviceLabel();

  {
    RenderLock lock(*this);
    state = PAIRING;
    statusMessage = tr(STR_CLAUDE_PAIRING);
  }
  requestUpdateAndWait();  // render "Pairing…" before the blocking network work

  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void ClaudePairingActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    {
      RenderLock lock(*this);
      state = FAILED;
      statusMessage = tr(STR_WIFI_CONN_FAILED);
    }
    requestUpdate(true);
    return;
  }
  performPairing();
}

void ClaudePairingActivity::performPairing() {
  {
    RenderLock lock(*this);
    state = PAIRING;
    statusMessage = tr(STR_CLAUDE_PAIRING);
  }
  requestUpdateAndWait();

  const ClaudeContextClient::Error err =
      ClaudeContextClient::pairStart(normalisedOrigin(), deviceLabelText, pairResult);

  // Drop the radio while the user finishes on their phone; full teardown is at silent reboot.
  esp_wifi_stop();

  {
    RenderLock lock(*this);
    if (err == ClaudeContextClient::OK) {
      // Point this device's push path at the hosted MCP /ingest with the freshly minted token.
      // That token only resolves at /ingest (hashed cred lookup), not the legacy relay /c.
      CLAUDE_CONTEXT_STORE.setConfig(normalisedOrigin() + "/ingest", pairResult.writeToken);
      CLAUDE_CONTEXT_STORE.saveToFile();
      state = SHOW_QR;
    } else {
      state = FAILED;
      statusMessage = ClaudeContextClient::errorString(err);
    }
  }
  requestUpdate(true);
}

void ClaudePairingActivity::loop() {
  if (state == UNAVAILABLE || state == SHOW_QR || state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      finish();
    }
  }
}

void ClaudePairingActivity::onExit() {
  Activity::onExit();

  // A WiFi session fragments the heap; reboot to clear it (matches the other WiFi activities).
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void ClaudePairingActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_CLAUDE_PAIR));

  if (state == PAIRING) {
    const int top = screen.y + screen.height / 2 - 20;
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, statusMessage.c_str(), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == SHOW_QR) {
    const int lh = renderer.getLineHeight(UI_10_FONT_ID);
    int y = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

    // Device label up top, so the user can confirm it matches the phone's consent screen.
    const std::string device = std::string(tr(STR_CLAUDE_PAIR_DEVICE)) + " " + deviceLabelText;
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, y, device.c_str(), true, EpdFontFamily::BOLD);
    y += lh + metrics.verticalSpacing;

    const int qrSize = std::min(screen.width, screen.height) / 2;
    const Rect qrBounds{screen.x + (screen.width - qrSize) / 2, y, qrSize, qrSize};
    QrUtils::drawQrCode(renderer, qrBounds, pairResult.verificationUriComplete);
    y = qrBounds.y + qrBounds.height + metrics.verticalSpacing;

    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, y, tr(STR_CLAUDE_PAIR_SCAN), true, EpdFontFamily::BOLD);
    y += lh + 6;

    // No-scan fallback: a short label, then the full URL char-wrapped at a small font so it
    // never clips (wrappedText() can't be used here — it ellipsizes spaceless tokens).
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, y, tr(STR_CLAUDE_PAIR_OR_VISIT));
    y += lh + 2;
    const int urlWidth = screen.width - 2 * metrics.contentSidePadding;
    const int urlLh = renderer.getLineHeight(SMALL_FONT_ID);
    for (const auto& line : wrapToWidth(renderer, SMALL_FONT_ID, pairResult.verificationUri, urlWidth)) {
      UITheme::drawCenteredText(renderer, screen, SMALL_FONT_ID, y, line.c_str());
      y += urlLh;
    }
    y += 6;

    const std::string code = std::string(tr(STR_CLAUDE_PAIR_AND_ENTER)) + " " + pairResult.nonce;
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, y, code.c_str(), true, EpdFontFamily::BOLD);
  } else if (state == UNAVAILABLE) {
    const int top = screen.y + screen.height / 2 - 20;
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_CLAUDE_PAIR_UNAVAILABLE), true,
                              EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, tr(STR_CLAUDE_SETUP_HINT));
  } else if (state == FAILED) {
    const int top = screen.y + screen.height / 2 - 20;
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_CLAUDE_PAIR_FAILED), true,
                              EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, statusMessage.c_str());
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
