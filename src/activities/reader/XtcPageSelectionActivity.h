#pragma once

#include <cstdint>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * XtcPageSelectionActivity
 *
 * Page-number selector for the XTC reader. Left/Right move one page, Up/Down move ten
 * (both repeat while held). Returns a PageResult with a 0-based page index.
 */
class XtcPageSelectionActivity final : public Activity {
 public:
  explicit XtcPageSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, uint32_t currentPage,
                                    uint32_t pageCount)
      : Activity("XtcPageSelection", renderer, mappedInput), page(currentPage), pageCount(pageCount) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }

 private:
  uint32_t page;
  uint32_t pageCount;
  ButtonNavigator buttonNavigator{150, 400};

  void adjust(int delta);
};
