# ESP32 MSX Mouse

A ESP32-based device for emulating a mouse for Roland S-750 Samplers and maybe MSX computer systems. This Arduino sketch allows to use a BLE-mouse and the ESP32 GPIO interface.

## Features

- **GPIO operations**: Using direct register access for speed
- **BLE support**: Connection with Bluetooth-enabled mice via NimBLE
- **Zoom control**: Dynamic zoom adjustment from 20% to 200% (adjustable) using the scrollwheel of the mouse
- **Web interface**: Configuration through web interface
- **OTA firmware update**: Update of firmware directly through the web interface
- **Thread-safe operations**: Mutex-protected operations for stability
- **Serial interface**: Complete configuration through serial monitor

## Technical Specifications

- **Microcontroller**: ESP32-WROOM-32D (30 Pins), Use the esp32 board configuration 3.0.0 by espressif systems
- **BLE**: NimBLE Version 2.1.0 by h2zero
- **MSX protocol**: strobe sync for compatibility
- **Pins**: 14, 27, 26, 25 for data lines, 33, 32 fpr mouse buttons, 13 for the strobe signal, 35 for manual scan and connect, 2 for the onboard LED
- All Pins are on the same side of the esp32 board and connection is made easy this way.
- consult  [build.md](./build.md) for infos concerning the wiring. I have mine connected using 9pin cable and think about opening the S-750 to put the ESP32 inside. 

## Pin Assignment

```
D14 = MX0 (Data bus)
D27 = MX1 (Data bus)
D26 = MX2 (Data bus)
D25 = MX3 (Data bus)
D33 = MX4 (Left Button)
D32 = MX5 (Right Button)
D13 = CS (Strobe input)
D35 = Scan trigger (low for manual scanning)
D2  = LED (built-in status LED)
D0  = BOOT button (web interface management)
```

## Installation

### Prerequisites

- ESP32 development board (ESP32-WROOM-32D) - 3.0.0 
- Arduino IDE
- Bluetooth-enabled mouse (I use a cheap chinese mouse that introduces itself as "BT5.2")
- Roland S-750 Sampler and probably others as well as MSX computers

### Firmware Installation

1. Open the Arduino IDE
2. Select the ESP32-WROOM-32D board under Tools → Board and take care, use version 3.0.0 of the board definition
3. Upload the desired [Firmware](./source/ESP32_MSX.ino) - can be found in the source folder
4. Start setup through serial interface at 115200 baud

## Operation

The ESP32 simulates the MSX mouse interface by directly manipulating GPIO pins. Input data from BLE mice is transmitted in real-time to the sampler.

### Activation methods

- **BOOT button**: Hold 3 seconds to start an access point (MSX_MOUSE, password 12345678) offering a web interface (192.168.4.1), 6 seconds to stop
- **Serial command**: "web" or "webinterface" to toggle, "web on" to start, "web off" to stop
- **Manual scan trigger**: Pull D35 low to scan for devices

### Serial Commands

| Command | Description |
|---------|-------------|
| `help` / `h` | Show all available commands |
| `s` | Scan and connect the first HID mouse |
| `scan` | Scan device list (20s) |
| `list` | Show device list |
| `select <nr>` | Select device from list |
| `d` | Disconnect mouse |
| `scale` | Show current zoom |
| `scale X` | Set zoom to X (4-40, 20%-200%) |
| `web` | Toggle web interface on/off |
| `web on` | Start web interface |
| `web off` | Stop web interface |
| `web status` | Show web interface status |
| `r` | Reset ESP32 |

## Web Interface

The web interface allows comfortable remote configuration through a web browser. It can be activated by pressing the boot-button for three seconds or using the serial interface. Access point name is MSX_MOUSE, password is 12345678, webinterface is on page 182.168.4.1

- **Connection status**: Shows current connection information
- **BLE devices**: Scanning and selection of available devices
- **Zoom control**: Dynamic zoom adjustment (20%-200%)
- **Mouse data**: Live display of X/Y movements and button states
- **Firmware update**: OTA update directly through web interface

Access is available through the ESP32's IP address (e.g. 192.168.4.1) when the web interface is active.

## Configuration

### Zoom Factor Adjustment

The default zoom factor can be adjusted in the firmware:

```cpp
volatile char currentScale = 15;  // Start: 100%
const char minScale = 4;          // Minimal: 20%
const char maxScale = 40;         // Maximal: 200%
```

The zoom function can also be dynamically adjusted using the scroll wheel of the BLE mouse.

### BLE Connection

The system automatically scans for HID mouse devices. It prefers the strongest connection based on RSSI value. I swith on the sampler, then swith on the mouse, press both buttons (that puts it in pairing mode) and move it and after a few seconds the ESP32 is connected to the mosue.
 
## Development Status

- **Version 0.04**: it finally works. 

## License

This project is licensed under the MIT License - see the LICENSE file for details.

## Contribution

Basic contributions came from NYYRIKKY and Peter Ullrich - thanks to both of you!

More contributors are welcome! Please send a pull request with improvements or report issues in the GitHub issues tracker. Copy it and use it.

## Acknowledgments

- NimBLE library for BLE support
- Arduino for ESP32 support
- MSX Community for protocol definitions


---

*This project is designed for use on Roland S-750 samplers and was developed to replace the MU-1-mouse.*

*Here some technical background to the strobe signal of the sampler: [background.md](./background.md)*
