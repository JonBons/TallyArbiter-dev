#include <M5StickC.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <SocketIOclient.h>
#include <Arduino_JSON.h>

// Configuration
// Debug Mode
// Set to true to enable verbose logging (device_states events, etc.)
// Set to false for minimal logging (only important events)
#define DEBUG_MODE false

// LED Control
// Set to true to turn on the built-in red LED (GPIO10) when on program bus
// Set to false to keep LED off (LED is active LOW, so LOW = on, HIGH = off)
#define LED_ON_PROGRAM true

// WiFi Configuration (TODO: Load from preferences)
const char* ssid = "WIFI_SSID";
const char* password = "WIFI_PASS";

// TallyArbiter Server Configuration (TODO: Load from preferences)
const char* tallyarbiter_host = "10.0.2.91";  // TallyArbiter server IP
const char* tallyarbiter_port = "4455";           // TallyArbiter server port

// Generate unique client UUID (stored in preferences in production)
String clientUUID = "m5stickc-" + String((uint32_t)ESP.getEfuseMac(), HEX);

// Screen and brightness control
int currentScreen = 0;  // 0 = Tally Screen, 1 = Settings Screen
int currentBrightness = 11;  // Start at 11 (max is 12 for M5StickC)
int startBrightness = 11;
int maxBrightness = 12;

// Button handling - using M5StickC built-in buttons
// M5StickC buttons: M5.BtnA (GPIO37) and M5.BtnB (GPIO39)

// Global state
struct TallyState {
    String deviceId;
    String deviceName;
    bool socket_connected;  // Socket.IO connection status
    String actualType;    // Current tally type (program/preview/aux)
    String actualColor;   // Current tally color
    int actualPriority;   // Current tally priority
    String prevType;      // Previous type to reduce flicker
} state = {
    .deviceId = "unassigned",  // Will be assigned by server
    .deviceName = "Unassigned",
    .socket_connected = false,
    .actualType = "",
    .actualColor = "",
    .actualPriority = 0,
    .prevType = ""
};

// Socket.IO client
SocketIOclient socket;

// Device states and bus options from server
JSONVar DeviceStates;
JSONVar BusOptions;
JSONVar Devices;

// Forward declarations
void updateDisplay();
void evaluateMode();
void updateLED();
void processTallyData();
void connectToServer();
void setupWiFi();
void initAXP192();
void setBacklight(uint8_t brightness);
void socket_event(socketIOmessageType_t type, uint8_t * payload, size_t length);
void socket_Connected(const char * payload, size_t length);
void socket_Disconnected();
void socket_Reassign(String content);
void socket_Flash();
String getBusById(String busId);
String getBusTypeById(String busId);
String getBusColorById(String busId);
int getBusPriorityById(String busId);
void SetDeviceName();
uint8_t hexToRgb(String hex, int index);  // index: 0=R, 1=G, 2=B
void showSettings();
void showDeviceInfo();
void updateBrightness();
int getBatteryLevel();
bool isCharging();

void setup() {
    Serial.begin(115200);
    M5.begin();
    
    Serial.println("Starting M5StickC Tally Device");
    
    // Disable WiFi sleep mode early to prevent connection issues
    WiFi.setSleep(false);
    
    // Initialize AXP192 (backlight)
    initAXP192();
    delay(100);
    
    // Initialize display
    M5.Lcd.setRotation(3);  // 270 degrees
    M5.Lcd.fillScreen(TFT_BLACK);
    M5.Lcd.setCursor(4, 30);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(TFT_DARKGREY);
    M5.Lcd.print("Booting...");
    
    // Initialize LED
    pinMode(10, OUTPUT);
    digitalWrite(10, HIGH);  // LED off (active LOW)
    updateLED();
    
    // Setup WiFi
    setupWiFi();
    
    // Setup Socket.IO
    socket.onEvent(socket_event);
    socket.begin(tallyarbiter_host, atoi(tallyarbiter_port));
    
    Serial.println("Setup complete");
    
    // Initial display - show settings screen first
    showSettings();
}

void loop() {
    // Handle Socket.IO - must be called frequently
    socket.loop();
    
    // Update button states (must be called to update button state)
    M5.update();
    
    // Handle button A (M5) - switch screens
    // wasPressed() returns true once per button press
    if (M5.BtnA.wasPressed()) {
        switch (currentScreen) {
            case 0:
                showSettings();
                break;
            case 1:
                showDeviceInfo();
                break;
        }
    }
    
    // Handle button B (Action) - change brightness
    if (M5.BtnB.wasPressed()) {
        updateBrightness();
    }
    
    // Periodically ensure WiFi is not sleeping
    // This prevents WiFi from entering power save mode
    static unsigned long lastWiFiCheck = 0;
    unsigned long now = millis();
    if (now - lastWiFiCheck > 10000) {  // Check every 10 seconds
        if (WiFi.status() == WL_CONNECTED) {
            WiFi.setSleep(false);  // Keep WiFi awake
        }
        lastWiFiCheck = now;
    }
    
    // Small delay to prevent watchdog issues, but keep it minimal
    delay(1);
}

void setupWiFi() {
    Serial.print("Connecting to WiFi: ");
    Serial.println(ssid);
    
    // Disable WiFi power saving to prevent connection issues
    WiFi.setSleep(false);
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(".");  // Keep dots on same line
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println();
        Serial.print("WiFi connected! IP: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println();
        Serial.println("WiFi connection failed!");
    }
}

void socket_event(socketIOmessageType_t type, uint8_t * payload, size_t length) {
    switch (type) {
        case sIOtype_CONNECT:
            socket_Connected((char*)payload, length);
            break;
            
        case sIOtype_DISCONNECT:
            socket_Disconnected();
            break;
            
        case sIOtype_EVENT: {
            String eventMsg = (char*)payload;
            String eventType = "";
            String eventContent = "";
            
            // Parse Socket.IO event format: ["eventname",{data}]
            eventType = eventMsg.substring(2, eventMsg.indexOf("\"", 2));
            eventContent = eventMsg.substring(eventType.length() + 4);
            eventContent.remove(eventContent.length() - 1);  // Remove trailing ]
            
            // Log events only in debug mode
#if DEBUG_MODE
            if (eventType == "device_states") {
                Serial.print("Got device_states (");
                Serial.print(eventContent.length());
                Serial.println(" bytes)");
            } else {
                Serial.print("Got event: ");
                Serial.println(eventType);
            }
#endif
            
            if (eventType == "bus_options") {
                BusOptions = JSON.parse(eventContent);
            } else if (eventType == "device_states") {
                // Store and process immediately - but skip if state hasn't changed
                DeviceStates = JSON.parse(eventContent);
                
                // Process immediately - the state comparison in processTallyData will skip duplicates
                processTallyData();
                
                // Only log occasionally in debug mode to reduce noise
#if DEBUG_MODE
                static unsigned long lastLoggedEventTime = 0;
                static int eventCount = 0;
                eventCount++;
                unsigned long now = millis();
                if (now - lastLoggedEventTime > 2000) {
                    Serial.print("Got device_states (");
                    Serial.print(eventContent.length());
                    Serial.print(" bytes, ");
                    Serial.print(eventCount);
                    Serial.println(" events)");
                    eventCount = 0;
                    lastLoggedEventTime = now;
                }
#endif
            } else if (eventType == "devices") {
                Devices = JSON.parse(eventContent);
                SetDeviceName();
            } else if (eventType == "deviceId") {
                state.deviceId = eventContent.substring(1, eventContent.length() - 1);  // Remove quotes
                Serial.print("Assigned device ID: ");
                Serial.println(state.deviceId);
                SetDeviceName();
                showDeviceInfo();
            } else if (eventType == "reassign") {
                socket_Reassign(eventContent);
            } else if (eventType == "flash") {
                socket_Flash();
            } else if (eventType == "error") {
                Serial.print("Error from server: ");
                Serial.println(eventContent);
            }
            break;
        }
        
        default:
            break;
    }
}

void socket_Connected(const char * payload, size_t length) {
    Serial.println("Connected to Tally Arbiter server");
#if DEBUG_MODE
    Serial.print("DeviceId: ");
    Serial.println(state.deviceId);
#endif
    state.socket_connected = true;
    
    // Emit listenerclient_connect - match reference format
    String deviceObj = "{\"deviceId\": \"" + state.deviceId + 
                       "\", \"listenerType\": \"" + clientUUID + 
                       "\", \"canBeReassigned\": true, \"canBeFlashed\": true, \"supportsChat\": false }";
    String msg = "[\"listenerclient_connect\"," + deviceObj + "]";
#if DEBUG_MODE
    Serial.print("Sending connect message: ");
    Serial.println(msg);
#endif
    socket.sendEVENT(msg);
    
    // Flash green LED twice to indicate connection
    for (int i = 0; i < 2; i++) {
        digitalWrite(10, LOW);  // LED on
        delay(300);
        digitalWrite(10, HIGH);  // LED off
        delay(300);
    }
    
    evaluateMode();
}

void socket_Disconnected() {
    Serial.println("Disconnected from Tally Arbiter server");
    state.socket_connected = false;
    
    // Clear tally state
    state.actualType = "";
    state.actualColor = "";
    state.actualPriority = 0;
    state.prevType = "";  // Force display update
    
    // Flash LED to indicate disconnection
    digitalWrite(10, LOW);
    delay(300);
    digitalWrite(10, HIGH);
    delay(300);
    
    evaluateMode();
    
    // Attempt to reconnect after delay
    delay(5000);
    socket.begin(tallyarbiter_host, atoi(tallyarbiter_port));
}

void socket_Reassign(String content) {
    // Parse reassign message: ["oldDeviceId","newDeviceId","internalId"]
    // Reference uses substring parsing instead of JSON parse
    String oldDeviceId = content.substring(0, content.indexOf(','));
    String newDeviceId = content.substring(oldDeviceId.length() + 1);
    newDeviceId = newDeviceId.substring(0, newDeviceId.indexOf(','));
    
    // Remove quotes
    if (oldDeviceId[0] == '"') {
        oldDeviceId.remove(0, 1);
    }
    if (oldDeviceId.endsWith("\"")) {
        oldDeviceId.remove(oldDeviceId.length() - 1, 1);
    }
    if (newDeviceId[0] == '"') {
        newDeviceId.remove(0, 1);
    }
    if (newDeviceId.endsWith("\"")) {
        newDeviceId.remove(newDeviceId.length() - 1, 1);
    }
    
    Serial.print("Reassigning from ");
    Serial.print(oldDeviceId);
    Serial.print(" to ");
    Serial.println(newDeviceId);
    
    // Flash red screen (like reference)
    M5.Lcd.fillScreen(TFT_RED);
    delay(200);
    M5.Lcd.fillScreen(TFT_BLACK);
    delay(200);
    M5.Lcd.fillScreen(TFT_RED);
    delay(200);
    M5.Lcd.fillScreen(TFT_BLACK);
    
    state.deviceId = newDeviceId;
    
    // Emit listener_reassign_object - match reference format
    String reassignObj = "{\"oldDeviceId\": \"" + oldDeviceId + "\", \"newDeviceId\": \"" + newDeviceId + "\"}";
    String msg = "[\"listener_reassign_object\"," + reassignObj + "]";
    socket.sendEVENT(msg);
    
    // Request devices list
    socket.sendEVENT("[\"devices\"]");
    
    SetDeviceName();
    showDeviceInfo();
}

void socket_Flash() {
#if DEBUG_MODE
    Serial.println("Flash command received");
#endif
    // Flash LED and screen white three times
    for (int i = 0; i < 3; i++) {
        M5.Lcd.fillScreen(TFT_WHITE);
        digitalWrite(10, LOW);
        delay(500);
        M5.Lcd.fillScreen(TFT_BLACK);
        digitalWrite(10, HIGH);
        delay(500);
    }
    
    // Restore current screen
    if (currentScreen == 0) {
        showDeviceInfo();
    } else {
        showSettings();
    }
}

String getBusById(String busId) {
    for (int i = 0; i < (int)BusOptions.length(); i++) {
        if (JSON.stringify(BusOptions[i]["id"]) == busId) {
            return JSON.stringify(BusOptions[i]);
        }
    }
    return "{}";
}

String getBusTypeById(String busId) {
    for (int i = 0; i < (int)BusOptions.length(); i++) {
        if (JSON.stringify(BusOptions[i]["id"]) == busId) {
            return JSON.stringify(BusOptions[i]["type"]);
        }
    }
    return "invalid";
}

String getBusColorById(String busId) {
    for (int i = 0; i < (int)BusOptions.length(); i++) {
        if (JSON.stringify(BusOptions[i]["id"]) == busId) {
            return JSON.stringify(BusOptions[i]["color"]);
        }
    }
    return "invalid";
}

int getBusPriorityById(String busId) {
    for (int i = 0; i < (int)BusOptions.length(); i++) {
        if (JSON.stringify(BusOptions[i]["id"]) == busId) {
            String priorityStr = JSON.stringify(BusOptions[i]["priority"]);
            return priorityStr.toInt();
        }
    }
    return 0;
}

void SetDeviceName() {
    for (int i = 0; i < (int)Devices.length(); i++) {
        // Match reference: compare with quotes
        if (JSON.stringify(Devices[i]["id"]) == "\"" + state.deviceId + "\"") {
            String strDevice = JSON.stringify(Devices[i]["name"]);
            state.deviceName = strDevice.substring(1, strDevice.length() - 1);
#if DEBUG_MODE
            Serial.print("DeviceName: ");
            Serial.println(state.deviceName);
#endif
            break;
        }
    }
}

uint8_t hexToRgb(String hex, int index) {
    // hex format: "#RRGGBB", index: 0=R, 1=G, 2=B
    if (hex.length() != 7 || hex[0] != '#') return 0;
    int start = 1 + (index * 2);
    String hexByte = hex.substring(start, start + 2);
    return (uint8_t)strtol(hexByte.c_str(), NULL, 16);
}

void processTallyData() {
    // Process device states - find the one for our assigned device with highest priority
    bool typeChanged = false;
    
    // Only process if we have an assigned device
    if (state.deviceId == "unassigned" || state.deviceId == "") {
        if (state.actualType != "") {
            state.actualType = "";
            state.actualColor = "";
            state.actualPriority = 0;
            state.prevType = "";  // Force update
            evaluateMode();
        }
        return;
    }
    
    // Quick check: if DeviceStates is empty, clear tally
    if ((int)DeviceStates.length() == 0) {
        if (state.actualType != "") {
            state.actualType = "";
            state.actualColor = "";
            state.actualPriority = 0;
            state.prevType = "";  // Force update
            evaluateMode();
        }
        return;
    }
    
    // Find the highest priority active bus for our device
    String bestType = "";
    String bestColor = "";
    int bestPriority = -1;
    bool foundActive = false;
    bool foundDevice = false;
    
    // Loop through ALL device states (not just the first one!)
    for (int i = 0; i < (int)DeviceStates.length(); i++) {
        // Get device ID from this state
        String devIdStr = JSON.stringify(DeviceStates[i]["deviceId"]);
        if (devIdStr.length() > 2) {
            devIdStr = devIdStr.substring(1, devIdStr.length() - 1);  // Remove quotes
        }
        
        // Check if this state is for our device
        if (devIdStr == state.deviceId) {
            foundDevice = true;
            int sourcesLen = (int)DeviceStates[i]["sources"].length();
            
            // Check if this device state has sources (is active)
            if (sourcesLen > 0) {
                // Get bus info for this device state
                String busId = JSON.stringify(DeviceStates[i]["busId"]);
                int priority = getBusPriorityById(busId);
                
                // Keep track of the highest priority active bus
                if (priority > bestPriority) {
                    bestPriority = priority;
                    bestType = getBusTypeById(busId);
                    bestColor = getBusColorById(busId);
                    foundActive = true;
                }
            }
        }
    }
    
    // Debug output (only when state changes)
    static String lastDebugState = "";
    String currentDebugState = foundActive ? (bestType + bestColor) : "OFF";
    if (currentDebugState != lastDebugState) {
#if DEBUG_MODE
        if (foundDevice) {
            if (foundActive) {
                Serial.print("Tally: ");
                Serial.print(bestType);
                Serial.print(" (");
                Serial.print(bestColor);
                Serial.print(", prio:");
                Serial.print(bestPriority);
                Serial.println(")");
            } else {
                Serial.println("Tally: OFF");
            }
        } else {
            Serial.println("Tally: device not found");
        }
#endif
        lastDebugState = currentDebugState;
    }
    
    // Update state if we found an active bus
    if (foundActive) {
        // Only update if changed
        if (bestType != state.actualType || bestColor != state.actualColor) {
            state.actualType = bestType;
            state.actualColor = bestColor;
            state.actualPriority = bestPriority;
            typeChanged = true;
        }
    } else {
        // No active tally found - clear it
        if (state.actualType != "") {
            state.actualType = "";
            state.actualColor = "";
            state.actualPriority = 0;
            typeChanged = true;
        }
    }
    
    // Only update display if something changed
    if (typeChanged) {
        state.prevType = "";  // Force display update
        evaluateMode();
    }
}

void evaluateMode() {
    // Only update display if we're on the tally screen (screen 0)
    if (currentScreen != 0) {
        return;
    }
    
    // Only update display if type changed (reduce flicker)
    if (state.actualType != state.prevType) {
        // Configure display for evaluate mode
        M5.Lcd.setCursor(4, 30);
        M5.Lcd.setTextSize(2);
        
        // Remove quotes from type and color strings
        String typeStr = state.actualType;
        typeStr.replace("\"", "");
        String colorStr = state.actualColor;
        colorStr.replace("\"", "");
        
        if (typeStr != "" && typeStr != "invalid") {
            // Parse color (remove #)
            colorStr.replace("#", "");
            
            // Convert hex to RGB
            long number = (long)strtol(colorStr.c_str(), NULL, 16);
            int r = (number >> 16) & 0xFF;
            int g = (number >> 8) & 0xFF;
            int b = number & 0xFF;
            
            // Fill screen with bus color, show device name in black text
            uint16_t color565 = M5.Lcd.color565(r, g, b);
            M5.Lcd.fillScreen(color565);
            M5.Lcd.setTextColor(TFT_BLACK);
            M5.Lcd.setCursor(4, 30);
            M5.Lcd.print(state.deviceName);
            
            // Update LED based on bus type
#if LED_ON_PROGRAM
            if (typeStr == "program") {
                digitalWrite(10, LOW);  // LED on (active LOW)
            } else {
                digitalWrite(10, HIGH);  // LED off
            }
#else
            // LED disabled - keep it off
            digitalWrite(10, HIGH);  // LED off
#endif
            
            // Display update confirmed
        } else {
            // No active tally - black screen, grey text
            M5.Lcd.fillScreen(TFT_BLACK);
            M5.Lcd.setTextColor(TFT_DARKGREY);
            M5.Lcd.setCursor(4, 30);
            M5.Lcd.print(state.deviceName);
            digitalWrite(10, HIGH);  // LED off
            // Display: OFF (no log needed, already logged in processTallyData)
        }
        
        state.prevType = state.actualType;
    }
}

void updateDisplay() {
    // Legacy function - now just calls evaluateMode
    evaluateMode();
}

void updateLED() {
    // LED is updated in processTallyData()
    // This function kept for compatibility
}

void initAXP192() {
    // M5StickC uses M5.begin() which initializes AXP192
    // But we can set backlight brightness manually
    Wire.beginTransmission(0x34);
    Wire.write(0x28);  // LDO2/3 voltage setting register
    Wire.write(0xCC);  // Enable LDO2, set to 3.3V
    Wire.endTransmission();
    delay(10);
    
    Wire.beginTransmission(0x34);
    Wire.write(0x12);  // Power output control register
    Wire.write(0x4D);  // Enable LDO2
    Wire.endTransmission();
    delay(10);
    
    setBacklight(12);  // Set initial brightness
}

void setBacklight(uint8_t brightness) {
    if (brightness > 12) brightness = 12;
    
    Wire.beginTransmission(0x34);
    Wire.write(0x91);  // Backlight control register
    Wire.write(brightness);
    Wire.endTransmission();
}

void showSettings() {
    currentScreen = 1;
    Serial.println("Showing settings screen");
    
    // Clear screen and configure for settings
    M5.Lcd.fillScreen(TFT_BLACK);
    M5.Lcd.setCursor(0, 0);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    
    // Display WiFi info
    M5.Lcd.println("SSID: " + String(WiFi.SSID()));
    M5.Lcd.println(WiFi.localIP().toString());
    M5.Lcd.println();
    
    // Display TallyArbiter server info
    M5.Lcd.println("Tally Arbiter:");
    M5.Lcd.println(String(tallyarbiter_host) + ":" + String(tallyarbiter_port));
    M5.Lcd.println();
    
    // Display connection status
    if (state.socket_connected) {
        M5.Lcd.setTextColor(TFT_GREEN, TFT_BLACK);
        M5.Lcd.println("Connected");
    } else {
        M5.Lcd.setTextColor(TFT_RED, TFT_BLACK);
        M5.Lcd.println("Disconnected");
    }
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Lcd.println();
    
    // Display battery info
    M5.Lcd.print("Battery: ");
    int batteryLevel = getBatteryLevel();
    if (isCharging()) {
        M5.Lcd.setTextColor(TFT_GREEN, TFT_BLACK);
        M5.Lcd.println("Charging...");
    } else {
        // Color based on battery level
        if (batteryLevel > 50) {
            M5.Lcd.setTextColor(TFT_GREEN, TFT_BLACK);
        } else if (batteryLevel > 20) {
            M5.Lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
        } else {
            M5.Lcd.setTextColor(TFT_RED, TFT_BLACK);
        }
        M5.Lcd.println(String(batteryLevel) + "%");
    }
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
}

void showDeviceInfo() {
    currentScreen = 0;
    Serial.println("Showing device info screen");
    
    // Clear screen and configure for device info
    M5.Lcd.fillScreen(TFT_BLACK);
    M5.Lcd.setCursor(4, 30);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
    M5.Lcd.println(state.deviceName);
    
    // Update tally display
    evaluateMode();
}

void updateBrightness() {
    Serial.print("Brightness: ");
    
    // Cycle brightness: 11 -> 12 -> 11
    if (currentBrightness >= maxBrightness) {
        currentBrightness = startBrightness;
    } else {
        currentBrightness = maxBrightness;
    }
    
    Serial.println(currentBrightness);
    setBacklight(currentBrightness);
}

int getBatteryLevel() {
    // M5StickC battery voltage calculation
    // Battery voltage range: ~3.0V (empty) to ~4.07V (full)
    float voltage = M5.Axp.GetBatVoltage();
    int level = floor(100.0 * ((voltage - 3.0) / (4.07 - 3.0)));
    
    // Clamp to 0-100
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    
    return level;
}

bool isCharging() {
    // Check if device is charging via AXP power management
    float chargeCurrent = M5.Axp.GetVBusCurrent();
    return chargeCurrent > 0;
}
