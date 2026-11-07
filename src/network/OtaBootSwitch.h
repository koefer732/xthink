#pragma once

#include <esp_partition.h>

#include <cstddef>
#include <cstdint>

// Ported from CrossPoint Reader (MIT, src/network/OtaBootSwitch.*).
//
// X4 (and X3) factory bootloaders accept our patch_firmware_image.py-patched
// firmware.bin (web flasher proves this), but the running ESP-IDF's
// esp_image_verify rejects with bogus efuse-blk-rev errors. Both SD-card and
// OTA update paths bypass that runtime check by writing the OTA app partition
// raw and updating otadata directly — same scheme as the web flasher
// (crosspoint-reader-docs/src/lib/flasher/OtaPartition.ts).
//
// Layout reference: esp_flash_partitions.h. CRC covers ota_seq (4 bytes) only.

namespace ota_boot {

struct __attribute__((packed)) SelectEntry {
  uint32_t ota_seq;
  uint8_t seq_label[20];
  uint32_t ota_state;
  uint32_t crc;
};
static_assert(sizeof(SelectEntry) == 32, "SelectEntry must be 32 bytes");

constexpr uint32_t kOtaImgNew = 0;      // ESP_OTA_IMG_NEW
constexpr uint32_t kOtaImgInvalid = 3;  // ESP_OTA_IMG_INVALID
constexpr uint32_t kOtaImgAborted = 4;  // ESP_OTA_IMG_ABORTED
constexpr size_t kOtaSeqCrcLen = 4;

// CRC32-LE over the 4-byte ota_seq, init UINT32_MAX. Matches IDF and web flasher.
uint32_t computeSeqCrc(uint32_t seq);

// Switch the bootloader's selected app partition to `dest` by writing a fresh
// otadata entry into the inactive otadata slot. Bypasses esp_ota_set_boot_partition's
// esp_image_verify call. The bytes in `dest` must already be a valid app image
// (e.g. patch_firmware_image.py output) — caller is responsible for that.
//
// Returns true on success.
bool switchTo(const esp_partition_t* dest);

// XThink addition. Returns the OTA app partition we are NOT running from, or
// nullptr if it does not start with a valid app image header (0xE9 magic and
// a chip_id matching the running image), so we never point the bootloader at
// an empty or foreign slot.
const esp_partition_t* otherValidAppSlot();

// XThink addition. Escape hatch for USB-locked units: when Back + Up are held
// while the firmware starts, point otadata at the other OTA slot (normally the
// firmware that was running before the last SD update, e.g. CrossPoint) and
// reboot into it. Call as early as possible in setup(), right after
// gpio.begin(), so it still works if something later in boot is broken.
// Does nothing (returns) if the combo is not held or the other slot is empty.
void checkEscapeCombo();

}  // namespace ota_boot
