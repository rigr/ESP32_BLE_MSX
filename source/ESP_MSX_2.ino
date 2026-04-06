// =================================================================
// ESP32 MSX MOUSE   ri 5.4.26   ---
// Support für mehr Mäuse.
//
// - Festes Zoom-Faktor-Verhalten unter 15%
// - OTA-Funktionalität für Firmware Updates
// - FIXED: Endgültige korrekte Maus-Tastenerkennung für 7-Byte-Pakete
//
// Board: ESP32-WROOM-32D (30 pins).  Board defintion 3.00 von espressif
// NimBLE Version: 2.1.0 by h2zero
//
// Features:
// - GPIO operations using direct register access
// - BLE device list and selection
// - Zoom control (20%-200%) with NO NVS storage (immediate save)
// - Web Interface Start/Stop via BOOT button
// - OTA firmware update capability
// - Thread-safe operations with proper mutex protection
// - HID Report ID support for flexible mouse report parsing
//
// PINS:
// - D14 = MX0, D27 = MX1, D26 = MX2, D25 = MX3 (Data bits)
// - D33 = Left Button (MX4) - OUTPUT
// - D32 = Right Button (MX5) - OUTPUT
// - D13 = CS Strobe Input
// - D35 = Manual scan trigger (pull low to scan)
// - D2  = LED (built-in)
// - D0  = BOOT button (for diagnostics toggle and web interface start/stop)
//
// USAGE:
// - BOOT button: Hold 3s to START web interface, 6s to STOP web interface
// - Serial command: "web" or "webinterface" to toggle, "web on" to start, "web off" to stop
// - Serial command: "help" or "h" for all available commands
// =================================================================

#include <NimBLEDevice.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <HTTPUpdateServer.h>
#include "driver/gpio.h"
#include "soc/gpio_struct.h"
#include "soc/gpio_reg.h"
#include <vector>
#include "freertos/semphr.h"

// =================================================================
// BLE Konfiguration
// =================================================================
NimBLEClient* client = nullptr;
bool connected = false;
bool isScanning = false;
unsigned long lastAlive = 0;
int currentRSSI = -999;
std::string mouseName = "";
std::string mouseAddr = "";

// Struktur für Geräteinformationen
struct DeviceInfo {
  std::string name;
  NimBLEAddress addr;
  int rssi;
};
std::vector<DeviceInfo> deviceList;
int selectedDevice = -1;
bool selectingDevice = false;
unsigned long selectionStart = 0;

// =================================================================
// HID Parser Strukturen
// =================================================================
struct HIDField {
  uint16_t usage;
  uint8_t reportID;
  uint16_t bitOffset;
  uint8_t bitSize;
  bool isSigned;
  int32_t rawValue;
  bool valueChanged;
};

struct HIDMouseFormat {
  uint8_t reportID = 0;
  HIDField x, y, wheel;
  HIDField leftButton;
  HIDField rightButton;
  bool valid = false;
  bool hasExplicitButtons = false;
  bool hasLeftButton = false;
  bool hasRightButton = false;
  bool usesStandardMouseFormat = false;
};

HIDMouseFormat hidFmt;

// =================================================================
// ESP32 serial / MAC
// =================================================================
uint64_t chipid;
char ssid[23];

// =================================================================
// WiFi/Web Konfiguration
// =================================================================
const char* ap_ssid = "MSX_MOUSE";
const char* ap_password = "12345678";
WebServer server(80);
HTTPUpdateServer httpUpdater;
bool webServerActive = false;

// =================================================================
// Pin Definitionen
// =================================================================
const uint8_t MX0_PIN = 14;
const uint8_t MX1_PIN = 27;
const uint8_t MX2_PIN = 26;
const uint8_t MX3_PIN = 25;
const uint8_t MX4_PIN = 33;   // Linker Mausbutton - OUTPUT
const uint8_t MX5_PIN = 32;   // Rechter Mausbutton - OUTPUT
const uint8_t CS_PIN = 13;    // Strobe Eingang
const uint8_t MAN_SCAN = 35;  // Manuelles Scan-Trigger (LOW = scan)
const uint8_t LED_PIN = 2;    // Eingebaute LED
const uint8_t BOOT_PIN = 0;   // BOOT button (GPIO0)  - Angabe bei board definition 3.00 von espressif notwendig

// Bitmasken für direkten GPIO-Zugriff
#define B0 (1UL << MX0_PIN)
#define B1 (1UL << MX1_PIN)
#define B2 (1UL << MX2_PIN)
#define B3 (1UL << MX3_PIN)

// =================================================================
// Globale Variablen
// =================================================================
TaskHandle_t msxTaskHandle = nullptr;
volatile bool msxEnabled = true;

// Maus-Koordinaten (geteilte Variablen für Dual-Core)
volatile int16_t lastX = 0, lastY = 0;
volatile bool leftBtn = false, rightBtn = false;
volatile bool lastLeftBtn = false, lastRightBtn = false;  // Tasten losgelassen?
unsigned long lastMouseUpdate = 0;

// Zoom-Kontrollvariablen (20%-200% Bereich)
volatile int8_t wheelDelta = 0;
volatile char currentScale = 15;  // Start: 100% (früher 20)
const char minScale = 4;          // Minimal: 20%
const char maxScale = 40;         // Maximal: 200%
bool scaleChanged = false;        // Für Web-Update


// Mausbuttons lange gedrückt?
unsigned long leftButtonPressTime = 0;
unsigned long rightButtonPressTime = 0;
unsigned long bootButtonPressTime = 0;
bool leftButtonPressed = false;
bool rightButtonPressed = false;
bool bootButtonPressed = false;
bool lastBootButtonState = false;

// Zeit-Schwelle für verschiedene Aktionen
const unsigned long WEB_START_BOOT_THRESHOLD = 3000;  // 3 Sekunden für Web-Start (Boot-Knopf)
const unsigned long WEB_STOP_BOOT_THRESHOLD = 6000;   // 6 Sekunden für Web-Stop (Boot-Knopf)
const unsigned long DEBOUNCE_DELAY = 300;             // 300ms Debounce für Toggle vs Langdruck

// Semaphore für Thread-Sicherheit
SemaphoreHandle_t scaleMutex;

// =================================================================
// Funktions-Prototypen
// =================================================================
void notifyCB(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool isNotify);
bool connectTo(const NimBLEAdvertisedDevice* dev);
void scanAndConnect();
void scanDevices();
void printDeviceList();
void connectSelectedDevice();
void disconnectMouse();
void setupWebServer();
void startWebServer();
void stopWebServer();
void msxProtocolTask(void* parameter);
void sendMSX(int8_t c);
void bootButtonISR();
void updateLED();
void checkLongButtonPresses();
void handleWebCommand(String cmd);
void handleSerialCommand(String cmd);

// ------------------------------------------------------
int32_t extractBits(uint8_t* data, int bitOffset, int bitSize, bool isSigned);
void parseHIDReport(uint8_t* data, size_t len);
void interpretMouseData(uint8_t* data, size_t len, int16_t& x, int16_t& y, int8_t& wheel, bool& leftButton, bool& rightButton);
// ------------------------------------------------------

// =================================================================
// GPIO FUNKTIONEN - Direkter Register-Zugriff
// =================================================================
// Hilfsfunktion: Pin auf LOW setzen und als Ausgang aktivieren
inline void lineLow(uint32_t mask) {
  GPIO.out_w1tc = mask;     // Ausgang LOW
  GPIO.enable_w1ts = mask;  // Ausgang aktivieren
}
// Hilfsfunktion: Pin als Eingang (hochohmig) schalten
inline void lineRelease(uint32_t mask) {
  GPIO.enable_w1tc = mask;  // Eingang (hochohmig)
}
// Bit senden: 0 = LOW, 1 = hochohmig
inline void sendBit(uint32_t mask, bool bit) {
  if (bit)
    lineRelease(mask);  // logisch 1
  else
    lineLow(mask);  // logisch 0
}
// Nibble (4 Bits) gleichzeitig senden
void sendNibble(uint8_t n) {
  uint32_t lowMask = 0;
  uint32_t relMask = 0;

  if (n & 8) relMask |= B3;
  else lowMask |= B3;
  if (n & 4) relMask |= B2;
  else lowMask |= B2;
  if (n & 2) relMask |= B1;
  else lowMask |= B1;
  if (n & 1) relMask |= B0;
  else lowMask |= B0;

  GPIO.out_w1tc = lowMask;
  GPIO.enable_w1ts = lowMask;

  GPIO.enable_w1tc = relMask;
}

// =================================================================
// BOOT BUTTON ISR - Verwaltung von Web-Interface und BLE-Maus-Auswahl
// =================================================================
void IRAM_ATTR bootButtonISR() {
  // Aktuellen Button-Zustand updaten
  bool currentButtonState = !digitalRead(BOOT_PIN);  // Active low

  // Nur verarbeiten, wenn sich der Zustand geändert hat
  if (currentButtonState != lastBootButtonState) {
    lastBootButtonState = currentButtonState;

    if (currentButtonState) {
      // Tastendruck
      bootButtonPressTime = millis();
      bootButtonPressed = true;
    } else {
      // Tasten-Release
      unsigned long pressDuration = millis() - bootButtonPressTime;
      bootButtonPressed = false;

      // Prüfen, ob es ein kurzer Tastendruck für BLE-Maus-Auswahl war
      if (pressDuration < DEBOUNCE_DELAY && !webServerActive && !isScanning && !connected) {
        scanDevices();
      }
    }
  }
}

// =================================================================
// Erkenne langes Drücken von boot
// =================================================================
void checkLongButtonPresses() {
  unsigned long currentTime = millis();

  // BOOT-Button prüfen für Web-Interface (zusätzlich zum ISR)
  if (bootButtonPressed) {
    unsigned long pressDuration = currentTime - bootButtonPressTime;

    if (pressDuration >= WEB_STOP_BOOT_THRESHOLD) {
      // Langer Boot-Button (6s): Web-Interface stoppen
      if (webServerActive) {
        stopWebServer();
      }
      bootButtonPressed = false;  // Mehrfach-Aktionen verhindern
    } else if (pressDuration >= WEB_START_BOOT_THRESHOLD && pressDuration < WEB_STOP_BOOT_THRESHOLD) {
      // Boot-Button (3s): Web-Interface starten
      if (!webServerActive) {
        startWebServer();
      }
      bootButtonPressed = false;  // Mehrfach-Aktionen verhindern
    } else if (pressDuration < DEBOUNCE_DELAY && !webServerActive && !isScanning && !connected) {
      // Kurzer Boot-Button Druck: BLE-Maus-Scan initiieren, wenn nicht beim Scan
      if (!isScanning) {
        scanDevices();
      }
      bootButtonPressed = false;  // Mehrfach-Aktionen verhindern
    }
  }
}

// =================================================================
// LED Update Funktion
// =================================================================
void updateLED() {
  static unsigned long lastBlink = 0;
  static bool state = false;
  int interval = 0;

  if (webServerActive) {
    interval = 200;  // Schnelles Blinken wenn Web aktiv
  } else if (isScanning) {
    interval = 100;  // 5Hz
  } else if (selectingDevice) {
    interval = 250;  // 2Hz
  } else if (connected) {
    digitalWrite(LED_PIN, HIGH);
    return;
  } else {
    digitalWrite(LED_PIN, LOW);
    return;
  }

  if (millis() - lastBlink > interval) {
    lastBlink = millis();
    state = !state;
    digitalWrite(LED_PIN, state);
  }
}

// =================================================================
// Web Server Management
// =================================================================

void startWebServer() {
  if (webServerActive) return;

  // Setup WiFi AccessPoint
  WiFi.softAP(ap_ssid, ap_password);
  IPAddress IP = WiFi.softAPIP();
  Serial.print("MSX WiFi AP ready: ");
  Serial.println(IP);

  // Setup web server
  setupWebServer();

  // Setup OTA Update
  httpUpdater.setup(&server);
  Serial.println("OTA Update ready");

  server.begin();
  webServerActive = true;

  // LED-Rückmeldung (3 kurze Blinker)
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(100);
    digitalWrite(LED_PIN, LOW);
    delay(100);
  }

  Serial.println("Web server started");
}

void stopWebServer() {
  if (!webServerActive) return;

  server.stop();
  WiFi.softAPdisconnect(true);
  webServerActive = false;

  // LED-Rückmeldung (2 lange Blinker)
  for (int i = 0; i < 2; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(500);
    digitalWrite(LED_PIN, LOW);
    delay(500);
  }

  Serial.println("Web server stopped");
}

// =================================================================
// SCAN FUNKTIONEN - Geräte-Liste-Management
// =================================================================

// Scant nach BLE-Geräten (20 Sekunden)
void scanDevices() {
  if (isScanning || connected) return;

  Serial.println("Starte 20s BLE Scan...");
  deviceList.clear();
  selectedDevice = -1;
  selectingDevice = false;

  isScanning = true;
  updateLED();

  NimBLEScan* scan = NimBLEDevice::getScan();

  // Blockierender Scan - warten auf Abschluss
  bool scanSuccess = scan->start(20, true);

  // Ergebnisse nach Abschluss des Scans erhalten
  NimBLEScanResults results = scan->getResults();

  Serial.print("Scan abgeschlossen. ");
  Serial.print(results.getCount());
  Serial.println(" Geräte gefunden.");

  // Geräte aus Ergebnissen sammeln
  for (int i = 0; i < results.getCount(); i++) {
    const NimBLEAdvertisedDevice* dev = results.getDevice(i);
    DeviceInfo info;
    info.name = dev->getName();
    info.addr = dev->getAddress();
    info.rssi = dev->getRSSI();
    deviceList.push_back(info);
  }

  // Nach RSSI absteigend sortieren (stärkstes Signal zuerst)
  std::sort(deviceList.begin(), deviceList.end(),
            [](const DeviceInfo& a, const DeviceInfo& b) {
              return a.rssi > b.rssi;
            });

  if (!deviceList.empty()) {
    selectedDevice = 0;
    selectingDevice = true;
    selectionStart = millis();
    printDeviceList();
  } else {
    Serial.println("Keine BLE-Geräte gefunden.");
    selectingDevice = false;
  }

  isScanning = false;
  updateLED();
}

// Geräte-Liste an serielle Schnittstelle ausgeben
void printDeviceList() {
  Serial.println("Verfügbare BLE-Geräte:");
  for (int i = 0; i < deviceList.size(); i++) {
    Serial.print(i);
    Serial.print(": ");
    Serial.print(deviceList[i].name.c_str());
    Serial.print("  RSSI:");
    Serial.println(deviceList[i].rssi);
  }
  Serial.println("Verwende Kommando: select <nr> oder Web-Interface");
}

// Verbindung zum ausgewählten Gerät herstellen
void connectSelectedDevice() {
  if (selectedDevice < 0 || selectedDevice >= (int)deviceList.size()) {
    Serial.println("Kein gültiges Gerät ausgewählt");
    return;
  }

  const NimBLEAddress& addr = deviceList[selectedDevice].addr;
  Serial.print("Verbinde mit Gerät: ");
  Serial.print(deviceList[selectedDevice].name.c_str());
  Serial.print(" (");
  Serial.print(addr.toString().c_str());
  Serial.println(")");

  // Versuchen, Gerät in Scan-Ergebnissen zu finden
  NimBLEScan* scan = NimBLEDevice::getScan();
  const NimBLEAdvertisedDevice* dev = scan->getResults().getDevice(addr);

  if (dev != nullptr) {
    // Gerät in Scan-Ergebnissen gefunden - direkt verbinden
    connectTo(dev);
  } else {
    // Gerät nicht in aktuellen Scan-Ergebnissen - nochmals kurz scannen
    Serial.println("Gerät nicht in aktuellen Scan-Ergebnissen - starte kurzen Scan...");

    isScanning = true;
    updateLED();

    // Starte einen kurzen Scan, um das spezifische Gerät zu finden
    scan->start(5, false);

    // Warte auf Abschluss des Scans
    delay(5000);

    // Versuche, das Gerät nach neuem Scan erneut zu erhalten
    const NimBLEAdvertisedDevice* dev2 = scan->getResults().getDevice(addr);
    if (dev2 != nullptr) {
      connectTo(dev2);
    } else {
      Serial.println("Konnte Gerät nicht anhand der Adresse finden");
      Serial.println("Bitte führe 'scan' Kommando aus und versuche erneut");
    }

    isScanning = false;
    updateLED();
  }

  selectingDevice = false;
}

// =====================================================================
// MSX PROTOCOL - DUAL CORE TASK (Core 1) - ANTI-DRIFT MIT DIREKTEM GPIO
// =====================================================================

void msxProtocolTask(void* parameter) {
  // Initialisiere Pins als INPUT by default (high impedance)
  pinMode(MX0_PIN, INPUT);
  pinMode(MX1_PIN, INPUT);
  pinMode(MX2_PIN, INPUT);
  pinMode(MX3_PIN, INPUT);
  pinMode(CS_PIN, INPUT_PULLUP);

  // Setze Output Latches auf LOW vor INPUT-Modus
  digitalWrite(MX0_PIN, LOW);
  digitalWrite(MX1_PIN, LOW);
  digitalWrite(MX2_PIN, LOW);
  digitalWrite(MX3_PIN, LOW);

  // Setup OUTPUT-Pins für Maus-Buttons
  pinMode(MX4_PIN, OUTPUT);     // Linker Button
  pinMode(MX5_PIN, OUTPUT);     // Rechter Button
  digitalWrite(MX4_PIN, HIGH);  // Initially high (nicht gedrückt)
  digitalWrite(MX5_PIN, HIGH);  // Initially high (nicht gedrückt)

  // MSX-Protokoll Variablen
  static int16_t mx = 0, my = 0;
  static unsigned long lastSend = 0;

  Serial.println("MSX Protokoll Task gestartet auf Core 1");
  Serial.println("Pin-Konfiguration:");
  Serial.println("MX0=14, MX1=27, MX2=26, MX3=25, MX4=33(L), MX5=32(R), CS=13, SCAN=35, LED=2");
  Serial.println(" ");
  Serial.print("Initialer Zoom-Faktor: ");
  Serial.print((int)currentScale);
  Serial.print(" (");
  Serial.print((int)(20.0 / currentScale * 100));
  Serial.println("%)");
  Serial.println("Zoom-Bereich: 4-40 (20%-200%)");

  while (1) {
    if (!msxEnabled) {
      delay(10);
      continue;
    }

    // Mausbewegung zusammenführen
    if (scaleMutex != NULL) {
      xSemaphoreTake(scaleMutex, portMAX_DELAY);
    }
    int16_t currentX = lastX;
    int16_t currentY = lastY;
    bool currentLeftBtn = leftBtn;
    bool currentRightBtn = rightBtn;
    lastX = 0;
    lastY = 0;
    if (scaleMutex != NULL) {
      xSemaphoreGive(scaleMutex);
    }

    // Bewegung hinzufügen
    mx += (currentX * -1);  // X invertiert
    my += (currentY * -1);  // Y invertiert

    // Auf CS HIGH warten → MSX ist bereit für nächsten Transfer
    while (digitalRead(CS_PIN) == LOW) {
      delayMicroseconds(20);
      yield();  // anderen Tasks Zeit lassen
    }

    // CS ist jetzt HIGH → bereite hohe Nibbles vor

    // Skaliere Bewegung gemäß aktuellem Zoom-Faktor
    char x = (char)(mx * currentScale / 15);  // geändert vom 20 auf 15
    char y = (char)(my * currentScale / 15);  // geändert vom 20 auf 15

    // Sende X (zwei Nibbles)
    sendMSX(x);

    // Auf nächsten Strobe-Zyklus warten (CS sollte LOW dann HIGH werden)
    unsigned long timeout = millis() + 20;
    while (digitalRead(CS_PIN) == HIGH && millis() < timeout) {
      delayMicroseconds(20);
    }
    if (millis() >= timeout) continue;  // timeout → überspringe Y

    // Sende Y
    sendMSX(y);

    // Nach vollständigem X+Y Paar → Akkumulator zurücksetzen
    mx = 0;
    my = 0;

    // Behandle Mausbuttons - setze OUTPUT-Pins LOW wenn gedrückt, HIGH wenn losgelassen
    if (currentLeftBtn) {
      digitalWrite(MX4_PIN, LOW);  // Pull LOW wenn gedrückt
    } else {
      digitalWrite(MX4_PIN, HIGH);  // Pull HIGH wenn losgelassen
    }

    if (currentRightBtn) {
      digitalWrite(MX5_PIN, LOW);  // Pull LOW wenn gedrückt
    } else {
      digitalWrite(MX5_PIN, HIGH);  // Pull HIGH wenn losgelassen
    }

    delay(1);
  }
}

// MSX sendMSX Funktion
void sendMSX(int8_t c) {
  // Warte auf HIGH (zwischen Transfers)
  while (digitalRead(CS_PIN) == LOW)
    ;

  // Erstes Nibble (Bits 7-4) senden
  sendNibble((c >> 4) & 0xF);

  // Warte auf LOW
  while (digitalRead(CS_PIN) == HIGH)
    ;

  // Zweites Nibble (Bits 3-0) senden
  sendNibble(c & 0xF);

  // strobeCount++;
}

// =================================================================
// BLE CALLBACK - Behandelt eingehende Maus-Daten mit Scroll-Wheel-Unterstützung
// =================================================================

// Funktions-Prototypen für GPIO-Funktionen
inline void lineLow(uint32_t mask);
inline void lineRelease(uint32_t mask);
inline void sendBit(uint32_t mask, bool bit);
void sendNibble(uint8_t n);

int32_t extractBits(uint8_t* data, int bitOffset, int bitSize, bool isSigned) {
  int32_t value = 0;

  for (int i = 0; i < bitSize; i++) {
    int byteIndex = (bitOffset + i) / 8;
    int bitIndex = (bitOffset + i) % 8;

    if (data[byteIndex] & (1 << bitIndex)) {
      value |= (1 << i);
    }
  }

  // Sign extension
  if (isSigned && (value & (1 << (bitSize - 1)))) {
    value |= (-1 << bitSize);
  }

  return value;
}

void parseHIDReport(uint8_t* data, size_t len) {
  uint8_t reportSize = 0;
  uint8_t reportCount = 0;
  uint8_t reportID = 0;
  uint16_t bitOffset = 0;
  uint8_t currentUsagePage = 0;

  uint16_t usageList[16];
  int usageCount = 0;

  hidFmt = {};

  Serial.println("Parsing HID Report Map:");

  for (int i = 0; i < len; i++) {
    uint8_t b = data[i];

    // Report ID (0x85)
    if (b == 0x85) {
      reportID = data[++i];
      Serial.printf("  Found Report ID: %d\n", reportID);
      bitOffset = 0;  // Reset bit offset for new report
    }
    // Usage Page (0x05)
    else if (b == 0x05) {
      currentUsagePage = data[++i];
      Serial.printf("  Usage Page: 0x%02X\n", currentUsagePage);
    }
    // Usage (0x09)
    else if (b == 0x09) {
      if (usageCount < 16) {
        uint8_t usage = data[++i];
        usageList[usageCount++] = usage;
        Serial.printf("  Usage: 0x%02X\n", usage);
      }
    }
    // Report Size (0x75)
    else if (b == 0x75) {
      reportSize = data[++i];
      Serial.printf("  Report Size: %d\n", reportSize);
    }
    // Report Count (0x95)
    else if (b == 0x95) {
      reportCount = data[++i];
      Serial.printf("  Report Count: %d\n", reportCount);
    }
    // Input (0x81)
    else if (b == 0x81) {
      uint8_t input = data[++i];
      Serial.printf("  Input found with properties: 0x%02X\n", input);

      // Process each field in this input element
      for (int j = 0; j < reportCount; j++) {
        uint16_t u = (j < usageCount) ? usageList[j] : 0;

        // Generic Desktop Page (0x01)
        if (currentUsagePage == 0x01) {
          // X-Axis (0x30)
          if (u == 0x30) {
            hidFmt.x = { u, reportID, bitOffset, reportSize, true };
            Serial.printf("  X-Axis @bitOffset=%d, bitSize=%d\n", hidFmt.x.bitOffset, hidFmt.x.bitSize);
          }
          // Y-Axis (0x31)
          else if (u == 0x31) {
            hidFmt.y = { u, reportID, bitOffset, reportSize, true };
            Serial.printf("  Y-Axis @bitOffset=%d, bitSize=%d\n", hidFmt.y.bitOffset, hidFmt.y.bitSize);
          }
          // Wheel (0x38)
          else if (u == 0x38) {
            hidFmt.wheel = { u, reportID, bitOffset, reportSize, true };
            Serial.printf("  Wheel @bitOffset=%d, bitSize=%d\n", hidFmt.wheel.bitOffset, hidFmt.wheel.bitSize);
          }
        }
        // Button Page (0x09)
        else if (currentUsagePage == 0x09) {
          hidFmt.hasExplicitButtons = true;

          // Buttons (0x01-0x08)
          if (u >= 0x01 && u <= 0x08) {
            if (u == 0x01) {
              hidFmt.leftButton = { u, reportID, bitOffset, reportSize, false };
              hidFmt.hasLeftButton = true;
              Serial.printf("  Left Button (0x01) @bitOffset=%d, bitSize=%d\n", bitOffset, reportSize);
            } else if (u == 0x02) {
              hidFmt.rightButton = { u, reportID, bitOffset, reportSize, false };
              hidFmt.hasRightButton = true;
              Serial.printf("  Right Button (0x02) @bitOffset=%d, bitSize=%d\n", bitOffset, reportSize);
            }
          }
        }

        bitOffset += reportSize;
      }

      usageCount = 0;  // Reset for next input element
    }
  }

  hidFmt.valid = true;
  hidFmt.reportID = reportID;

  // Wenn keine expliziten Button-Definitionen gefunden wurden, nehmen wir an,
  // dass das erste Byte die Button-Informationen enthält (Standard-Mausformat)
  if (!hidFmt.hasExplicitButtons) {
    hidFmt.usesStandardMouseFormat = true;
    Serial.println("  No explicit button definitions found - assuming standard mouse format (first byte for buttons)");
  }

  Serial.println("HID Maus erkannt:");
  Serial.printf("  Report ID: %d\n", hidFmt.reportID);
  Serial.printf("  X @bitOffset=%d, bitSize=%d\n", hidFmt.x.bitOffset, hidFmt.x.bitSize);
  Serial.printf("  Y @bitOffset=%d, bitSize=%d\n", hidFmt.y.bitOffset, hidFmt.y.bitSize);
  Serial.printf("  Wheel @bitOffset=%d, bitSize=%d\n", hidFmt.wheel.bitOffset, hidFmt.wheel.bitSize);
  if (hidFmt.hasExplicitButtons) {
    if (hidFmt.hasLeftButton) {
      Serial.printf("  Left Button defined @bitOffset=%d, bitSize=%d\n", hidFmt.leftButton.bitOffset, hidFmt.leftButton.bitSize);
    }
    if (hidFmt.hasRightButton) {
      Serial.printf("  Right Button defined @bitOffset=%d, bitSize=%d\n", hidFmt.rightButton.bitOffset, hidFmt.rightButton.bitSize);
    }
  } else {
    Serial.println("  Using standard mouse format: first byte contains button status");
  }
}


// =================================================================
// Mausaten - INTERPRETATION
// =================================================================
void interpretMouseData(uint8_t* data, size_t len, int16_t& x, int16_t& y, int8_t& wheel, bool& leftButton, bool& rightButton) {
  x = 0;
  y = 0;
  wheel = 0;
  leftButton = false;
  rightButton = false;

  if (len == 0) return;

  // =========================
  // Report ID nur dann überspringen, wenn es wirklich eine sinnvolle ID ist
  // Viele Mäuse senden 0x01, 0x02, 0x03, 0x04 direkt als Button-Byte → nicht als ID behandeln!
  // =========================
  uint8_t offset = 0;

  // Nur Report IDs behandeln, die üblicherweise verwendet werden (z.B. 1..10 oder höher)
  if (len >= 2 && hidFmt.valid && hidFmt.reportID != 0 && data[0] == hidFmt.reportID && hidFmt.reportID >= 0x10) {
    Serial.printf("Using Report ID: %d ", data[0]);
    offset = 1;
  }

  if (len <= offset) {
    Serial.println("Invalid length!");
    return;
  }

  // =========================
  // Bewegung extrahieren
  // =========================
  if (hidFmt.x.bitSize > 0) {
    x = extractBits(data + offset, hidFmt.x.bitOffset, hidFmt.x.bitSize, hidFmt.x.isSigned);
  }
  if (hidFmt.y.bitSize > 0) {
    y = extractBits(data + offset, hidFmt.y.bitOffset, hidFmt.y.bitSize, hidFmt.y.isSigned);
  }
  if (hidFmt.wheel.bitSize > 0) {
    wheel = extractBits(data + offset, hidFmt.wheel.bitOffset, hidFmt.wheel.bitSize, hidFmt.wheel.isSigned);
  }

  // =========================
  // BUTTONS
  // =========================
  uint8_t buttonByte = data[offset];

  leftButton = (buttonByte & 0x01) != 0;   // Bit 0 = links
  rightButton = (buttonByte & 0x02) != 0;  // Bit 1 = rechts

  // Debug
  Serial.printf("Buttons: 0x%02X → ", buttonByte);

  uint8_t state = buttonByte & 0x07;
  switch (state) {
    case 0x00: Serial.print("0x00(keine Taste)"); break;
    case 0x01: Serial.print("0x01(linke Taste)"); break;
    case 0x02: Serial.print("0x02(rechte Taste)"); break;
    case 0x03: Serial.print("0x03(beide Tasten)"); break;
    case 0x04: Serial.print("0x04(mittlere Taste)"); break;
    case 0x05: Serial.print("0x05(linke + mittlere)"); break;
    case 0x06: Serial.print("0x06(rechte + mittlere)"); break;
    case 0x07: Serial.print("0x07(alle drei Tasten)"); break;
    default: Serial.printf("0x%02X(?)", state); break;
  }

  Serial.printf(" L:%d R:%d", leftButton, rightButton);
}

void notifyCB(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool isNotify) {

  // =========================
  // 🔍 DEBUG: Rohdaten anzeigen
  // =========================
  Serial.print("LEN=");
  Serial.print(len);
  Serial.print(" DATA: ");
  for (int i = 0; i < len; i++) {
    Serial.printf("%02X ", data[i]);
  }
  Serial.print(" -  ");

  // =========================
  // ❌ Ohne gültigen HID-Parser → nichts tun
  // =========================
  if (!hidFmt.valid) {
    Serial.println("No valid HID format detected!");
    return;
  }

  // =========================
  // 🧠 Mausdaten-Interpretation
  // =========================
  int16_t x = 0, y = 0;
  int8_t wheel = 0;
  bool leftButton = false, rightButton = false;

  interpretMouseData(data, len, x, y, wheel, leftButton, rightButton);

  // =========================
  // 🧵 THREAD-SAFE UPDATE
  // =========================
  bool prevLeftBtn = leftBtn;
  bool prevRightBtn = rightBtn;

  if (scaleMutex != NULL) {
    xSemaphoreTake(scaleMutex, portMAX_DELAY);
  }

  lastX = x;
  lastY = y;
  leftBtn = leftButton;
  rightBtn = rightButton;
  lastMouseUpdate = millis();

  if (scaleMutex != NULL) {
    xSemaphoreGive(scaleMutex);
  }

  // =========================
  // 🌀 SCROLL → ZOOM
  // =========================
  if (wheel != 0) {
    if (scaleMutex != NULL) {
      xSemaphoreTake(scaleMutex, portMAX_DELAY);
    }

    char oldScale = currentScale;
    currentScale -= wheel;

    if (currentScale < minScale) currentScale = minScale;
    if (currentScale > maxScale) currentScale = maxScale;

    if (oldScale != currentScale) {
      scaleChanged = true;

      Serial.print("ZOOM: Faktor = ");
      Serial.print((int)currentScale);
      Serial.print(" (");
      Serial.print((int)(20.0 / currentScale * 100));
      Serial.print("%)");
    }

    if (scaleMutex != NULL) {
      xSemaphoreGive(scaleMutex);
    }
  }

  // Debug-Ausgabe (Bewegung + Buttons)
  bool buttonChanged = (prevLeftBtn != leftBtn) || (prevRightBtn != rightBtn);

  if (x != 0 || y != 0 || buttonChanged || wheel != 0) {
    Serial.print("BLE: x");
    Serial.print(x >= 0 ? "+" : "");
    Serial.print(x);

    Serial.print(" y");
    Serial.print(y >= 0 ? "+" : "");
    Serial.print(y);

    Serial.print(" w");
    Serial.print(wheel >= 0 ? "+" : "");
    Serial.print(wheel);

    Serial.print(" L:");
    Serial.print(leftBtn ? "1" : "0");
    if (prevLeftBtn && !leftBtn) Serial.print("^");

    Serial.print(" R:");
    Serial.print(rightBtn ? "1" : "0");
    if (prevRightBtn && !rightBtn) Serial.print("^");

    Serial.print(" Z:");
    Serial.print((int)currentScale);
    Serial.print("%");
  }

  Serial.println();
}

// =================================================================
// BLE FUNKTIONEN
// =================================================================

bool connectTo(const NimBLEAdvertisedDevice* dev) {
  Serial.println("MSX: Verbinde...");

  client = NimBLEDevice::createClient();
  client->setConnectTimeout(10);

  if (!client->connect(dev)) {
    Serial.println("MSX: Verbindung fehlgeschlagen");
    NimBLEDevice::deleteClient(client);
    Serial.println("NimBLE: Client gelöscht...");
    client = nullptr;
    return false;
  }

  connected = true;
  Serial.println("MSX: Verbunden!");

  // ==========================
  // HID Report Map lesen
  // ==========================
  NimBLERemoteService* hidSvc = client->getService("1812");
  if (hidSvc) {
    NimBLERemoteCharacteristic* reportMap = hidSvc->getCharacteristic("2A4B");

    if (reportMap) {
      std::string map = reportMap->readValue();

      Serial.println("HID Report Map:");
      for (int i = 0; i < map.length(); i++) {
        printf("%02X ", (uint8_t)map[i]);
      }
      Serial.println();

      parseHIDReport((uint8_t*)map.data(), map.length());
    } else {
      Serial.println("Keine Report Map gefunden!");
    }
  } else {
    Serial.println("Kein HID Service gefunden!");
  }



  mouseAddr = dev->getAddress().toString();
  mouseName = dev->getName().c_str();
  if (mouseName.empty()) mouseName = "Unbekannte Maus";

  // Abonniere alle Notify Characteristics
  auto services = client->getServices(true);
  for (auto svc : services) {
    auto chrs = svc->getCharacteristics(true);
    for (auto chr : chrs) {
      if (chr->canNotify()) {
        chr->subscribe(true, notifyCB);
      }
    }
  }

  return true;
}

void scanAndConnect() {
  Serial.println("MSX: Scanne nach HID Maus...");
  isScanning = true;

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setDuplicateFilter(false);

  scan->start(15, true);

  isScanning = false;
  auto res = scan->getResults();

  for (int i = 0; i < res.getCount(); i++) {
    const NimBLEAdvertisedDevice* dev = res.getDevice(i);

    if (dev->haveServiceUUID() && dev->isAdvertisingService(NimBLEUUID((uint16_t)0x1812))) {
      Serial.println("MSX: HID Maus gefunden");
      connectTo(dev);
      return;
    }
  }

  Serial.println("MSX: Keine HID-Geräte gefunden");
}

void disconnectMouse() {
  if (client && connected) {
    client->disconnect();
    connected = false;
    mouseName = "";
    mouseAddr = "";
    Serial.println("MSX: Getrennt");
  }
}

// =================================================================
// WEB SERVER INTERFACE mit Zoom-Kontrollen und Toggle
// =================================================================

void setupWebServer() {
  // Hauptseite
  server.on("/", HTTP_GET, []() {
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta http-equiv='Content-Type' content='text/html; charset=utf-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<title>MSX MOUSE - VERSION 013</title>";
    html += "<style>";
    html += "body{font-family:Arial;margin:20px;background:#f0f0f0;}";
    html += "h1{text-align:center;color:#333;}";
    html += ".section{margin:15px 0;padding:15px;background:white;border-radius:5px;box-shadow:0 20px 5px rgba(0,0,0,0.1);}";
    html += ".status{padding:10px;margin:10px 0;border-radius:5px;}";
    html += ".connected{background:#d4edda;color:#155724;}";
    html += ".disconnected{background:#f8d7da;color:#721c24;}";
    html += ".web-active{background:#fff3cd;color:#856404;}";
    html += "button{width:100%;padding:12px;margin:5px 0;font-size:14px;border:none;border-radius:5px;background:#007bff;color:#fff;cursor:pointer;}";
    html += "button:active{background:#0056b3;}";
    html += ".data-row{display:flex;align-items:center;margin:8px 0;}";
    html += ".data-label{width:120px;font-weight:bold;color:#555;}";
    html += ".data-value{color:#333;font-family:monospace;}";
    html += "input[type=number]{width:80px;padding:5px;margin:0 5px;}";
    html += "form{display:inline;}";
    html += "a{display:block;padding:8px;margin:4px 0;text-decoration:none;color:#007bff;border:1px solid #007bff;border-radius:3px;}";
    html += "a:hover{background:#007bff;color:#fff;}";
    html += "</style></head><body>";

    html += "<h1>MSX MOUSE v013</h1>";

    // Verbindungsstatus
    html += "<div class='section'>";
    html += "<h3>VERBINDUNGSSTATUS</h3>";
    html += "<div class='status ";
    html += connected ? "connected" : "disconnected";
    html += "'>";
    if (connected) {
      html += "BLE: " + String(mouseName.c_str()) + "<br>";
      html += "RSSI: " + String(currentRSSI) + " dBm<br>";
      html += "Daten: " + String((millis() - lastMouseUpdate < 1000) ? "Empfangen" : "Keine Daten");
    } else {
      html += "Nicht verbunden - Verwende D35, SCAN Knopf, oder wähle Gerät unten aus";
    }
    html += "</div>";
    html += "</div>";

    // Web Server Status und Steuerung
    html += "<div class='section'>";
    html += "<h3>WEB SERVER STATUS</h3>";
    html += "<div class='status web-active'>";
    html += "Status: " + String(webServerActive ? "AKTIV" : "INAKTIV") + "<br>";
    html += "Web-Interface: ";
    html += "<button onclick=\"location.href='/web_toggle'\">";
    html += webServerActive ? "STOP Web-Interface" : "START Web-Interface";
    html += "</button>";
    html += "</div>";
    html += "</div>";

    // BLE Geräte-Liste
    html += "<div class='section'>";
    html += "<h3>BLE GERÄTE</h3>";
    if (deviceList.empty()) {
      html += "<em>Keine Geräte gescannt. Verwende SCAN Knopf um Geräte zu finden.</em>";
    } else {
      for (int i = 0; i < deviceList.size(); i++) {
        html += "<a href='/select?id=" + String(i) + "'>";
        html += String(i) + ": " + String(deviceList[i].name.c_str());
        html += " (RSSI: " + String(deviceList[i].rssi) + " dBm)";
        html += "</a>";
      }
    }
    html += "</div>";

    // Maus-Daten
    html += "<div class='section'>";
    html += "<h3>MAUS DATEN</h3>";
    html += "<div class='data-row'><span class='data-label'>X:</span><span class='data-value'>";
    html += String(lastX >= 0 ? "+" : "") + String(lastX) + "</span></div>";
    html += "<div class='data-row'><span class='data-label'>Y:</span><span class='data-value'>";
    html += String(lastY >= 0 ? "+" : "") + String(lastY) + "</span></div>";
    html += "<div class='data-row'><span class='data-label'>Links:</span><span class='data-value'>";
    html += leftBtn ? "GEDRÜCKT" : "AUS";
    if (lastLeftBtn && !leftBtn) html += " (LOSGELOST)";
    html += "</span></div>";
    html += "<div class='data-row'><span class='data-label'>Rechts:</span><span class='data-value'>";
    html += rightBtn ? "GEDRÜCKT" : "AUS";
    if (lastRightBtn && !rightBtn) html += " (LOSGELOST)";
    html += "</span></div>";
    html += "</div>";

    // Zoom-Kontrolle
    html += "<div class='section'>";
    html += "<h3>ZOOM KONTROLLE (20%-200%)</h3>";
    html += "<button onclick=\"location.href='/zoom_out'\">Zoom RAUS (langsamer)</button>";
    html += "<button onclick=\"location.href='/zoom_in'\">Zoom REIN (schneller)</button>";
    html += "<button onclick=\"location.href='/zoom_reset'\">Zoom Zurücksetzen (100%)</button>";
    html += "<form action='/set_zoom' method='get' style='margin-top:10px;'>";
    html += "Faktor (4-40): <input type='number' min='4' max='40' name='value' value='" + String((int)currentScale) + "'>";
    html += "<input type='submit' value='Setzen' style='padding:5px 10px;margin-left:5px;'>";
    html += "</form>";
    html += "<div style='margin-top:10px;font-size:12px;color:#666;'>";
    html += "Scroll-Wheel: RAUS = Zoom (langsamer), REIN = schneller<br>";
    html += "Knopf-Release: angezeigt mit ^ Symbol<br>";
    html += "Kommandos: scale X | scale (Zeige aktuell)</div>";
    html += "</div>";

    // Steuerungen
    html += "<div class='section'>";
    html += "<h3>STEUERUNGEN</h3>";
    html += "<button onclick=\"location.href='/scan'\">Scan & Maus Verbinden</button>";
    html += "<button onclick=\"location.href='/scanlist'\">Scan Geräte-Liste</button>";
    html += "<button onclick=\"location.href='/disconnect'\">Maus Trennen</button>";
    html += "<button onclick=\"location.href='/update'\">Firmware Update</button>";
    html += "<button onclick=\"location.href='/reset'\">ESP32 Zurücksetzen</button>";
    html += "</div>";

    // Info
    html += "<div class='section'>";
    html += "<h3>GERÄTE INFO</h3>";
    html += "<h4>";
    html += "<div class='data-row'><span class='data-label'>Platine:</span><span class='data-value'>" + String(ssid) + "</span></div>";
    html += "<div class='data-row'><span class='data-label'>Pins:</span><span class='data-value'>Daten: 14,27,26,25, Knöpfe: 33,32, Strobe: 13, Scan: 35, LED: 2</span></div>";
    html += "<div class='data-row'><span class='data-label'>GPIO Modus:</span><span class='data-value'>Direkter Register-Zugriff synchron zu Strobe</span></div>";
    html += "<div class='data-row'><span class='data-label'>Betriebszeit:</span><span class='data-value'>" + String(millis() / 1000) + "s</span></div>";
    html += "<h4>https://github.com/rigr/ESP32_BLE_MSX";
    html += "</div>";

    // Auto-refresh alle 2 Sekunden
    html += "<script>setTimeout(() => location.reload(), 2000);</script>";

    html += "</body></html>";

    server.send(200, "text/html; charset=utf-8", html);
  });

  // Web-Interface Toggle-Endpunkt
  server.on("/web_toggle", HTTP_GET, []() {
    if (webServerActive) {
      stopWebServer();
    } else {
      startWebServer();
    }
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
  });

  // Scan-Endpunkt
  server.on("/scan", HTTP_GET, []() {
    if (!connected) {
      scanAndConnect();
    }
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Weiterleiten...");
  });

  // Scan Geräte-Liste Endpunkt
  server.on("/scanlist", HTTP_GET, []() {
    if (!connected) {
      scanDevices();
    }
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Weiterleiten...");
  });

  // Gerät auswählen Endpunkt
  server.on("/select", HTTP_GET, []() {
    int id = server.arg("id").toInt();
    if (id >= 0 && id < deviceList.size()) {
      selectedDevice = id;
      Serial.print("Ausgewähltes Gerät: ");
      Serial.println(deviceList[id].name.c_str());
    }
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
  });

  // Trennen Endpunkt
  server.on("/disconnect", HTTP_GET, []() {
    disconnectMouse();
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Weiterleiten...");
  });

  // Zoom-Kontrollen
  server.on("/zoom_out", HTTP_GET, []() {
    if (scaleMutex != NULL) {
      xSemaphoreTake(scaleMutex, portMAX_DELAY);
    }
    if (currentScale > minScale) {
      currentScale--;
      scaleChanged = true;

      Serial.print("ZOOM RAUS: Faktor = ");
      Serial.print((int)currentScale);
      Serial.print(" (");
      Serial.print((int)(20.0 / currentScale * 100));
      Serial.print("%)");
    }
    if (scaleMutex != NULL) {
      xSemaphoreGive(scaleMutex);
    }
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Weiterleiten...");
  });

  server.on("/zoom_in", HTTP_GET, []() {
    if (scaleMutex != NULL) {
      xSemaphoreTake(scaleMutex, portMAX_DELAY);
    }
    if (currentScale < maxScale) {
      currentScale++;
      scaleChanged = true;

      Serial.print("ZOOM REIN: Faktor = ");
      Serial.print((int)currentScale);
      Serial.print(" (");
      Serial.print((int)(20.0 / currentScale * 100));
      Serial.print("%)");
    }
    if (scaleMutex != NULL) {
      xSemaphoreGive(scaleMutex);
    }
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Weiterleiten...");
  });

  server.on("/zoom_reset", HTTP_GET, []() {
    if (scaleMutex != NULL) {
      xSemaphoreTake(scaleMutex, portMAX_DELAY);
    }
    currentScale = 15;  // 100% (geändert vom 20 auf 15)
    scaleChanged = true;

    Serial.print("ZOOM ZURÜCKSETZEN: Faktor = ");
    Serial.print((int)currentScale);
    Serial.print(" (");
    Serial.print((int)(20.0 / currentScale * 100));
    Serial.print("%)");
    if (scaleMutex != NULL) {
      xSemaphoreGive(scaleMutex);
    }
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Weiterleiten...");
  });

  server.on("/set_zoom", HTTP_GET, []() {
    if (server.hasArg("value")) {
      int newScale = server.arg("value").toInt();
      if (newScale >= minScale && newScale <= maxScale) {
        if (scaleMutex != NULL) {
          xSemaphoreTake(scaleMutex, portMAX_DELAY);
        }
        currentScale = newScale;
        scaleChanged = true;

        Serial.print("Web: Faktor auf ");
        Serial.print((int)currentScale);
        Serial.print(" (");
        Serial.print((int)(20.0 / currentScale * 100));
        Serial.print("%) gesetzt");
        if (scaleMutex != NULL) {
          xSemaphoreGive(scaleMutex);
        }
      }
    }
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Weiterleiten...");
  });

  // Firmware-Update Endpunkt (POST für OTA Update)
  server.on("/update", HTTP_GET, []() {
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta http-equiv='Content-Type' content='text/html; charset=utf-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<title>Firmware Update</title>";
    html += "<style>body{font-family:Arial;margin:20px;}</style></head><body>";
    html += "<h2>Firmware Update</h2>";
    html += "<p><strong>Aktuelle Firmware Version: 013</strong></p>";
    html += "<p><strong>Aktueller Status: " + String(webServerActive ? "AKTIV" : "INAKTIV") + "</strong></p>";
    html += "<form method='POST' action='/update' enctype='multipart/form-data'>";
    html += "<input type='file' name='update' accept='.bin'>";
    html += "<input type='submit' value='Firmware Update'>";
    html += "</form>";
    html += "<p><a href='/'>Zurück zur Hauptseite</a></p>";
    html += "</body></html>";
    server.send(200, "text/html; charset=utf-8", html);
  });

  // Firmware-Update POST-Endpunkt (für OTA Update)
  server.on(
    "/update", HTTP_POST, []() {
      server.send(200, "text/plain; charset=utf-8", "OTA Update gestartet");
    },
    []() {
      HTTPUpload& upload = server.upload();
      if (upload.status == UPLOAD_FILE_START) {
        Serial.setDebugOutput(true);
        String filename = upload.filename;
        if (!filename.startsWith("/")) filename = "/" + filename;
        Serial.print("Update: ");
        Serial.println(filename);
        uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
        if (!Update.begin(maxSketchSpace)) {
          Serial.println("Update beginn gescheitert");
        }
      } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
          Update.printError(Serial);
        }
      } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
          Serial.println("Update erfolgreich");
          Serial.println("ESP32 wird jetzt neu gestartet...");
          delay(1000);
          ESP.restart();
        } else {
          Update.printError(Serial);
        }
        Serial.setDebugOutput(false);
      }
    });

  // Zurücksetzen Endpunkt
  server.on("/reset", HTTP_GET, []() {
    ESP.restart();
  });

  server.begin();
}

// =================================================================
// Serielle Kommandos-Handler
// =================================================================

void handleWebCommand(String cmd) {
  if (cmd == "web" || cmd == "webinterface" || cmd == "web toggle") {
    if (webServerActive) {
      stopWebServer();
    } else {
      startWebServer();
    }
  } else if (cmd.startsWith("web ")) {
    String subCmd = cmd.substring(4);  // Entferne "web " Präfix
    if (subCmd == "on" || subCmd == "start") {
      if (!webServerActive) startWebServer();
    } else if (subCmd == "off" || subCmd == "stop") {
      if (webServerActive) stopWebServer();
    }
  } else if (cmd == "web status" || cmd == "web state") {
    Serial.println("Web-Server: " + String(webServerActive ? "AKTIV" : "INAKTIV"));
  }
}

void handleSerialCommand(String cmd) {
  if (cmd.equals("s")) {
    if (!connected) scanAndConnect();
    else Serial.println("MSX: Bereits verbunden");
  } else if (cmd.equals("d")) {
    disconnectMouse();
  } else if (cmd.equals("r")) {
    Serial.println("MSX: Zurücksetzen...");
    ESP.restart();
  } else if (cmd.equals("scale")) {
    if (scaleMutex != NULL) {
      xSemaphoreTake(scaleMutex, portMAX_DELAY);
    }
    Serial.print("Aktueller Faktor: ");
    Serial.print((int)currentScale);
    Serial.print(" (");
    Serial.print((int)(20.0 / currentScale * 100));
    Serial.print("%)");
    if (scaleMutex != NULL) {
      xSemaphoreGive(scaleMutex);
    }
  } else if (cmd.startsWith("scale ")) {
    int newScale = cmd.substring(6).toInt();
    if (newScale >= minScale && newScale <= maxScale) {
      if (scaleMutex != NULL) {
        xSemaphoreTake(scaleMutex, portMAX_DELAY);
      }
      currentScale = newScale;
      scaleChanged = true;

      Serial.print("Faktor gesetzt auf: ");
      Serial.print((int)currentScale);
      Serial.print(" (");
      Serial.print((int)(20.0 / currentScale * 100));
      Serial.print("%)");
      if (scaleMutex != NULL) {
        xSemaphoreGive(scaleMutex);
      }
    } else {
      Serial.print("Faktor muss zwischen ");
      Serial.print((int)minScale);
      Serial.print(" und ");
      Serial.print((int)maxScale);
      Serial.println(" sein (20%-200%)");
    }
  } else if (cmd.equals("scan")) {
    scanDevices();
  } else if (cmd.equals("list")) {
    printDeviceList();
  } else if (cmd.startsWith("select ")) {
    int id = cmd.substring(7).toInt();
    if (id >= 0 && id < deviceList.size()) {
      selectedDevice = id;
      Serial.print("Ausgewähltes Gerät: ");
      Serial.println(deviceList[id].name.c_str());
    } else {
      Serial.println("Ungültige Geräte-Nummer");
    }
  } else if (cmd.equals("help") || cmd.equals("h")) {
    Serial.println("=== MSX MAUS KOMMANDOS ===");
    Serial.println("s - Scan & Verbinde erste HID-Maus");
    Serial.println("scan - Scan Geräte-Liste (20s)");
    Serial.println("list - Zeige Geräte-Liste");
    Serial.println("select <nr> - Wähle Gerät aus Liste");
    Serial.println("d - Trenne Maus");
    Serial.println("scale - Zeige aktuellen Zoom");
    Serial.println("scale X - Setze Zoom (4-40, 20%-200%)");
    Serial.println("web - Schalte Web-Interface an/aus");
    Serial.println("web on - Starte Web-Interface");
    Serial.println("web off - Stoppe Web-Interface");
    Serial.println("web status - Zeige Web-Interface Status");
    Serial.println("r - Zurücksetzen ESP32");
    Serial.println("h oder help - Zeige diese Hilfe");
    Serial.println("^ in Ausgabe zeigt Knopf-Release an");
    Serial.println("GPIO Operationen: Direkter Register-Zugriff + Strobe Sync");
    Serial.println("");
    Serial.println("=== AKTIVIERUNGS METHODEN ===");
    Serial.println("BOOT Knopf: 3s um Web-Interface zu starten, 6s um zu stoppen");
    Serial.println("Serielles Kommando: 'web' oder 'webinterface' um zu toggeln");
    Serial.println("");
    Serial.println("Aktueller Web-Server Status: " + String(webServerActive ? "AKTIV" : "INAKTIV"));
  } else {
    // Behandle Web-Interface-Kommandos
    if (cmd.startsWith("web")) {
      handleWebCommand(cmd);
    }
  }
}

// =================================================================
// SETUP UND LOOP (Core 0)
// =================================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  // LED Blink-Sequenz
  pinMode(LED_PIN, OUTPUT);
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(200);
    digitalWrite(LED_PIN, LOW);
    delay(200);
  }


  uint64_t chipid = ESP.getEfuseMac();

  Serial.println("=====================================");
  Serial.println("ESP32 MSX MAUS - VERSION 013");
  Serial.println("https://github.com/rigr/ESP32_BLE_MSX");
  Serial.println("NimBLE Version: 2.1.0 by h2zero");
  // Print the ID in hexadecimal format
  Serial.print("ESP32 Chip ID: ");
  Serial.printf("%04X%08X\n", (uint16_t)(chipid >> 32), (uint32_t)chipid);
  Serial.println("=====================================");
  Serial.println("Web: START via BOOT Knopf (3s), STOP via BOOT Knopf (6s)");
  Serial.println("Kommandos: scale X | scale (zeigen) | web (toggle) | s/d/scale/scan/list/select X | help");

  // char ssid[23];
  snprintf(ssid, 23, "ESP32-%04X%08X", (uint16_t)(chipid >> 32), (uint32_t)chipid);
  Serial.println(ssid);

  // Erstelle Mutex für Thread-Sicherheit
  scaleMutex = xSemaphoreCreateMutex();
  if (scaleMutex == NULL) {
    Serial.println("Fehler beim Erstellen des Mutex");
  }

  // Initialisiere Pins
  pinMode(BOOT_PIN, INPUT_PULLUP);  // BOOT Knopf
  pinMode(MAN_SCAN, INPUT);         // Manuelles Scan-Trigger
  pinMode(CS_PIN, INPUT);           // Strobe-Eingang.    //ri 20.3.26

  // Interrupt an BOOT Knopf anbringen
  attachInterrupt(digitalPinToInterrupt(BOOT_PIN), bootButtonISR, CHANGE);

  // Initialisiere BLE
  NimBLEDevice::init("");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  // Scanner konfigurieren
  NimBLEDevice::getScan()->setActiveScan(true);
  NimBLEDevice::getScan()->setDuplicateFilter(false);  // wichtig bei Maus

  // Starte MSX Protokoll Task auf Core 1
  xTaskCreatePinnedToCore(
    msxProtocolTask,
    "MSXProtocolTask",
    4096,
    NULL,
    1,
    &msxTaskHandle,
    1);

  Serial.println("D35 nach Masse ziehen, um nach Devices zu scannen und zu verbinden");
  Serial.println("BOOT Knopf (D0) - 3s drücken, um Web-Interface zu starten, 6s zum Stoppen");
  Serial.println("ROLAND / MSX Maus-Emulation bereit!");
  Serial.println("Seriell: 'web' um Web-Interface zu toggeln");
  Serial.println("Gib 'help' oder 'h' für alle Kommandos ein");
}

void loop() {
  // Behandle Web-Anfragen (Core 0) - nur wenn Server aktiv
  if (webServerActive) {
    server.handleClient();
  }

  // Heartbeat alle 5 Sekunden
  if (millis() - lastAlive >= 5000) {
    lastAlive = millis();
    Serial.print("Heartbeat | Betriebszeit: ");
    Serial.print(millis() / 1000);
    Serial.print("s | BLE: ");

    if (connected && client && client->isConnected()) {
      currentRSSI = client->getRssi();
      Serial.print("VERBUNDEN | ");
      Serial.print(mouseName.c_str());
      Serial.print(" | RSSI: ");
      Serial.print(currentRSSI);
      Serial.print(" | Web: ");
      Serial.print(webServerActive ? "AKTIV" : "INAKTIV");
      Serial.print(" | Zoom: ");
      Serial.print((int)currentScale);
      Serial.print(" (");
      Serial.print((int)(20.0 / currentScale * 100));
      Serial.print("%)");
    } else {
      Serial.print("GETRENNT");
    }
    Serial.println();
  }

  // Behandle Geräte-Auswahl-Timeout
  if (selectingDevice) {
    if (millis() - selectionStart > 30000) {
      Serial.println("Auto-Verbindung zum ausgewählten Gerät");
      selectingDevice = false;
      connectSelectedDevice();
    }
  }

  // Update LED
  updateLED();

  // BOOT-Button lange gedrückt?
  checkLongButtonPresses();

  // Behandle serielle Kommandos
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    cmd.toLowerCase();

    handleSerialCommand(cmd);
  }

  // Manueller Scan-Trigger (D35 lOW)
  if (!digitalRead(MAN_SCAN) && !connected && !isScanning) {
    Serial.println("MSX: Manueller Scan über D35 ausgelöst...");
    scanAndConnect();
    delay(1000);
  }

  delay(10);
}
