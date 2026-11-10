#include "XtcPageSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int kSmallStep = 1;
constexpr int kLargeStep = 10;
}  // namespace

void XtcPageSelectionActivity::onEnter() {
  Activity::onEnter();
  if (pageCount == 0) {
    pageCount = 1;
  }
  if (page >= pageCount) {
    page = pageCount - 1;
  }
  requestUpdate();
}

void XtcPageSelectionActivity::onExit() { Activity::onExit(); }

void XtcPageSelectionActivity::adjust(const int delta) {
  const int64_t next = static_cast<int64_t>(page) + delta;
  if (next < 0) {
    page = 0;
  } else if (next >= static_cast<int64_t>(pageCount)) {
    page = pageCount - 1;
  } else {
    page = static_cast<uint32_t>(next);
  }
  requestUpdate();
}

void XtcPageSelectionActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    setResult(PageResult{page});
    finish();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { adjust(-kSmallStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { adjust(kSmallStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] { adjust(kLargeStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] { adjust(-kLargeStep); });
}

void XtcPageSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_GO_TO_PAGE), true, EpdFontFamily::BOLD);

  char text[32];
  snprintf(text, sizeof(text), "%lu / %lu", static_cast<unsigned long>(page + 1),
           static_cast<unsigned long>(pageCount));
  renderer.drawCenteredText(UI_12_FONT_ID, 90, text, true, EpdFontFamily::BOLD);

  // Position bar: where the chosen page sits in the book.
  const int screenWidth = renderer.getScreenWidth();
  constexpr int barWidth = 360;
  constexpr int barHeight = 16;
  const int barX = (screenWidth - barWidth) / 2;
  const int barY = 140;
  renderer.drawRect(barX, barY, barWidth, barHeight);
  const int fillWidth =
      pageCount > 1 ? static_cast<int>((barWidth - 4) * static_cast<int64_t>(page) / (pageCount - 1)) : barWidth - 4;
  if (fillWidth > 0) {
    renderer.fillRect(barX + 2, barY + 2, fillWidth, barHeight - 4);
  }

  renderer.drawCenteredText(SMALL_FONT_ID, barY + 30, tr(STR_PAGE_STEP_HINT), true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "-", "+");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
