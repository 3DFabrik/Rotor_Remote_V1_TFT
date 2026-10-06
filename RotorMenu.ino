const int MENU_TOP = 36;
const int MENU_ROW = 28;
const int MENU_VIS = 6;

const char MENU_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 -_.,:;!?@#%&*+=/";
const int MENU_CHAR_N = (int)sizeof(MENU_CHARS) - 1;
const int MENU_CHAR_DEL = MENU_CHAR_N;
const int MENU_CHAR_OK = MENU_CHAR_N + 1;

static MenuPage menuPage = MP_SETUP;
static int menuSel = 0;
static int menuScroll = 0;

static EditTarget editTarget = ED_SSID;
static String editBuf = "";
static int editMax = 32;
static int charIdx = 0;
static MenuPage editReturn = MP_WLAN;

static int octVal[4] = {192, 168, 1, 50};
static int octSel = 0;
static OctTarget octTarget = OCT_LOCAL;

static int numVal = 0;
static int numMin = 0;
static int numMax = 9;
static NumTarget numTarget = NUM_PORT;
static int numRepeat = 0;

static char lastLive0[64] = "";
static char lastLive1[64] = "";
static char lastCalStatus[48] = "";
static char lastHeaderIp[24] = "";

static const unsigned long CAL_ABORT_MS = 5000;
static const unsigned long CAL_NEAR_MS = 1800;
static const unsigned long CAL_BACK_MS = 4000;
static const unsigned long CAL_STOP_MS = 900;
static const unsigned long CAL_SPEED_MS = 250;
static const float CAL_NOISE_BAND = 12.0f;       // Stand-Rauschen des Drahtpotis +-10
static const float CAL_PROGRESS_DIGITS = 22.0f;
static const float CAL_MIN_TRAVEL = 80.0f;
static const int CAL_BACK_DIGITS = 28;

static CalPhase calPhase = CAL_IDLE;
static int calJogCmd = 0;
static int calPhaseCmd = 0;
static float calStopRaw = 0;
static float calEndRaw = 0;
static float calPrevFilt = 0;
static float calSpeed = 0;
static float calBest = 0;
static unsigned long calPhaseAt = 0;
static unsigned long calSlowAt = 0;
static unsigned long calSpeedAt = 0;
static bool calSawMove = false;
static bool calCwDone = false;
static bool calCcwDone = false;
static bool calFirstSeek = false;
static char calStatus[48] = "";

int calMotorCmd() {
  if (calPhase != CAL_IDLE) return calPhaseCmd;
  return calJogCmd;
}

static void calSetStatus(const char *msg) {
  strncpy(calStatus, msg, sizeof(calStatus) - 1);
  calStatus[sizeof(calStatus) - 1] = '\0';
}

static void calDrawStatus() {
  if (strcmp(calStatus, lastCalStatus) == 0) return;
  strncpy(lastCalStatus, calStatus, sizeof(lastCalStatus) - 1);
  lastCalStatus[sizeof(lastCalStatus) - 1] = '\0';
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(calPhase == CAL_IDLE ? TFT_ORANGE : TFT_GREEN, TFT_BLACK);
  tft.setTextPadding(320);
  tft.drawString(calStatus, 8, 210, 2);
  tft.setTextPadding(0);
}

void calStopAll() {
  calJogCmd = 0;
  calPhase = CAL_IDLE;
  calPhaseCmd = 0;
  calSawMove = false;
  if (calStatus[0] && strncmp(calStatus, "Cal: done", 9) != 0 &&
      strncmp(calStatus, "Cal: abort", 10) != 0) {
    calSetStatus("");
  }
}

static const char *calMsgFor(CalPhase phase) {
  switch (phase) {
    case CAL_CW_SEEK: return "Cal: CW to stop";
    case CAL_CCW_SEEK: return "Cal: CCW to stop";
    case CAL_CW_BACK:
    case CAL_CCW_BACK: return "Cal: back off";
    default: return "Cal:";
  }
}

static int calCmdFor(CalPhase phase) {
  if (phase == CAL_CW_SEEK || phase == CAL_CCW_BACK) return 2;
  if (phase == CAL_CCW_SEEK || phase == CAL_CW_BACK) return 1;
  return 0;
}

static void calBeginPhase(CalPhase phase, int cmd, const char *msg) {
  calPhase = phase;
  calPhaseCmd = cmd;
  calJogCmd = 0;
  calStopRaw = dig_AZ_m;
  calPrevFilt = dig_AZ_m;
  calSpeed = 0;
  calBest = 0;
  calPhaseAt = millis();
  calSlowAt = millis();
  calSpeedAt = millis();
  calSawMove = false;
  calSetStatus(msg);
}

static void calAbort(const char *msg) {
  calPhase = CAL_IDLE;
  calPhaseCmd = 0;
  calJogCmd = 0;
  calSetStatus(msg);
}

static void calUpdateSpeed() {
  unsigned long now = millis();
  if (calSpeedAt == 0) {
    calSpeedAt = now;
    calPrevFilt = dig_AZ_m;
    return;
  }
  unsigned long dtMs = now - calSpeedAt;
  if (dtMs < CAL_SPEED_MS) return;
  float d = fabsf(dig_AZ_m - calPrevFilt);
  calPrevFilt = dig_AZ_m;
  calSpeedAt = now;
  float inst = (d < CAL_NOISE_BAND) ? 0.0f : (d * 1000.0f / (float)dtMs);
  calSpeed = calSpeed * 0.8f + inst * 0.2f;
  if (calSpeed < 8.0f) calSpeed = 0;
}

static bool calNearEnd(float stored) {
  int span = abs(az_max_digit - az_min_digit);
  if (span < 400) return false;
  int eps = span / 12;
  if (eps < 80) eps = 80;
  return fabsf(dig_AZ_m - stored) <= (float)eps;
}

static void calStartBack(CalPhase backPhase) {
  calEndRaw = dig_AZ_m;
  calBeginPhase(backPhase, calCmdFor(backPhase), calMsgFor(backPhase));
}

static void calAfterBackOk() {
  if (calPhase == CAL_CW_BACK) {
    az_max_digit = (int)calEndRaw;
    preferences.putInt("az_max_digit", az_max_digit);
    calCwDone = true;
  } else {
    az_min_digit = (int)calEndRaw;
    preferences.putInt("az_min_digit", az_min_digit);
    calCcwDone = true;
  }
  if (calCwDone && calCcwDone) {
    calPhase = CAL_IDLE;
    calPhaseCmd = 0;
    calSetStatus("Cal: done");
    return;
  }
  CalPhase next = calCwDone ? CAL_CCW_SEEK : CAL_CW_SEEK;
  calFirstSeek = false;
  calBeginPhase(next, calCmdFor(next), calMsgFor(next));
}

static void calStartAuto() {
  calCwDone = false;
  calCcwDone = false;
  calFirstSeek = true;
  int span = abs(az_max_digit - az_min_digit);
  bool startCcw = false;
  if (span >= 400) {
    startCcw = fabsf(dig_AZ_m - (float)az_min_digit) <= fabsf(dig_AZ_m - (float)az_max_digit);
  }
  CalPhase first = startCcw ? CAL_CCW_SEEK : CAL_CW_SEEK;
  calBeginPhase(first, calCmdFor(first), calMsgFor(first));
}

void calService() {
  calUpdateSpeed();
  if (calPhase == CAL_IDLE) return;

  float pos = dig_AZ_m;
  float traveled = fabsf(pos - calStopRaw);
  unsigned long now = millis();

  if (traveled >= calBest + CAL_PROGRESS_DIGITS) {
    calBest = traveled;
    calSawMove = true;
    calSlowAt = now;
  }

  if (!calSawMove) {
    unsigned long waitMs = CAL_ABORT_MS;
    bool maybeAlreadyThere = false;
    if (calPhase == CAL_CW_SEEK) {
      maybeAlreadyThere = calFirstSeek && calNearEnd((float)az_max_digit);
      if (maybeAlreadyThere) waitMs = CAL_NEAR_MS;
    } else if (calPhase == CAL_CCW_SEEK) {
      maybeAlreadyThere = calFirstSeek && calNearEnd((float)az_min_digit);
      if (maybeAlreadyThere) waitMs = CAL_NEAR_MS;
    }
    if (now - calPhaseAt >= waitMs) {
      if (maybeAlreadyThere) {
        calStartBack(calPhase == CAL_CW_SEEK ? CAL_CW_BACK : CAL_CCW_BACK);
      } else {
        calAbort("Cal: aborted, no travel");
      }
      return;
    }
  }

  if (calPhase == CAL_CW_SEEK || calPhase == CAL_CCW_SEEK) {
    if (calSawMove && calBest >= CAL_MIN_TRAVEL && (now - calSlowAt >= CAL_STOP_MS)) {
      calStartBack(calPhase == CAL_CW_SEEK ? CAL_CW_BACK : CAL_CCW_BACK);
    }
    return;
  }

  if (calPhase == CAL_CW_BACK || calPhase == CAL_CCW_BACK) {
    if (now - calPhaseAt >= CAL_BACK_MS) {
      calAbort("Cal: aborted, no travel");
      return;
    }
    if (traveled >= (float)CAL_BACK_DIGITS) {
      calAfterBackOk();
    }
  }
}

static void menuClearLiveCache() {
  lastLive0[0] = '\0';
  lastLive1[0] = '\0';
  lastHeaderIp[0] = '\0';
}

static void menuFreeSprites() {
  spr.deleteSprite();
  spr_angle.deleteSprite();
}

static void parseIp(const String &s, int o[4]) {
  o[0] = 0;
  o[1] = 0;
  o[2] = 0;
  o[3] = 0;
  int idx = 0;
  int val = 0;
  bool any = false;
  for (unsigned i = 0; i <= s.length() && idx < 4; i++) {
    char c = (i < s.length()) ? s.charAt(i) : '.';
    if (c >= '0' && c <= '9') {
      val = val * 10 + (c - '0');
      if (val > 255) val = 255;
      any = true;
    } else if (c == '.') {
      if (any) o[idx++] = val;
      val = 0;
      any = false;
    }
  }
}

static String joinIp(const int o[4]) {
  return String(o[0]) + "." + String(o[1]) + "." + String(o[2]) + "." + String(o[3]);
}

static const char *menuTitle() {
  switch (menuPage) {
    case MP_WLAN: return "Wi-Fi";
    case MP_SSID: return "SSID";
    case MP_BT: return "Bluetooth";
    case MP_CAL: return "Calibration";
    case MP_SYS: return "System";
    case MP_CHAR:
      if (editTarget == ED_PASS) return "Password";
      if (editTarget == ED_BTNAME) return "BT name";
      return "SSID";
    case MP_OCTET:
      if (octTarget == OCT_GW) return "Gateway";
      if (octTarget == OCT_MASK) return "Netmask";
      if (octTarget == OCT_DNS) return "DNS";
      return "IP address";
    case MP_NUM:
      return (numTarget == NUM_OVER) ? "Overshoot" : "rotctld port";
    default: return "Setup";
  }
}

static int wlanCount() {
  return ipMode ? 12 : 8;
}

int menuCount() {
  switch (menuPage) {
    case MP_SETUP: return 6;
    case MP_WLAN: return wlanCount();
    case MP_SSID:
      if (wifiScanRunning()) return 2;
      return wifiScanCount() + 2;
    case MP_BT: return 3;
    case MP_CAL: return 8;
    case MP_SYS: return 5;
    default: return 0;
  }
}

static uint8_t menuKind(int i) {
  switch (menuPage) {
    case MP_WLAN:
      if (i == 0 || i == 1) return 0;
      return 1;
    case MP_SSID:
      if (wifiScanRunning() && i == 0) return 0;
      return 1;
    case MP_CAL:
      if (i == 0) return 0;
      return 1;
    case MP_SYS:
      if (i == 0 || i == 1) return 0;
      return 1;
    default:
      return 1;
  }
}

static void menuLabel(int i, char *buf, size_t n) {
  buf[0] = '\0';
  switch (menuPage) {
    case MP_SETUP:
      switch (i) {
        case 0: snprintf(buf, n, "Link: %s", linkModeLabel()); break;
        case 1: snprintf(buf, n, "Wi-Fi..."); break;
        case 2: snprintf(buf, n, "Bluetooth..."); break;
        case 3: snprintf(buf, n, "Calibration..."); break;
        case 4: snprintf(buf, n, "System..."); break;
        case 5: snprintf(buf, n, "Back"); break;
      }
      break;
    case MP_WLAN:
      if (ipMode == 0) {
        switch (i) {
          case 0: snprintf(buf, n, "Status: %s", wifiStateLabel()); break;
          case 1: snprintf(buf, n, "IP: %s", wifiIpCurrent().c_str()); break;
          case 2: snprintf(buf, n, "SSID: %s", wifiSsid.length() ? wifiSsid.c_str() : "(empty)"); break;
          case 3: snprintf(buf, n, "Password: %s", wifiPass.length() ? "********" : "(empty)"); break;
          case 4: snprintf(buf, n, "Address: DHCP"); break;
          case 5: snprintf(buf, n, "Port: %d", rotPort); break;
          case 6: snprintf(buf, n, "Save and connect"); break;
          case 7: snprintf(buf, n, "Back"); break;
        }
      } else {
        switch (i) {
          case 0: snprintf(buf, n, "Status: %s", wifiStateLabel()); break;
          case 1: snprintf(buf, n, "IP: %s", wifiIpCurrent().c_str()); break;
          case 2: snprintf(buf, n, "SSID: %s", wifiSsid.length() ? wifiSsid.c_str() : "(empty)"); break;
          case 3: snprintf(buf, n, "Password: %s", wifiPass.length() ? "********" : "(empty)"); break;
          case 4: snprintf(buf, n, "Address: Static"); break;
          case 5: snprintf(buf, n, "IP: %s", ipLocal.c_str()); break;
          case 6: snprintf(buf, n, "Gateway: %s", ipGw.c_str()); break;
          case 7: snprintf(buf, n, "Mask: %s", ipMask.c_str()); break;
          case 8: snprintf(buf, n, "DNS: %s", ipDns.c_str()); break;
          case 9: snprintf(buf, n, "Port: %d", rotPort); break;
          case 10: snprintf(buf, n, "Save and connect"); break;
          case 11: snprintf(buf, n, "Back"); break;
        }
      }
      break;
    case MP_SSID:
      if (wifiScanRunning()) {
        if (i == 0) snprintf(buf, n, "Scanning...");
        else snprintf(buf, n, "Back");
      } else {
        int nets = wifiScanCount();
        if (i < nets) {
          String ssid = wifiScanSSID(i);
          snprintf(buf, n, "%s  %ddBm", ssid.c_str(), wifiScanRSSI(i));
        } else if (i == nets) {
          snprintf(buf, n, "Manual...");
        } else {
          snprintf(buf, n, "Back");
        }
      }
      break;
    case MP_BT:
      switch (i) {
        case 0: snprintf(buf, n, "Bluetooth: %s", linkWantsBt() ? "On" : "Off"); break;
        case 1: snprintf(buf, n, "Name: %s", btName.c_str()); break;
        case 2: snprintf(buf, n, "Back"); break;
      }
      break;
    case MP_CAL:
      switch (i) {
        case 0: snprintf(buf, n, "Raw:%d Med:%d %d/s", (int)dig_AZ, (int)dig_AZ_m, (int)(calSpeed + 0.5f)); break;
        case 1: snprintf(buf, n, calJogCmd == 1 ? "CCW running  (stop)" : "Jog CCW"); break;
        case 2: snprintf(buf, n, calJogCmd == 2 ? "CW running  (stop)" : "Jog CW"); break;
        case 3: snprintf(buf, n, "Save Max CCW (%d)", az_min_digit); break;
        case 4: snprintf(buf, n, "Save MAX CW (%d)", az_max_digit); break;
        case 5: snprintf(buf, n, "Overshoot: %d'", a_overshoot); break;
        case 6: snprintf(buf, n, calPhase != CAL_IDLE ? "Cal run  (stop)" : "Cal run"); break;
        case 7: snprintf(buf, n, "Back"); break;
      }
      break;
    case MP_SYS:
      switch (i) {
        case 0: snprintf(buf, n, "Version: %s", FW_VERSION); break;
        case 1:
          if (WiFi.status() == WL_CONNECTED) {
            snprintf(buf, n, "OTA: %s", WiFi.localIP().toString().c_str());
          } else {
            snprintf(buf, n, "OTA: Wi-Fi needed");
          }
          break;
        case 2: snprintf(buf, n, "Debug: %s", debug ? "On" : "Off"); break;
        case 3: snprintf(buf, n, "Restart"); break;
        case 4: snprintf(buf, n, "Back"); break;
      }
      break;
    default:
      break;
  }
}

static void menuEnsureVisible() {
  int n = menuCount();
  if (menuSel < 0) menuSel = 0;
  if (n > 0 && menuSel >= n) menuSel = n - 1;
  if (menuSel < menuScroll) menuScroll = menuSel;
  if (menuSel >= menuScroll + MENU_VIS) menuScroll = menuSel - MENU_VIS + 1;
  if (menuScroll < 0) menuScroll = 0;
}

static void menuDrawRow(int absIndex) {
  int n = menuCount();
  int vis = absIndex - menuScroll;
  int y = MENU_TOP + vis * MENU_ROW;
  if (absIndex < menuScroll || vis >= MENU_VIS) return;

  uint16_t bg = TFT_BLACK;
  uint16_t fg = TFT_WHITE;
  char buf[64] = "";
  if (absIndex >= 0 && absIndex < n) {
    bool sel = (absIndex == menuSel);
    bg = sel ? TFT_NAVY : TFT_BLACK;
    fg = sel ? TFT_YELLOW : (menuKind(absIndex) == 0 ? TFT_SILVER : TFT_WHITE);
    menuLabel(absIndex, buf, sizeof(buf));
  }

  tft.fillRect(0, y, 320, MENU_ROW, bg);
  tft.setTextDatum(TL_DATUM);
  tft.setTextPadding(0);
  tft.setTextColor(fg, bg);
  if (buf[0]) tft.drawString(buf, 8, y + 2, 4);
}

static void menuDrawList() {
  int n = menuCount();
  for (int i = 0; i < MENU_VIS; i++) {
    menuDrawRow(menuScroll + i);
  }
  menuClearLiveCache();
  lastCalStatus[0] = '\0';
  if (menuPage == MP_CAL) calDrawStatus();
}

static void menuDrawHeaderIp(bool force) {
  if (menuPage != MP_SETUP) return;
  char ip[24];
  snprintf(ip, sizeof(ip), "%s", wifiIpCurrent().c_str());
  if (!force && strcmp(ip, lastHeaderIp) == 0) return;
  strncpy(lastHeaderIp, ip, sizeof(lastHeaderIp) - 1);
  lastHeaderIp[sizeof(lastHeaderIp) - 1] = '\0';
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextPadding(150);
  tft.drawString(ip, 312, 10, 2);
  tft.setTextDatum(TL_DATUM);
  tft.setTextPadding(0);
}

static void menuDrawChrome() {
  tft.fillRect(0, 0, 320, 33, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextPadding(0);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(menuTitle(), 8, 4, 4);
  tft.drawFastHLine(0, 32, 320, TFT_WHITE);
  lastHeaderIp[0] = '\0';
  menuDrawHeaderIp(true);
  tft.fillRect(0, 220, 320, 20, TFT_BLACK);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  if (menuPage == MP_CHAR) {
    tft.drawString("CCW/CW char   BRK take   3s Back", 4, 222, 2);
  } else if (menuPage == MP_OCTET) {
    tft.drawString("CCW/CW value   BRK next   3s Back", 4, 222, 2);
  } else if (menuPage == MP_NUM) {
    tft.drawString("CCW/CW value   BRK save   3s Back", 4, 222, 2);
  } else {
    tft.drawString("CCW/CW select   BRK OK   3s Back", 4, 222, 2);
  }
}

static void menuDrawChar() {
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextPadding(304);
  tft.drawString(editBuf.length() ? editBuf : "(empty)", 8, 48, 4);
  char shown[12];
  if (charIdx == MENU_CHAR_DEL) {
    strcpy(shown, "<DEL>");
  } else if (charIdx == MENU_CHAR_OK) {
    strcpy(shown, "<DONE>");
  } else {
    shown[0] = MENU_CHARS[charIdx];
    shown[1] = '\0';
  }
  tft.fillRect(40, 92, 240, 48, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextPadding(0);
  tft.drawString(shown, 160, 116, 4);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.setTextPadding(80);
  char info[24];
  snprintf(info, sizeof(info), "%d / %d", editBuf.length(), editMax);
  tft.drawString(info, 8, 190, 4);
  tft.setTextPadding(0);
}

static void menuDrawOctet() {
  tft.setTextDatum(MC_DATUM);
  int x0 = 40;
  for (int i = 0; i < 4; i++) {
    int x = x0 + i * 76;
    uint16_t bg = (i == octSel) ? TFT_NAVY : TFT_BLACK;
    uint16_t fg = (i == octSel) ? TFT_YELLOW : TFT_WHITE;
    tft.fillRoundRect(x - 30, 96, 60, 36, 4, bg);
    tft.setTextColor(fg, bg);
    tft.drawString(String(octVal[i]), x, 114, 4);
    if (i < 3) {
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.drawString(".", x + 38, 114, 4);
    }
  }
  tft.setTextDatum(TL_DATUM);
}

static void menuDrawNum() {
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextPadding(160);
  tft.drawString(String(numVal), 160, 114, 4);
  tft.setTextPadding(0);
  tft.setTextDatum(TL_DATUM);
}

static void menuDraw() {
  tft.fillScreen(TFT_BLACK);
  menuDrawChrome();
  if (menuPage == MP_CHAR) {
    menuDrawChar();
    return;
  }
  if (menuPage == MP_OCTET) {
    menuDrawOctet();
    return;
  }
  if (menuPage == MP_NUM) {
    menuDrawNum();
    return;
  }
  menuEnsureVisible();
  menuDrawList();
}

static void menuGoto(MenuPage page) {
  calStopAll();
  menuPage = page;
  menuSel = 0;
  menuScroll = 0;
  int n = menuCount();
  for (int t = 0; t < n; t++) {
    if (menuKind(menuSel) != 0) break;
    menuSel++;
    if (menuSel >= n) menuSel = 0;
  }
  menuClearLiveCache();
  menuDraw();
}

static void menuLeave() {
  calStopAll();
  menuOpen = false;
  wifiScanStop();
  drawMainScreen();
}

static void openCharEdit(EditTarget target, const String &initial, int maxLen, MenuPage back) {
  editTarget = target;
  editBuf = initial;
  editMax = maxLen;
  editReturn = back;
  charIdx = 0;
  menuPage = MP_CHAR;
  menuDraw();
}

static void openOctetEdit(OctTarget target, const String &initial) {
  octTarget = target;
  parseIp(initial, octVal);
  octSel = 0;
  menuPage = MP_OCTET;
  menuDraw();
}

static void openNumEdit(NumTarget target, int val, int mn, int mx) {
  numTarget = target;
  numVal = val;
  numMin = mn;
  numMax = mx;
  numRepeat = 0;
  menuPage = MP_NUM;
  menuDraw();
}

static void commitCharEdit() {
  if (editTarget == ED_SSID) {
    wifiSsid = editBuf;
    menuGoto(MP_WLAN);
  } else if (editTarget == ED_PASS) {
    wifiPass = editBuf;
    menuGoto(MP_WLAN);
  } else {
    btName = editBuf;
    preferences.putString("name", btName);
    restartBluetooth();
    menuGoto(MP_BT);
  }
}

static void commitOctetEdit() {
  String ip = joinIp(octVal);
  if (octTarget == OCT_LOCAL) ipLocal = ip;
  else if (octTarget == OCT_GW) ipGw = ip;
  else if (octTarget == OCT_MASK) ipMask = ip;
  else ipDns = ip;
  menuGoto(MP_WLAN);
}

static void commitNumEdit() {
  if (numTarget == NUM_PORT) {
    rotPort = numVal;
    preferences.putInt("rot_port", rotPort);
  } else {
    a_overshoot = numVal;
    preferences.putInt("a_overshoot", a_overshoot);
  }
  menuGoto(numTarget == NUM_PORT ? MP_WLAN : MP_CAL);
}

void menuEnter() {
  menuOpen = true;
  stopAutorotate();
  menuFreeSprites();
  menuGoto(MP_SETUP);
}

void menuOnBack() {
  switch (menuPage) {
    case MP_SETUP:
      menuLeave();
      break;
    case MP_WLAN:
    case MP_BT:
    case MP_CAL:
    case MP_SYS:
      menuGoto(MP_SETUP);
      break;
    case MP_SSID:
      wifiScanStop();
      menuGoto(MP_WLAN);
      break;
    case MP_CHAR:
      menuGoto(editReturn);
      break;
    case MP_OCTET:
      menuGoto(MP_WLAN);
      break;
    case MP_NUM:
      menuGoto(numTarget == NUM_OVER ? MP_CAL : MP_WLAN);
      break;
    default:
      menuLeave();
      break;
  }
}

static void menuMove(int dir) {
  int n = menuCount();
  if (n <= 0) return;
  int oldSel = menuSel;
  int oldScroll = menuScroll;
  for (int tries = 0; tries < n; tries++) {
    menuSel += dir;
    if (menuSel < 0) menuSel = n - 1;
    if (menuSel >= n) menuSel = 0;
    if (menuKind(menuSel) != 0) break;
  }
  menuEnsureVisible();
  if (menuScroll != oldScroll) {
    menuDrawList();
  } else {
    menuDrawRow(oldSel);
    menuDrawRow(menuSel);
  }
}

void menuResetHold() {
  numRepeat = 0;
}

void menuOnLeft() {
  if (menuPage == MP_CHAR) {
    charIdx--;
    if (charIdx < 0) charIdx = MENU_CHAR_OK;
    menuDrawChar();
    return;
  }
  if (menuPage == MP_OCTET) {
    octVal[octSel]--;
    if (octVal[octSel] < 0) octVal[octSel] = 255;
    menuDrawOctet();
    return;
  }
  if (menuPage == MP_NUM) {
    int step = 1;
    numRepeat++;
    if (numTarget == NUM_PORT && numRepeat > 20) step = 10;
    if (numTarget == NUM_PORT && numRepeat > 80) step = 100;
    numVal -= step;
    if (numVal < numMin) numVal = numMin;
    menuDrawNum();
    return;
  }
  menuMove(-1);
}

void menuOnRight() {
  if (menuPage == MP_CHAR) {
    charIdx++;
    if (charIdx > MENU_CHAR_OK) charIdx = 0;
    menuDrawChar();
    return;
  }
  if (menuPage == MP_OCTET) {
    octVal[octSel]++;
    if (octVal[octSel] > 255) octVal[octSel] = 0;
    menuDrawOctet();
    return;
  }
  if (menuPage == MP_NUM) {
    int step = 1;
    numRepeat++;
    if (numTarget == NUM_PORT && numRepeat > 20) step = 10;
    if (numTarget == NUM_PORT && numRepeat > 80) step = 100;
    numVal += step;
    if (numVal > numMax) numVal = numMax;
    menuDrawNum();
    return;
  }
  menuMove(1);
}

static void menuSelectSetup() {
  switch (menuSel) {
    case 0:
      linkMode++;
      if (linkMode > LINK_BOTH) linkMode = LINK_BT;
      applyLinkMode();
      menuDrawRow(menuSel);
      break;
    case 1:
      menuGoto(MP_WLAN);
      break;
    case 2:
      menuGoto(MP_BT);
      break;
    case 3:
      menuGoto(MP_CAL);
      break;
    case 4:
      menuGoto(MP_SYS);
      break;
    case 5:
      menuLeave();
      break;
  }
}

static void menuSelectWlan() {
  if (ipMode == 0) {
    switch (menuSel) {
      case 2:
        wifiStartScan();
        menuGoto(MP_SSID);
        break;
      case 3:
        openCharEdit(ED_PASS, wifiPass, 63, MP_WLAN);
        break;
      case 4:
        ipMode = 1;
        preferences.putInt("ip_mode", ipMode);
        menuDraw();
        break;
      case 5:
        openNumEdit(NUM_PORT, rotPort, 1, 65535);
        break;
      case 6:
        wifiConnectNow();
        menuDraw();
        break;
      case 7:
        menuGoto(MP_SETUP);
        break;
    }
  } else {
    switch (menuSel) {
      case 2:
        wifiStartScan();
        menuGoto(MP_SSID);
        break;
      case 3:
        openCharEdit(ED_PASS, wifiPass, 63, MP_WLAN);
        break;
      case 4:
        ipMode = 0;
        preferences.putInt("ip_mode", ipMode);
        menuSel = 4;
        menuDraw();
        break;
      case 5:
        openOctetEdit(OCT_LOCAL, ipLocal);
        break;
      case 6:
        openOctetEdit(OCT_GW, ipGw);
        break;
      case 7:
        openOctetEdit(OCT_MASK, ipMask);
        break;
      case 8:
        openOctetEdit(OCT_DNS, ipDns);
        break;
      case 9:
        openNumEdit(NUM_PORT, rotPort, 1, 65535);
        break;
      case 10:
        wifiConnectNow();
        menuDraw();
        break;
      case 11:
        menuGoto(MP_SETUP);
        break;
    }
  }
}

static void menuSelectSsid() {
  if (wifiScanRunning()) {
    if (menuSel == 1) {
      wifiScanStop();
      menuGoto(MP_WLAN);
    }
    return;
  }
  int nets = wifiScanCount();
  if (menuSel < nets) {
    wifiSsid = wifiScanSSID(menuSel);
    wifiScanStop();
    menuGoto(MP_WLAN);
  } else if (menuSel == nets) {
    wifiScanStop();
    openCharEdit(ED_SSID, wifiSsid, 32, MP_WLAN);
  } else {
    wifiScanStop();
    menuGoto(MP_WLAN);
  }
}

static void menuSelectBt() {
  switch (menuSel) {
    case 0:
      if (linkWantsBt()) {
        linkMode = LINK_WIFI;
      } else {
        linkMode = LINK_BOTH;
      }
      applyLinkMode();
      menuDrawRow(menuSel);
      break;
    case 1:
      openCharEdit(ED_BTNAME, btName, 15, MP_BT);
      break;
    case 2:
      menuGoto(MP_SETUP);
      break;
  }
}

static void calToggleJog(int cmd) {
  if (calPhase != CAL_IDLE) {
    calAbort("Cal: aborted");
  }
  calJogCmd = (calJogCmd == cmd) ? 0 : cmd;
}

static void menuSelectCal() {
  switch (menuSel) {
    case 1:
      calToggleJog(1);
      menuDrawRow(1);
      menuDrawRow(2);
      break;
    case 2:
      calToggleJog(2);
      menuDrawRow(1);
      menuDrawRow(2);
      break;
    case 3:
      calStopAll();
      az_min_digit = (int)dig_AZ_m;
      preferences.putInt("az_min_digit", az_min_digit);
      menuDrawRow(3);
      break;
    case 4:
      calStopAll();
      az_max_digit = (int)dig_AZ_m;
      preferences.putInt("az_max_digit", az_max_digit);
      menuDrawRow(4);
      break;
    case 5:
      calStopAll();
      openNumEdit(NUM_OVER, a_overshoot, 0, 9);
      break;
    case 6:
      if (calPhase != CAL_IDLE) {
        calAbort("Cal: aborted");
      } else {
        calStartAuto();
      }
      menuDrawRow(1);
      menuDrawRow(2);
      menuDrawRow(6);
      break;
    case 7:
      menuGoto(MP_SETUP);
      break;
  }
}

static void menuSelectSys() {
  switch (menuSel) {
    case 2:
      debug = !debug;
      menuDrawRow(menuSel);
      break;
    case 3:
      tft.fillScreen(TFT_BLACK);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.drawString("Restarting...", 160, 120, 4);
      delay(200);
      ESP.restart();
      break;
    case 4:
      menuGoto(MP_SETUP);
      break;
  }
}

void menuOnSelect() {
  if (menuPage == MP_CHAR) {
    if (charIdx == MENU_CHAR_DEL) {
      if (editBuf.length() > 0) editBuf.remove(editBuf.length() - 1);
      menuDrawChar();
    } else if (charIdx == MENU_CHAR_OK) {
      commitCharEdit();
    } else if ((int)editBuf.length() < editMax) {
      editBuf += MENU_CHARS[charIdx];
      menuDrawChar();
    }
    return;
  }
  if (menuPage == MP_OCTET) {
    if (octSel < 3) {
      octSel++;
      menuDrawOctet();
    } else {
      commitOctetEdit();
    }
    return;
  }
  if (menuPage == MP_NUM) {
    commitNumEdit();
    return;
  }

  switch (menuPage) {
    case MP_SETUP: menuSelectSetup(); break;
    case MP_WLAN: menuSelectWlan(); break;
    case MP_SSID: menuSelectSsid(); break;
    case MP_BT: menuSelectBt(); break;
    case MP_CAL: menuSelectCal(); break;
    case MP_SYS: menuSelectSys(); break;
    default: break;
  }
}

void menuRefreshLive() {
  if (menuPage == MP_SSID && wifiTakeScanDirty()) {
    menuSel = 0;
    menuScroll = 0;
    menuEnsureVisible();
    menuDrawList();
    return;
  }

  char buf[64];
  if (menuPage == MP_CAL) {
    menuLabel(0, buf, sizeof(buf));
    if (strcmp(buf, lastLive0) != 0) {
      strncpy(lastLive0, buf, sizeof(lastLive0) - 1);
      lastLive0[sizeof(lastLive0) - 1] = '\0';
      menuDrawRow(0);
    }
    char state[32];
    snprintf(state, sizeof(state), "%d:%d:%d:%d", calJogCmd, (int)calPhase, az_min_digit, az_max_digit);
    if (strcmp(state, lastLive1) != 0) {
      strncpy(lastLive1, state, sizeof(lastLive1) - 1);
      lastLive1[sizeof(lastLive1) - 1] = '\0';
      menuDrawRow(1);
      menuDrawRow(2);
      menuDrawRow(3);
      menuDrawRow(4);
      menuDrawRow(6);
    }
    calDrawStatus();
    return;
  }
  if (menuPage == MP_WLAN) {
    menuLabel(0, buf, sizeof(buf));
    if (strcmp(buf, lastLive0) != 0) {
      strncpy(lastLive0, buf, sizeof(lastLive0) - 1);
      lastLive0[sizeof(lastLive0) - 1] = '\0';
      menuDrawRow(0);
    }
    menuLabel(1, buf, sizeof(buf));
    if (strcmp(buf, lastLive1) != 0) {
      strncpy(lastLive1, buf, sizeof(lastLive1) - 1);
      lastLive1[sizeof(lastLive1) - 1] = '\0';
      menuDrawRow(1);
    }
    return;
  }
  if (menuPage == MP_SETUP) {
    menuDrawHeaderIp(false);
    return;
  }
  if (menuPage == MP_SYS) {
    menuLabel(1, buf, sizeof(buf));
    if (strcmp(buf, lastLive0) != 0) {
      strncpy(lastLive0, buf, sizeof(lastLive0) - 1);
      lastLive0[sizeof(lastLive0) - 1] = '\0';
      menuDrawRow(1);
    }
  }
}
