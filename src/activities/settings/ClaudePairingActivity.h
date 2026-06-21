#pragma once

#include <ClaudeContextClient.h>

#include <string>

#include "activities/Activity.h"

/**
 * No-type device pairing for the Claude reading-context feature (device-pairing-plan.md,
 * Track B). Makes ONE POST <origin>/pair/start, saves the returned write token, and renders a
 * QR (plus a short, typeable nonce) that the user finishes on their phone. No polling — the
 * device shows the code and is done; the next "Send context" doubles as the success check.
 *
 * Pairs against the device's configured server origin (the baked-in
 * -DCLAUDE_DEFAULT_RELAY_URL default, or a self-hoster's override); pairing is only offered
 * when an origin is set. Manual URL + token entry remains the alternative provisioning path.
 */
class ClaudePairingActivity final : public Activity {
 public:
  explicit ClaudePairingActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ClaudePairing", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == PAIRING; }

 private:
  enum State {
    UNAVAILABLE,  // no server origin baked into this build
    PAIRING,      // connecting / handshake in flight
    SHOW_QR,      // success — QR + nonce on screen, user finishes on their phone
    FAILED,
  };

  State state = PAIRING;
  std::string statusMessage;
  std::string deviceLabelText;  // shown on screen and sent in the POST — must match the phone
  ClaudeContextClient::PairResult pairResult;

  void onWifiSelectionComplete(bool success);
  void performPairing();
};
