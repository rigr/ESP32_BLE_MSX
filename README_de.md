# ESP32 MSX Mouse - Version 004

Ein leistungsstarkes und optimiertes ESP32-basiertes Gerät zur Emulation einer Maus für MSX-Computersysteme. Diese Firmware ermöglicht die Verwendung einer Bluetooth-leistenden Maus als MSX-Maus-Eingabe per direktem GPIO-Interface.

## Features

- **Optimierte GPIO-Operationen**: Verwendung direkter Register-Zugriffe für maximale Geschwindigkeit
- **BLE-Unterstützung**: Verbindung mit Bluetooth-leistenden Mäusen über NimBLE
- **Zoom-Kontrolle**: Dynamische Zoomanpassung von 20% bis 200% (anpassbar)
- **Web-Interface**: Einfache Konfiguration über Web-Oberfläche
- **OTA-Firmware-Update**: Update der Firmware direkt über das Web-Interface
- **Thread-sichere Operationen**: Mutex-geschützte Operationen für stabile Performance
- **Serielle Schnittstelle**: Vollständige Konfiguration über seriellen Monitor

## Technische Spezifikationen

- **Microcontroller**: ESP32-WROOM-32D (30 Pins)
- **BLE**: NimBLE Version 2.1.0 by h2zero
- **GPIO-Optimierung**: Direkter Register-Zugriff für schnelle Kommunikation
- **MSX-Protokoll**: Optimiertes Strobe-Sync für vollständige Kompatibilität
- **Pins**: 14, 27, 26, 25, 33, 32, 13, 35, 2

## Pin-Belegung

```
D14 = MX0 (Datenstrecke)
D27 = MX1 (Datenstrecke)
D26 = MX2 (Datenstrecke)
D25 = MX3 (Datenstrecke)
D33 = MX4 (Linker Knopf)
D32 = MX5 (Rechter Knopf)
D13 = CS (Strobe-Eingang)
D35 = Scan-Trigger (low für manuelles Scannen)
D2  = LED (eingebaute Status-LED)
D0  = BOOT-Knopf (Verwaltung des Web-Interfaces)
```

## Installation

### Voraussetzungen

- ESP32-Entwicklungsboard (ESP32-WROOM-32D)
- Arduino IDE oder PlatformIO
- Bluetooth-leistende Maus
- MSX-Computersystem (empfohlen)

### Firmware-Installation

1. Öffne die Arduino IDE (oder PlatformIO)
2. Wählen Sie das ESP32-WROOM-32D Board unter Tools → Board
3. Laden Sie die gewünschte Firmware (z. B. ESP32_MSX_v004.ino) hoch
4. Starten Sie das Setup über serielle Schnittstelle mit 115200 Baud

## Funktionsweise

Das ESP32 simuliert die MSX-Maus-Schnittstelle durch direktes Manipulieren der GPIO-Pins. Die Eingabedaten von BLE-Mäusen werden in Echtzeit an MSX übertragen.

### Aktivierungsmethoden

- **BOOT-Knopf**: Halten Sie 3 Sekunden zum Starten des Web-Interfaces, 6 Sekunden zum Stoppen
- **Serielles Kommando**: "web" oder "webinterface" zum Toggle, "web on" zum Starten, "web off" zum Stoppen
- **Manuelle Scan-Trigger**: Pull D35 low zum Scannen nach Geräten

### Serielle Kommandos

| Kommando | Beschreibung |
|----------|-------------|
| `help` / `h` | Zeige alle verfügbaren Kommandos |
| `s` | Scan und verbinde die erste HID-Maus |
| `scan` | Scan Geräte-Liste (20s) |
| `list` | Zeige Geräte-Liste |
| `select <nr>` | Wähle Gerät aus der Liste aus |
| `d` | Trenne Maus |
| `scale` | Zeige aktuellen Zoom |
| `scale X` | Setze Zoom auf X (4-40, 20%-200%) |
| `web` | Schalte Web-Interface an/aus |
| `web on` | Starte Web-Interface |
| `web off` | Stoppe Web-Interface |
| `web status` | Zeige Web-Interface Status |
| `r` | Zurücksetzen ESP32 |

## Web-Interface

Das Web-Interface ermöglicht eine komfortable Fernkonfiguration über einen Webbrowser:

- **Verbindungsstatus**: Zeigt aktuelle Verbindungsinformationen
- **BLE-Geräte**: Scannen und Auswahl verfügbarer Geräte
- **Zoomsteuerung**: Dynamische Zoom-Anpassung (20%-200%)
- **Maus-Daten**: Live-Anzeige von X/Y-Bewegungen und Knopfzuständen
- **Firmware-Update**: OTA-Update direkt über Web-Oberfläche

Zugriff erfolgt über die IP-Adresse des ESP32 (z. B. 192.168.4.1), sobald das Web-Interface aktiv ist.

## Konfiguration

### Zoom-Faktor-Anpassung

Der Standard-Zoom-Faktor kann in der Firmware angepasst werden:

```cpp
volatile char currentScale = 15;  // Start: 100%
const char minScale = 4;          // Minimal: 20%
const char maxScale = 40;         // Maximal: 200%
```

Die Zoom-Funktion kann auch über Scrollrad der BLE-Maus dynamisch angepasst werden.

### BLE-Verbindung

Das System scannt automatisch nach HID-Maus-Geräten. Es wird die stärkste Verbindung auf Basis des RSSI-Werts bevorzugt.

## Entwicklungsstand

- **Version 0.04**: Ausführliche Kommentare, korrigierte OTA-Funktionalität, ohne NVS-Speicher

## Lizenz

Dieses Projekt ist unter der MIT-Lizenz lizenziert - siehe die LICENSE-Datei für Details.

## Mitwirkung

Beitragende sind herzlich eingeladen! Bitte senden Sie ein Pull-Request mit Ihren Verbesserungen oder melden Sie Probleme im GitHub-Issues-Tracker.

## Danksagung

- NimBLE-Bibliothek für BLE-Unterstützung
- Arduino für die ESP32-Unterstützung
- MSX-Community für die Protokoll-Definitionen

## Kontakt

Falls Sie Fragen haben oder Unterstützung benötigen:

- Erstellen Sie einen Issue auf GitHub
- Senden Sie eine E-Mail an den Maintainer
- Treten Sie der MSX-Community bei (wenn verfügbar)

---

*Dieses Projekt ist für den Einsatz auf MSX-Computersystemen konzipiert und wurde entwickelt, um die historische Computer-Emulation zu unterstützen.*