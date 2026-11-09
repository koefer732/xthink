/**
 * XtcPageStrips.h
 *
 * Reads the pixels of one XTC/XTCH page through a small strip buffer, so a page can be
 * drawn without holding its whole bitmap (48 KB in 1 bit, 96 KB in 2 bits) in RAM.
 */

#pragma once

#include <Xtc.h>

#include <cstdint>
#include <cstdlib>

class XtcPageStrips {
 public:
  XtcPageStrips(const Xtc& xtc, uint32_t page, uint16_t width, uint16_t height)
      : xtc(xtc), page(page), width(width), height(height) {
    buffer = static_cast<uint8_t*>(malloc(kBufferSize));
  }
  ~XtcPageStrips() { free(buffer); }
  XtcPageStrips(const XtcPageStrips&) = delete;
  XtcPageStrips& operator=(const XtcPageStrips&) = delete;

  bool allocated() const { return buffer != nullptr; }
  // True once any strip failed to load; the pixels read since then are white.
  bool failed() const { return readFailed; }

  // 1-bit pages: is the pixel black? Fastest when y goes up one row at a time.
  bool isBlack(const uint16_t x, const uint16_t y) {
    const size_t rowBytes = (width + 7) / 8;
    if (y < firstLine || y >= firstLine + lineCount) {
      if (!loadRows(y, rowBytes)) return false;
    }
    const uint8_t* row = buffer + static_cast<size_t>(y - firstLine) * rowBytes;
    return !((row[x / 8] >> (7 - (x % 8))) & 1);  // XTG: 0 = black, 1 = white
  }

  // 2-bit pages: 0 white, 1 dark grey, 2 light grey, 3 black. Fastest when x is the outer
  // loop and goes up one column at a time (columns are stored right to left).
  uint8_t value(const uint16_t x, const uint16_t y) {
    const size_t colBytes = (height + 7) / 8;
    const uint16_t col = width - 1 - x;
    if (col < firstLine || col >= firstLine + lineCount) {
      if (!loadColumns(col, colBytes)) return 0;
    }
    const size_t offset = static_cast<size_t>(col - firstLine) * colBytes + y / 8;
    const uint8_t bit = 7 - (y % 8);
    const uint8_t bit1 = (buffer[offset] >> bit) & 1;
    const uint8_t bit2 = (buffer[kBufferSize / 2 + offset] >> bit) & 1;
    return (bit1 << 1) | bit2;
  }

 private:
  static constexpr size_t kBufferSize = 6000;  // 100 rows of 1-bit, or 2 x 30 columns of 2-bit

  bool loadRows(const uint16_t y, const size_t rowBytes) {
    if (readFailed || !buffer) return false;
    const uint16_t rows = static_cast<uint16_t>(kBufferSize / rowBytes);
    const uint16_t count = (height - y < rows) ? height - y : rows;
    if (xtc.readPageBitmap(page, static_cast<size_t>(y) * rowBytes, buffer, count * rowBytes) == 0) {
      readFailed = true;
      return false;
    }
    firstLine = y;
    lineCount = count;
    return true;
  }

  bool loadColumns(const uint16_t col, const size_t colBytes) {
    if (readFailed || !buffer) return false;
    const uint16_t cols = static_cast<uint16_t>((kBufferSize / 2) / colBytes);
    // Drawing goes left to right, which walks the stored columns downwards: keep col at the
    // top of the strip so the next columns are already loaded.
    const uint16_t first = col + 1 >= cols ? col + 1 - cols : 0;
    const uint16_t count = (width - first < cols) ? width - first : cols;
    const size_t planeSize = (static_cast<size_t>(width) * height + 7) / 8;
    const size_t start = static_cast<size_t>(first) * colBytes;
    const size_t length = count * colBytes;
    if (xtc.readPageBitmap(page, start, buffer, length) == 0 ||
        xtc.readPageBitmap(page, planeSize + start, buffer + kBufferSize / 2, length) == 0) {
      readFailed = true;
      return false;
    }
    firstLine = first;
    lineCount = count;
    return true;
  }

  const Xtc& xtc;
  const uint32_t page;
  const uint16_t width;
  const uint16_t height;
  uint8_t* buffer = nullptr;
  uint16_t firstLine = 0;  // first row (1-bit) or stored column (2-bit) in the buffer
  uint16_t lineCount = 0;
  bool readFailed = false;
};
