#pragma once

#include <cstdint>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * XtcReaderMenuActivity
 *
 * Small menu opened with Confirm inside the XTC reader. Returns a MenuResult whose
 * `action` is one of the Action values below.
 */
class XtcReaderMenuActivity final : public Activity {
 public:
  enum Action : int { CHAPTERS = 0, GO_TO_PAGE = 1, ZOOM = 2 };

  explicit XtcReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool hasChapters,
                                 uint32_t currentPage, uint32_t pageCount)
      : Activity("XtcReaderMenu", renderer, mappedInput),
        hasChapters(hasChapters),
        currentPage(currentPage),
        pageCount(pageCount) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }

 private:
  static constexpr int kMaxItems = 3;
  Action items[kMaxItems] = {};
  int itemCount = 0;
  int selectorIndex = 0;
  bool hasChapters;
  uint32_t currentPage;
  uint32_t pageCount;
  ButtonNavigator buttonNavigator;

  const char* labelFor(Action action) const;
};
