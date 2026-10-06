const int MENU_TOP = 36;
const int MENU_ROW = 18;
const int MENU_VIS = 10;

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

static const unsigned long CAL_ABORT_MS = 5000;
static const unsigned long CAL_STOP_MS = 1200;
static const int CAL_MOVE_EPS = 12;
static const int CAL_BACK_DIGITS = 80;

static CalPhase calPhase = CAL_IDLE;
static int calJogCmd = 0;
static int calPhaseCmd = 0;
static float calWatchRaw = 0;
static float calStopRaw = 0;
static unsigned long calWatchAt = 0;
static bool calSawMove = false;
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
  tft.drawString(calStatus, 8, 206, 2);
  tft.setTextPadding(0);
}

void calStopAll() {
  calJogCmd = 0;
  calPhase = CAL_IDLE;
  calPhaseCmd = 0;
  calSawMove = false;
  if (calStatus[0] && strncmp(calStatus, "Kalib: fertig", 13) != 0 &&
      strncmp(calStatus, "Kalib: Abbruch", 14) != 0) {
    calSetStatus("");
  }
}

static void calBeginPhase(CalPhase phase, int cmd, const char *msg) {
  calPhase = phase;
  calPhaseCmd = cmd;
  calJogCmd = 0;
  calWatchRaw = dig_AZ;
  calStopRaw = dig_AZ;
  calWatchAt = millis();
  calSawMove = false;
  calSetStatus(msg);
}

static void calAbort(const char *msg) {
  calPhase = CAL_IDLE;
  calPhaseCmd = 0;
  calJogCmd = 0;
  calSetStatus(msg);
}

static void calWatchRawMove() {
  if (abs(dig_AZ - calWatchRaw) >= CAL_MOVE_EPS) {
    calWatchRaw = dig_AZ;
    calWatchAt = millis();
    calSawMove = true;
  }
}

void calService() {
  if (calPhase == CAL_IDLE) return;

  calWatchRawMove();
  unsigned long still = millis() - calWatchAt;

  if (still >= CAL_ABORT_MS) {
    calAbort("Kalib: Abbruch, kein Weg");
    return;
  }

  if (calPhase == CAL_CW_SEEK || calPhase == CAL_CCW_SEEK) {
    if (calSawMove && still >= CAL_STOP_MS) {
      calStopRaw = dig_AZ;
      if (calPhase == CAL_CW_SEEK) {
        calBeginPhase(CAL_CW_BACK, 1, "Kalib: etwas zurueck");
      } else {
        calBeginPhase(CAL_CCW_BACK, 2, "Kalib: etwas zurueck");
      }
    }
    return;
  }

  if (calPhase == CAL_CW_BACK || calPhase == CAL_CCW_BACK) {
    if (abs(dig_AZ - calStopRaw) >= CAL_BACK_DIGITS) {
      if (calPhase == CAL_CW_BACK) {
        az_min_digit = (int)dig_AZ_f;
        preferences.putInt("az_min_digit", az_min_digit);
        calBeginPhase(CAL_CCW_SEEK, 1, "Kalib: CCW zum Anschlag");
      } else {
        az_max_digit = (int)dig_AZ_f;
        preferences.putInt("az_max_digit", az_max_digit);
        calPhase = CAL_IDLE;
        calPhaseCmd = 0;
        calSetStatus("Kalib: fertig");
      }
    }
  }
}

static void menuClearLiveCache() {
  lastLive0[0] = '\0';
  lastLive1[0] = '\0';
}

static void menuEnsureRowSprite() {
  if (spr_angle.width() != 320 || spr_angle.height() != MENU_ROW) {
    spr_angle.deleteSprite();
    spr_angle.createSprite(320, MENU_ROW);
  }
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
    case MP_WLAN: return "WLAN";
    case MP_SSID: return "SSID";
    case MP_BT: return "Bluetooth";
    case MP_CAL: return "Kalibrierung";
    case MP_SYS: return "System";
    case MP_CHAR:
      if (editTarget == ED_PASS) return "Passwort";
      if (editTarget == ED_BTNAME) return "BT-Name";
      return "SSID";
    case MP_OCTET:
      if (octTarget == OCT_GW) return "Gateway";
      if (octTarget == OCT_MASK) return "Netzmaske";
      if (octTarget == OCT_DNS) return "DNS";
      return "IP-Adresse";
    case MP_NUM:
      return (numTarget == NUM_OVER) ? "Overshoot" : "rotctld-Port";
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
    case MP_SYS: return 3;
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
    default:
      return 1;
  }
}

static void menuLabel(int i, char *buf, size_t n) {
  buf[0] = '\0';
  switch (menuPage) {
    case MP_SETUP:
      switch (i) {
        case 0: snprintf(buf, n, "Betrieb: %s", linkModeLabel()); break;
        case 1: snprintf(buf, n, "WLAN..."); break;
        case 2: snprintf(buf, n, "Bluetooth..."); break;
        case 3: snprintf(buf, n, "Kalibrierung..."); break;
        case 4: snprintf(buf, n, "System..."); break;
        case 5: snprintf(buf, n, "Zurueck"); break;
      }
      break;
    case MP_WLAN:
      if (ipMode == 0) {
        switch (i) {
          case 0: snprintf(buf, n, "Status: %s", wifiStateLabel()); break;
          case 1: snprintf(buf, n, "IP: %s", wifiIpCurrent().c_str()); break;
          case 2: snprintf(buf, n, "SSID: %s", wifiSsid.length() ? wifiSsid.c_str() : "(leer)"); break;
          case 3: snprintf(buf, n, "Passwort: %s", wifiPass.length() ? "********" : "(leer)"); break;
          case 4: snprintf(buf, n, "Adresse: DHCP"); break;
          case 5: snprintf(buf, n, "Port: %d", rotPort); break;
          case 6: snprintf(buf, n, "Speichern und verbinden"); break;
          case 7: snprintf(buf, n, "Zurueck"); break;
        }
      } else {
        switch (i) {
          case 0: snprintf(buf, n, "Status: %s", wifiStateLabel()); break;
          case 1: snprintf(buf, n, "IP: %s", wifiIpCurrent().c_str()); break;
          case 2: snprintf(buf, n, "SSID: %s", wifiSsid.length() ? wifiSsid.c_str() : "(leer)"); break;
          case 3: snprintf(buf, n, "Passwort: %s", wifiPass.length() ? "********" : "(leer)"); break;
          case 4: snprintf(buf, n, "Adresse: Statisch"); break;
          case 5: snprintf(buf, n, "Eigene IP: %s", ipLocal.c_str()); break;
          case 6: snprintf(buf, n, "Gateway: %s", ipGw.c_str()); break;
          case 7: snprintf(buf, n, "Maske: %s", ipMask.c_str()); break;
          case 8: snprintf(buf, n, "DNS: %s", ipDns.c_str()); break;
          case 9: snprintf(buf, n, "Port: %d", rotPort); break;
          case 10: snprintf(buf, n, "Speichern und verbinden"); break;
          case 11: snprintf(buf, n, "Zurueck"); break;
        }
      }
      break;
    case MP_SSID:
      if (wifiScanRunning()) {
        if (i == 0) snprintf(buf, n, "Suche Netze...");
        else snprintf(buf, n, "Zurueck");
      } else {
        int nets = wifiScanCount();
        if (i < nets) {
          String ssid = wifiScanSSID(i);
          snprintf(buf, n, "%s  %ddBm", ssid.c_str(), wifiScanRSSI(i));
        } else if (i == nets) {
          snprintf(buf, n, "Manuell...");
        } else {
          snprintf(buf, n, "Zurueck");
        }
      }
      break;
    case MP_BT:
      switch (i) {
        case 0: snprintf(buf, n, "Bluetooth: %s", linkWantsBt() ? "An" : "Aus"); break;
        case 1: snprintf(buf, n, "Name: %s", btName.c_str()); break;
        case 2: snprintf(buf, n, "Zurueck"); break;
      }
      break;
    case MP_CAL:
      switch (i) {
        case 0: snprintf(buf, n, "Roh: %d  Filt: %d", (int)dig_AZ, (int)dig_AZ_f); break;
        case 1: snprintf(buf, n, calJogCmd == 1 ? "CCW laeuft...  (stop)" : "CCW fahren"); break;
        case 2: snprintf(buf, n, calJogCmd == 2 ? "CW laeuft...  (stop)" : "CW fahren"); break;
        case 3: snprintf(buf, n, "Min speichern (%d)", az_min_digit); break;
        case 4: snprintf(buf, n, "Max speichern (%d)", az_max_digit); break;
        case 5: snprintf(buf, n, "Overshoot: %d'", a_overshoot); break;
        case 6: snprintf(buf, n, calPhase != CAL_IDLE ? "Kalibrierfahrt  (stop)" : "Kalibrierfahrt"); break;
        case 7: snprintf(buf, n, "Zurueck"); break;
      }
      break;
    case MP_SYS:
      switch (i) {
        case 0: snprintf(buf, n, "Debug: %s", debug ? "An" : "Aus"); break;
        case 1: snprintf(buf, n, "Neu starten"); break;
        case 2: snprintf(buf, n, "Zurueck"); break;
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

  menuEnsureRowSprite();
  uint16_t bg = TFT_BLACK;
  uint16_t fg = TFT_SILVER;
  if (absIndex >= 0 && absIndex < n) {
    bool sel = (absIndex == menuSel);
    bg = sel ? TFT_NAVY : TFT_BLACK;
    fg = TFT_WHITE;
    if (menuKind(absIndex) == 0) fg = TFT_SILVER;
    if (sel) fg = TFT_YELLOW;
  }

  spr_angle.fillSprite(bg);
  if (absIndex >= 0 && absIndex < n) {
    char buf[64];
    menuLabel(absIndex, buf, sizeof(buf));
    spr_angle.setTextDatum(TL_DATUM);
    spr_angle.setTextColor(fg, bg);
    spr_angle.drawString(buf, 8, 1, 2);
  }
  spr_angle.pushSprite(0, y);
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

static void menuDrawChrome() {
  tft.fillRect(0, 0, 320, 33, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(menuTitle(), 8, 4, 4);
  tft.drawFastHLine(0, 32, 320, TFT_WHITE);
  tft.fillRect(0, 220, 320, 20, TFT_BLACK);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  if (menuPage == MP_CHAR) {
    tft.drawString("CCW/CW Zeichen   BRK uebernehmen   3s Zurueck", 4, 226, 1);
  } else if (menuPage == MP_OCTET) {
    tft.drawString("CCW/CW Wert   BRK naechstes Oktett   3s Zurueck", 4, 226, 1);
  } else if (menuPage == MP_NUM) {
    tft.drawString("CCW/CW Wert   BRK speichern   3s Zurueck", 4, 226, 1);
  } else {
    tft.drawString("CCW/CW waehlen   BRK OK   3s Zurueck", 4, 226, 1);
  }
}

static void menuDrawChar() {
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextPadding(304);
  tft.drawString(editBuf.length() ? editBuf : "(leer)", 8, 48, 2);
  char shown[12];
  if (charIdx == MENU_CHAR_DEL) {
    strcpy(shown, "<DEL>");
  } else if (charIdx == MENU_CHAR_OK) {
    strcpy(shown, "<FERTIG>");
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
  tft.drawString(info, 8, 190, 2);
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
    calAbort("Kalib: Abbruch");
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
      az_min_digit = (int)dig_AZ_f;
      preferences.putInt("az_min_digit", az_min_digit);
      menuDrawRow(3);
      break;
    case 4:
      calStopAll();
      az_max_digit = (int)dig_AZ_f;
      preferences.putInt("az_max_digit", az_max_digit);
      menuDrawRow(4);
      break;
    case 5:
      calStopAll();
      openNumEdit(NUM_OVER, a_overshoot, 0, 9);
      break;
    case 6:
      if (calPhase != CAL_IDLE) {
        calAbort("Kalib: Abbruch");
      } else {
        calBeginPhase(CAL_CW_SEEK, 2, "Kalib: CW zum Anschlag");
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
    case 0:
      debug = !debug;
      menuDrawRow(menuSel);
      break;
    case 1:
      tft.fillScreen(TFT_BLACK);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.drawString("Neustart...", 160, 120, 4);
      delay(200);
      ESP.restart();
      break;
    case 2:
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
  }
}
