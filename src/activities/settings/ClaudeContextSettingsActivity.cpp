#include "ClaudeContextSettingsActivity.h"

#include <ClaudeContextStore.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEMS = 2;
const StrId menuNames[MENU_ITEMS] = {StrId::STR_CLAUDE_RELAY_URL, StrId::STR_CLAUDE_WRITE_TOKEN};
}  // namespace

void ClaudeContextSettingsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  requestUpdate();
}

void ClaudeContextSettingsActivity::onExit() { Activity::onExit(); }

void ClaudeContextSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  buttonNavigator.onNext([this] {
    selectedIndex = (selectedIndex + 1) % MENU_ITEMS;
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = (selectedIndex + MENU_ITEMS - 1) % MENU_ITEMS;
    requestUpdate();
  });
}

void ClaudeContextSettingsActivity::handleSelection() {
  if (selectedIndex == 0) {
    // Relay URL - prefill with https:// if empty to save typing.
    const std::string current = CLAUDE_CONTEXT_STORE.getRelayUrl();
    const std::string prefill = current.empty() ? "https://" : current;
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_CLAUDE_RELAY_URL),
                                                                   prefill, 128, InputType::Url),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               const std::string url = (kb.text == "https://" || kb.text == "http://") ? "" : kb.text;
                               CLAUDE_CONTEXT_STORE.setRelayUrl(url);
                               CLAUDE_CONTEXT_STORE.saveToFile();
                             }
                           });
  } else if (selectedIndex == 1) {
    // Write token (masked entry).
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_CLAUDE_WRITE_TOKEN),
                                                CLAUDE_CONTEXT_STORE.getWriteToken(), 128, InputType::Password),
        [this](const ActivityResult& result) {
          if (!result.isCancelled) {
            const auto& kb = std::get<KeyboardResult>(result.data);
            CLAUDE_CONTEXT_STORE.setWriteToken(kb.text);
            CLAUDE_CONTEXT_STORE.saveToFile();
          }
        });
  }
}

void ClaudeContextSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CLAUDE_CONTEXT));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(MENU_ITEMS),
      static_cast<int>(selectedIndex), [](int index) { return std::string(I18N.get(menuNames[index])); }, nullptr,
      nullptr,
      [](int index) {
        if (index == 0) {
          const auto url = CLAUDE_CONTEXT_STORE.getRelayUrl();
          return url.empty() ? std::string(tr(STR_NOT_SET)) : url;
        }
        return CLAUDE_CONTEXT_STORE.getWriteToken().empty() ? std::string(tr(STR_NOT_SET)) : std::string("******");
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
