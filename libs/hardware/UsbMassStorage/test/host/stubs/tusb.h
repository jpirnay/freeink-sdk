#pragma once

// Host stub of the TinyUSB device configuration UsbMassStorage::begin() uses to
// double-buffer the bulk IN endpoint (arduino_tinyusb's src/device/usbd.h).
// Records the last call so a test can see what begin() asked for.

#include <cstdint>

enum {
  TUD_CFGID_INVALID = 0,
  TUD_CFGID_DWC2 = 100,
};

typedef struct {
  uint16_t bm_double_buffered;
  bool vbus_sensing;
} tud_configure_dwc2_t;

#define CFG_TUD_CONFIGURE_DWC2_DEFAULT {.bm_double_buffered = 0, .vbus_sensing = false}

typedef union {
  tud_configure_dwc2_t dwc2;
} tud_configure_param_t;

inline int gTudConfigureCalls = 0;
inline uint16_t gTudDoubleBuffered = 0;

inline bool tud_configure(uint8_t, uint32_t cfg_id, const void* cfg_param) {
  ++gTudConfigureCalls;
  if (cfg_id == TUD_CFGID_DWC2 && cfg_param) {
    gTudDoubleBuffered = static_cast<const tud_configure_param_t*>(cfg_param)->dwc2.bm_double_buffered;
  }
  return true;
}
