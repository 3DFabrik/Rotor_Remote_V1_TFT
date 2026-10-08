//      CDE Rotor Control
//      By Patric Elsen
//      DF7ZZ

// Kalman-Filter-Klasse definieren
class KalmanFilter {
public:
  KalmanFilter(float processNoise, float measurementNoise, float estimationError, float initialEstimate) {
    Q = processNoise;
    R = measurementNoise;
    P0 = estimationError;
    reset(initialEstimate);
  }

  void reset(float initialEstimate) {
    X = initialEstimate;
    P = P0;
    K = 0;
  }

  float update(float measurement, float factor) {
    float r = R * factor;
    P = P + Q;
    K = P / (P + r);
    X = X + K * (measurement - X);
    P = (1 - K) * P;
    return X;
  }

private:
  float Q;   // Prozessrauschen in ADC-Digits^2 pro Sample (1 ms)
  float R;   // Messrauschen in ADC-Digits^2
  float P0;  // Start-Kovarianz
  float P;
  float X;
  float K;
};

// Median wirft Drahtpoti-Spikes raus. Kalman glaettet nur die Anzeige, nicht die Kalibrierung.
float processNoise = 0.02f;
float measurementNoise = 50.0f;
float estimationError = 50.0f;
float initialEstimate = 0;

KalmanFilter kalman(processNoise, measurementNoise, estimationError, initialEstimate);

static const float STAND_FILTER_FACTOR = 10.0f;

// Librarys
#include "BluetoothSerial.h"
BluetoothSerial SerialBT;
#include <Preferences.h>
Preferences preferences;    // Objekt für die Verwendung des Preferences-Speichers
#include <RunningMedian.h>  // Running median Filter for the sensor input
#include <TFT_eSPI.h>  // Graphics and font library for ST7789 driver chip, be careful with updating as the fucking update will delete your pin-settings
#include <SPI.h>
#include <WiFi.h>
#include <FS.h>
using fs::FS;
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <ctype.h>
#include <esp_bt.h>
#include <time.h>
#include "RotorTypes.h"

void webLog(const char *src, const char *msg);
bool otaIsBusy();
extern int webJog;
extern unsigned long otaWifiLostAt;

int medianSamples = 100;
RunningMedian *azMedian = nullptr;

void applyMedianSamples(int n) {
  if (n < 3) n = 3;
  if (n > 255) n = 255;
  medianSamples = n;
  if (azMedian && azMedian->getSize() == (uint8_t)n) return;
  delete azMedian;
  azMedian = new RunningMedian((uint8_t)n);
}

#define TFT_VDARKGREY 0x3186  // super dark grey
int COLOR_BG = TFT_BLACK;

TFT_eSPI tft = TFT_eSPI();  // Invoke library
TFT_eSprite spr = TFT_eSprite(&tft);
TFT_eSprite spr_angle = TFT_eSprite(&tft);

bool debug = false;
bool menuOpen = false;
bool msgStuck = false;
uint32_t lastFooterKey = 0xFFFFFFFF;
unsigned long glitchMsgUntil = 0;
bool serialBtOn = false;
bool wifiWanted = false;

static const int FOOT_Y = 214;
static const int FOOT_H = 26;
static const int FOOT_MSG_W = 218;

#define LINK_BT 0
#define LINK_WIFI 1
#define LINK_BOTH 2
int linkMode = LINK_BT;
int linkPendingMode = -1;  // radio change that takes effect when the menu is left
String wifiSsid = "";
String wifiPass = "";
int ipMode = 0;  // 0 = DHCP, 1 = statisch
String ipLocal = "192.168.1.50";
String ipGw = "192.168.1.1";
String ipMask = "255.255.255.0";
String ipDns = "192.168.1.1";
int rotPort = 4533;

const unsigned long BRK_LONG_MS = 3000;

unsigned long brakeReleaseTime = 0;
bool brakeReleasePending = false;
bool brakeReleaseCompleted = false;
bool b_turning = false;  // Bit set when rotator is turning

// Pinout
int pin_in_CCW = 12;
int pin_in_BRK = 13;
int pin_in_CW = 14;
int pin_in_AZ = 35;
int pin_out_CCW_relais = 25;
int pin_out_BRK_relais = 26;
int pin_out_CW_relais = 27;
int pin_out_AUX_relais = 32;

//PWM Pins for LED output
const int LEDPin = 33;      // der Output Pin der Alarm-LED
const int freq = 5000;      // Output PWM frequency
const int resolution = 10;  // Resolution of the PWM output (0-1023)

// Button variables set to true when button is pressed
bool but_CCW = false;
bool but_BRK = false;
bool but_CW = false;

float dig_AZ = 0;               // Rohwert ADC (spiked)
float dig_AZ_m = 0;             // Median, spike-bereinigt, schnell
float dig_AZ_f = 0;             // Kalman auf dem Median, fuer Anzeige
float azimut = 0;               // Kompasswinkel (Anzeige-Wert + 180°)
float azimut_abs = 0;           // absolut azimuth (0-360°) as target for automatic rotor movement
int azimut_tar = 0;             // azimut target as requested via serial port
bool b_autorotate = false;      // Gets set whenever a rotate-command is received and reset when rotation is finished
int a_overshoot = 3;            // Angle that the rotor overshoots
int az_max_digit = 3510;        // Wert vom AZ-pin beim max. Endanschlag vom Rotor
int az_min_digit = 0;           // Wert vom AZ-pin beim min. Endanschlag vom Rotor
float v_turn = 0;               // holds the calculated turning speed per second
const int speedSamples = 1500;  // amout of samples to average the measured rotor speed
float v_turn_history[speedSamples];
int v_turn_index = 0;
int angle_old = -1000;
int lastShownAngle = -1;  // whole degrees on the angle bar
int lastShownSpeed = -1;
unsigned long lastTftUpdate = 0;
const unsigned long TFT_TURN_MS = 250;   // display refresh while the rotor turns
const unsigned long TFT_IDLE_MS = 2000;  // display refresh while it stands
int rotCmd = 0;  // 0 = nichts, 1 = CCW (gegen den Uhrzeigersinn), 2 = CW (im Uhrzeigersinn)

// variables for serial comms
String Azimuth = "";

// Timer 10ms
unsigned long previousMillis = 0;
const long interval = 1;
// Timer 100ms
unsigned long prevMillis = 0;
const long interv = 100;

// Glitchtimer
unsigned long startTime = 0;             // Speichert den Startzeitpunkt
const unsigned long blinkDuration = 10;  // Dauer, für die die LED leuchten soll (in Millisekunden)
bool ledOn = false;                      // Zustand der LED
bool alarmOn = false;                    // Zustand des Alarms

// Misc Commands
int command_old = 0;       // used to detect if the command has changed
int stucktime = 0;         // increases once per display cycle if the expected angle change has not happened
int stop_stucktime = 3;    // When elapsed the rotator power gets switched off
float degpersec = 3;       // expected degree per second that the rotor should do when running free (measured roughly 6.5°/s)
float azimut_abs_old = 0;  // hold the sample taken interv-time before
String btName = "";        // holds the name of the bluetooth link

const char *linkModeLabel();
const char *wifiStateLabel();
String wifiIpCurrent();
int wifiRssiBars();
void applyLinkMode();
void switchLinkAndRestart(int mode);
int linkEffectiveMode();
void linkTogglePending();
void linkApplyPending();
const char *linkModeName(int mode);
void applyAzimuthTarget(int compassDeg);
void stopAutorotate();
void restartBluetooth();
void wifiService();
void rotctlService();
void wifiConnectNow();
void wifiStartScan();
void wifiScanStop();
bool wifiScanRunning();
bool wifiTakeScanDirty();
int wifiScanCount();
String wifiScanSSID(int i);
int wifiScanRSSI(int i);
bool linkWantsBt();
bool linkWantsWifi();
void menuEnter();
void menuOnBack();
void menuOnSelect();
void menuOnLeft();
void menuOnRight();
void menuResetHold();
void menuRefreshLive();
void calService();
int calMotorCmd();
bool calIsActive();
void calStopAll();
void drawMainScreen();
void drawLinkStatus();
void drawFooterAlert();
void readButtons();
void processButtonEvents();
void replyBoth(const String &msg);

void makeSprite(TFT_eSprite &s, int16_t w, int16_t h) {
  s.setColorDepth(16);
  if (!s.createSprite(w, h)) {  // with Bluetooth running 16 bit may not fit
    s.setColorDepth(8);
    s.createSprite(w, h);
  }
}

void makeScaleSprite() {  // 16 bit colors; the scale is drawn in strips to save 38 KB of heap
  spr.setColorDepth(16);
  for (int h = 60; h >= 15; h /= 2) {
    if (spr.createSprite(320, h)) return;
  }
}

void setup() {
  preferences.begin("RotorRemote", false);  // Beginne die Verwendung des Preferences-Speichers für die Anwendung "RotorRemote"
  GetStoredSetup();                         // Read all preference values out of the memory
  Serial.begin(115200);                     // Begin Serial communication on the cable interface

  if (linkMode == LINK_BT) {  // a crash in Bluetooth mode must not leave the unit in a reboot loop
    esp_reset_reason_t rr = esp_reset_reason();
    if (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT) {
      linkMode = LINK_WIFI;
      preferences.putInt("link_mode", linkMode);
    }
  }

  if (linkMode == LINK_WIFI) {  // must run before any Wi-Fi or Bluetooth init
    uint32_t heapBefore = ESP.getFreeHeap();
    esp_err_t btRel = esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
    NETLOG("bt mem release=%d heap %u -> %u", (int)btRel, (unsigned)heapBefore, (unsigned)ESP.getFreeHeap());
  }

  tft.init();
  tft.setRotation(1);
  tft.setTextColor(TFT_WHITE, COLOR_BG);
  tft.setTextSize(1);
  tft.fillScreen(COLOR_BG);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawRoundRect(0, 0, 105, 35, 4, TFT_WHITE);
  tft.drawRoundRect(110, 0, 100, 35, 4, TFT_WHITE);
  tft.drawRoundRect(215, 0, 105, 35, 4, TFT_WHITE);

  pinMode(pin_in_AZ, INPUT);
  pinMode(pin_in_CCW, INPUT_PULLUP);
  pinMode(pin_in_BRK, INPUT_PULLUP);
  pinMode(pin_in_CW, INPUT_PULLUP);
  pinMode(pin_out_CCW_relais, OUTPUT);
  pinMode(pin_out_BRK_relais, OUTPUT);
  pinMode(pin_out_CW_relais, OUTPUT);
  pinMode(pin_out_AUX_relais, OUTPUT);
  ledcAttach(LEDPin, freq, resolution);
  ledcWrite(LEDPin, 25);                    // Say Hello with the ALARM-LED...
  delay(50);                                // Wait 50ms...
  ledcWrite(LEDPin, 0);                     // And LED off
  initialEstimate = analogRead(pin_in_AZ);
  kalman.reset(initialEstimate);
  dig_AZ = initialEstimate;
  dig_AZ_m = initialEstimate;
  dig_AZ_f = initialEstimate;

  applyLinkMode();  // the radio needs its heap before the sprite takes its share
  NETLOG("radio up heap=%u maxblk=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  makeScaleSprite();                       // Erstelle das Sprite
  NETLOG("sprite %s heap=%u", spr.created() ? "ok" : "FAILED", (unsigned)ESP.getFreeHeap());
  spr.setTextColor(TFT_WHITE, TFT_BLACK);  // Textfarbe festlegen
  spr.setTextDatum(MC_DATUM);              // Textausrichtung zentriert
  display_rotation_arrow();
  drawAngleScale(roundedAzimuth());        // Initiale Anzeige des Rotorwinkels
  drawLinkStatus();
}

void loop() {  //***************************************************************************************************************************
  if (otaIsBusy()) {
    if (WiFi.status() != WL_CONNECTED) {
      if (!otaWifiLostAt) otaWifiLostAt = millis();
      else if (millis() - otaWifiLostAt > 8000) ESP.restart();
    } else {
      otaWifiLostAt = 0;
    }
    wifiService();
    yield();
    return;
  }

  unsigned long currentMillis = millis();

  if (currentMillis - previousMillis >= interval) {  // 1ms/1KHz timer for std. work
    previousMillis = currentMillis;
    readButtons();
    processButtonEvents();
    CalcPosition();
    if (!menuOpen) {
      AutoRotate(azimut_tar);
      ManualRotate();
    } else {
      calService();
      DriveRotator(calMotorCmd());
    }
    if (b_turning) {
      glitchalarm();
    } else if (alarmOn || ledOn || glitchMsgUntil) {
      alarmOn = false;
      ledOn = false;
      glitchMsgUntil = 0;
      if (!msgStuck) ledcWrite(LEDPin, 0);
      drawFooterAlert();
    }
  }

  if (currentMillis - prevMillis >= interv) {  // 100ms timer for display and stuck detection
    prevMillis = currentMillis;
    rotctlService();
    if (menuOpen) menuRefreshLive();
    int abs_int = (int)azimut_abs;
    if (debug) Serial.println("Abs position: " + String(abs_int) + "deg - Target: " + String(azimut_tar) + "deg");
    if (b_autorotate == true && !menuOpen) {
      if (abs(v_turn) >= degpersec) {
        stucktime = 0;
      } else {
        stucktime++;
        if ((stucktime / 10) >= stop_stucktime) {
          b_autorotate = false;
          DriveRotator(0);
          msgStuck = true;
          Serial.println("Rotor stuck detection fired!");
          webLog("rotor", "stuck");
          ledcWrite(LEDPin, 25);
          drawFooterAlert();
        }
      }
    }
  }

  if (!menuOpen && millis() - lastTftUpdate >= (b_turning ? TFT_TURN_MS : TFT_IDLE_MS)) {
    unsigned long drawStart = millis();
    tft_update();
    if (debug && millis() - drawStart >= 30) NETLOG("slow tft_update %lums", (unsigned long)(millis() - drawStart));
  }

  processSerialInput();
  processBluetoothInput();
  wifiService();
  delay(1);
}

void readCommandLine(Stream &s, char *buffer, int &i) {
  while (s.available() > 0) {
    char c = s.read();

    // Wenn ein CR-Zeichen empfangen wird, beende und verarbeite den Befehl
    if (c == '\r') {
      buffer[i] = '\0';  // Nullterminator am Ende hinzufügen
      SerComm(buffer);   // Befehl verarbeiten
      i = 0;             // Pufferindex zurücksetzen für das nächste Kommando
      return;            // Verlasse die Funktion, um Mehrfachverarbeitung zu verhindern
    }

    // Überprüfung auf Pufferüberlauf
    if (i < 14) {
      buffer[i++] = c;
    }
  }
}

void processSerialInput() {
  static char buffer[15];
  static int i = 0;
  readCommandLine(Serial, buffer, i);
}

void processBluetoothInput() {
  if (!serialBtOn) return;
  static char buffer[15];
  static int i = 0;
  readCommandLine(SerialBT, buffer, i);
}

static float standFilterFactor() {
  return b_turning ? 1.0f : STAND_FILTER_FACTOR;
}

void CalcPosition() {
  dig_AZ = analogRead(pin_in_AZ);
  azMedian->add(dig_AZ);
  dig_AZ_m = azMedian->getMedian();
  dig_AZ_f = kalman.update(dig_AZ_m, standFilterFactor());
  if (b_turning && abs(dig_AZ - dig_AZ_m) >= 300) alarmOn = true;


  // Berechnung des gefilterten Winkelwerts in Grad (float)
  float gain = 360.0 / (az_max_digit - az_min_digit);  // Skalierungsfaktor berechnen
  azimut_abs = (dig_AZ_f - az_min_digit) * gain;       // Anwendung des Skalierungsfaktors

  // Berechnung der Rotationsgeschwindigkeit (Grad pro Sekunde)
  float v_turn_raw = (azimut_abs - azimut_abs_old) * 1000.0;  // Geschwindigkeit pro Sekunde
  v_turn = calculateFilteredSpeed(v_turn_raw);                // Geschwindigkeit filtern
  if (!b_turning) v_turn = 0;

  // Aktualisieren des alten Azimut-Werts für den nächsten Zyklus
  azimut_abs_old = azimut_abs;

  // Azimut für Steuerung
  azimut = azimut_abs + 180;
  if (azimut >= 360) { azimut = azimut - 360; }
}

float calculateFilteredSpeed(float newSpeed) {
  v_turn_history[v_turn_index] = newSpeed;
  v_turn_index = (v_turn_index + 1) % speedSamples;
  float sum = 0;
  for (int i = 0; i < speedSamples; i++) {
    sum += v_turn_history[i];
  }

  return sum / speedSamples;
}

void glitchalarm() { // this switches off the alarm LED and gives a Display message for a short time
  unsigned long now = millis();

  if ((!ledOn) && (alarmOn)) {
    ledcWrite(LEDPin, 25);
    startTime = now;
    ledOn = true;
    glitchMsgUntil = now + 400;
    drawFooterAlert();
  }

  if (ledOn && (now - startTime >= blinkDuration)) {
    ledcWrite(LEDPin, 0);
    ledOn = false;
    alarmOn = false;
  }

  if (glitchMsgUntil && now >= glitchMsgUntil) {
    glitchMsgUntil = 0;
    drawFooterAlert();
  }
}

void drawAngleScale(int angle) { // Here we draw the big sprite with the angle scale
  if (angle == angle_old) {
    return;
  } else {
    angle_old = angle;
  }
  const uint16_t centerColor = TFT_NAVY;  // Farbe in der Mitte
  const uint16_t edgeColor = TFT_BLACK;   // Farbe am Rand
  const uint16_t foregroundColor = TFT_WHITE;
  const uint16_t pointerColor = TFT_RED;
  const int textSize = 2;
  const float longLineRatio = 0.2;     // Länge der langen Markierungen relativ zur Höhe
  const float shortLineRatio = 0.1;    // Länge der kurzen Markierungen
  const float textOffsetRatio = 0.33;  // Abstand des Texts zur unteren Kante

  // Berechnungen
  const int scaleH = 120;  // full instrument height, the sprite may hold only a strip of it
  const int stripH = spr.height();
  if (stripH <= 0) return;
  int centerX = spr.width() / 2;
  int centerY = scaleH / 2;
  float scaleWidth = 360 * 2;
  float pixelsPerDegree = 3;
  float offset = (angle * pixelsPerDegree);

  spr.setTextDatum(MC_DATUM);  // Ankerpunkt auf das Zentrum des Textes setzen

  for (int yo = 0; yo < scaleH; yo += stripH) {
  // Farbverlauf von der Mitte nach außen
  for (int x = 0; x < spr.width(); x++) {
    float distance = abs(centerX - x);  // Abstand von der Mitte
    float ratio = distance / centerX;   // Verhältnis zur halben Breite

    // Lineare Interpolation zwischen centerColor und edgeColor
    uint8_t r = ((1 - ratio) * ((centerColor >> 11) & 0x1F) + ratio * ((edgeColor >> 11) & 0x1F));
    uint8_t g = ((1 - ratio) * ((centerColor >> 5) & 0x3F) + ratio * ((edgeColor >> 5) & 0x3F));
    uint8_t b = ((1 - ratio) * (centerColor & 0x1F) + ratio * (edgeColor & 0x1F));
    uint16_t gradientColor = (r << 11) | (g << 5) | b;

    spr.drawLine(x, 0, x, stripH, gradientColor);  // Zeichnet die Farbverlaufslinie
  }

  spr.drawLine(0, centerY - yo, spr.width(), centerY - yo, foregroundColor);  // Horizontale Linie zeichnen
  spr.drawRoundRect(0, -yo, spr.width(), scaleH, 4, TFT_WHITE);

  // Markierungen und Beschriftungen zeichnen
  for (int i = -540; i <= 540; i += 10) {
    int xPos = centerX + i * pixelsPerDegree - offset;

    int lineLength = 0;
    if (i % 30 == 0) {  // Markerlänge bestimmen
      lineLength = scaleH * longLineRatio;
      spr.drawLine(xPos, centerY - lineLength - yo, xPos, centerY + lineLength - yo, foregroundColor);  // Vertikale Marker zeichnen
    } else {
      lineLength = scaleH * shortLineRatio;
      spr.drawLine(xPos, centerY - lineLength - yo, xPos, centerY + lineLength - yo, foregroundColor);  // Vertikale Marker zeichnen
    }

    if (i % 30 == 0) {  // Beschriftung bei jedem 45. Grad
      int textHeight = scaleH * textOffsetRatio;
      int displayAngle = (i + 360) % 360;
      spr.setTextColor(foregroundColor);
      char angBuf[8];
      snprintf(angBuf, sizeof(angBuf), "%d", displayAngle);
      spr.drawString(angBuf, xPos + 2, centerY - textHeight - yo, textSize);
      const char *direction = nullptr;
      switch (displayAngle) {
        case 0: direction = "N"; break;
        case 45: direction = "NE"; break;
        case 90: direction = "E"; break;
        case 135: direction = "SE"; break;
        case 180: direction = "S"; break;
        case 225: direction = "SW"; break;
        case 270: direction = "W"; break;
        case 315: direction = "NW"; break;
      }
      if (direction) {
        spr.drawString(direction, xPos, centerY + textHeight - yo, textSize);
      }
    }
  }

  spr.drawLine(centerX, scaleH - 10 - yo, centerX, 10 - yo, pointerColor);  // Den roten vertikalen Zeiger zeichnen
  spr.pushSprite(0, 45 + yo);                                               // Sprite auf den Bildschirm übertragen
  }
}

void AutoRotate(int targetAzimuth) {
  // Überprüfen, ob die Autorotation aktiv ist
  if (b_autorotate) {
    // Wenn der Rotor bereits dreht, prüfen wir nur, ob er das Ziel erreicht hat
    if (rotCmd > 0) {
      if ((rotCmd == 1 && azimut_abs - a_overshoot <= targetAzimuth) ||  // Dreht CCW und hat das Ziel erreicht
          (rotCmd == 2 && azimut_abs + a_overshoot >= targetAzimuth)) {  // Dreht CW und hat das Ziel erreicht
        b_autorotate = false;                                            // Stoppen der Autorotation
        rotCmd = 0;                                                      // Stoppen des Rotors
      }
    } else {  // Wenn der Rotor nicht dreht, bestimmen wir die Drehrichtung
      if (azimut_abs > targetAzimuth + a_overshoot) {
        rotCmd = 1;  // Drehen gegen den Uhrzeigersinn (CCW)
      } else if (azimut_abs < targetAzimuth - a_overshoot) {
        rotCmd = 2;  // Drehen im Uhrzeigersinn (CW)
      } else {       // Ziel bereits innerhalb des Toleranzbereichs erreicht
        b_autorotate = false;
        rotCmd = 0;  // Rotor bleibt stehen
      }
    }
    DriveRotator(rotCmd);  // Führe den aktuellen Drehbefehl aus
  }
}

void ManualRotate() {
  int rot_cmd = 0;  // 0-nothing, 1=CCW, 2=CW
  if (b_autorotate == false) {
    if (but_CCW || but_CW) {
      if ((but_CCW == 1) && (but_CW == 0)) { rot_cmd = 1; }
      if ((but_CCW == 0) && (but_CW == 1)) { rot_cmd = 2; }
    } else {
      rot_cmd = webJog;
    }
    DriveRotator(rot_cmd);
  } else {
    if (but_CCW == 0 && but_BRK == 1 && but_CW == 0) {
      DriveRotator(0);
      b_autorotate = false;
    }
  }
}

void DriveRotator(int command) {
  if (command != 0 && !calIsActive() && (az_max_digit - az_min_digit) >= 400) {
    bool atSoftEnd = false;
    if (command == 2 && dig_AZ_m >= (float)az_max_digit - 30.0f) atSoftEnd = true;
    if (command == 1 && dig_AZ_m <= (float)az_min_digit + 30.0f) atSoftEnd = true;
    if (atSoftEnd) {
      command = 0;
      if (b_autorotate) {
        b_autorotate = false;
        rotCmd = 0;
      }
    }
  }

  if (command_old != command) {
    command_old = command;

    ledcWrite(LEDPin, 0);
    msgStuck = false;
    drawFooterAlert();
    switch (command) {  // 0=nichts, 1=CCW, 2=CW

      case 1:
      case 2:
        // Bremse aktivieren, bevor der Rotor beginnt zu drehen
        digitalWrite(pin_out_BRK_relais, HIGH);
        display_rotation_arrow();

        // Setze die Zeit, um die 250 ms Verzögerung zu beginnen
        brakeReleaseTime = millis();
        brakeReleaseCompleted = false;
        brakeReleasePending = false;  // Stelle sicher, dass die Bremse nicht sofort wieder geschlossen wird

        // Hier wird der Rotor erst nach 250 ms aktiviert, siehe unten
        break;

      case 0:  // Rotor sofort stoppen

        digitalWrite(pin_out_CCW_relais, LOW);
        digitalWrite(pin_out_CW_relais, LOW);
        display_rotation_arrow();

        // Setze die Zeit für das Bremsen-Delay und aktiviere das Pending-Flag
        brakeReleaseTime = millis();
        brakeReleasePending = true;
        break;
    }
  }

  // Überprüfe, ob 250 ms vergangen sind, um den Rotor zu starten
  if (!brakeReleaseCompleted && (millis() - brakeReleaseTime >= 250)) {
    brakeReleaseCompleted = true;

    if (command_old == 1) {
      digitalWrite(pin_out_CCW_relais, HIGH);
      digitalWrite(pin_out_CW_relais, LOW);
    } else if (command_old == 2) {
      digitalWrite(pin_out_CCW_relais, LOW);
      digitalWrite(pin_out_CW_relais, HIGH);
    }

    display_rotation_arrow();
  }

  // Überprüfe, ob die Bremse nach dem Delay deaktiviert werden soll
  if (brakeReleasePending && (millis() - brakeReleaseTime >= 1000)) {
    digitalWrite(pin_out_BRK_relais, LOW);
    display_rotation_arrow();
    brakeReleasePending = false;  // Rücksetzen des Pending-Flags
  }
}

void display_rotation_arrow() {
  if (digitalRead(pin_out_BRK_relais) == HIGH) {
    b_turning = true;
  } else {
    b_turning = false;
  }
  if (menuOpen) return;

  if (digitalRead(pin_out_CCW_relais) == HIGH) {
    drawArrow(95, 17, 85, 34, 180, TFT_GREEN);  // Pfeil nach links (180 Grad) (x, Y, length, width, angle, color)
  } else {
    drawArrow(95, 17, 85, 34, 180, TFT_VDARKGREY);  // Pfeil nach links (180 Grad)
  }

  if (digitalRead(pin_out_CW_relais) == HIGH) {
    drawArrow(225, 17, 85, 34, 0, TFT_GREEN);  // Pfeil nach links (180 Grad)
  } else {
    drawArrow(225, 17, 85, 34, 0, TFT_VDARKGREY);  // Pfeil nach links (180 Grad)
  }
  tft.setTextDatum(MC_DATUM);
  if (digitalRead(pin_out_BRK_relais) == HIGH) {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString("BRAKE", 160, 21, 4);
  } else {
    tft.setTextColor(TFT_VDARKGREY, TFT_BLACK);
    tft.drawString("BRAKE", 160, 21, 4);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
  }
  tft.setTextDatum(TL_DATUM);
}

void drawArrow(int x, int y, int length, int width, int angle, uint16_t color) {
  // Berechne die Endpunkte der Linie für den Pfeilkörper
  int x1 = x + length * cos(radians(angle));
  int y1 = y + length * sin(radians(angle));

  // Zeichne den Pfeilkörper (Linie)
  tft.drawLine(x, y, x1, y1, color);
  tft.drawLine(x, y + 1, x1, y1 + 1, color);
  tft.drawLine(x, y - 1, x1, y1 - 1, color);

  // Berechne die Eckpunkte des Pfeilkopfes (Dreieck)
  int arrowHeight = width / 2;
  int x2 = x1 + arrowHeight * cos(radians(angle + 135));
  int y2 = y1 + arrowHeight * sin(radians(angle + 135));

  int x3 = x1 + arrowHeight * cos(radians(angle - 135));
  int y3 = y1 + arrowHeight * sin(radians(angle - 135));

  // Zeichne den Pfeilkopf (Dreieck)
  tft.fillTriangle(x1, y1, x2, y2, x3, y3, color);
}

void SerComm(char *buffer) {

  webLog("serial", buffer);
  if (debug) Serial.println("Received buffer: " + String(buffer));

  Azimuth = "";
  bool B_properCommand = false;

  // looking for command "C" and reply the actual azimuth value
  if (strncmp(buffer, "C", 1) == 0) {
    replyBoth("+ " + String(azimut, 1));
    return;
  }

  // New Target values detected <M>
  if (strncmp(buffer, "M", 1) == 0) {
    char token1[4];  // Platz für die dreistellige Zahl + Nullterminator
    strncpy(token1, buffer + 1, 3);
    token1[3] = '\0';  // Nullterminator hinzufügen
    Azimuth = token1;  // Konvertieren der Werte in Integer
    B_properCommand = true;
  }

  // New overshoot values detected <O>
  if (buffer[0] == 'O' && isdigit(buffer[1]) && buffer[2] == '\0') {
    int overshootValue = buffer[1] - '0';  // Konvertiere das Zeichen zu einer Zahl (0-9)
    preferences.putInt("a_overshoot", overshootValue);
    Serial.println("Overshoot value stored, value = " + String(overshootValue) + "'");
    PrintStoredSetup();
    return;  // Wir sind hier fertig
  }

  // New bluetooth name detected <N>
  if (buffer[0] == 'N') {                                                      // Überprüfen, ob das Eingabekommando mit "N" beginnt und maximal 15 Zeichen lang ist
    char commandString[16];                                                    // Größe 16 für das "N"-Kommando (15 Zeichen + 1 für den Nullterminator)
    strncpy(commandString, buffer + 1, 15);                                    // Kopiere maximal 15 Zeichen
    commandString[15] = '\0';                                                  // Sicherstellen, dass der String terminiert wird
    preferences.putString("name", commandString);                              // Speichere den String in den Preferences
    Serial.println("New BT-Name stored as: '" + String(commandString) + "'");  // Informiere den User was gerade passiert ist
    PrintStoredSetup();                                                        // Und der Vollständigkeit halber hier noch mal alle gespeicherten Variablen für den User
    return;                                                                    // Wir sind hier fertig
  }

  //  if a new angle is received
  if (Azimuth != "") {
    tft_update();
    applyAzimuthTarget(Azimuth.toInt());
    Serial.println("New Azimuth received: " + Azimuth + "deg - Abs: " + String(azimut_tar) + "deg");
    return;
  }

  // looking for  <S> as stop signal
  if (strncmp(buffer, "S", 1) == 0) {
    b_autorotate = false;
    replyBoth("Stop command received");
    return;
  }

  // looking for  <D> - Request a digital reading from the sensor
  if (strncmp(buffer, "D", 1) == 0) {
    Serial.println("dig_AZ = " + String(dig_AZ) + " / med = " + String(dig_AZ_m) + " / dig_AZ_f = " + String(dig_AZ_f));
    return;
  }

  // looking for  <L> - Store the minimum rotor position
  if (strncmp(buffer, "L", 1) == 0) {
    preferences.putInt("az_min_digit", dig_AZ_m);
    Serial.println("Minimum position stored, value = " + String(dig_AZ_m, 0) + " digits");
    PrintStoredSetup();
    return;
  }

  // looking for  <H> - Store the maximum rotor position
  if (strncmp(buffer, "H", 1) == 0) {
    preferences.putInt("az_max_digit", dig_AZ_m);
    Serial.println("Maximum position stored, value = " + String(dig_AZ_m, 0) + " digits");
    PrintStoredSetup();
    return;
  }

  // looking for  <V> - Request all stored values
  if (strncmp(buffer, "V", 1) == 0) {
    PrintStoredSetup();
    return;
  }

  // looking for  <?> - Print Help
  if (strncmp(buffer, "?", 1) == 0) {
    replyBoth("**********************************************************************************************************");
    replyBoth("List of possible commands:");
    replyBoth("C    - Reports the current calculated rotator position in ° compass angle");
    replyBoth("S    - Stops the rotation");
    replyBoth("MXXX - Requests the controller to turn the rotator to XXX position in ° compass angle");
    replyBoth("D    - Reports the current values of the unfiltered and filtered angle in raw digits");
    replyBoth("L    - Stores the current raw angle as lowest reachable position of the rotator");
    replyBoth("H    - Stores the current raw angle as highest reachable position of the rotator");
    replyBoth("OX   - Stores the overshoot value at which the rotor stops rotating prior reaching the target, 3' default.");
    replyBoth("V    - Reports all currently stored setup values in EEPROM");
    replyBoth("**********************************************************************************************************");
    replyBoth("");
    return;
  }

  if (!B_properCommand) Serial.println("Unknown command received - " + String(buffer) + " - Type '?' for help.");
}

static void drawFixedCell(TFT_eSprite &s, char c, int x, int cellW) {
  if (c == ' ' || c == '\0') return;
  char tmp[2] = {c, 0};
  int w = s.textWidth(tmp, 4);
  s.drawString(tmp, x + (cellW - w) / 2, 0, 4);
}

int roundedAzimuth() {
  int deg = (int)lroundf(azimut) % 360;
  if (deg < 0) deg += 360;
  return deg;
}

void tft_update() {
  if (menuOpen) return;
  lastTftUpdate = millis();
  if (spr_angle.width() != 320 || spr_angle.height() != 25) {
    spr_angle.deleteSprite();
    makeSprite(spr_angle, 320, 25);
    lastShownAngle = -1;
  }

  int show = roundedAzimuth();
  int speed = (int)v_turn;
  if (speed < 0) speed = 0;
  if (speed > 999) speed = 999;
  if (show == lastShownAngle && speed == lastShownSpeed) {
    drawLinkStatus();
    return;
  }
  lastShownAngle = show;
  lastShownSpeed = speed;

  spr_angle.setTextDatum(TL_DATUM);
  spr_angle.setTextPadding(0);
  spr_angle.setTextColor(TFT_WHITE, COLOR_BG);
  spr_angle.fillScreen(TFT_BLACK);

  static int digitW = 0;
  if (!digitW) {
    for (char c = '0'; c <= '9'; c++) {
      char tmp[2] = {c, 0};
      int w = spr_angle.textWidth(tmp, 4);
      if (w > digitW) digitW = w;
    }
  }

  char angleTxt[8];
  snprintf(angleTxt, sizeof(angleTxt), "%3d", show);
  char speedTxt[8];
  snprintf(speedTxt, sizeof(speedTxt), "%3d", speed);

  int x = 2;
  spr_angle.drawString("Angle", x, 6, 2);
  x += spr_angle.textWidth("Angle", 2) + 4;
  for (int i = 0; angleTxt[i]; i++) {
    drawFixedCell(spr_angle, angleTxt[i], x, digitW);
    x += digitW + 1;
  }
  x += 8;
  spr_angle.drawString("Speed", x, 6, 2);
  x += spr_angle.textWidth("Speed", 2) + 4;
  for (int i = 0; speedTxt[i]; i++) {
    drawFixedCell(spr_angle, speedTxt[i], x, digitW);
    x += digitW + 1;
  }

  spr_angle.pushSprite(0, 180);
  drawAngleScale(show);
  drawLinkStatus();
}

void GetStoredSetup() {
  az_min_digit = preferences.getInt("az_min_digit", 200);
  az_max_digit = preferences.getInt("az_max_digit", 3800);
  a_overshoot = preferences.getInt("a_overshoot", 3);
  medianSamples = preferences.getInt("median_n", 100);
  applyMedianSamples(medianSamples);
  btName = preferences.getString("name", "RotorRemote_2");
  linkMode = preferences.getInt("link_mode", LINK_BT);
  if (linkMode == LINK_BOTH) {  // older firmware allowed both radios at once
    linkMode = LINK_WIFI;
    preferences.putInt("link_mode", linkMode);
  }
  if (linkMode < LINK_BT || linkMode > LINK_WIFI) linkMode = LINK_BT;
  wifiSsid = preferences.getString("wifi_ssid", "");
  wifiPass = preferences.getString("wifi_pass", "");
  ipMode = preferences.getInt("ip_mode", 0);
  if (ipMode != 1) ipMode = 0;
  ipLocal = preferences.getString("ip_local", "192.168.1.50");
  ipGw = preferences.getString("ip_gw", "192.168.1.1");
  ipMask = preferences.getString("ip_mask", "255.255.255.0");
  ipDns = preferences.getString("ip_dns", "192.168.1.1");
  rotPort = preferences.getInt("rot_port", 4533);
  if (rotPort < 1 || rotPort > 65535) rotPort = 4533;
}

void PrintStoredSetup() {
  GetStoredSetup();
  replyBoth("*********************************************************************************************************");
  replyBoth("Stored setup values:");
  replyBoth("Maximum position value: " + String(az_max_digit) + " digits");
  replyBoth("Minimum position value: " + String(az_min_digit) + " digits");
  replyBoth("Rotor overshoot value : " + String(a_overshoot) + "'");
  replyBoth("Bluetooth Name        : " + btName);
  replyBoth("Link mode             : " + String(linkModeLabel()));
  replyBoth("WLAN SSID             : " + wifiSsid);
  replyBoth("WLAN IP mode          : " + String(ipMode ? "static" : "DHCP"));
  replyBoth("WLAN IP / GW / Mask   : " + ipLocal + " / " + ipGw + " / " + ipMask);
  replyBoth("WLAN DNS              : " + ipDns);
  replyBoth("rotctld port          : " + String(rotPort));
  replyBoth("*********************************************************************************************************");
  replyBoth("");
}

const char *linkModeName(int mode) {
  return mode == LINK_WIFI ? "Wi-Fi" : "Bluetooth";
}

const char *linkModeLabel() {
  return linkModeName(linkMode);
}

void applyAzimuthTarget(int compassDeg) {
  if (compassDeg < 0) compassDeg = 0;
  compassDeg = compassDeg % 360;
  azimut_tar = compassDeg;
  if (azimut_tar >= 180) {
    azimut_tar = azimut_tar - 180;
  } else {
    azimut_tar = azimut_tar + 180;
  }
  azimut_tar = azimut_tar % 360;
  b_autorotate = true;
  prevMillis = millis();
  stucktime = 0;
}

void stopAutorotate() {
  b_autorotate = false;
  rotCmd = 0;
  DriveRotator(0);
}

void readButtons() {
  but_CCW = !digitalRead(pin_in_CCW);
  but_BRK = !digitalRead(pin_in_BRK);
  but_CW = !digitalRead(pin_in_CW);
}

void processButtonEvents() {
  static bool ccwPrev = false;
  static bool brkPrev = false;
  static bool cwPrev = false;
  static unsigned long brkDownAt = 0;
  static bool brkLongFired = false;
  static unsigned long repeatAt = 0;
  static int repeatDir = 0;
  static unsigned long lastNav = 0;

  unsigned long now = millis();

  if (but_BRK && !brkPrev) {
    brkDownAt = now;
    brkLongFired = false;
  }
  if (but_BRK && !brkLongFired && (now - brkDownAt >= BRK_LONG_MS)) {
    brkLongFired = true;
    if (!menuOpen) {
      menuEnter();
    } else {
      menuOnBack();
    }
  }
  if (!but_BRK && brkPrev) {
    unsigned long held = now - brkDownAt;
    if (!brkLongFired && held >= 30 && held < BRK_LONG_MS && menuOpen) {
      menuOnSelect();
    }
  }

  if (menuOpen) {
    if (but_CCW && !ccwPrev && (now - lastNav > 40)) {
      menuOnLeft();
      lastNav = now;
      repeatAt = now + 400;
      repeatDir = -1;
    }
    if (but_CW && !cwPrev && (now - lastNav > 40)) {
      menuOnRight();
      lastNav = now;
      repeatAt = now + 400;
      repeatDir = 1;
    }
    if (but_CCW && repeatDir == -1 && now >= repeatAt) {
      menuOnLeft();
      repeatAt = now + 80;
    }
    if (but_CW && repeatDir == 1 && now >= repeatAt) {
      menuOnRight();
      repeatAt = now + 80;
    }
    if (!but_CCW && !but_CW) {
      repeatDir = 0;
      menuResetHold();
    }
  }

  ccwPrev = but_CCW;
  brkPrev = but_BRK;
  cwPrev = but_CW;
}

void drawMainScreen() {
  angle_old = -1000;
  lastShownAngle = -1;
  lastShownSpeed = -1;
  lastFooterKey = 0xFFFFFFFF;
  glitchMsgUntil = 0;
  if (!spr.created()) {
    spr.deleteSprite();
    makeScaleSprite();
    spr.setTextColor(TFT_WHITE, TFT_BLACK);
    spr.setTextDatum(MC_DATUM);
  }
  if (spr_angle.width() != 320 || spr_angle.height() != 25) {
    spr_angle.deleteSprite();
    makeSprite(spr_angle, 320, 25);
  }
  tft.fillScreen(COLOR_BG);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawRoundRect(0, 0, 105, 35, 4, TFT_WHITE);
  tft.drawRoundRect(110, 0, 100, 35, 4, TFT_WHITE);
  tft.drawRoundRect(215, 0, 105, 35, 4, TFT_WHITE);
  display_rotation_arrow();
  tft_update();
}

static void drawFanArc(int cx, int cy, int r, uint16_t color) {
  float t = -0.75f;
  int x0 = cx + (int)(r * sin(t));
  int y0 = cy - (int)(r * cos(t));
  for (int i = 1; i <= 8; i++) {
    t = -0.75f + (1.5f * i) / 8.0f;
    int x = cx + (int)(r * sin(t));
    int y = cy - (int)(r * cos(t));
    tft.drawLine(x0, y0, x, y, color);
    tft.drawLine(x0 + 1, y0, x + 1, y, color);
    x0 = x;
    y0 = y;
  }
}

static void drawBtIcon(int x, int y, uint16_t color) {
  int cx = x + 6;
  int y0 = y;
  int y1 = y + 16;
  int ym = y + 8;
  int r = x + 12;
  tft.drawLine(cx, y0, cx, y1, color);
  tft.drawLine(cx + 1, y0, cx + 1, y1, color);
  tft.drawLine(cx, y0, r, y0 + 4, color);
  tft.drawLine(r, y0 + 4, cx, ym, color);
  tft.drawLine(cx, ym, r, y1 - 4, color);
  tft.drawLine(r, y1 - 4, cx, y1, color);
  tft.drawLine(x, y0 + 4, r, y1 - 4, color);
  tft.drawLine(x, y1 - 4, r, y0 + 4, color);
}

static void drawWifiIcon(int cx, int cy, bool connected, int bars, uint16_t color) {
  uint16_t dim = TFT_DARKGREY;
  tft.fillCircle(cx, cy, 2, color);
  drawFanArc(cx, cy, 6, (connected && bars >= 1) ? color : dim);
  drawFanArc(cx, cy, 11, (connected && bars >= 2) ? color : dim);
  drawFanArc(cx, cy, 16, (connected && bars >= 3) ? color : dim);
}

void drawFooterAlert() {
  if (menuOpen) return;
  tft.fillRect(0, FOOT_Y, FOOT_MSG_W, FOOT_H, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  if (msgStuck) {
    tft.drawString("ROTOR STUCK!", 6, FOOT_Y + 5, 2);
  } else if (glitchMsgUntil && millis() < glitchMsgUntil) {
    tft.drawString("SENSOR GLITCH", 6, FOOT_Y + 5, 2);
  }
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
}

void drawLinkStatus() {
  if (menuOpen) return;

  bool btOn = serialBtOn;
  bool btCli = btOn && SerialBT.hasClient();
  bool wifiOn = linkWantsWifi();
  int st = wifiOn ? (int)WiFi.status() : (int)WL_DISCONNECTED;
  bool wifiOk = (st == WL_CONNECTED);
  int bars = wifiOk ? wifiRssiBars() : 0;

  uint32_t key = (btOn ? 1u : 0u) | (btCli ? 2u : 0u) | (wifiOn ? 4u : 0u) |
                 (wifiOk ? 8u : 0u) | ((uint32_t)(bars & 3) << 4) |
                 ((uint32_t)st << 8);
  if (key == lastFooterKey) return;
  lastFooterKey = key;

  tft.fillRect(FOOT_MSG_W, FOOT_Y, 320 - FOOT_MSG_W, FOOT_H, TFT_BLACK);

  int x = 318;
  if (wifiOn) {
    uint16_t col = TFT_CYAN;
    if (!wifiOk && (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL)) col = TFT_ORANGE;
    else if (!wifiOk) col = TFT_YELLOW;
    x -= 34;
    drawWifiIcon(x + 16, FOOT_Y + 20, wifiOk, bars, col);
  }
  if (btOn) {
    x -= 22;
    drawBtIcon(x, FOOT_Y + 4, btCli ? TFT_GREEN : TFT_CYAN);
  }
}

void replyBoth(const String &msg) {
  Serial.println(msg);
  if (serialBtOn) SerialBT.println(msg);
}
