#include "XtcReaderMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int kTitleY = 15;
constexpr int kSubtitleY = 45;
constexpr int kListY = 90;
constexpr int kLineHeight = 40;
}  // namespace

void XtcReaderMenuActivity::onEnter() {
  Activity::onEnter();
  itemCount = 0;
  if (hasChapters) {
    items[itemCount++] = CHAPTERS;
  }
  items[itemCount++] = GO_TO_PAGE;
  items[itemCount++] = ZOOM;
  selectorIndex = 0;
  requestUpdate();
}

void XtcReaderMenuActivity::onExit() { Activity::onExit(); }

const char* XtcReaderMenuActivity::labelFor(const Action action) const {
  switch (action) {
    case CHAPTERS:
      return tr(STR_SELECT_CHAPTER);
    case GO_TO_PAGE:
      return tr(STR_GO_TO_PAGE);
    case ZOOM:
      return tr(STR_ZOOM_MODE);
  }
  return "";
}

void XtcReaderMenuActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    MenuResult menu;
    menu.action = items[selectorIndex];
    setResult(menu);
    finish();
    return;
  }

  buttonNavigator.onNextRelease([this] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, itemCount);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, itemCount);
    requestUpdate();
  });
}

void XtcReaderMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const int width = renderer.getScreenWidth();

  renderer.drawCenteredText(UI_12_FONT_ID, kTitleY, tr(STR_MENU), true, EpdFontFamily::BOLD);

  char pageText[32];
  snprintf(pageText, sizeof(pageText), "%lu / %lu", static_cast<unsigned long>(currentPage + 1),
           static_cast<unsigned long>(pageCount));
  renderer.drawCenteredText(UI_10_FONT_ID, kSubtitleY, pageText, true);

  renderer.fillRect(0, kListY + selectorIndex * kLineHeight - 6, width - 1, kLineHeight);
  for (int i = 0; i < itemCount; i++) {
    renderer.drawText(UI_12_FONT_ID, 20, kListY + i * kLineHeight, labelFor(items[i]), i != selectorIndex);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
