//      CDE Rotor Control
//      By Patric Elsen
//      DF7ZZ
//      Ver. 06.10.2026 07:00

// Kalman-Filter-Klasse definieren
class KalmanFilter {
public:
  KalmanFilter(float processNoise, float measurementNoise, float estimationError, float initialEstimate) {
    Q = processNoise;      // Prozessrauschen
    R = measurementNoise;  // Messrauschen
    P = estimationError;   // Anfangsfehler
    X = initialEstimate;   // Anfangsschätzung
    K = 0;                 // Kalman-Verstärkung
  }

  float update(float measurement) {
    P = P + Q;                      // Vorhersage-Update
    K = P / (P + R);                // Kalman-Verstärkung berechnen
    X = X + K * (measurement - X);  // Schätzung aktualisieren
    P = (1 - K) * P;                // Fehler-Kovarianz aktualisieren
    return X;
  }

private:
  float Q;  // Prozessrauschen
  float R;  // Messrauschen
  float P;  // Fehler-Schätzung
  float X;  // Schätzung
  float K;  // Kalman-Verstärkung
};

// Globale Variablen und Objekte für den Kalman Filter
float processNoise = pow(0.05, 2);  // Rauschen des Prozesses, Bei einem großen Wert vertraut der Filter mehr auf die Messungen als auf das Modell. Hier wurde die tatsächliche Winkelgeschwindigkeit 5°/s eingestellt.
float measurementNoise = 1.5;       // Das Messrauschen, das Unsicherheit in den Messungen beschreibt. Bei einem großen Wert vertraut der Filter mehr auf das Modell als auf die Messungen. (Wert empirisch ermittelt)
float estimationError = 1;          // Der Fehler in der anfänglichen Schätzung. Dieser Wert wird bei jeder Iteration angepasst.
float initialEstimate = 0;          // Die Anfangsschätzung des Zustands.

KalmanFilter kalman(processNoise, measurementNoise, estimationError, initialEstimate);

// Librarys
#include "BluetoothSerial.h"
BluetoothSerial SerialBT;
#include <Preferences.h>
Preferences preferences;    // Objekt für die Verwendung des Preferences-Speichers
#include <RunningMedian.h>  // Running median Filter for the sensor input
RunningMedian samples = RunningMedian(1000);
#include <TFT_eSPI.h>  // Graphics and font library for ST7789 driver chip, be careful with updating as the fucking update will delete your pin-settings
#include <SPI.h>
#include <WiFi.h>
#include <ctype.h>
#include "RotorTypes.h"

#define TFT_GREY 0x5AEB       // New colours....
#define TFT_VDARKGREY 0x3186  // super dark grey
int COLOR_BG = TFT_BLACK;

TFT_eSPI tft = TFT_eSPI();  // Invoke library
TFT_eSprite spr = TFT_eSprite(&tft);
TFT_eSprite spr_angle = TFT_eSprite(&tft);

bool debug = false;
bool menuOpen = false;
bool msgStuck = false;
String lastLinkStatus = "";
bool serialBtOn = false;
bool wifiWanted = false;

#define LINK_BT 0
#define LINK_WIFI 1
#define LINK_BOTH 2
int linkMode = LINK_BT;
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
const int resolution = 10;  // Resolution of the PWM output (0-255)

// Button variables set to true when button is pressed
bool but_CCW = false;
bool but_BRK = false;
bool but_CW = false;

float dig_AZ = 0;               // unfiltered bits of AZ-pin
float dig_AZ_f = 0;             // Kalman filtered bits of AZ-pin
float azimut = 0;               // unfiltered azimut
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
int angle_old = 0;
int rotCmd = 0;  // 0 = nichts, 1 = CCW (gegen den Uhrzeigersinn), 2 = CW (im Uhrzeigersinn)

// variables for serial comms
String Azimuth = "";
int Azimuth_tar = 0;
String serial_in;
String serial_out;

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
String linkStatusText();
const char *wifiStateLabel();
String wifiIpCurrent();
void applyLinkMode();
void applyAzimuthTarget(int compassDeg);
void stopAutorotate();
void restartBluetooth();
void wifiService();
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
void calStopAll();
void drawMainScreen();
void drawLinkStatus();
void readButtons();
void processButtonEvents();
void replyBoth(const String &msg);

void setup() {
  preferences.begin("RotorRemote", false);  // Beginne die Verwendung des Preferences-Speichers für die Anwendung "RotorRemote"
  GetStoredSetup();                         // Read all preference values out of the memory
  Serial.begin(115200);                     // Begin Serial communication on the cable interface

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
  initialEstimate = analogRead(pin_in_AZ);  // init kalman with first value

  spr.createSprite(320, 120);              // Erstelle das Sprite
  spr.setTextColor(TFT_WHITE, TFT_BLACK);  // Textfarbe festlegen
  spr.setTextDatum(MC_DATUM);              // Textausrichtung zentriert
  applyLinkMode();
  display_rotation_arrow();
  drawAngleScale(azimut);                  // Initiale Anzeige des Rotorwinkels
  drawLinkStatus();
}

void loop() {  //***************************************************************************************************************************
 
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
    glitchalarm();
  }

  if (currentMillis - prevMillis >= interv) {  // 100ms timer for display and stuck detection
    prevMillis = currentMillis;
    if (!menuOpen) {
      tft_update();
    } else {
      menuRefreshLive();
    }
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
          ledcWrite(LEDPin, 25);
          tft.setTextColor(TFT_RED, TFT_BLACK);
          tft.drawString("ROTOR STUCK!", 70, 215, 4);
          tft.setTextColor(TFT_WHITE, TFT_BLACK);
        }
      }
    }
  }

  processSerialInput();
  processBluetoothInput();
  wifiService();
}

void processSerialInput() {
  static char buffer[15];  // Puffer für den Befehl
  static int i = 0;        // Index für den Puffer

  // Überprüfe, ob Daten über Serial empfangen werden
  while (Serial.available() > 0) {
    char c = Serial.read();

    // Wenn ein CR-Zeichen empfangen wird, beende und verarbeite den Befehl
    if (c == '\r') {
      buffer[i] = '\0';  // Nullterminator am Ende hinzufügen
      //Serial.println("Received serial buffer: " + String(buffer));  // Debug-Ausgabe
      SerComm(buffer);  // Befehl verarbeiten
      i = 0;            // Pufferindex zurücksetzen für das nächste Kommando
      return;           // Verlasse die Funktion, um Mehrfachverarbeitung zu verhindern
    }

    // Überprüfung auf Pufferüberlauf
    if (i < sizeof(buffer) - 1) {
      buffer[i++] = c;
    }
  }
}

void processBluetoothInput() {
  if (!serialBtOn) return;

  static char buffer[15];  // Puffer für den Befehl
  static int i = 0;        // Index für den Puffer

  // Überprüfe, ob Daten über SerialBT empfangen werden
  while (SerialBT.available() > 0) {
    char c = SerialBT.read();

    // Wenn ein CR-Zeichen empfangen wird, beende und verarbeite den Befehl
    if (c == '\r') {
      buffer[i] = '\0';  // Nullterminator am Ende hinzufügen
      //Serial.println("Received bluetooth buffer: " + String(buffer));  // Debug-Ausgabe
      SerComm(buffer);  // Befehl verarbeiten
      i = 0;            // Pufferindex zurücksetzen für das nächste Kommando
      return;           // Verlasse die Funktion, um Mehrfachverarbeitung zu verhindern
    }

    // Überprüfung auf Pufferüberlauf
    if (i < sizeof(buffer) - 1) {
      buffer[i++] = c;
    }
  }
}

void CalcPosition() {  // Running average Version

  dig_AZ = analogRead(pin_in_AZ);
  samples.add(dig_AZ);
  long m = samples.getMedian();
  long a = samples.getAverage();
  dig_AZ_f = kalman.update((m + a) / 2);
  if (abs(dig_AZ - dig_AZ_f) >= 300) alarmOn = true;


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
  
  if ((!ledOn) && (alarmOn)) {
    ledcWrite(LEDPin, 25);  // Schalte die LED ein
    if (!menuOpen) {
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.drawString("SENSOR GLITCH", 60, 210, 4);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
    }
    startTime = millis();  // Speichere die aktuelle Zeit
    ledOn = true;          // Markiere die LED als eingeschaltet
  }

  // Überprüfe, ob die Zeit abgelaufen ist
  if (ledOn && (millis() - startTime >= blinkDuration)) {
    ledcWrite(LEDPin, 0);  // Schalte die LED aus
    if (!menuOpen) {
      tft.setTextColor(TFT_BLACK, TFT_BLACK);
      tft.drawString("SENSOR GLITCH", 60, 210, 4);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
    }
    ledOn = false;    // Markiere die LED als ausgeschaltet
    alarmOn = false;  // Setze den Alarmzustand zurück
  }
}

void drawAngleScale(float angle) { // Here we draw the big sprite with the angle scale
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
  int centerX = spr.width() / 2;
  int centerY = spr.height() / 2;
  float scaleWidth = 360 * 2;
  float pixelsPerDegree = 3;
  float offset = (angle * pixelsPerDegree);

  spr.setTextDatum(MC_DATUM);  // Ankerpunkt auf das Zentrum des Textes setzen

  // Farbverlauf von der Mitte nach außen
  for (int x = 0; x < spr.width(); x++) {
    float distance = abs(centerX - x);  // Abstand von der Mitte
    float ratio = distance / centerX;   // Verhältnis zur halben Breite

    // Lineare Interpolation zwischen centerColor und edgeColor
    uint8_t r = ((1 - ratio) * ((centerColor >> 11) & 0x1F) + ratio * ((edgeColor >> 11) & 0x1F));
    uint8_t g = ((1 - ratio) * ((centerColor >> 5) & 0x3F) + ratio * ((edgeColor >> 5) & 0x3F));
    uint8_t b = ((1 - ratio) * (centerColor & 0x1F) + ratio * (edgeColor & 0x1F));
    uint16_t gradientColor = (r << 11) | (g << 5) | b;

    spr.drawLine(x, 0, x, spr.height(), gradientColor);  // Zeichnet die Farbverlaufslinie
  }

  spr.drawLine(0, centerY, spr.width(), centerY, foregroundColor);  // Horizontale Linie zeichnen
  spr.drawRoundRect(0, 0, spr.width(), spr.height(), 4, TFT_WHITE);

  // Markierungen und Beschriftungen zeichnen
  for (int i = -540; i <= 540; i += 10) {
    int xPos = centerX + i * pixelsPerDegree - offset;

    int lineLength = 0;
    if (i % 30 == 0) {  // Markerlänge bestimmen
      lineLength = spr.height() * longLineRatio;
      spr.drawLine(xPos, centerY - lineLength, xPos, centerY + lineLength, foregroundColor);  // Vertikale Marker zeichnen
    } else {
      lineLength = spr.height() * shortLineRatio;
      spr.drawLine(xPos, centerY - lineLength, xPos, centerY + lineLength, foregroundColor);  // Vertikale Marker zeichnen
    }

    if (i % 30 == 0) {  // Beschriftung bei jedem 45. Grad
      int textHeight = spr.height() * textOffsetRatio;
      int displayAngle = (i + 360) % 360;
      spr.setTextColor(foregroundColor);
      spr.drawString(String(displayAngle), xPos + 2, centerY - textHeight, textSize);  // Position der Winkelbeschriftung
      String direction = "";                                                           // Himmelsrichtungen anzeigen
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
      if (direction != "") {
        spr.drawString(direction, xPos, centerY + textHeight, textSize);  // Position der Himmelsrichtung
      }
    }
  }

  spr.drawLine(centerX, spr.height() - 10, centerX, 10, pointerColor);  // Den roten vertikalen Zeiger zeichnen
  spr.pushSprite(0, 45);                                                // Sprite auf den Bildschirm übertragen
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
    if ((but_CCW == 1) && (but_CW == 0)) { rot_cmd = 1; }
    if ((but_CCW == 0) && (but_CW == 1)) { rot_cmd = 2; }
    DriveRotator(rot_cmd);
  } else {
    if ((but_CCW == 0) && (but_BRK == 1) && (but_CW == 0) && (b_autorotate == true)) {
      rot_cmd = 0;
      b_autorotate = false;
      DriveRotator(rot_cmd);
    }
  }
}

void DriveRotator(int command) {
  if (command_old != command) {
    command_old = command;

    ledcWrite(LEDPin, 0);
    msgStuck = false;
    if (!menuOpen) {
      tft.setTextColor(TFT_BLACK, TFT_BLACK);
      tft.drawString("ROTOR STUCK!", 70, 215, 4);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
    }
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

  if (debug) Serial.println("Received buffer: " + String(buffer));

  int overshootValue = 11;
  serial_in = "";
  Azimuth = "";
  String glitches = "";
  String filtervalue = "";
  bool B_properCommand = false;

  // looking for command "C" and reply the actual azimuth value
  if (strncmp(buffer, "C", 1) == 0) {
    serial_out = "+ " + String(azimut, 1);
    replyBoth(serial_out);
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
    overshootValue = buffer[1] - '0';  // Konvertiere das Zeichen zu einer Zahl (0-9)
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

  if (strncmp(buffer, "C", 1) == 0) {
    serial_out = "+ " + String(azimut, 1);
    replyBoth(serial_out);
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
    Serial.println("dig_AZ = " + String(dig_AZ) + " / dig_AZ_f = " + String(dig_AZ_f));
    return;
  }

  // looking for  <L> - Store the minimum rotor position
  if (strncmp(buffer, "L", 1) == 0) {
    preferences.putInt("az_min_digit", dig_AZ_f);
    Serial.println("Minimum position stored, value = " + String(dig_AZ_f, 0) + " digits");
    PrintStoredSetup();
    return;
  }

  // looking for  <H> - Store the maximum rotor position
  if (strncmp(buffer, "H", 1) == 0) {
    preferences.putInt("az_max_digit", dig_AZ_f);
    Serial.println("Maximum position stored, value = " + String(dig_AZ_f, 0) + " digits");
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
    Serial.println("**********************************************************************************************************");
    Serial.println("List of possible commands:");
    Serial.println("C    - Reports the current calculated rotator position in ° compass angle");
    Serial.println("S    - Stops the rotation");
    Serial.println("MXXX - Requests the controller to turn the rotator to XXX position in ° compass angle");
    Serial.println("D    - Reports the current values of the unfiltered and filtered angle in raw digits");
    Serial.println("L    - Stores the current raw angle as lowest reachable position of the rotator");
    Serial.println("H    - Stores the current raw angle as highest reachable position of the rotator");
    Serial.println("OX   - Stores the overshoot value at which the rotor stops rotating prior reaching the target, 3' default.");
    Serial.println("V    - Reports all currently stored setup values in EEPROM");
    Serial.println("**********************************************************************************************************");
    Serial.println("");
    return;
  }

  if (B_properCommand = false) Serial.println("Unknown command received - " + String(buffer) + " - Type '?' for help.");
}

void tft_update() {
  if (menuOpen) return;
  spr_angle.createSprite(320, 25);
  spr_angle.setTextDatum(TL_DATUM);  // Textausrichtung TL (TopLeft)
  spr_angle.setTextColor(TFT_WHITE, COLOR_BG);
  spr_angle.fillScreen(TFT_BLACK);
  String message = " Angle: " + String((int)azimut) + "'" + " - Speed: " + String((int)v_turn) + " ";
  int str_length = tft.textWidth(message) * 4;  // Breite für Schriftgröße 1 berechnen und mit 4 multiplizieren
  int x_start = 320 - str_length / 2;
  spr_angle.drawString(message, x_start, 0, 4);
  spr_angle.pushSprite(0, 180);
  drawAngleScale(azimut);
  drawLinkStatus();
}

void GetStoredSetup() {
  az_min_digit = preferences.getInt("az_min_digit", 200);
  az_max_digit = preferences.getInt("az_max_digit", 3800);
  a_overshoot = preferences.getInt("a_overshoot", 3);
  btName = preferences.getString("name", "RotorRemote_2");
  linkMode = preferences.getInt("link_mode", LINK_BT);
  if (linkMode < LINK_BT || linkMode > LINK_BOTH) linkMode = LINK_BT;
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
  Serial.println("*********************************************************************************************************");
  Serial.println("Stored setup values:");
  Serial.println("Maximum position value: " + String(az_max_digit) + " digits");
  Serial.println("Minimum position value: " + String(az_min_digit) + " digits");
  Serial.println("Rotor overshoot value : " + String(a_overshoot) + "'");
  Serial.println("Bluetooth Name        : " + btName);
  Serial.println("Link mode             : " + String(linkModeLabel()));
  Serial.println("WLAN SSID             : " + wifiSsid);
  Serial.println("WLAN IP mode          : " + String(ipMode ? "static" : "DHCP"));
  Serial.println("WLAN IP / GW / Mask   : " + ipLocal + " / " + ipGw + " / " + ipMask);
  Serial.println("WLAN DNS              : " + ipDns);
  Serial.println("rotctld port          : " + String(rotPort));
  Serial.println("*********************************************************************************************************");
  Serial.println("");
}

const char *linkModeLabel() {
  switch (linkMode) {
    case LINK_WIFI: return "WLAN";
    case LINK_BOTH: return "Beides";
    default: return "Bluetooth";
  }
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
  lastLinkStatus = "";
  if (spr_angle.width() != 320 || spr_angle.height() != 25) {
    spr_angle.deleteSprite();
    spr_angle.createSprite(320, 25);
  }
  tft.fillScreen(COLOR_BG);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawRoundRect(0, 0, 105, 35, 4, TFT_WHITE);
  tft.drawRoundRect(110, 0, 100, 35, 4, TFT_WHITE);
  tft.drawRoundRect(215, 0, 105, 35, 4, TFT_WHITE);
  display_rotation_arrow();
  tft_update();
}

void drawLinkStatus() {
  if (menuOpen || alarmOn || ledOn || msgStuck) return;
  String s = linkStatusText();
  if (s == lastLinkStatus) return;
  lastLinkStatus = s;
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextPadding(320);
  tft.drawString(s, 4, 224, 2);
  tft.setTextPadding(0);
}

void replyBoth(const String &msg) {
  Serial.println(msg);
  if (serialBtOn) SerialBT.println(msg);
}
