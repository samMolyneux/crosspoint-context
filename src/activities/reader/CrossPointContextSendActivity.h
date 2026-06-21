#pragma once
#include <Epub.h>

#include <memory>
#include <string>

#include "activities/Activity.h"

/**
 * Activity that sends the book-so-far to the CrossPoint Context relay.
 *
 * Launched from the reader menu (SEND_CONTEXT). The reader releases its EPUB before
 * replacing itself with this activity, mirroring the KOReader sync flow.
 *
 * Flow:
 * 1. Extract the book up to the current page to a temp file on the SD card (EPUB reloaded
 *    for metadata only; spoiler truncation happens here, on-device).
 * 2. Release the EPUB, connect WiFi (the extraction and the network stack are never
 *    resident at the same time — the TLS handshake needs the heap the EPUB was using).
 * 3. POST the temp file to the relay and show the result.
 */
class CrossPointContextSendActivity final : public Activity {
 public:
  explicit CrossPointContextSendActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string epubPath,
                                         int spineIndex, int page)
      : Activity("CrossPointContextSend", renderer, mappedInput),
        epubPath(std::move(epubPath)),
        spineIndex(spineIndex),
        page(page) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == EXTRACTING || state == SENDING; }

 private:
  enum State {
    NOT_CONFIGURED,
    EXTRACTING,
    SENDING,
    DONE,
    FAILED,
  };

  std::string epubPath;
  int spineIndex;
  int page;

  State state = EXTRACTING;
  std::string statusMessage;
  bool wifiActivated = false;
  bool canRepair = false;  // true when the failure was a rejected token (offer "Re-pair")

  // Writes the truncated book-so-far to the temp file. EPUB is loaded and released here.
  bool extractToTempFile();
  void onWifiSelectionComplete(bool success);
  void performUpload();
  void returnToReader();
  void startRepair();
};
