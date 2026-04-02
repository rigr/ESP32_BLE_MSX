# ESP32 MSX Maus

Ein ESP32-basiertes Gerät zur Emulation einer Maus für Roland S-750 Sampler und möglicherweise MSX-Computersysteme. Dieser Arduino-Sketch ermöglicht die Verwendung einer BLE-Maus mit einem ESP32 und  über die GPIO-Schnittstelle mit einem alten Sampler.

## Funktionen

- **GPIO-Operationen**: Verwendung direkter Register-Zugriffe für Geschwindigkeit
- **BLE-Unterstützung**: Verbindung mit BLE-Mäusen über NimBLE
- **Zoom-Kontrolle**: Dynamische Zoom-Anpassung von 20% bis 200% (anpassbar) über das Scrollrad der Maus
- **Web-Interface**: Konfiguration über Web-Interface
- **OTA-Firmware-Update**: Update der Firmware direkt über das Web-Interface
- **Thread-sichere Operationen**: Mutex-geschützte Operationen für Stabilität
- **Serielle Schnittstelle**: Vollständige Konfiguration über seriellen Monitor

## Technische Spezifikationen

- **Mikrocontroller**: ESP32-WROOM-32D (30 Pins), Verwenden Sie die esp32-Board-Konfiguration 3.0.0 von espressif systems
- **BLE**: NimBLE Version 2.1.0 von h2zero
- **MSX-Protokoll**: Strobe-Sync für Kompatibilität
- **Pins**: 14, 27, 26, 25 für Datenleitungen, 33, 32 für Mausknöpfe, 13 für das Strobe-Signal, 35 für manuelles Scannen und Verbinden, 2 für die Onboard-LED
- Alle Pins sind auf derselben Seite des ESP32-Boards und die Verbindung ist dadurch einfach herstellbar.

## Pin-Belegung

```
D14 = MX0 (Datenbus)
D27 = MX1 (Datenbus)
D26 = MX2 (Datenbus)
D25 = MX3 (Datenbus)
D33 = MX4 (Linker Knopf)
D32 = MX5 (Rechter Knopf)
D13 = CS (Strobe-Eingang)
D35 = Scan-Trigger (low für manuelles Scannen)
D2  = LED (eingebaute Status-LED)
D0  = BOOT-Knopf (Web-Interface-Verwaltung)
```

## Installation

### Voraussetzungen

- ESP32-Entwicklungsboard (ESP32-WROOM-32D) - Version 3.0.0 
- Arduino IDE
- Bluetooth-Maus (ich verwende eine billige chinesische Maus, die sich als "BT5.2" meldet)
- Roland S-750 Sampler und wahrscheinlich andere sowie MSX-Computer

### Firmware-Installation

1. Arduino IDE öffnen
2. ESP32-WROOM-32D-Board unter Tools → Board wählen und darauf achten, Version 3.0.0 der Board-Definition zu verwenden
3. NimBLE 2.1.0 von h2zero
4. Die gewünschte [Firmware](./source/ESP32_MSX.ino) hochladen (zu finden im source-Ordner)
5. Setup über serielle Schnittstelle mit 115200 Baud starten

## Betrieb

Das ESP32 simuliert die MSX-Maus-Schnittstelle durch direktes Manipulieren der GPIO-Pins. Die Eingabedaten von BLE-Mäusen werden in Echtzeit an den Sampler übertragen.

### Aktivierungsmethoden

- **BOOT-Knopf**: 3 Sekunden gedrückt halten, um einen Zugangspunkt (MSX_MOUSE, Passwort 12345678) zu starten, der ein Web-Interface (192.168.4.1) bietet, 6 Sekunden zum Stoppen
- **Serielles Kommando**: "web" oder "webinterface" zum Toggle, "web on" zum Starten, "web off" zum Stoppen
- **Manueller Scan-Trigger**: D35 auf low ziehen, um nach Geräten zu scannen

### Serielle Kommandos

| Kommando | Beschreibung |
|---------|-------------|
| `help` / `h` | Zeige alle verfügbaren Kommandos |
| `s` | Scan und Verbindung der ersten HID-Maus |
| `scan` | Scan Geräte-Liste (20s) |
| `list` | Zeige Geräte-Liste |
| `select <nr>` | Wähle Gerät aus Liste |
| `d` | Trenne Maus |
| `scale` | Zeige aktuellen Zoom |
| `scale X` | Setze Zoom auf X (4-40, 20%-200%) |
| `web` | Toggle Web-Interface an/aus |
| `web on` | Starte Web-Interface |
| `web off` | Stoppe Web-Interface |
| `web status` | Zeige Web-Interface Status |
| `r` | Zurücksetzen ESP32 |

## Web-Interface

Das Web-Interface ermöglicht eine bequeme Fernkonfiguration über einen Webbrowser. Es kann durch Drücken des Boot-Knopfes für drei Sekunden oder über die serielle Schnittstelle aktiviert werden. Der Zugangspunkt heißt MSX_MOUSE, das Passwort ist 12345678, das Webinterface ist auf Seite 182.168.4.1

- **Verbindungsstatus**: Zeigt aktuelle Verbindungsinformationen
- **BLE-Geräte**: Scannen und Auswahl verfügbarer Geräte
- **Zoom-Kontrolle**: Dynamische Zoom-Anpassung (20%-200%)
- **Maus-Daten**: Live-Anzeige von X/Y-Bewegungen und Knopfzuständen
- **Firmware-Update**: OTA-Update direkt über Web-Interface

Zugriff ist über die IP-Adresse des ESP32 (z.B. 192.168.4.1) verfügbar, wenn das Web-Interface aktiv ist.

## Konfiguration

### Zoom-Faktor-Anpassung

Der Standard-Zoom-Faktor kann in der Firmware angepasst werden:

```cpp
volatile char currentScale = 15;  // Start: 100%
const char minScale = 4;          // Minimal: 20%
const char maxScale = 40;         // Maximal: 200%
```

Die Zoom-Funktion kann auch dynamisch über das Scrollrad der BLE-Maus angepasst werden.

### BLE-Verbindung

Das System scannt automatisch nach HID-Mäusen. Es bevorzugt die stärkste Verbindung basierend auf dem RSSI-Wert. Ich schalte den Sampler ein, dann die Maus, drücke beide Knöpfe (das bringt sie in den Pairing-Modus) und bewege sie, nach ein paar Sekunden ist das ESP32 mit der Maus verbunden.
 
## Entwicklungsstand

- **Version 0.04**: Funktioniert endlich.

## Lizenz

Dieses Projekt ist unter der MIT-Lizenz lizenziert - siehe die LICENSE-Datei für Details.

## Mitwirkung

Grundlegende Beiträge kamen von NYYRIKKY und Peter Ullrich - danke an beide!

Weitere Mitwirkende sind willkommen! Bitte senden Sie einen Pull-Request mit Verbesserungen oder melden Sie Probleme im GitHub-Tracker. Nutzen und kopieren Sie es.

## Danksagung

- NimBLE-Bibliothek für BLE-Unterstützung
- Arduino für ESP32-Unterstützung
- MSX-Community für Protokoll-Definitionen


---

*Dieses Projekt ist für die Verwendung auf Roland S-750 Samplern konzipiert und wurde entwickelt, um die MU-1-Maus zu ersetzen.*

*Hier noch zum technischen Hintergrund des strobe Signals des Samplers: [background.md](./background.md)*
