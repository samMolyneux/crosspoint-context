#include "ClaudeContextSendActivity.h"

#include <ClaudeContextClient.h>
#include <ClaudeContextStore.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <algorithm>
#include <memory>

#include "CrossPointSettings.h"
#include "Epub/Section.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Staging file for the extracted body. Written with the EPUB loaded, then uploaded after
// the EPUB is released so the two never compete for heap.
constexpr char TEMP_PATH[] = "/.crosspoint/claude_ctx.tmp";
}  // namespace

bool ClaudeContextSendActivity::extractToTempFile() {
  auto epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
  epub->setupCacheDir();
  // Metadata only: the sections up to the reading point were already rendered (and cached)
  // during normal reading, so there is no need to load CSS or rebuild anything here.
  if (!epub->load(false, true)) {
    LOG_ERR("CTX", "Failed to load epub for context extraction");
    return false;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount <= 0) {
    LOG_ERR("CTX", "Book has no spine items");
    return false;
  }

  // Clamp the cut point. spineIndex can equal the spine count at end-of-book.
  int upToSpine = spineIndex;
  if (upToSpine >= spineCount) {
    upToSpine = spineCount - 1;
  }
  if (upToSpine < 0) {
    upToSpine = 0;
  }
  const int upToPage = page < 0 ? 0 : page;

  HalFile out;
  if (!Storage.openFileForWrite("CTX", TEMP_PATH, out)) {
    LOG_ERR("CTX", "Could not open temp file for write");
    return false;
  }

  // Header: title/author + truncation marker + spoiler instruction (see CONTRACT.md).
  std::string header = "# " + epub->getTitle() + " — " + epub->getAuthor() + "\n";
  header += "# Read up to: section " + std::to_string(upToSpine) + ", page " + std::to_string(upToPage) +
            ". Do not reveal anything beyond this point.\n\n";
  out.write(header.data(), header.size());

  // Replicate the reader's viewport so the section cache key matches and we read the
  // existing cache rather than triggering a rebuild (which would need CSS we didn't load).
  int marginTop, marginRight, marginBottom, marginLeft;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  marginTop += SETTINGS.screenMargin;
  marginLeft += SETTINGS.screenMargin;
  marginRight += SETTINGS.screenMargin;
  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  marginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);
  const uint16_t viewportWidth = renderer.getScreenWidth() - marginLeft - marginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - marginTop - marginBottom;

  for (int s = 0; s <= upToSpine; ++s) {
    Section section(epub, s, renderer);
    if (!section.loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                 SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                 viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                 SETTINGS.imageRendering, SETTINGS.focusReadingEnabled)) {
      // Cache miss/mismatch (e.g. read in a different orientation). Skip rather than rebuild.
      LOG_ERR("CTX", "Section %d cache unavailable; skipping", s);
      continue;
    }

    int lastPage = section.pageCount - 1;
    if (s == upToSpine) {
      lastPage = upToPage;
      if (lastPage >= section.pageCount) {
        lastPage = section.pageCount - 1;
      }
    }

    for (int p = 0; p <= lastPage; ++p) {
      // loadPageFromSectionFile() seeks to currentPage without advancing it, so set the
      // target page explicitly before each read.
      section.currentPage = p;
      const std::string text = section.getTextFromSectionFile();
      if (!text.empty()) {
        const std::string chunk = text + "\n\n";
        out.write(chunk.data(), chunk.size());
      }
    }
  }

  out.flush();
  LOG_DBG("CTX", "Extracted context up to section %d page %d", upToSpine, upToPage);
  return true;
}

void ClaudeContextSendActivity::onEnter() {
  Activity::onEnter();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  if (!CLAUDE_CONTEXT_STORE.isConfigured()) {
    state = NOT_CONFIGURED;
    requestUpdate();
    return;
  }

  state = EXTRACTING;
  statusMessage = tr(STR_CLAUDE_PREPARING);
  requestUpdateAndWait();  // render "Preparing…" before the blocking extraction

  if (!extractToTempFile()) {
    {
      RenderLock lock(*this);
      state = FAILED;
      statusMessage = tr(STR_CLAUDE_EXTRACT_FAILED);
    }
    requestUpdate(true);
    return;
  }

  // Past this point every path uses WiFi (and triggers a silent reboot on exit to clear
  // the heap fragmentation a wifi session leaves behind).
  wifiActivated = true;

  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void ClaudeContextSendActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_DBG("CTX", "WiFi connection failed/cancelled, exiting");
    returnToReader();
    return;
  }
  performUpload();
}

void ClaudeContextSendActivity::performUpload() {
  {
    RenderLock lock(*this);
    state = SENDING;
    statusMessage = tr(STR_CLAUDE_SENDING);
  }
  requestUpdateAndWait();

  const ClaudeContextClient::Error result = ClaudeContextClient::postFile(TEMP_PATH);

  // Drop the radio while the user reads the result; full teardown happens at silent reboot.
  esp_wifi_stop();

  {
    RenderLock lock(*this);
    if (result == ClaudeContextClient::OK) {
      state = DONE;
    } else {
      state = FAILED;
      statusMessage = ClaudeContextClient::errorString(result);
    }
  }
  requestUpdate(true);
}

void ClaudeContextSendActivity::returnToReader() { activityManager.goToReader(epubPath); }

void ClaudeContextSendActivity::onExit() {
  Activity::onExit();

  Storage.remove(TEMP_PATH);

  if (wifiActivated) {
    WiFi.disconnect(false);
    delay(30);
    silentRestartToReader();
  }
}

void ClaudeContextSendActivity::loop() {
  if (state == NOT_CONFIGURED || state == DONE || state == FAILED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
  }
}

void ClaudeContextSendActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_SEND_CONTEXT));

  const int top = screen.y + screen.height / 2 - 40;

  if (state == EXTRACTING || state == SENDING) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, statusMessage.c_str(), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == DONE) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_CLAUDE_SENT), true, EpdFontFamily::BOLD);
  } else if (state == NOT_CONFIGURED) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_CLAUDE_NOT_CONFIGURED), true,
                              EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, tr(STR_CLAUDE_SETUP_HINT), true,
                              EpdFontFamily::BOLD);
  } else if (state == FAILED) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_CLAUDE_SEND_FAILED), true,
                              EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, statusMessage.c_str());
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
