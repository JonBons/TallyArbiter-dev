# M5StickC Tally - Arduino Framework Project

This is an Arduino framework project for the M5StickC tally device, using Socket.IO for direct TallyArbiter protocol communication.

## Features

- WiFi connection with auto-reconnect
- Socket.IO client for direct TallyArbiter integration (no MQTT needed)
- ST7735 display with black background and colored text
- AXP192 power management for backlight control
- Button A support for test mode (blue screen)
- Built-in LED control (red LED for program state)
- Device reassignment support
- Flash command support

## Building with PlatformIO

1. Install PlatformIO (VS Code extension or standalone)
2. Open the project directory in PlatformIO
3. Configure WiFi and MQTT settings in `src/main.ino`:
   - Edit `ssid` and `password` for WiFi
   - Edit `mqtt_server`, `mqtt_port`, `mqtt_user`, and `mqtt_password` for MQTT
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
     - M5StickC
     - PubSubClient
     - ArduinoJson
4. Select board: Tools → Board → ESP32 Arduino → M5Stick-C
5. Open `src/main.ino` in Arduino IDE
6. Configure WiFi and MQTT settings in the code
7. Upload to device

## Configuration

Before building, configure these settings in `src/main.ino`:

1. **WiFi credentials:**
   ```cpp
   const char* ssid = "YOUR_WIFI_SSID";
   const char* password = "YOUR_WIFI_PASSWORD";
   ```

2. **MQTT broker settings:**
   ```cpp
   const char* mqtt_server = "YOUR_MQTT_BROKER";
   const int mqtt_port = 1883;
   const char* mqtt_user = "YOUR_MQTT_USERNAME";
   const char* mqtt_password = "YOUR_MQTT_PASSWORD";
   ```

3. **TallyArbiter server settings:**
   ```cpp
   const char* tallyarbiter_host = "192.168.1.100";  // TallyArbiter server IP
   const char* tallyarbiter_port = "4455";           // TallyArbiter server port
   ```
   
   Note: The device ID will be assigned by the TallyArbiter server. You can set it to "null" initially or use a previously assigned ID.

## Usage

- **Button A**: Press to toggle test mode (blue screen)
- **Display**: Shows device name, tally state (PROG/PREV/OFF), and MQTT status
- **LED**: Red LED turns on when in program state

## Hardware

- M5StickC board
- ST7735 display (80x160)
- AXP192 power management
- Built-in red LED on GPIO10
- Button A on GPIO37

## Dependencies

- M5StickC library (for hardware abstraction)
- WebSockets library (for Socket.IO client)
- ArduinoJson (for JSON parsing)

## Protocol

This implementation uses the Socket.IO protocol directly, following the same pattern as the blink1-listener.py:

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

- The M5StickC library handles most hardware initialization
- WiFi and TallyArbiter server settings are currently hardcoded. For production, consider using Preferences or a web configuration interface
- The device ID starts as "null" and will be assigned by the server, or you can use a previously assigned ID
- The code uses ArduinoJson for parsing Socket.IO event data

