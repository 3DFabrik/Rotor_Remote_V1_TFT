#pragma once

#ifndef FW_VERSION
#define FW_VERSION "08.10.2026 07:10"
#endif

// Serial trace for network diagnosis, active only while Debug is on in the System menu.
#define NETLOG(...) do { if (debug) { Serial.printf("[%lu] ", (unsigned long)millis()); Serial.printf(__VA_ARGS__); Serial.println(); } } while (0)

enum MenuPage : uint8_t {
  MP_SETUP,
  MP_WLAN,
  MP_SSID,
  MP_BT,
  MP_CAL,
  MP_SYS,
  MP_CHAR,
  MP_OCTET,
  MP_NUM
};

enum EditTarget : uint8_t { ED_SSID, ED_PASS, ED_BTNAME };
enum OctTarget : uint8_t { OCT_LOCAL, OCT_GW, OCT_MASK, OCT_DNS };
enum NumTarget : uint8_t { NUM_PORT, NUM_OVER, NUM_MEDIAN };

enum CalPhase : uint8_t {
  CAL_IDLE,
  CAL_CW_SEEK,
  CAL_CW_BACK,
  CAL_CCW_SEEK,
  CAL_CCW_BACK
};
