#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Submenu for configuring the CrossPoint Context relay.
 * Lets the user set the relay URL and the write-only bearer token.
 */
class CrossPointContextSettingsActivity final : public Activity {
 public:
  explicit CrossPointContextSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("CrossPointContextSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  size_t selectedIndex = 0;

  void handleSelection();
};
