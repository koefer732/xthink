#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * SD-card firmware update (adapted from CrossPoint Reader's activity of the same name).
 *
 * On USB-locked X4 units the SD card is the only way to reflash, so this screen must
 * always exist in XThink builds. Flow:
 *  1) list the .bin files in the SD root and in /firmware;
 *  2) on Confirm, fully validate the image (magic, segments, checksum, SHA-256, chip);
 *  3) ask for a second Confirm;
 *  4) write it into the inactive OTA slot, switch otadata and reboot.
 *
 * Reached from Settings -> System, or at boot by holding Up while pressing Power
 * (recoveryMode = true: Back does not leave the screen).
 */
class SdFirmwareUpdateActivity final : public Activity {
 public:
  explicit SdFirmwareUpdateActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool recoveryMode = false)
      : Activity("SdFirmwareUpdate", renderer, mappedInput), recoveryMode(recoveryMode) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == State::UPDATING || state == State::VALIDATING; }
  bool skipLoopDelay() override { return state == State::UPDATING; }

 private:
  enum class State { PICKING, VALIDATING, CONFIRMING, UPDATING, SUCCESS, FAILED };

  State state = State::PICKING;
  bool recoveryMode = false;
  ButtonNavigator buttonNavigator;

  std::vector<std::string> files;  // full SD paths
  int selectedIndex = 0;

  std::string firmwarePath;
  size_t firmwareSize = 0;
  size_t writtenBytes = 0;
  unsigned int lastRenderedPercent = 101;
  std::string errorMessage;

  void scanFiles();
  void scanDir(const char* dir);
  bool validateFirmware();
  void performUpdate();
};
