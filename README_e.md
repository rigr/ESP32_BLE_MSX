# ESP32 MSX Mouse - Version 004

A powerful and optimized ESP32-based device for emulating a mouse for MSX computer systems. This firmware allows using a Bluetooth-enabled mouse as MSX mouse input via direct GPIO interface.

## Features

- **Optimized GPIO operations**: Using direct register access for maximum speed
- **BLE support**: Connection with Bluetooth-enabled mice via NimBLE
- **Zoom control**: Dynamic zoom adjustment from 20% to 200% (adjustable)
- **Web interface**: Simple configuration through web interface
- **OTA firmware update**: Update of firmware directly through the web interface
- **Thread-safe operations**: Mutex-protected operations for stable performance
- **Serial interface**: Complete configuration through serial monitor

## Technical Specifications

- **Microcontroller**: ESP32-WROOM-32D (30 Pins)
- **BLE**: NimBLE Version 2.1.0 by h2zero
- **GPIO optimization**: Direct register access for fast communication
- **MSX protocol**: Optimized strobe sync for complete compatibility
- **Pins**: 14, 27, 26, 25, 33, 32, 13, 35, 2

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

- ESP32 development board (ESP32-WROOM-32D)
- Arduino IDE or PlatformIO
- Bluetooth-enabled mouse
- MSX computer system (recommended)

### Firmware Installation

1. Open the Arduino IDE (or PlatformIO)
2. Select the ESP32-WROOM-32D board under Tools → Board
3. Upload the desired firmware (e.g. ESP32_MSX_v004.ino)
4. Start setup through serial interface at 115200 baud

## Operation

The ESP32 simulates the MSX mouse interface by directly manipulating GPIO pins. Input data from BLE mice is transmitted in real-time to MSX.

### Activation methods

- **BOOT button**: Hold 3 seconds to start web interface, 6 seconds to stop
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

The web interface allows comfortable remote configuration through a web browser:

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

The system automatically scans for HID mouse devices. It prefers the strongest connection based on RSSI value.

## Development Status

- **Version 0.04**: Detailed comments, corrected OTA functionality, without NVS storage

## License

This project is licensed under the MIT License - see the LICENSE file for details.

## Contribution

Contributors are welcome! Please send a pull request with improvements or report issues in the GitHub issues tracker.

## Acknowledgments

- NimBLE library for BLE support
- Arduino for ESP32 support
- MSX Community for protocol definitions

## Contact

If you have questions or need assistance:

- Create an issue on GitHub
- Send an email to the maintainer
- Join the MSX community (if available)

---

*This project is designed for use on MSX computer systems and was developed to support historical computer emulation.*