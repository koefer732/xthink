#include "SdFirmwareUpdateActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_ota_ops.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/FirmwareFlasher.h"

namespace {
bool endsWithBin(const char* name) {
  const size_t n = strlen(name);
  if (n < 4) return false;
  const char* ext = name + n - 4;
  return ext[0] == '.' && (ext[1] == 'b' || ext[1] == 'B') && (ext[2] == 'i' || ext[2] == 'I') &&
         (ext[3] == 'n' || ext[3] == 'N');
}

std::string baseName(const std::string& path) {
  const auto pos = path.find_last_of('/');
  return pos == std::string::npos ? path : path.substr(pos + 1);
}
}  // namespace

void SdFirmwareUpdateActivity::onEnter() {
  Activity::onEnter();
  LOG_INF("FW", "SdFirmwareUpdateActivity recovery=%d", recoveryMode ? 1 : 0);
  state = State::PICKING;
  scanFiles();
  requestUpdate();
}

void SdFirmwareUpdateActivity::scanDir(const char* dir) {
  auto root = Storage.open(dir);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }
  char name[128];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    const bool candidate = !file.isDirectory() && name[0] != '.' && endsWithBin(name);
    file.close();
    if (!candidate) continue;
    std::string path = dir;
    if (path.back() != '/') path += '/';
    path += name;
    files.push_back(path);
  }
  root.close();
}

void SdFirmwareUpdateActivity::scanFiles() {
  files.clear();
  scanDir("/");
  scanDir("/firmware");
  std::sort(files.begin(), files.end());
  selectedIndex = 0;
}

bool SdFirmwareUpdateActivity::validateFirmware() {
  HalFile file;
  if (!Storage.openFileForRead("FW", firmwarePath.c_str(), file) || !file) {
    errorMessage = tr(STR_FIRMWARE_FILE_OPEN_FAILED);
    return false;
  }
  firmwareSize = file.fileSize();
  file.close();

  const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
  if (!dest) {
    errorMessage = tr(STR_INVALID_FIRMWARE);
    return false;
  }

  const auto vr = firmware_flash::validateImageFile(firmwarePath.c_str(), dest->size);
  if (vr != firmware_flash::Result::OK) {
    LOG_ERR("FW", "image validation failed: %s", firmware_flash::resultName(vr));
    if (vr == firmware_flash::Result::TOO_LARGE) {
      errorMessage = tr(STR_FIRMWARE_TOO_LARGE);
    } else if (vr == firmware_flash::Result::TOO_SMALL) {
      errorMessage = tr(STR_FIRMWARE_TOO_SMALL);
    } else if (vr == firmware_flash::Result::BAD_CHIP) {
      errorMessage = tr(STR_FIRMWARE_WRONG_DEVICE);
    } else {
      errorMessage = tr(STR_INVALID_FIRMWARE);
    }
    return false;
  }
  return true;
}

void SdFirmwareUpdateActivity::performUpdate() {
  LOG_INF("FW", "SD update: %s (%u bytes)", firmwarePath.c_str(), static_cast<unsigned>(firmwareSize));

  auto progressCb = +[](size_t written, size_t total, void* ctx) {
    auto* self = static_cast<SdFirmwareUpdateActivity*>(ctx);
    self->writtenBytes = written;
    self->firmwareSize = total;
    self->requestUpdate(true);
  };

  // Validate again at flash time: the SD card is removable.
  const auto result = firmware_flash::flashFromSdPath(firmwarePath.c_str(), progressCb, this);
  if (result != firmware_flash::Result::OK) {
    LOG_ERR("FW", "flash failed: %s", firmware_flash::resultName(result));
    errorMessage =
        result == firmware_flash::Result::BAD_CHIP ? tr(STR_FIRMWARE_WRONG_DEVICE) : tr(STR_FIRMWARE_WRITE_FAILED);
    RenderLock lock(*this);
    state = State::FAILED;
    requestUpdate();
    return;
  }

  LOG_INF("FW", "SD firmware update complete, restarting");
  {
    RenderLock lock(*this);
    state = State::SUCCESS;
  }
  requestUpdateAndWait();
  delay(1500);
  ESP.restart();
}

void SdFirmwareUpdateActivity::loop() {
  using Button = MappedInputManager::Button;

  if (state == State::PICKING) {
    if (mappedInput.wasPressed(Button::Back)) {
      if (recoveryMode) {
        // Re-scan so a card swapped in recovery shows up.
        scanFiles();
        requestUpdate();
      } else {
        finish();
      }
      return;
    }
    if (mappedInput.wasPressed(Button::Confirm) && !files.empty()) {
      firmwarePath = files[selectedIndex];
      {
        RenderLock lock(*this);
        state = State::VALIDATING;
      }
      requestUpdateAndWait();
      const bool ok = validateFirmware();
      RenderLock lock(*this);
      state = ok ? State::CONFIRMING : State::FAILED;
      requestUpdate();
      return;
    }
    const int count = static_cast<int>(files.size());
    if (count > 0) {
      buttonNavigator.onNext([this, count] {
        selectedIndex = ButtonNavigator::nextIndex(selectedIndex, count);
        requestUpdate();
      });
      buttonNavigator.onPrevious([this, count] {
        selectedIndex = ButtonNavigator::previousIndex(selectedIndex, count);
        requestUpdate();
      });
    }
    return;
  }

  if (state == State::CONFIRMING) {
    if (mappedInput.wasPressed(Button::Back)) {
      RenderLock lock(*this);
      state = State::PICKING;
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(Button::Confirm)) {
      {
        RenderLock lock(*this);
        state = State::UPDATING;
        writtenBytes = 0;
        lastRenderedPercent = 101;
      }
      requestUpdateAndWait();
      performUpdate();
    }
    return;
  }

  if (state == State::FAILED) {
    if (mappedInput.wasPressed(Button::Back) || mappedInput.wasPressed(Button::Confirm)) {
      RenderLock lock(*this);
      state = State::PICKING;
      requestUpdate();
    }
  }
}

void SdFirmwareUpdateActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  if (state == State::UPDATING) {
    // Throttle e-ink redraws to once per percent.
    const unsigned int pct = firmwareSize > 0 ? static_cast<unsigned int>((writtenBytes * 100) / firmwareSize) : 0;
    if (pct == lastRenderedPercent) return;
    lastRenderedPercent = pct;
  }

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 recoveryMode ? tr(STR_RECOVERY_MODE) : tr(STR_SD_FIRMWARE_UPDATE));

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int top = (pageHeight - lineHeight) / 2;

  switch (state) {
    case State::PICKING: {
      if (files.empty()) {
        renderer.drawCenteredText(UI_10_FONT_ID, top - lineHeight, tr(STR_NO_FIRMWARE_FILES), true,
                                  EpdFontFamily::BOLD);
        renderer.drawCenteredText(UI_10_FONT_ID, top + metrics.verticalSpacing, tr(STR_NO_FIRMWARE_FILES_HINT));
        const auto labels = mappedInput.mapLabels(recoveryMode ? tr(STR_RETRY) : tr(STR_BACK), "", "", "");
        GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
        break;
      }
      const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
      const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
      GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(files.size()),
                   selectedIndex, [this](int i) { return baseName(files[i]); },
                   [this](int i) { return files[i]; });
      const auto labels =
          mappedInput.mapLabels(recoveryMode ? tr(STR_RETRY) : tr(STR_BACK), tr(STR_SELECT), "^", "v");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    case State::VALIDATING:
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_VALIDATING_FIRMWARE));
      break;
    case State::CONFIRMING: {
      renderer.drawCenteredText(UI_10_FONT_ID, top - lineHeight - metrics.verticalSpacing,
                                tr(STR_FIRMWARE_UPDATE_PROMPT), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, top, baseName(firmwarePath).c_str());
      renderer.drawCenteredText(UI_10_FONT_ID, top + lineHeight + metrics.verticalSpacing,
                                tr(STR_FIRMWARE_UPDATE_DO_NOT_POWER_OFF));
      const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CONFIRM), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    case State::UPDATING: {
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATING), true, EpdFontFamily::BOLD);
      int y = top + lineHeight + metrics.verticalSpacing;
      GUI.drawProgressBar(
          renderer,
          Rect{metrics.contentSidePadding, y, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
          writtenBytes, firmwareSize > 0 ? firmwareSize : 1);
      y += metrics.progressBarHeight + lineHeight + metrics.verticalSpacing * 2;
      renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_FIRMWARE_UPDATE_DO_NOT_POWER_OFF));
      break;
    }
    case State::SUCCESS:
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_COMPLETE), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, top + lineHeight + metrics.verticalSpacing, tr(STR_RESTARTING_HINT));
      break;
    case State::FAILED: {
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_FAILED), true, EpdFontFamily::BOLD);
      if (!errorMessage.empty()) {
        renderer.drawCenteredText(UI_10_FONT_ID, top + lineHeight + metrics.verticalSpacing, errorMessage.c_str());
      }
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
  }

  renderer.displayBuffer();
}
