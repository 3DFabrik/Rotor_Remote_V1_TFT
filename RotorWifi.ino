WiFiServer rotServer;
WiFiClient rotClient;
bool rotServerStarted = false;

static int wifiScanState = 0;  // 0 idle, 1 running, 2 done
static int wifiScanN = 0;
static bool wifiScanDirty = false;

static char rotLine[96];
static int rotLineLen = 0;

bool linkWantsBt() {
  return linkMode == LINK_BT || linkMode == LINK_BOTH;
}

bool linkWantsWifi() {
  return linkMode == LINK_WIFI || linkMode == LINK_BOTH;
}

const char *wifiStateLabel() {
  if (!wifiWanted && !linkWantsWifi()) return "aus";
  switch (WiFi.status()) {
    case WL_CONNECTED: return "verbunden";
    case WL_NO_SSID_AVAIL: return "SSID fehlt";
    case WL_CONNECT_FAILED: return "Fehler";
    case WL_CONNECTION_LOST: return "getrennt";
    case WL_DISCONNECTED: return "getrennt";
    default: return wifiSsid.length() ? "verbinden..." : "kein SSID";
  }
}

String wifiIpCurrent() {
  if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
  return ipMode ? ipLocal : String("-");
}

String linkStatusText() {
  String s = "";
  if (linkWantsBt()) s += "BT";
  if (linkWantsWifi()) {
    if (s.length()) s += "  ";
    s += "WLAN";
    if (WiFi.status() == WL_CONNECTED) {
      s += " ";
      s += WiFi.localIP().toString();
    } else if (wifiSsid.length() == 0) {
      s += " --";
    } else {
      s += " ...";
    }
  }
  return s;
}

void rotctlCloseClient() {
  if (rotClient) rotClient.stop();
}

void rotctlStopServer() {
  rotctlCloseClient();
  if (rotServerStarted) {
    rotServer.stop();
    rotServerStarted = false;
  }
}

void wifiStop() {
  rotctlStopServer();
  wifiWanted = false;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void wifiRestart() {
  rotctlStopServer();
  wifiWanted = true;
  WiFi.disconnect(true);
  delay(50);
  WiFi.mode(WIFI_STA);
  if (ipMode == 1) {
    IPAddress a, g, m, d;
    if (!a.fromString(ipLocal)) a = IPAddress(192, 168, 1, 50);
    if (!g.fromString(ipGw)) g = IPAddress(192, 168, 1, 1);
    if (!m.fromString(ipMask)) m = IPAddress(255, 255, 255, 0);
    if (!d.fromString(ipDns)) d = IPAddress(192, 168, 1, 1);
    WiFi.config(a, g, m, d);
  } else {
    WiFi.config(IPAddress((uint32_t)0), IPAddress((uint32_t)0), IPAddress((uint32_t)0));
  }
  if (wifiSsid.length() > 0) {
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  }
}

void restartBluetooth() {
  if (serialBtOn) {
    SerialBT.end();
    serialBtOn = false;
  }
  if (linkWantsBt()) {
    SerialBT.begin(btName);
    serialBtOn = true;
  }
}

void applyLinkMode() {
  preferences.putInt("link_mode", linkMode);
  if (linkWantsBt()) {
    if (!serialBtOn) {
      SerialBT.begin(btName);
      serialBtOn = true;
    }
  } else if (serialBtOn) {
    SerialBT.end();
    serialBtOn = false;
  }

  if (linkWantsWifi()) {
    wifiRestart();
  } else {
    wifiStop();
  }
}

void wifiConnectNow() {
  preferences.putString("wifi_ssid", wifiSsid);
  preferences.putString("wifi_pass", wifiPass);
  preferences.putInt("ip_mode", ipMode);
  preferences.putString("ip_local", ipLocal);
  preferences.putString("ip_gw", ipGw);
  preferences.putString("ip_mask", ipMask);
  preferences.putString("ip_dns", ipDns);
  preferences.putInt("rot_port", rotPort);
  if (linkMode == LINK_BT) {
    linkMode = LINK_BOTH;
  }
  applyLinkMode();
}

void wifiStartScan() {
  WiFi.mode(WIFI_STA);
  WiFi.scanDelete();
  WiFi.scanNetworks(true, true);
  wifiScanState = 1;
  wifiScanN = 0;
  wifiScanDirty = false;
}

void wifiPollScan() {
  if (wifiScanState != 1) return;
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  if (n < 0) {
    wifiScanN = 0;
  } else {
    wifiScanN = n;
  }
  wifiScanState = 2;
  wifiScanDirty = true;
}

void wifiScanStop() {
  WiFi.scanDelete();
  wifiScanState = 0;
  wifiScanN = 0;
  wifiScanDirty = false;
  if (!wifiWanted && !linkWantsWifi()) {
    WiFi.mode(WIFI_OFF);
  }
}

bool wifiScanRunning() {
  return wifiScanState == 1;
}

bool wifiTakeScanDirty() {
  if (!wifiScanDirty) return false;
  wifiScanDirty = false;
  return true;
}

int wifiScanCount() {
  return wifiScanN;
}

String wifiScanSSID(int i) {
  if (i < 0 || i >= wifiScanN) return "";
  return WiFi.SSID(i);
}

int wifiScanRSSI(int i) {
  if (i < 0 || i >= wifiScanN) return 0;
  return WiFi.RSSI(i);
}

void rotctlSendPos(bool ext) {
  rotClient.println(String(azimut, 1));
  rotClient.println("0.0");
  if (ext) rotClient.println("RPRT 0");
}

void rotctlHandle(char *line) {
  while (*line == ' ' || *line == '\t') line++;
  if (*line == 0) return;

  bool ext = false;
  if (*line == '\\' || *line == '+') {
    ext = true;
    line++;
  }
  while (*line == ' ' || *line == '\t') line++;
  if (*line == 0) return;

  if (line[0] == 'p' && (line[1] == 0 || isspace((unsigned char)line[1]))) {
    rotctlSendPos(ext);
    return;
  }
  if (!strncmp(line, "get_pos", 7) && (line[7] == 0 || isspace((unsigned char)line[7]))) {
    rotctlSendPos(ext);
    return;
  }

  if (line[0] == 'P' && (line[1] == 0 || isspace((unsigned char)line[1]))) {
    float az = 0, el = 0;
    sscanf(line + 1, "%f %f", &az, &el);
    applyAzimuthTarget((int)(az + 0.5f));
    rotClient.println("RPRT 0");
    return;
  }
  if (!strncmp(line, "set_pos", 7)) {
    float az = 0, el = 0;
    sscanf(line + 7, "%f %f", &az, &el);
    applyAzimuthTarget((int)(az + 0.5f));
    rotClient.println("RPRT 0");
    return;
  }

  if ((line[0] == 'S' && (line[1] == 0 || isspace((unsigned char)line[1]))) ||
      (!strncmp(line, "stop", 4) && (line[4] == 0 || isspace((unsigned char)line[4])))) {
    stopAutorotate();
    rotClient.println("RPRT 0");
    return;
  }

  if (line[0] == 'q' || !strncmp(line, "quit", 4)) {
    rotClient.stop();
    return;
  }

  if (!strncmp(line, "dump_state", 10)) {
    rotClient.println("2");
    rotClient.println("1");
    rotClient.println("0.000000");
    rotClient.println("360.000000");
    rotClient.println("0.000000");
    rotClient.println("180.000000");
    if (ext) rotClient.println("RPRT 0");
    return;
  }

  if (line[0] == '_' || !strncmp(line, "get_info", 8)) {
    rotClient.println("RotorRemote");
    if (ext) rotClient.println("RPRT 0");
    return;
  }

  rotClient.println("RPRT -11");
}

void rotctlRead() {
  while (rotClient.connected() && rotClient.available() > 0) {
    char c = (char)rotClient.read();
    if (c == '\r' || c == '\n') {
      if (rotLineLen > 0) {
        rotLine[rotLineLen] = '\0';
        rotctlHandle(rotLine);
        rotLineLen = 0;
      }
      continue;
    }
    if (rotLineLen < (int)sizeof(rotLine) - 1) {
      rotLine[rotLineLen++] = c;
    }
  }
}

void wifiService() {
  wifiPollScan();

  if (!wifiWanted) return;

  if (WiFi.status() == WL_CONNECTED) {
    if (!rotServerStarted) {
      rotServer.begin(rotPort);
      rotServerStarted = true;
      rotLineLen = 0;
      if (debug) Serial.println("rotctld listening on " + WiFi.localIP().toString() + ":" + String(rotPort));
    }
    if (rotServer.hasClient()) {
      WiFiClient incoming = rotServer.available();
      if (rotClient && rotClient.connected()) {
        rotClient.stop();
      }
      rotClient = incoming;
      rotClient.setNoDelay(true);
      rotLineLen = 0;
      if (debug) Serial.println("rotctld client connected");
    }
    if (rotClient && rotClient.connected()) {
      rotctlRead();
    }
  } else if (rotServerStarted) {
    rotctlStopServer();
  }
}
