#include "OtaBootSwitch.h"

#include <Arduino.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <esp_ota_ops.h>
#include <esp_rom_crc.h>
#include <spi_flash_mmap.h>
#include <string.h>

namespace ota_boot {

uint32_t computeSeqCrc(uint32_t seq) {
  return esp_rom_crc32_le(UINT32_MAX, reinterpret_cast<const uint8_t*>(&seq), kOtaSeqCrcLen);
}

bool switchTo(const esp_partition_t* dest) {
  if (!dest) return false;

  const esp_partition_t* otadata =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  if (!otadata) {
    LOG_ERR("BOOT", "otadata partition not found");
    return false;
  }
  if (otadata->size < 2 * SPI_FLASH_SEC_SIZE) {
    LOG_ERR("BOOT", "otadata too small: %u", static_cast<unsigned>(otadata->size));
    return false;
  }

  SelectEntry slots[2] = {};
  if (esp_partition_read(otadata, 0, &slots[0], sizeof(SelectEntry)) != ESP_OK ||
      esp_partition_read(otadata, SPI_FLASH_SEC_SIZE, &slots[1], sizeof(SelectEntry)) != ESP_OK) {
    LOG_ERR("BOOT", "otadata read failed");
    return false;
  }

  // Pick the slot with valid CRC and highest seq, ignoring INVALID/ABORTED.
  int activeIdx = -1;
  uint32_t activeSeq = 0;
  for (int i = 0; i < 2; ++i) {
    if (slots[i].ota_seq == 0xFFFFFFFFu) continue;
    if (slots[i].crc != computeSeqCrc(slots[i].ota_seq)) continue;
    if (slots[i].ota_state == kOtaImgInvalid || slots[i].ota_state == kOtaImgAborted) continue;
    if (activeIdx < 0 || slots[i].ota_seq > activeSeq) {
      activeIdx = i;
      activeSeq = slots[i].ota_seq;
    }
  }
  LOG_INF("BOOT", "otadata: active slot=%d seq=%u", activeIdx, static_cast<unsigned>(activeSeq));

  // ota_seq encoding: (seq - 1) % NUM_OTA_PARTITIONS picks the partition.
  const uint32_t destOtaIdx =
      static_cast<uint32_t>(dest->subtype) - static_cast<uint32_t>(ESP_PARTITION_SUBTYPE_APP_OTA_0);
  if (destOtaIdx > 15) {
    LOG_ERR("BOOT", "dest is not an OTA app partition (subtype=0x%02X)", dest->subtype);
    return false;
  }

  // Find smallest seq > activeSeq such that (seq-1) % 2 == destOtaIdx,
  // assuming 2 OTA partitions (matches our partitions.csv with ota_0 + ota_1).
  uint32_t newSeq = activeSeq + 1;
  while (((newSeq - 1u) % 2u) != (destOtaIdx % 2u)) ++newSeq;

  SelectEntry next = {};
  next.ota_seq = newSeq;
  memset(next.seq_label, 0xFF, sizeof(next.seq_label));
  next.ota_state = kOtaImgNew;
  next.crc = computeSeqCrc(next.ota_seq);

  // Write to the OTHER slot (so the bootloader sees a higher seq there).
  const int targetSlot = (activeIdx == 0) ? 1 : 0;
  const size_t targetOff = static_cast<size_t>(targetSlot) * SPI_FLASH_SEC_SIZE;

  if (esp_partition_erase_range(otadata, targetOff, SPI_FLASH_SEC_SIZE) != ESP_OK) {
    LOG_ERR("BOOT", "otadata erase failed (slot=%d)", targetSlot);
    return false;
  }
  if (esp_partition_write(otadata, targetOff, &next, sizeof(next)) != ESP_OK) {
    LOG_ERR("BOOT", "otadata write failed (slot=%d)", targetSlot);
    return false;
  }

  LOG_INF("BOOT", "otadata: wrote slot=%d seq=%u crc=0x%08x -> %s", targetSlot, static_cast<unsigned>(newSeq),
          static_cast<unsigned>(next.crc), dest->label);
  return true;
}

const esp_partition_t* otherValidAppSlot() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
  if (!running || !other || other == running) return nullptr;

  uint8_t header[16];
  if (esp_partition_read(other, 0, header, sizeof(header)) != ESP_OK) return nullptr;
  if (header[0] != 0xE9) return nullptr;

  uint16_t otherChip = 0;
  uint16_t runningChip = 0xFFFF;
  memcpy(&otherChip, header + 12, sizeof(otherChip));
  if (esp_partition_read(running, 12, &runningChip, sizeof(runningChip)) != ESP_OK) return nullptr;
  if (otherChip != runningChip) return nullptr;
  return other;
}

void checkEscapeCombo() {
  // Buttons are an ADC ladder: Back sits on GPIO1, Up on GPIO2, so the pair can
  // be read together. Sample for a short while so debounce settles.
  bool held = false;
  const unsigned long start = millis();
  while (millis() - start < 120) {
    gpio.update();
    held = gpio.isPressed(HalGPIO::BTN_BACK) && gpio.isPressed(HalGPIO::BTN_UP);
    if (!held && millis() - start > 40) return;
    delay(10);
  }
  if (!held) return;

  const esp_partition_t* other = otherValidAppSlot();
  if (!other) {
    LOG_ERR("BOOT", "escape combo held but other slot has no valid app; continuing");
    return;
  }
  LOG_INF("BOOT", "escape combo held: switching to %s and rebooting", other->label);
  if (!switchTo(other)) return;
  delay(100);
  esp_restart();
}

}  // namespace ota_boot
