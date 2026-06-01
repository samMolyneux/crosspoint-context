#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Submenu for configuring the Claude reading-context relay.
 * Lets the user set the relay URL and the write-only bearer token.
 */
class ClaudeContextSettingsActivity final : public Activity {
 public:
  explicit ClaudeContextSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ClaudeContextSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  size_t selectedIndex = 0;

  void handleSelection();
};
