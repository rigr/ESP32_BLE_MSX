// =================================================================
// ESP32 MSX MOUSE
// 
// - Zoomfaktor geändert von 20 auf 15
// - Korrigierte OTA-Funktionalität
// 
// Board: ESP32-WROOM-32D (30 pins).  board defintion 3.00 von espressif
// NimBLE Version: 2.1.0 by h2zero
//
// Features:
// - Optimized GPIO operations using direct register access
// - BLE device list and selection
// - Zoom control (20%-200%) with NO NVS storage (immediate save)
// - Button release detection
// - Web Interface Start/Stop via BOOT button
// - OTA firmware update capability
// - Thread-safe operations with proper mutex protection
// - Improved button debouncing and timing
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
const uint8_t MX4_PIN = 33;  // Linker Knopf - OUTPUT
const uint8_t MX5_PIN = 32;  // Rechter Knopf - OUTPUT
const uint8_t CS_PIN =  13;  // Strobe Eingang
const uint8_t MAN_SCAN = 35; // Manuelles Scan-Trigger (low = scan)
const uint8_t LED_PIN =  2;  // Eingebaute LED
const uint8_t BOOT_PIN = 0;  // BOOT button (GPIO0)  bei board definition 3.0.0 von espressif notwendig

// Bitmasken für optimierten GPIO-Zugriff
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
volatile bool lastLeftBtn = false, lastRightBtn = false;  // Für Release-Detektion
unsigned long lastMouseUpdate = 0;

// Zoom-Kontrollvariablen (20%-200% Bereich)
volatile int8_t wheelDelta = 0;
volatile char currentScale = 15;  // Start: 100% (geändert vom 20 auf 15)
const char minScale = 4;          // Minimal: 20%
const char maxScale = 40;         // Maximal: 200%
bool scaleChanged = false;        // Für Web-Update



// Knopf-Langdruck-Erkennung
unsigned long leftButtonPressTime = 0;
unsigned long rightButtonPressTime = 0;
unsigned long bootButtonPressTime = 0;
bool leftButtonPressed = false;
bool rightButtonPressed = false;
bool bootButtonPressed = false;
bool lastBootButtonState = false;

// Zeit-Schwelle für verschiedene Aktionen
const unsigned long WEB_START_BOOT_THRESHOLD = 3000; // 3 Sekunden für Web-Start (Boot-Knopf)
const unsigned long WEB_STOP_BOOT_THRESHOLD = 6000;   // 6 Sekunden für Web-Stop (Boot-Knopf)
const unsigned long DEBOUNCE_DELAY = 300;            // 300ms Debounce für Toggle vs Langdruck

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

// =================================================================
// OPTIMIERTE GPIO FUNKTIONEN - Direkter Register-Zugriff
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
    if(bit)
        lineRelease(mask);  // logisch 1
    else
        lineLow(mask);      // logisch 0
}

// Nibble (4 Bits) gleichzeitig senden - optimiert
void sendNibble(uint8_t n) {
    uint32_t lowMask = 0;
    uint32_t relMask = 0;

    if(n & 8) relMask |= B3; else lowMask |= B3;
    if(n & 4) relMask |= B2; else lowMask |= B2;
    if(n & 2) relMask |= B1; else lowMask |= B1;
    if(n & 1) relMask |= B0; else lowMask |= B0;

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
// Knopf-Langdruck-Erkennung
// =================================================================
void checkLongButtonPresses() {
  unsigned long currentTime = millis();

  // BOOT-Knopf prüfen für Web-Interface (zusätzlich zum ISR)
  if (bootButtonPressed) {
    unsigned long pressDuration = currentTime - bootButtonPressTime;

    if (pressDuration >= WEB_STOP_BOOT_THRESHOLD) {
      // Langer Boot-Knopf (6s): Web-Interface stoppen
      if (webServerActive) {
        stopWebServer();
      }
      bootButtonPressed = false; // Mehrfach-Aktionen verhindern
    } else if (pressDuration >= WEB_START_BOOT_THRESHOLD && pressDuration < WEB_STOP_BOOT_THRESHOLD) {
      // Mittlerer Boot-Knopf (3s): Web-Interface starten
      if (!webServerActive) {
        startWebServer();
      }
      bootButtonPressed = false; // Mehrfach-Aktionen verhindern
    } else if (pressDuration < DEBOUNCE_DELAY && !webServerActive && !isScanning && !connected) {
      // Kurzer Boot-Knopf Druck: BLE-Maus-Scan initiieren, wenn nicht beim Scan
      if (!isScanning) {
        scanDevices();
      }
      bootButtonPressed = false; // Mehrfach-Aktionen verhindern
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

// Scan für alle BLE-Geräte (20 Sekunden)
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

// Verbindung zum ausgewählten Gerät
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

// =================================================================
// MSX PROTOCOL - DUAL CORE TASK (Core 1) - ANTI-DRIFT MIT OPTIMIERTEM GPIO
// =================================================================

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

  // Setup OUTPUT-Pins für Maus-Knöpfe
  pinMode(MX4_PIN, OUTPUT);  // Linker Knopf
  pinMode(MX5_PIN, OUTPUT);  // Rechter Knopf
  digitalWrite(MX4_PIN, HIGH);  // Initially high (nicht gedrückt)
  digitalWrite(MX5_PIN, HIGH);  // Initially high (nicht gedrückt)

  // MSX-Protokoll Variablen
  static int16_t mx = 0, my = 0;
  static unsigned long lastSend = 0;

  Serial.println("MSX Protokoll Task gestartet auf Core 1");
  Serial.println("Pin-Konfiguration:");
  Serial.println("MX0=14, MX1=27, MX2=26, MX3=25, MX4=33(L), MX5=32(R), CS=13, SCAN=35, LED=2");
  Serial.println("Direkter Register-Zugriff für schnellere Kommunikation");
  Serial.print("Initialer Zoom-Faktor: ");
  Serial.print((int)currentScale);
  Serial.print(" (");
  Serial.print((int)(20.0/currentScale*100));
  Serial.println("%)");
  Serial.println("Zoom-Bereich: 4-40 (20%-200%)");

  while(1) {
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
    mx += (currentX * -1); // X invertiert
    my += (currentY * -1); // Y invertiert

    // Auf CS HIGH warten → MSX ist bereit für nächsten Transfer
    while (digitalRead(CS_PIN) == LOW) {
      delayMicroseconds(20);
      yield();  // andere Tasks Zeit lassen
    }

    // CS ist jetzt HIGH → bereite hohe Nibbles vor

    // Skaliere Bewegung gemäß aktuellem Zoom-Faktor
    char x = (char)(mx * currentScale / 15); // geändert vom 20 auf 15
    char y = (char)(my * currentScale / 15); // geändert vom 20 auf 15

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

    // Nach vollständiger X+Y Paar → Akkumulator zurücksetzen
    mx = 0;
    my = 0;

    // Behandle Knöpfe - setze OUTPUT-Pins LOW wenn gedrückt, HIGH wenn losgelassen
    if (currentLeftBtn) {
      digitalWrite(MX4_PIN, LOW);  // Pull LOW wenn gedrückt
    } else {
      digitalWrite(MX4_PIN, HIGH); // Pull HIGH wenn losgelassen
    }

    if (currentRightBtn) {
      digitalWrite(MX5_PIN, LOW);  // Pull LOW wenn gedrückt
    } else {
      digitalWrite(MX5_PIN, HIGH); // Pull HIGH wenn losgelassen
    }

    delay(1);
  }
}

// MSX sendMSX Funktion - Optimiert mit direktem Register-Zugriff
void sendMSX(int8_t c) {
  // Warte auf HIGH (zwischen Transfers)
  while (digitalRead(CS_PIN) == LOW);

  // Erstes Nibble (Bits 7-4) senden
  sendNibble((c >> 4) & 0xF);

  // Warte auf LOW
  while (digitalRead(CS_PIN) == HIGH);

  // Zweites Nibble (Bits 3-0) senden
  sendNibble(c & 0xF);

  // strobeCount++;
}

// =================================================================
// BLE CALLBACK - Behandelt eingehende Maus-Daten mit Scroll-Wheel-Unterstützung
// =================================================================

void notifyCB(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool isNotify) {
  if (len < 7) return; // zu wenig Daten

  uint8_t buttons = data[0];
  int16_t x = (int16_t)(data[1] | (data[2] << 8));
  int16_t y = (int16_t)(data[3] | (data[4] << 8));

  // Vorherige Knopf-Zustände für Release-Detektion speichern
  bool prevLeftBtn = leftBtn;
  bool prevRightBtn = rightBtn;

  // Aktualisiere geteilte Variablen für MSX Task
  if (scaleMutex != NULL) {
    xSemaphoreTake(scaleMutex, portMAX_DELAY);
  }
  lastX = x;
  lastY = y;
  leftBtn = (buttons & 0x01);
  rightBtn = (buttons & 0x02);
  lastMouseUpdate = millis();
  if (scaleMutex != NULL) {
    xSemaphoreGive(scaleMutex);
  }

  // Scroll-Wheel-Verarbeitung für Zoom-Kontrolle
  int8_t wheel = (int8_t)(data[5] | (data[6] << 8));
  if (wheel != 0) {
    // Scroll nach oben = Zoom (kleinere Bewegungen)
    // Scroll nach unten = größere Bewegungen
    if (scaleMutex != NULL) {
      xSemaphoreTake(scaleMutex, portMAX_DELAY);
    }
    char oldScale = currentScale;
    currentScale -= wheel;  // Umgekehrte Logik für Zoom-Effekt

    if (currentScale < minScale) currentScale = minScale;
    if (currentScale > maxScale) currentScale = maxScale;

    if (oldScale != currentScale) {
      scaleChanged = true;
      
      Serial.print("ZOOM: Faktor = ");
      Serial.print((int)currentScale);
      Serial.print(" (");
      Serial.print((int)(20.0/currentScale*100));
      Serial.println("%)");
    }
    if (scaleMutex != NULL) {
      xSemaphoreGive(scaleMutex);
    }
  }

  // Debug-Ausgabe mit Knopf-Release-Detektion
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
    if (prevLeftBtn && !leftBtn) Serial.print("^");  // Release-Indikator
    Serial.print(" R:");
    Serial.print(rightBtn ? "1" : "0");
    if (prevRightBtn && !rightBtn) Serial.print("^"); // Release-Indikator
    Serial.print(" Z:");
    Serial.print((int)currentScale);
    Serial.println("%");
  }
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
    html += "<title>MSX MOUSE - VERSION 004</title>";
    html += "<style>";
    html += "body{font-family:Arial;margin:20px;background:#f0f0f0;}";
    html += "h1{text-align:center;color:#333;}";
    html += ".section{margin:15px 0;padding:15px;background:white;border-radius:5px;box-shadow:0 2px 5px rgba(0,0,0,0.1);}";
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

    html += "<h1>MSX MOUSE - VERSION 004</h1>";

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
    html += "<h4>https://github.com/rigr/ESP32_BLE_MSX";
    html += "<h4>";
    html += "<div class='data-row'><span class='data-label'>Platine:</span><span class='data-value'>ESP32-WROOM-32D</span></div>";
    html += "<div class='data-row'><span class='data-label'>Verbindung:</span><span class='data-value'>Kombiniert optimiert + Anti-Drift</span></div>";
    html += "<div class='data-row'><span class='data-label'>Pins:</span><span class='data-value'>Daten: 14,27,26,25, Knöpfe: 33,32, Strobe: 13, Scan: 35, LED: 2</span></div>";
    html += "<div class='data-row'><span class='data-label'>GPIO Modus:</span><span class='data-value'>Direkter Register-Zugriff + Strobe Sync</span></div>";
    html += "<div class='data-row'><span class='data-label'>Betriebszeit:</span><span class='data-value'>" + String(millis() / 1000) + "s</span></div>";
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
      Serial.print((int)(20.0/currentScale*100));
      Serial.println("%)");
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
      Serial.print((int)(20.0/currentScale*100));
      Serial.println("%)");
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
    Serial.print((int)(20.0/currentScale*100));
    Serial.println("%)");
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
        Serial.print((int)(20.0/currentScale*100));
        Serial.println("%) gesetzt");
        if (scaleMutex != NULL) {
          xSemaphoreGive(scaleMutex);
        }
      }
    }
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Weiterleiten...");
  });

  // Firmware-Update Endpunkt (POST für OTA Update)
  // Firmware-Update Hauptseite (GET)
  server.on("/update", HTTP_GET, []() {
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta http-equiv='Content-Type' content='text/html; charset=utf-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<title>Firmware Update</title>";
    html += "<style>body{font-family:Arial;margin:20px;}</style></head><body>";
    html += "<h2>Firmware Update</h2>";
    html += "<p><strong>Aktuelle Firmware Version: 004</strong></p>";
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
  server.on("/update", HTTP_POST, []() {
    server.send(200, "text/plain; charset=utf-8", "OTA Update gestartet");
  }, []() {
    HTTPUpload& upload = server.upload();
    if(upload.status == UPLOAD_FILE_START){
      Serial.setDebugOutput(true);
      String filename = upload.filename;
      if(!filename.startsWith("/")) filename = "/"+filename;
      Serial.print("Update: ");
      Serial.println(filename);
      uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
      if(!Update.begin(maxSketchSpace)){
        Serial.println("Update beginn gescheitert");
      }
    } else if(upload.status == UPLOAD_FILE_WRITE){
      if(Update.write(upload.buf, upload.currentSize) != upload.currentSize){
        Update.printError(Serial);
      }
    } else if(upload.status == UPLOAD_FILE_END){
      if(Update.end(true)){
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
  }
  else if (cmd.equals("d")) {
    disconnectMouse();
  }
  else if (cmd.equals("r")) {
    Serial.println("MSX: Zurücksetzen...");
    ESP.restart();
  }
  else if (cmd.equals("scale")) {
    if (scaleMutex != NULL) {
      xSemaphoreTake(scaleMutex, portMAX_DELAY);
    }
    Serial.print("Aktueller Faktor: ");
    Serial.print((int)currentScale);
    Serial.print(" (");
    Serial.print((int)(20.0/currentScale*100));
    Serial.println("%)");
    if (scaleMutex != NULL) {
      xSemaphoreGive(scaleMutex);
    }
  }
  else if (cmd.startsWith("scale ")) {
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
      Serial.print((int)(20.0/currentScale*100));
      Serial.println("%)");
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
  }
  else if (cmd.equals("scan")) {
    scanDevices();
  }
  else if (cmd.equals("list")) {
    printDeviceList();
  }
  else if (cmd.startsWith("select ")) {
    int id = cmd.substring(7).toInt();
    if (id >= 0 && id < deviceList.size()) {
      selectedDevice = id;
      Serial.print("Ausgewähltes Gerät: ");
      Serial.println(deviceList[id].name.c_str());
    } else {
      Serial.println("Ungültige Geräte-Nummer");
    }
  }
  else if (cmd.equals("help") || cmd.equals("h")) {
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
    Serial.println("Linker Maus-Knopf: 5s um Zoom-Wert in NVS-Speicher zu speichern");
    Serial.println("");
    Serial.println("Aktueller Web-Server Status: " + String(webServerActive ? "AKTIV" : "INAKTIV"));
  }
  else {
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

  Serial.println("====================================");
  Serial.println("ESP32 MSX MAUS - VERSION 004");
  Serial.println("https://github.com/rigr/ESP32_BLE_MSX");
  Serial.println("NimBLE Version: 2.1.0 by h2zero");
  Serial.println("====================================");
  Serial.println("Web: START via BOOT Knopf (3s), STOP via BOOT Knopf (6s)");
  Serial.println("Hardware: Pull D35 low zum Scannen");
  Serial.println("PINOUT: 14,27,26,25,33,32,13,35,2");
  Serial.println("Zoom-Kontroll (20%-200%) + Button-release-Detektion!");
  Serial.println("Kommandos: scale X | scale (zeigen) | web (toggle) | s/d/scale/scan/list/select X | help");

  // Erstelle Mutex für Thread-Sicherheit
  scaleMutex = xSemaphoreCreateMutex();
  if (scaleMutex == NULL) {
    Serial.println("Fehler beim Erstellen des Mutex");
  }

  // Initialisiere Pins
  pinMode(BOOT_PIN, INPUT_PULLUP);  // BOOT Knopf
  pinMode(MAN_SCAN, INPUT);         // Manuelles Scan-Trigger
  pinMode(CS_PIN, INPUT);    // Strobe-Eingang.    //ri 20.3.26

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
    1
  );

  Serial.println("Manuelles Scan-Trigger auf D35 - pull low zum Scannen");
  Serial.println("BOOT Knopf auf D0 - halte 3s für Web-Interface Start, 6s zum Stoppen");
  Serial.println("MSX Maus bereit!");
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
      Serial.print((int)(20.0/currentScale*100));
      Serial.println("%)");
    } else {
      Serial.println("GETRENNT");
    }
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

  // Prüfe auf lange Knopf-Drücke
  checkLongButtonPresses();

  // Behandle serielle Kommandos
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    cmd.toLowerCase();

    handleSerialCommand(cmd);
  }

  // Behandle manuelles Scan-Trigger (pull D35 low)
  if (!digitalRead(MAN_SCAN) && !connected && !isScanning) {
    Serial.println("MSX: Manueller Scan über D35 ausgelöst...");
    scanAndConnect();
    delay(1000);
  }

  delay(10);
}
