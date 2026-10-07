# Rotor Remote

ESP32-Fernbedienung für einen CDE-Antennenrotor. Das Display zeigt den Kompasswinkel, die Tasten drehen von Hand, und hamlib spricht den Rotor über WLAN als rotctld an. Dieselbe Schnittstelle gibt es über USB-Serial und Bluetooth.

Firmware-Version steht im Systemmenü (`FW_VERSION` in `RotorTypes.h`).

## Hardware

ESP32 Dev Module, 4 MB Flash, Partition `min_spiffs` (zwei App-Partitionen für OTA). Display: ST7789, 320×240, Landscape. Azimut kommt von einem Drahtpoti am ADC.

Tasten sind aktiv low und haben einen internen Pull-up. Relaisausgänge sind high-aktiv. Die Bremse zieht zuerst an, der Motor folgt nach 250 ms. Nach dem Stopp bleibt die Bremse noch 1 s angezogen.

### Pinout

| GPIO | Richtung | Funktion |
| --- | --- | --- |
| 35 | Eingang | Azimut-Poti, ADC |
| 12 | Eingang | Taste CCW |
| 13 | Eingang | Taste BRK |
| 14 | Eingang | Taste CW |
| 25 | Ausgang | Relais CCW |
| 26 | Ausgang | Relais Bremse |
| 27 | Ausgang | Relais CW |
| 32 | Ausgang | Relais AUX, nur als Ausgang gesetzt |
| 33 | PWM | Alarm-LED, 5 kHz |
| 4 | SPI | TFT MOSI |
| 18 | SPI | TFT SCLK |
| 19 | SPI | TFT MISO |
| 15 | Ausgang | TFT CS |
| 2 | Ausgang | TFT DC |
| 23 | Ausgang | TFT RST |

Die Display-Pins stehen in `TFT_eSPI/User_Setup.h` (ESP32-Block). USB-Serial läuft mit 115200 Baud. Das ist die Kommandoschnittstelle, nicht die Flash-Geschwindigkeit.

## Bedienung am Gerät

Auf der Hauptseite drehen **CCW** und **CW**, solange die Taste gehalten wird. **BRK** drei Sekunden öffnet das Menü. Im Menü wählen CCW/CW die Zeile, ein kurzer Druck auf BRK bestätigt, drei Sekunden gehen eine Ebene zurück.

Das Menü:

- **Link** schaltet Bluetooth, WLAN oder beides.
- **Wi-Fi** setzt SSID, Passwort, DHCP oder feste IP, Gateway, Maske, DNS und den rotctld-Port. Speichern verbindet neu.
- **Bluetooth** schaltet die Schnittstelle und den Gerätenamen.
- **Calibration** zeigt Rohwert, Median und Geschwindigkeit, fährt von Hand an die Anschläge, speichert Max CCW und MAX CW, setzt den Overshoot und startet die Auto-Kalibrierung.
- **System** zeigt Version und OTA-Adresse, stellt die Median-Länge ein (3 bis 255, Startwert 100), schaltet Debug und startet neu.

Gespeichert wird in den Preferences unter dem Namen `RotorRemote`.

## Fahren und Anschläge

CW erhöht die ADC-Digits, CCW senkt sie. `az_min_digit` ist Max CCW, `az_max_digit` ist MAX CW.

Handfahrt und Autorotation bleiben 30 Digits vor diesen gespeicherten Enden stehen. Die Kalibrierung ist davon ausgenommen, sonst erreicht sie den mechanischen Anschlag nicht.

Die Auto-Kalibrierung sucht einen Anschlag nur über die Geschwindigkeit: 3 s unter 3 °/s gelten als Stopp. Danach fährt sie 28 Digits zurück und speichert den Punkt. Zuerst wird das nähere gespeicherte Ende angefahren. Ein Start schon am Anschlag wird erkannt, weil die Geschwindigkeit nie über 3 °/s kommt.

Der Overshoot (0 bis 9 °) lässt die Autorotation vor dem Ziel stehen.

## Winkel und Filter

Der ADC wird jede Millisekunde gelesen.

1. Ein laufender Median wirft kurze Spikes des Drahtpotis weg. Die Länge ist im Systemmenü einstellbar und bleibt gespeichert. Eine gerade Länge mittelt die beiden mittleren Samples.
2. Ein Kalman-Filter glättet daraus den angezeigten Winkel. Im Stand, solange die Bremse offen ist, ist dieser Filter zehnmal stärker. Während der Fahrt bleibt er leicht, damit das Ziel nicht zu spät gemeldet wird.
3. Die Geschwindigkeitsanzeige ist ein Mittel über 1500 Samples, also etwa 1,5 s. Steht der Rotor, zeigt sie 0.

Der Kompasswinkel auf dem Display und über rotctld ist der absolute Winkel plus 180 °, um 360 ° gefaltet. Kalibrierte Endpunkte und der 30-Digit-Abstand nutzen den Median, nicht den Kalman-Wert.

Ein Glitch (Rohwert und Median liegen 300 Digits auseinander) löst LED und Footer nur während der Fahrt aus. Im Stand passiert das nicht.

## rotctld

Bei verbundenem WLAN hört der Controller auf TCP-Port 4533, sofern nichts anderes gespeichert ist. Ein neuer Client ersetzt den alten. Es gibt immer nur einen.

| Befehl | Wirkung |
| --- | --- |
| `p` oder `get_pos` | Azimut und Elevation `0.0` |
| `P <az> <el>` oder `set_pos` | Fährt den Kompasswinkel an |
| `S` oder `stop` | Stoppt |
| `q` oder `quit` | Schließt den Client |
| `dump_state` | Azimut 0–360, Elevation 0–180 |
| `_` oder `get_info` | `RotorRemote` |

Ein Winkeltest vom PC steht in `tools/rotor_angle_test.py`. Er fährt ein Stück, wartet bis der Rotor steht und gibt den Fehler aus:

```text
py -3 tools\rotor_angle_test.py --host 192.168.1.77 --step 40
py -3 tools\rotor_angle_test.py --targets 0,90,180,270
```

## USB und Bluetooth

Dieselben Kurzbefehle gehen an USB-Serial (115200) und an Bluetooth. Die Antwort kommt auf beiden Wegen zurück, außer wo nur Serial genannt ist.

| Befehl | Wirkung |
| --- | --- |
| `C` | Aktueller Kompasswinkel |
| `Mxxx` | Zielwinkel, drei Ziffern |
| `S` | Stoppt die Autorotation |
| `D` | Rohwert, Median und Kalman-Wert |
| `L` | Speichert die aktuelle Position als Minimum |
| `H` | Speichert die aktuelle Position als Maximum |
| `Ox` | Overshoot 0–9 |
| `Nname` | Bluetooth-Name, max. 15 Zeichen |
| `V` | Gespeicherte Werte |
| `?` | Hilfe |

## WLAN-Update

Im Browser `http://<ip>/` und dann **Update**. Hochladen nur `firmware/RotorRemote_ota.bin`. Die 4-MB-Datei `RotorRemote.bin` ist das USB-Image und bricht im Browser ab.

Sobald der Upload oder ArduinoOTA startet, werden rotctld und Bluetooth getrennt. Bis zum Neustart nimmt der Controller keine neuen Clients dieser Art an.

Ein USB-Flash, falls nötig, nutzt den ESP32 Dev Module mit `PartitionScheme=min_spiffs` und 921600 Baud. Port und Board stehen in `sketch.yaml`.

## Bauen

Arduino CLI, Core `esp32:esp32` 3.3.7. Bibliotheken liegen unter `../libraries`, unter anderem TFT_eSPI und RunningMedian.

```text
arduino-cli compile --libraries ../libraries --output-dir firmware .
```

Die App-Datei daraus nach `firmware/RotorRemote_ota.bin` kopieren. Das ist die Datei für das WLAN-Update.
