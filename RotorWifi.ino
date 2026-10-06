WiFiServer rotServer;
WiFiClient rotClient;
bool rotServerStarted = false;
WebServer httpOta(80);
static bool otaReady = false;

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

int wifiRssiBars() {
  if (WiFi.status() != WL_CONNECTED) return 0;
  int r = WiFi.RSSI();
  if (r >= -55) return 3;
  if (r >= -70) return 2;
  if (r >= -85) return 1;
  return 0;
}

static void otaShowScreen(const char *msg) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString(msg, 160, 120, 4);
  tft.setTextDatum(TL_DATUM);
}

static void otaStop() {
  if (!otaReady) return;
  httpOta.stop();
  ArduinoOTA.end();
  otaReady = false;
}

static void otaBegin() {
  if (otaReady) return;

  ArduinoOTA.setHostname(btName.length() ? btName.c_str() : "RotorRemote");
  ArduinoOTA.onStart([]() {
    stopAutorotate();
    otaShowScreen("Firmware-Update...");
  });
  ArduinoOTA.onEnd([]() {
    otaShowScreen("Update OK");
  });
  ArduinoOTA.onError([](ota_error_t err) {
    (void)err;
    otaShowScreen("Update Fehler");
  });
  ArduinoOTA.begin();

  httpOta.on("/", HTTP_GET, []() {
    String ip = WiFi.localIP().toString();
    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>RotorRemote</title>";
    html += "<style>body{font-family:sans-serif;background:#111;color:#eee;margin:24px}";
    html += "a{color:#6cf}</style></head><body>";
    html += "<h1>RotorRemote</h1>";
    html += "<p>rotctld: " + ip + ":" + String(rotPort) + "</p>";
    html += "<p><a href='/update'>Firmware online flashen</a></p>";
    html += "</body></html>";
    httpOta.send(200, "text/html", html);
  });

  httpOta.on("/update", HTTP_GET, []() {
    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Firmware</title>";
    html += "<style>body{font-family:sans-serif;background:#111;color:#eee;margin:24px}";
    html += "input{font-size:16px;margin-top:12px}</style></head><body>";
    html += "<h1>Firmware-Update</h1>";
    html += "<p>App-Image (.ino.bin), nicht das USB-merged.bin.</p>";
    html += "<form method='POST' action='/update' enctype='multipart/form-data'>";
    html += "<input type='file' name='firmware' accept='.bin'>";
    html += "<br><input type='submit' value='Flashen'></form>";
    html += "</body></html>";
    httpOta.send(200, "text/html", html);
  });

  httpOta.on("/update", HTTP_POST, []() {
    httpOta.sendHeader("Connection", "close");
    if (Update.hasError()) {
      httpOta.send(500, "text/plain", "Fehler beim Update");
    } else {
      httpOta.send(200, "text/plain", "OK, starte neu");
      delay(400);
      ESP.restart();
    }
  }, []() {
    HTTPUpload &upload = httpOta.upload();
    if (upload.status == UPLOAD_FILE_START) {
      stopAutorotate();
      otaShowScreen("Firmware-Update...");
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
        otaShowScreen("Update OK");
      } else {
        otaShowScreen("Update Fehler");
      }
    }
  });

  httpOta.begin();
  otaReady = true;
  if (debug) Serial.println("OTA http://" + WiFi.localIP().toString() + "/update");
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
  otaStop();
  rotctlStopServer();
  wifiWanted = false;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void wifiRestart() {
  otaStop();
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
    otaBegin();
    ArduinoOTA.handle();
    httpOta.handleClient();
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
  } else if (rotServerStarted || otaReady) {
    otaStop();
    rotctlStopServer();
  }
}
