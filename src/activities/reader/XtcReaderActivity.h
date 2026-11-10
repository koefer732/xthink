/**
 * XtcReaderActivity.h
 *
 * XTC ebook reader activity for CrossPoint Reader
 * Displays pre-rendered XTC pages on e-ink display
 */

#pragma once

#include <Xtc.h>

#include "activities/Activity.h"

class XtcPageStrips;

class XtcReaderActivity final : public Activity {
  std::shared_ptr<Xtc> xtc;

  uint32_t currentPage = 0;
  int pagesUntilFullRefresh = 0;

  // Zoom mode: the page is magnified and the buttons move a viewport over it.
  bool zoomActive = false;
  uint8_t zoomLevel = 0;  // index into the zoom level table in the .cpp
  int viewX = 0;          // viewport origin in magnified pixels
  int viewY = 0;

  // Set while Confirm stays held after a long press toggled zoom, so its release is ignored.
  bool confirmLatched = false;

  void renderPage();
  void drawZoomed(XtcPageStrips& strips, uint16_t pageWidth, uint16_t pageHeight, uint8_t bitDepth);
  void drawZoomIndicator(uint16_t pageWidth, uint16_t pageHeight) const;
  void openMenu();
  void handleMenuAction(int action);
  void turnPage(int direction, int amount);
  void enterZoom();
  void exitZoom();
  void cycleZoomLevel();
  void zoomStep(int direction);
  void zoomPanHorizontal(int direction);
  int zoomedWidth() const;
  int zoomedHeight() const;
  void clampView();
  void saveProgress() const;
  void loadProgress();

 public:
  explicit XtcReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Xtc> xtc)
      : Activity("XtcReader", renderer, mappedInput), xtc(std::move(xtc)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
};
