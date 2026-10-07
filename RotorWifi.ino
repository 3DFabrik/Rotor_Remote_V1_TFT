WiFiServer rotServer;
WiFiClient rotClient;
bool rotServerStarted = false;
WebServer httpOta(80);
static bool otaReady = false;
static bool otaBusy = false;
static bool otaWeb = false;
static bool otaBeginOk = false;
static bool otaOk = false;
static bool otaUploadStarted = false;
unsigned long otaWifiLostAt = 0;
static String otaErr;
static void otaQuiesceNetwork();

static int wifiScanState = 0;  // 0 idle, 1 running, 2 done
static int wifiScanN = 0;
static bool wifiScanDirty = false;

static char rotLine[96];
static int rotLineLen = 0;

static const int WEB_LOG_N = 16;
static const int WEB_LOG_W = 96;
static char webLogLines[WEB_LOG_N][WEB_LOG_W];
static uint8_t webLogHead = 0;
static uint8_t webLogCount = 0;
static bool ntpStarted = false;

int webJog = 0;
unsigned long webJogAt = 0;

void webLog(const char *src, const char *msg) {
  if (!src) src = "";
  if (!msg) msg = "";
  char stamp[24] = "--";
  time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm tmNow;
    localtime_r(&now, &tmNow);
    strftime(stamp, sizeof(stamp), "%d.%m.%Y %H:%M:%S", &tmNow);
  }
  char *d = webLogLines[webLogHead];
  snprintf(d, WEB_LOG_W, "%s %s %s", stamp, src, msg);
  webLogHead = (webLogHead + 1) % WEB_LOG_N;
  if (webLogCount < WEB_LOG_N) webLogCount++;
}

static void webJogExpire() {
  if (webJog != 0 && (millis() - webJogAt > 500)) {
    webJog = 0;
    webLog("web", "stop");
  }
}

bool linkWantsBt() {
  return linkMode == LINK_BT || linkMode == LINK_BOTH;
}

bool linkWantsWifi() {
  return linkMode == LINK_WIFI || linkMode == LINK_BOTH;
}

const char *wifiStateLabel() {
  if (!wifiWanted && !linkWantsWifi()) return "off";
  switch (WiFi.status()) {
    case WL_CONNECTED: return "connected";
    case WL_NO_SSID_AVAIL: return "no SSID";
    case WL_CONNECT_FAILED: return "failed";
    case WL_CONNECTION_LOST: return "lost";
    case WL_DISCONNECTED: return "offline";
    default: return wifiSsid.length() ? "connecting..." : "no SSID";
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

static void otaGiveUp(const char *msg) {
  static bool once = false;
  if (once) return;
  once = true;
  Update.abort();
  otaShowScreen(msg);
  delay(1200);
  ESP.restart();
}

static void otaFailScreen(const String &err) {
  otaShowScreen("Update failed");
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(err, 160, 170, 2);
  tft.setTextDatum(TL_DATUM);
}

bool otaIsBusy() {
  return otaBusy;
}

static String otaHtmlHead(const char *title, bool onUpdate) {
  String h = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  h += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  h += "<title>";
  h += title;
  h += "</title><style>";
  h += "body{font-family:sans-serif;background:#111;color:#eee;margin:0}";
  h += "header{background:#1a2332;padding:16px 20px;border-bottom:1px solid #345}";
  h += "header h1{margin:0;font-size:22px;color:#6cf}";
  h += ".ver{color:#9ab;margin-top:6px;font-size:14px}";
  h += "nav{margin-top:14px}";
  h += "nav a{color:#9ab;margin-right:18px;text-decoration:none}";
  h += "nav a.on{color:#6cf;font-weight:bold}";
  h += "main{padding:24px;max-width:640px}";
  h += "p{line-height:1.45}";
  h += ".note{color:#9ab}";
  h += ".grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin:16px 0}";
  h += ".card{background:#1a2332;border:1px solid #345;border-radius:8px;padding:14px}";
  h += ".card h2{margin:0 0 8px;font-size:13px;color:#9ab;font-weight:normal}";
  h += ".big{font-size:28px;color:#6cf}";
  h += "select.log{width:100%;height:220px;background:#0d1117;color:#c9d1d9;border:1px solid #345;";
  h += "font-family:ui-monospace,monospace;font-size:13px;padding:4px}";
  h += ".jog{display:flex;gap:12px;margin:8px 0 18px}";
  h += ".jog button{flex:1;font-size:18px;padding:14px;background:#246;color:#fff;border:0;border-radius:8px;touch-action:none}";
  h += ".jog button:active{background:#3a7}";
  h += "input[type=file]{display:block;margin:16px 0}";
  h += "input[type=submit]{font-size:16px;padding:10px 18px;background:#246;color:#fff;border:0;border-radius:6px}";
  h += "</style></head><body><header><h1>RotorRemote</h1>";
  h += "<div class='ver'>Firmware ";
  h += FW_VERSION;
  h += "</div><nav>";
  if (onUpdate) {
    h += "<a href='/'>Home</a><a class='on' href='/update'>Update</a>";
  } else {
    h += "<a class='on' href='/'>Home</a><a href='/update'>Update</a>";
  }
  h += "</nav></header><main>";
  return h;
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
    otaQuiesceNetwork();
    otaShowScreen("Updating...");
  });
  ArduinoOTA.onEnd([]() {
    otaShowScreen("Update OK");
  });
  ArduinoOTA.onError([](ota_error_t err) {
    (void)err;
    otaGiveUp("Update failed");
  });
  ArduinoOTA.begin();

  httpOta.on("/", HTTP_GET, []() {
    String ip = WiFi.localIP().toString();
    String html = otaHtmlHead("RotorRemote", false);
    html += "<p>rotctld: " + ip + ":" + String(rotPort) + "</p>";
    html += "<div class='grid'><div class='card'><h2>Azimuth</h2><div class='big' id='az'>-</div></div>";
    html += "<div class='card'><h2>Motion</h2><div class='big' id='dir'>-</div></div></div>";
    html += "<p class='note'>Target <span id='tgt'>-</span> &nbsp; Debug <span id='dbg'>-</span> &nbsp; Client <span id='cli'>-</span></p>";
    html += "<div class='jog'><button id='ccw' type='button'>CCW</button><button id='cw' type='button'>CW</button></div>";
    html += "<h2>rotctld / debug</h2>";
    html += "<select class='log' id='log' size='12'></select>";
    html += "<p><a href='/update'>Flash firmware over Wi-Fi</a></p>";
    html += "<script>";
    html += "async function tick(){try{const r=await fetch('/status');if(!r.ok)return;const s=await r.json();";
    html += "az.textContent=Number(s.az).toFixed(1)+'\\u00b0';";
    html += "dir.textContent=s.dir;dir.style.color=s.turning?'#6f6':'#9ab';";
    html += "tgt.textContent=s.target+'\\u00b0';dbg.textContent=s.debug?'on':'off';";
    html += "cli.textContent=s.client?'connected':'none';";
    html += "const el=log;el.innerHTML='';";
    html += "(s.log||[]).forEach(t=>el.add(new Option(t)));";
    html += "if(el.options.length)el.selectedIndex=el.options.length-1;";
    html += "}catch(e){}}setInterval(tick,500);tick();";
    html += "function bindJog(id,dir){const el=document.getElementById(id);let on=false,t=null;";
    html += "const go=()=>fetch('/jog',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'dir='+dir});";
    html += "el.addEventListener('pointerdown',e=>{e.preventDefault();el.setPointerCapture(e.pointerId);on=true;go();t=setInterval(go,200);});";
    html += "const stop=()=>{if(!on)return;on=false;clearInterval(t);fetch('/jog',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'dir=stop'});};";
    html += "el.addEventListener('pointerup',stop);el.addEventListener('pointercancel',stop);}";
    html += "bindJog('ccw','ccw');bindJog('cw','cw');";
    html += "</script>";
    html += "</main></body></html>";
    httpOta.send(200, "text/html", html);
  });

  httpOta.on("/status", HTTP_GET, []() {
    if (otaBusy) {
      httpOta.send(503, "text/plain", "busy");
      return;
    }
    const char *dir = "Stopped";
    bool ccw = digitalRead(pin_out_CCW_relais);
    bool cw = digitalRead(pin_out_CW_relais);
    if (ccw) dir = "Turning CCW";
    else if (cw) dir = "Turning CW";
    int compassTarget = azimut_tar + 180;
    if (compassTarget >= 360) compassTarget -= 360;
    String json = "{";
    json += "\"az\":" + String(azimut, 1);
    json += ",\"target\":" + String(compassTarget);
    json += ",\"turning\":";
    json += (ccw || cw) ? "true" : "false";
    json += ",\"dir\":\"";
    json += dir;
    json += "\",\"debug\":";
    json += debug ? "true" : "false";
    json += ",\"client\":";
    json += (rotClient && rotClient.connected()) ? "true" : "false";
    json += ",\"log\":[";
    int start = (webLogHead + WEB_LOG_N - webLogCount) % WEB_LOG_N;
    for (int i = 0; i < webLogCount; i++) {
      if (i) json += ",";
      json += "\"";
      const char *s = webLogLines[(start + i) % WEB_LOG_N];
      for (; *s; s++) {
        if (*s == '"' || *s == '\\') json += '\\';
        if (*s >= 32) json += *s;
      }
      json += "\"";
    }
    json += "]}";
    httpOta.send(200, "application/json", json);
  });

  httpOta.on("/jog", HTTP_POST, []() {
    if (otaBusy || menuOpen || b_autorotate) {
      if (webJog) {
        webJog = 0;
        webLog("web", "stop");
      }
      httpOta.send(409, "text/plain", "busy");
      return;
    }
    String dir = httpOta.arg("dir");
    int cmd = 0;
    if (dir == "ccw") cmd = 1;
    else if (dir == "cw") cmd = 2;
    if (cmd != webJog) {
      if (cmd == 1) webLog("web", "CCW");
      else if (cmd == 2) webLog("web", "CW");
      else webLog("web", "stop");
    }
    webJog = cmd;
    webJogAt = millis();
    httpOta.send(200, "text/plain", "ok");
  });

  httpOta.on("/update", HTTP_GET, []() {
    String html = otaHtmlHead("Firmware update", true);
    html += "<h2>Firmware update</h2>";
    html += "<p>Use <strong>RotorRemote_ota.bin</strong> (app image). Do not upload the USB merged <strong>RotorRemote.bin</strong>.</p>";
    html += "<form method='POST' action='/update' enctype='multipart/form-data'>";
    html += "<input type='file' name='firmware' accept='.bin'>";
    html += "<input type='submit' value='Flash'></form>";
    html += "<p class='note'>Leave the home page before flashing: it polls the controller. "
            "The display switches to Updating when the file transfer starts. "
            "The controller restarts after a successful update.</p>";
    html += "</main></body></html>";
    httpOta.send(200, "text/html", html);
  });

  httpOta.on("/update", HTTP_POST, []() {
    httpOta.sendHeader("Connection", "close");
    if (otaOk) {
      otaShowScreen("Update OK");
      httpOta.send(200, "text/html",
        "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Restarting</title>"
        "<style>body{font-family:sans-serif;background:#111;color:#eee;padding:24px}h1{color:#6cf}</style>"
        "</head><body><h1>Neustart</h1><p>Die neue Firmware kommt gleich.</p>"
        "<script>async function back(){try{const r=await fetch('/',{cache:'no-store'});"
        "if(r.ok){location.replace('/');return;}}catch(e){}setTimeout(back,1000);}"
        "setTimeout(back,2000);</script></body></html>");
      delay(800);
      ESP.restart();
    } else {
      if (!otaErr.length() || otaErr == "No Error") otaErr = "aborted";
      Update.abort();
      otaFailScreen(otaErr);
      httpOta.send(500, "text/plain", otaErr);
      delay(2500);
      ESP.restart();
    }
  }, []() {
    HTTPUpload &upload = httpOta.upload();
    if (upload.status == UPLOAD_FILE_START) {
      otaUploadStarted = true;
      otaWeb = true;
      otaBusy = true;
      b_autorotate = false;
      rotCmd = 0;
      webJog = 0;
      digitalWrite(pin_out_CCW_relais, LOW);
      digitalWrite(pin_out_CW_relais, LOW);
      digitalWrite(pin_out_BRK_relais, LOW);
      rotctlStopServer();
      rotLineLen = 0;
      spr.deleteSprite();
      spr_angle.deleteSprite();
      otaShowScreen("Updating...");
      Update.abort();
      WiFi.setSleep(false);
      httpOta.client().setTimeout(60000);
      otaBeginOk = Update.begin(UPDATE_SIZE_UNKNOWN);
      if (!otaBeginOk) {
        otaErr = Update.getError() ? Update.errorString() : ("start failed " + String(ESP.getMaxAllocHeap()));
        Update.printError(Serial);
        Serial.println(otaErr);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (otaBeginOk && !otaErr.length() && upload.currentSize) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
          otaErr = Update.getError() ? Update.errorString() : "write failed";
          otaBeginOk = false;
          Update.printError(Serial);
          Serial.println(otaErr);
        }
      }
      yield();
    } else if (upload.status == UPLOAD_FILE_END) {
      if (otaBeginOk && !otaErr.length()) {
        if (Update.end(true)) {
          otaOk = true;
        } else {
          otaErr = Update.getError() ? Update.errorString() : "finalize failed";
          otaErr += " ";
          otaErr += String((unsigned long)upload.totalSize);
          Update.printError(Serial);
          Serial.println(otaErr);
        }
      } else if (!otaErr.length()) {
        otaErr = "no image";
      }
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
      if (!otaOk && !otaErr.length()) otaErr = "connection lost";
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

static void otaQuiesceNetwork() {
  otaBusy = true;
  stopAutorotate();
  rotctlStopServer();
  rotLineLen = 0;
  if (serialBtOn) {
    SerialBT.end();
    serialBtOn = false;
  }
  webLog("ota", "rotctld dropped");
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
        webLog("rotctl", rotLine);
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
  webJogExpire();

  if (!wifiWanted) return;

  if (WiFi.status() == WL_CONNECTED) {
    if (!ntpStarted) {
      configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org");
      ntpStarted = true;
    }
    otaBegin();
    if (otaBusy) {
      if (!otaWeb) ArduinoOTA.handle();
      httpOta.handleClient();
      if (otaUploadStarted && !otaOk && otaErr.length()) {
        otaFailScreen(otaErr);
        delay(2000);
        ESP.restart();
      }
      return;
    }
    if (!rotServerStarted) {
      rotServer.begin(rotPort);
      rotServerStarted = true;
      rotLineLen = 0;
      webLog("wifi", "rotctld listening");
      if (debug) Serial.println("rotctld listening on " + WiFi.localIP().toString() + ":" + String(rotPort));
    }
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
      webLog("rotctl", "client connected");
      if (debug) Serial.println("rotctld client connected");
    }
    static bool rotHadClient = false;
    if (rotClient && rotClient.connected()) {
      rotHadClient = true;
      rotctlRead();
    } else if (rotHadClient) {
      rotHadClient = false;
      webLog("rotctl", "client gone");
      rotClient.stop();
    }
  } else if (!otaBusy && (rotServerStarted || otaReady)) {
    otaStop();
    rotctlStopServer();
  }
}
