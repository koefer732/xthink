/**
 * XtcReaderActivity.cpp
 *
 * XTC ebook reader activity implementation
 * Displays pre-rendered XTC pages on e-ink display
 */

#include "XtcReaderActivity.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "XtcPageSelectionActivity.h"
#include "XtcReaderChapterSelectionActivity.h"
#include "XtcReaderMenuActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr unsigned long skipPageMs = 700;
constexpr unsigned long goHomeMs = 1000;
// How long a side-button page turn waits for the other side button (zoom chord).
constexpr unsigned long chordWindowMs = 120;

// Zoom levels as num/den: 1.5x and 2x.
struct ZoomLevel {
  uint8_t num;
  uint8_t den;
  const char* label;
};
constexpr ZoomLevel kZoomLevels[] = {{3, 2, "1.5x"}, {2, 1, "2x"}};
constexpr int kZoomLevelCount = sizeof(kZoomLevels) / sizeof(kZoomLevels[0]);

// A vertical zoom step moves at most 85% of the screen height.
constexpr int kStepPercent = 85;
}  // namespace

void XtcReaderActivity::onEnter() {
  Activity::onEnter();

  if (!xtc) {
    return;
  }

  xtc->setupCacheDir();

  // Load saved progress
  loadProgress();

  // Save current XTC as last opened book and add to recent books
  APP_STATE.openEpubPath = xtc->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(xtc->getPath(), xtc->getTitle(), xtc->getAuthor(), xtc->getThumbBmpPath());

  // Trigger first update
  requestUpdate();
}

void XtcReaderActivity::onExit() {
  Activity::onExit();

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  xtc.reset();
}

void XtcReaderActivity::loop() {
  using Button = MappedInputManager::Button;

  // Zoom chord: both side buttons together. Ignore every page button until both are released.
  const bool sideBack = mappedInput.isPressed(Button::PageBack);
  const bool sideForward = mappedInput.isPressed(Button::PageForward);
  if (sideBack && sideForward) {
    if (!chordLatched) {
      chordLatched = true;
      pendingTurn = 0;
      if (zoomActive) {
        exitZoom();
      } else {
        enterZoom();
      }
    }
    return;
  }
  if (chordLatched) {
    if (!sideBack && !sideForward) {
      chordLatched = false;
    }
    return;
  }

  // A side-button turn that waited out the chord window happens now.
  if (pendingTurn != 0 && millis() - pendingSince >= chordWindowMs) {
    const int direction = pendingTurn;
    pendingTurn = 0;
    if (zoomActive) {
      zoomStep(direction);
    } else {
      turnPage(direction, 1);
    }
    return;
  }

  if (zoomActive) {
    if (mappedInput.wasReleased(Button::Back)) {
      exitZoom();
      return;
    }
    if (mappedInput.wasReleased(Button::Confirm)) {
      cycleZoomLevel();
      return;
    }
    if (mappedInput.wasPressed(Button::Left)) {
      zoomPanHorizontal(-1);
      return;
    }
    if (mappedInput.wasPressed(Button::Right)) {
      zoomPanHorizontal(1);
      return;
    }
    if (mappedInput.wasPressed(Button::PageForward)) {
      pendingTurn = 1;
      pendingSince = millis();
    } else if (mappedInput.wasPressed(Button::PageBack)) {
      pendingTurn = -1;
      pendingSince = millis();
    }
    return;
  }

  // Confirm opens the reader menu (chapters, go to page, zoom)
  if (mappedInput.wasReleased(Button::Confirm)) {
    openMenu();
    return;
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(Button::Back) && mappedInput.getHeldTime() >= goHomeMs) {
    activityManager.goToFileBrowser(xtc ? xtc->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home
  if (mappedInput.wasReleased(Button::Back) && mappedInput.getHeldTime() < goHomeMs) {
    onGoHome();
    return;
  }

  // When long-press chapter skip is disabled, turn pages on press instead of release.
  const bool usePressForPageTurn = !SETTINGS.longPressChapterSkip;
  const bool powerPageTurn =
      SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN && mappedInput.wasReleased(Button::Power);

  if (usePressForPageTurn) {
    // Front buttons turn at once; side buttons wait briefly in case the zoom chord follows.
    if (mappedInput.wasPressed(Button::Left)) {
      turnPage(-1, 1);
    } else if (mappedInput.wasPressed(Button::Right) || powerPageTurn) {
      turnPage(1, 1);
    } else if (mappedInput.wasPressed(Button::PageBack)) {
      pendingTurn = -1;
      pendingSince = millis();
    } else if (mappedInput.wasPressed(Button::PageForward)) {
      pendingTurn = 1;
      pendingSince = millis();
    }
    return;
  }

  // Release mode: a held button skips 10 pages. The chord latch above already swallows
  // the releases that end a chord.
  const bool prevTriggered = mappedInput.wasReleased(Button::PageBack) || mappedInput.wasReleased(Button::Left);
  const bool nextTriggered =
      mappedInput.wasReleased(Button::PageForward) || powerPageTurn || mappedInput.wasReleased(Button::Right);
  if (!prevTriggered && !nextTriggered) {
    return;
  }
  const bool skipPages = SETTINGS.longPressChapterSkip && mappedInput.getHeldTime() > skipPageMs;
  turnPage(prevTriggered ? -1 : 1, skipPages ? 10 : 1);
}

void XtcReaderActivity::turnPage(const int direction, const int amount) {
  if (!xtc) {
    return;
  }
  const uint32_t pageCount = xtc->getPageCount();
  if (currentPage >= pageCount) {
    currentPage = pageCount - 1;
    requestUpdate();
    return;
  }
  if (direction < 0) {
    currentPage = currentPage >= static_cast<uint32_t>(amount) ? currentPage - amount : 0;
  } else {
    currentPage += amount;
    if (currentPage >= pageCount) {
      currentPage = pageCount;  // Allow showing "End of book"
    }
  }
  requestUpdate();
}

void XtcReaderActivity::openMenu() {
  const bool hasChapters = xtc && xtc->hasChapters() && !xtc->getChapters().empty();
  startActivityForResult(
      std::make_unique<XtcReaderMenuActivity>(renderer, mappedInput, hasChapters, currentPage, xtc->getPageCount()),
      [this](const ActivityResult& result) {
        // The activity manager allows starting the next activity from a result handler.
        if (!result.isCancelled) {
          handleMenuAction(std::get<MenuResult>(result.data).action);
        }
      });
}

void XtcReaderActivity::handleMenuAction(const int action) {
  switch (action) {
    case XtcReaderMenuActivity::CHAPTERS:
      startActivityForResult(
          std::make_unique<XtcReaderChapterSelectionActivity>(renderer, mappedInput, xtc, currentPage),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              currentPage = std::get<PageResult>(result.data).page;
            }
          });
      break;
    case XtcReaderMenuActivity::GO_TO_PAGE:
      startActivityForResult(
          std::make_unique<XtcPageSelectionActivity>(renderer, mappedInput, currentPage, xtc->getPageCount()),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              currentPage = std::get<PageResult>(result.data).page;
            }
          });
      break;
    case XtcReaderMenuActivity::ZOOM:
      enterZoom();
      break;
    default:
      requestUpdate();
      break;
  }
}

// ---------------------------------------------------------------------------
// Zoom

int XtcReaderActivity::zoomedWidth() const {
  const auto& z = kZoomLevels[zoomLevel];
  return xtc->getPageWidth() * z.num / z.den;
}

int XtcReaderActivity::zoomedHeight() const {
  const auto& z = kZoomLevels[zoomLevel];
  return xtc->getPageHeight() * z.num / z.den;
}

void XtcReaderActivity::clampView() {
  const int maxX = zoomedWidth() - xtc->getPageWidth();
  const int maxY = zoomedHeight() - xtc->getPageHeight();
  viewX = viewX < 0 ? 0 : (viewX > maxX ? maxX : viewX);
  viewY = viewY < 0 ? 0 : (viewY > maxY ? maxY : viewY);
}

void XtcReaderActivity::enterZoom() {
  if (!xtc || currentPage >= xtc->getPageCount()) {
    return;
  }
  zoomActive = true;
  viewX = 0;
  viewY = 0;
  clampView();
  requestUpdate();
}

void XtcReaderActivity::exitZoom() {
  zoomActive = false;
  pagesUntilFullRefresh = 0;  // clean full refresh when returning to the whole page
  requestUpdate();
}

void XtcReaderActivity::cycleZoomLevel() {
  // Keep the centre of the screen on the same spot of the page.
  const int w = xtc->getPageWidth();
  const int h = xtc->getPageHeight();
  const auto& from = kZoomLevels[zoomLevel];
  const int64_t cx = static_cast<int64_t>(viewX + w / 2) * from.den / from.num;
  const int64_t cy = static_cast<int64_t>(viewY + h / 2) * from.den / from.num;
  zoomLevel = (zoomLevel + 1) % kZoomLevelCount;
  const auto& to = kZoomLevels[zoomLevel];
  viewX = static_cast<int>(cx * to.num / to.den) - w / 2;
  viewY = static_cast<int>(cy * to.num / to.den) - h / 2;
  clampView();
  requestUpdate();
}

void XtcReaderActivity::zoomStep(const int direction) {
  const int w = xtc->getPageWidth();
  const int h = xtc->getPageHeight();
  const int maxX = zoomedWidth() - w;
  const int maxY = zoomedHeight() - h;
  // Split each axis into equal steps so no step is a thin sliver. Vertical steps keep at
  // least 15% overlap, so a line cut at the bottom edge shows again at the top.
  const int maxStepY = h * kStepPercent / 100;
  const int rows = maxY > 0 ? (maxY + maxStepY - 1) / maxStepY : 1;
  const int stepY = maxY > 0 ? (maxY + rows - 1) / rows : h;
  const int cols = maxX > 0 ? (maxX + w - 1) / w : 1;
  const int stepX = maxX > 0 ? (maxX + cols - 1) / cols : w;

  if (direction > 0) {
    // Down the column, then the next column from the top, then the next page.
    if (viewY < maxY) {
      viewY += stepY;
    } else if (viewX < maxX) {
      viewX += stepX;
      viewY = 0;
    } else {
      if (currentPage + 1 >= xtc->getPageCount()) {
        currentPage = xtc->getPageCount();  // "End of book"
        exitZoom();
        return;
      }
      currentPage++;
      viewX = 0;
      viewY = 0;
    }
  } else {
    if (viewY > 0) {
      viewY -= stepY;
    } else if (viewX > 0) {
      viewX -= stepX;
      viewY = maxY;
    } else {
      if (currentPage == 0) {
        return;
      }
      currentPage--;
      viewX = maxX;
      viewY = maxY;
    }
  }
  clampView();
  requestUpdate();
}

void XtcReaderActivity::zoomPanHorizontal(const int direction) {
  viewX += direction * (xtc->getPageWidth() / 2);
  clampView();
  requestUpdate();
}

void XtcReaderActivity::render(RenderLock&&) {
  if (!xtc) {
    return;
  }

  // Bounds check
  if (currentPage >= xtc->getPageCount()) {
    // Show end of book screen
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  renderPage();
  saveProgress();
}

void XtcReaderActivity::renderPage() {
  const uint16_t pageWidth = xtc->getPageWidth();
  const uint16_t pageHeight = xtc->getPageHeight();
  const uint8_t bitDepth = xtc->getBitDepth();

  // Calculate buffer size for one page
  // XTG (1-bit): Row-major, ((width+7)/8) * height bytes
  // XTH (2-bit): Two bit planes, column-major, ((width * height + 7) / 8) * 2 bytes
  size_t pageBufferSize;
  if (bitDepth == 2) {
    pageBufferSize = ((static_cast<size_t>(pageWidth) * pageHeight + 7) / 8) * 2;
  } else {
    pageBufferSize = ((pageWidth + 7) / 8) * pageHeight;
  }

  // Allocate page buffer
  uint8_t* pageBuffer = static_cast<uint8_t*>(malloc(pageBufferSize));
  if (!pageBuffer) {
    LOG_ERR("XTR", "Failed to allocate page buffer (%lu bytes)", pageBufferSize);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_MEMORY_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Load page data
  size_t bytesRead = xtc->loadPage(currentPage, pageBuffer, pageBufferSize);
  if (bytesRead == 0) {
    LOG_ERR("XTR", "Failed to load page %lu", currentPage);
    free(pageBuffer);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Clear screen first
  renderer.clearScreen();

  if (zoomActive) {
    drawZoomed(pageBuffer, pageWidth, pageHeight, bitDepth);
    free(pageBuffer);
    drawZoomIndicator(pageWidth, pageHeight);
    if (pagesUntilFullRefresh <= 1) {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    } else {
      renderer.displayBuffer();
      pagesUntilFullRefresh--;
    }
    LOG_DBG("XTR", "Rendered page %lu zoomed %s at %d,%d", currentPage + 1, kZoomLevels[zoomLevel].label, viewX, viewY);
    return;
  }

  // Copy page bitmap using GfxRenderer's drawPixel
  // XTC/XTCH pages are pre-rendered with status bar included, so render full page
  const uint16_t maxSrcY = pageHeight;

  if (bitDepth == 2) {
    // XTH 2-bit mode: Two bit planes, column-major order
    // - Columns scanned right to left (x = width-1 down to 0)
    // - 8 vertical pixels per byte (MSB = topmost pixel in group)
    // - First plane: Bit1, Second plane: Bit2
    // - Pixel value = (bit1 << 1) | bit2
    // - Grayscale: 0=White, 1=Dark Grey, 2=Light Grey, 3=Black

    const size_t planeSize = (static_cast<size_t>(pageWidth) * pageHeight + 7) / 8;
    const uint8_t* plane1 = pageBuffer;              // Bit1 plane
    const uint8_t* plane2 = pageBuffer + planeSize;  // Bit2 plane
    const size_t colBytes = (pageHeight + 7) / 8;    // Bytes per column (100 for 800 height)

    // Lambda to get pixel value at (x, y)
    auto getPixelValue = [&](uint16_t x, uint16_t y) -> uint8_t {
      const size_t colIndex = pageWidth - 1 - x;
      const size_t byteInCol = y / 8;
      const size_t bitInByte = 7 - (y % 8);
      const size_t byteOffset = colIndex * colBytes + byteInCol;
      const uint8_t bit1 = (plane1[byteOffset] >> bitInByte) & 1;
      const uint8_t bit2 = (plane2[byteOffset] >> bitInByte) & 1;
      return (bit1 << 1) | bit2;
    };

    // Optimized grayscale rendering without storeBwBuffer (saves 48KB peak memory)
    // Flow: BW display → LSB/MSB passes → grayscale display → re-render BW for next frame

    // Count pixel distribution for debugging
    uint32_t pixelCounts[4] = {0, 0, 0, 0};
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        pixelCounts[getPixelValue(x, y)]++;
      }
    }
    LOG_DBG("XTR", "Pixel distribution: White=%lu, DarkGrey=%lu, LightGrey=%lu, Black=%lu", pixelCounts[0],
            pixelCounts[1], pixelCounts[2], pixelCounts[3]);

    // Pass 1: BW buffer - draw all non-white pixels as black
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) >= 1) {
          renderer.drawPixel(x, y, true);
        }
      }
    }

    // Display BW with conditional refresh based on pagesUntilFullRefresh
    if (pagesUntilFullRefresh <= 1) {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    } else {
      renderer.displayBuffer();
      pagesUntilFullRefresh--;
    }

    // Pass 2: LSB buffer - mark DARK gray only (XTH value 1)
    // In LUT: 0 bit = apply gray effect, 1 bit = untouched
    renderer.clearScreen(0x00);
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) == 1) {  // Dark grey only
          renderer.drawPixel(x, y, false);
        }
      }
    }
    renderer.copyGrayscaleLsbBuffers();

    // Pass 3: MSB buffer - mark LIGHT AND DARK gray (XTH value 1 or 2)
    // In LUT: 0 bit = apply gray effect, 1 bit = untouched
    renderer.clearScreen(0x00);
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        const uint8_t pv = getPixelValue(x, y);
        if (pv == 1 || pv == 2) {  // Dark grey or Light grey
          renderer.drawPixel(x, y, false);
        }
      }
    }
    renderer.copyGrayscaleMsbBuffers();

    // Display grayscale overlay
    renderer.displayGrayBuffer();

    // Pass 4: Re-render BW to framebuffer (restore for next frame, instead of restoreBwBuffer)
    renderer.clearScreen();
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) >= 1) {
          renderer.drawPixel(x, y, true);
        }
      }
    }

    // Cleanup grayscale buffers with current frame buffer
    renderer.cleanupGrayscaleWithFrameBuffer();

    free(pageBuffer);

    LOG_DBG("XTR", "Rendered page %lu/%lu (2-bit grayscale)", currentPage + 1, xtc->getPageCount());
    return;
  } else {
    // 1-bit mode: 8 pixels per byte, MSB first
    const size_t srcRowBytes = (pageWidth + 7) / 8;  // 60 bytes for 480 width

    for (uint16_t srcY = 0; srcY < maxSrcY; srcY++) {
      const size_t srcRowStart = srcY * srcRowBytes;

      for (uint16_t srcX = 0; srcX < pageWidth; srcX++) {
        // Read source pixel (MSB first, bit 7 = leftmost pixel)
        const size_t srcByte = srcRowStart + srcX / 8;
        const size_t srcBit = 7 - (srcX % 8);
        const bool isBlack = !((pageBuffer[srcByte] >> srcBit) & 1);  // XTC: 0 = black, 1 = white

        if (isBlack) {
          renderer.drawPixel(srcX, srcY, true);
        }
      }
    }
  }
  // White pixels are already cleared by clearScreen()

  free(pageBuffer);

  // XTC pages already have status bar pre-rendered, no need to add our own

  // Display with appropriate refresh
  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }

  LOG_DBG("XTR", "Rendered page %lu/%lu (%u-bit)", currentPage + 1, xtc->getPageCount(), bitDepth);
}

void XtcReaderActivity::drawZoomed(const uint8_t* pageBuffer, const uint16_t pageWidth, const uint16_t pageHeight,
                                   const uint8_t bitDepth) {
  // Nearest-neighbour magnification of the visible part of the page. Grey levels are
  // shown in black and white here: dark grey and black become black.
  const auto& z = kZoomLevels[zoomLevel];
  if (bitDepth == 2) {
    const size_t planeSize = (static_cast<size_t>(pageWidth) * pageHeight + 7) / 8;
    const uint8_t* plane1 = pageBuffer;
    const uint8_t* plane2 = pageBuffer + planeSize;
    const size_t colBytes = (pageHeight + 7) / 8;
    for (uint16_t sy = 0; sy < pageHeight; sy++) {
      const uint32_t srcY = static_cast<uint32_t>(viewY + sy) * z.den / z.num;
      if (srcY >= pageHeight) break;
      const size_t byteInCol = srcY / 8;
      const uint8_t bitInByte = 7 - (srcY % 8);
      for (uint16_t sx = 0; sx < pageWidth; sx++) {
        const uint32_t srcX = static_cast<uint32_t>(viewX + sx) * z.den / z.num;
        if (srcX >= pageWidth) break;
        const size_t byteOffset = (pageWidth - 1 - srcX) * colBytes + byteInCol;
        const uint8_t bit1 = (plane1[byteOffset] >> bitInByte) & 1;
        const uint8_t bit2 = (plane2[byteOffset] >> bitInByte) & 1;
        const uint8_t value = (bit1 << 1) | bit2;  // 0 white, 1 dark grey, 2 light grey, 3 black
        if (value == 1 || value == 3) {
          renderer.drawPixel(sx, sy, true);
        }
      }
    }
    return;
  }

  const size_t srcRowBytes = (pageWidth + 7) / 8;
  for (uint16_t sy = 0; sy < pageHeight; sy++) {
    const uint32_t srcY = static_cast<uint32_t>(viewY + sy) * z.den / z.num;
    if (srcY >= pageHeight) break;
    const uint8_t* row = pageBuffer + srcY * srcRowBytes;
    for (uint16_t sx = 0; sx < pageWidth; sx++) {
      const uint32_t srcX = static_cast<uint32_t>(viewX + sx) * z.den / z.num;
      if (srcX >= pageWidth) break;
      const bool isBlack = !((row[srcX / 8] >> (7 - (srcX % 8))) & 1);  // XTC: 0 = black, 1 = white
      if (isBlack) {
        renderer.drawPixel(sx, sy, true);
      }
    }
  }
}

void XtcReaderActivity::drawZoomIndicator(const uint16_t pageWidth, const uint16_t pageHeight) const {
  // Mini map in the bottom-right corner: the page outline with the visible part filled.
  constexpr int mapW = 30;
  constexpr int mapH = 50;
  constexpr int pad = 4;
  const int boxW = mapW + 2 * pad;
  const int boxH = mapH + 2 * pad + 16;
  const int boxX = pageWidth - boxW - 6;
  const int boxY = pageHeight - boxH - 6;

  renderer.fillRect(boxX, boxY, boxW, boxH, false);
  renderer.drawRect(boxX, boxY, boxW, boxH);

  const int mapX = boxX + pad;
  const int mapY = boxY + pad;
  renderer.drawRect(mapX, mapY, mapW, mapH);
  const int zw = zoomedWidth();
  const int zh = zoomedHeight();
  const int vx = mapX + viewX * mapW / zw;
  const int vy = mapY + viewY * mapH / zh;
  const int vw = pageWidth * mapW / zw;
  const int vh = pageHeight * mapH / zh;
  renderer.fillRect(vx, vy, vw, vh);

  renderer.drawText(SMALL_FONT_ID, mapX, mapY + mapH + 2, kZoomLevels[zoomLevel].label);
}

void XtcReaderActivity::saveProgress() const {
  FsFile f;
  if (Storage.openFileForWrite("XTR", xtc->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    data[0] = currentPage & 0xFF;
    data[1] = (currentPage >> 8) & 0xFF;
    data[2] = (currentPage >> 16) & 0xFF;
    data[3] = (currentPage >> 24) & 0xFF;
    f.write(data, 4);
    f.close();
  }
}

void XtcReaderActivity::loadProgress() {
  FsFile f;
  if (Storage.openFileForRead("XTR", xtc->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
      LOG_DBG("XTR", "Loaded progress: page %lu", currentPage);

      // Validate page number
      if (currentPage >= xtc->getPageCount()) {
        currentPage = 0;
      }
    }
    f.close();
  }
}
