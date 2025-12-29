# M5StickC Plus 2 Tally - Arduino Framework Project

This is an Arduino framework project for the M5StickC Plus 2 tally device, using Socket.IO for direct TallyArbiter protocol communication.

## Features

- WiFi connection with auto-reconnect
- Socket.IO client for direct TallyArbiter integration (no MQTT needed)
- ST7789 display (240x135) with black background and colored text
- AXP2101 power management for backlight control
- Button A support for screen switching
- Button B support for brightness control
- Built-in red LED control (GPIO10) for program state indication
- Device reassignment support
- Flash command support

## Building with PlatformIO

1. Install PlatformIO (VS Code extension or standalone)
2. Open the project directory in PlatformIO
3. Configure WiFi and TallyArbiter server settings in `src/main.ino`:
   - Edit `ssid` and `password` for WiFi
   - Edit `tallyarbiter_host` and `tallyarbiter_port` for TallyArbiter server
4. Build and upload:
   ```bash
   pio run -t upload
   ```
5. Monitor serial output:
   ```bash
   pio device monitor
   ```

## Building with Arduino IDE

1. Install Arduino IDE
2. Install ESP32 board support:
   - File → Preferences → Additional Board Manager URLs
   - Add: `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
   - Tools → Board → Boards Manager → Search "ESP32" → Install
3. Install libraries:
   - Tools → Manage Libraries → Search and install:
     - M5StickCPlus2
     - WebSockets
     - ArduinoJson
4. Select board: Tools → Board → ESP32 Arduino → ESP32 Dev Module
5. Open `src/main.ino` in Arduino IDE
6. Configure WiFi and TallyArbiter server settings in the code
7. Upload to device

## Configuration

Before building, configure these settings in `src/main.ino`:

1. **WiFi credentials:**
   ```cpp
   const char* ssid = "YOUR_WIFI_SSID";
   const char* password = "YOUR_WIFI_PASSWORD";
   ```

2. **TallyArbiter server settings:**
   ```cpp
   const char* tallyarbiter_host = "192.168.1.100";  // TallyArbiter server IP
   const char* tallyarbiter_port = "4455";           // TallyArbiter server port
   ```
   
   Note: The device ID will be assigned by the TallyArbiter server. You can set it to "unassigned" initially or use a previously assigned ID.

## Usage

- **Button A**: Press to toggle between tally screen and settings screen
- **Button B**: Press to cycle brightness (11 → 12 → 11)
- **Display**: Shows device name, tally state (program/preview/aux), and connection status
- **Red LED**: Indicates program state (when enabled)

## Hardware

- M5StickC Plus 2 board
- ST7789 display (240x135 pixels)
- AXP2101 power management
- Built-in red LED on GPIO10
- Button A on GPIO37
- Button B on GPIO39

## Differences from M5StickC

The M5StickC Plus 2 has several improvements over the original M5StickC:

- **Larger display**: 240x135 vs 80x160 (rotated)
- **LED**: Built-in red LED (same as original M5StickC)
- **Power management**: AXP2101 instead of AXP192
- **More memory**: More RAM and flash storage
- **Better performance**: Faster processor

## Dependencies

- M5StickCPlus2 library (for hardware abstraction)
- WebSockets library (for Socket.IO client)
- ArduinoJson (for JSON parsing)

## Protocol

This implementation uses the Socket.IO protocol directly, following the same pattern as other TallyArbiter listeners:

1. **Connection**: Connects to TallyArbiter server via Socket.IO
2. **Registration**: Emits `listenerclient_connect` with device info
3. **Events**:
   - `device_states`: Array of device states with bus assignments
   - `bus_options`: Array of bus configurations with colors and priorities
   - `devices`: Array of all devices
   - `deviceId`: Assigned device ID from server
   - `reassign`: Device reassignment command
   - `flash`: Flash command for device identification
4. **Tally Processing**: Processes device_states to determine active bus based on priority

## Notes

- The M5StickC Plus 2 library handles most hardware initialization
- WiFi and TallyArbiter server settings are currently hardcoded. For production, consider using Preferences or a web configuration interface
- The device ID starts as "unassigned" and will be assigned by the server, or you can use a previously assigned ID
- The code uses ArduinoJson for parsing Socket.IO event data
- Red LED control uses simple GPIO on/off control (active LOW)
- Display rotation is set to 3 (270 degrees) for portrait orientation

