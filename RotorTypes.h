#pragma once

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
enum NumTarget : uint8_t { NUM_PORT, NUM_OVER };

enum CalPhase : uint8_t {
  CAL_IDLE,
  CAL_CW_SEEK,
  CAL_CW_BACK,
  CAL_CCW_SEEK,
  CAL_CCW_BACK
};
