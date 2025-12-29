#include <M5StickCPlus2.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <SocketIOclient.h>
#include <Arduino_JSON.h>
#include <WiFiManager.h>
#include <Preferences.h>

// Configuration
// Debug Mode
// Set to true to enable verbose logging (device_states events, etc.)
// Set to false for minimal logging (only important events)
#define DEBUG_MODE false

// LED Control
// M5StickC Plus 2 has a built-in red LED on GPIO10
// Set to true to turn on the LED when on program bus
// Set to false to keep LED off (LED is active LOW, so LOW = on, HIGH = off)
#define LED_ON_PROGRAM true

// TallyArbiter Server Configuration (loaded from preferences)
char tallyarbiter_host[40] = "192.168.0.110";  // TallyArbiter server IP
char tallyarbiter_port[6] = "4455";            // TallyArbiter server port

// Generate unique client UUID (stored in preferences in production)
String clientUUID = "m5stickc-plus2-" + String((uint32_t)ESP.getEfuseMac(), HEX);

// WiFi and Preferences
WiFiManager wm;
Preferences preferences;
bool networkConnected = false;

// Screen and brightness control
int currentScreen = 0;  // 0 = Tally Screen, 1 = Settings Screen
int currentBrightness = 11;  // Start at 11 (max is 12 for M5StickC Plus 2)
int startBrightness = 11;
int maxBrightness = 12;

// Button handling - using M5StickC Plus 2 built-in buttons
// M5StickC Plus 2 buttons: M5.BtnA (GPIO37) and M5.BtnB (GPIO39)

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
void connectToNetwork();
String getParam(String name);
void saveParamCallback();
bool isValidIP(String ip);
bool isValidPort(String port);
void initPower();
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
    
    Serial.println("Starting M5StickC Plus 2 Tally Device");
    
    // Disable WiFi sleep mode early to prevent connection issues
    WiFi.setSleep(false);
    
    // Initialize power management
    initPower();
    delay(100);
    
    // Initialize display
    M5.Lcd.setRotation(3);  // 270 degrees
    M5.Lcd.fillScreen(TFT_BLACK);
    M5.Lcd.setCursor(4, 50);
    M5.Lcd.setTextSize(3);
    M5.Lcd.setTextColor(TFT_DARKGREY);
    M5.Lcd.print("Booting...");
    
    // Initialize LED
    pinMode(10, OUTPUT);
    digitalWrite(10, HIGH);  // LED off (active LOW)
    updateLED();
    
    // Load preferences for TallyArbiter server settings
    preferences.begin("tally-arbiter", false);
    if (preferences.getString("taHost").length() > 0) {
        String newHost = preferences.getString("taHost");
        Serial.print("Setting TallyArbiter host as ");
        Serial.println(newHost);
        newHost.toCharArray(tallyarbiter_host, 40);
    }
    if (preferences.getString("taPort").length() > 0) {
        String newPort = preferences.getString("taPort");
        Serial.print("Setting TallyArbiter port as ");
        Serial.println(newPort);
        newPort.toCharArray(tallyarbiter_port, 6);
    }
    preferences.end();
    
    // Setup WiFi using WiFiManager
    connectToNetwork();
    
    // Wait for network connection
    while (!networkConnected) {
        delay(200);
    }
    
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
    
    // Handle button B (Action) - change brightness on click, reset WiFi on hold
    // Check if button B is held for 5 seconds to reset WiFi configuration
    if (M5.BtnB.pressedFor(5)) {
        Serial.println("WiFi reset triggered - button B held for 5 seconds");
        wm.resetSettings();
        ESP.restart();
    }
    
    // Handle button B click - change brightness
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

void connectToNetwork() {
    WiFi.mode(WIFI_STA);  // explicitly set mode, esp defaults to STA+AP
    
    // Disable WiFi power saving to prevent connection issues
    WiFi.setSleep(false);
    
    // Set hostname
    wm.setHostname((const char *)clientUUID.c_str());
    
    // Add custom parameters for TallyArbiter server settings
    WiFiManagerParameter custom_taServer("taHostIP", "Tally Arbiter Server", tallyarbiter_host, 40);
    WiFiManagerParameter custom_taPort("taHostPort", "Port", tallyarbiter_port, 6);
    
    wm.addParameter(&custom_taServer);
    wm.addParameter(&custom_taPort);
    
    wm.setSaveParamsCallback(saveParamCallback);
    
    // Custom menu via array or vector
    std::vector<const char *> menu = {"wifi", "param", "info", "sep", "restart", "exit"};
    wm.setMenu(menu);
    
    // Set dark theme
    wm.setClass("invert");
    
    wm.setConfigPortalTimeout(120);  // auto close configportal after n seconds
    
    bool res;
    res = wm.autoConnect(clientUUID.c_str());  // AP name for setup
    
    if (!res) {
        Serial.println("Failed to connect");
        M5.Lcd.fillScreen(TFT_BLACK);
        M5.Lcd.setCursor(4, 50);
        M5.Lcd.setTextSize(2);
        M5.Lcd.setTextColor(TFT_RED);
        M5.Lcd.println("WiFi Failed");
    } else {
        // If you get here you have connected to the WiFi
        Serial.println("WiFi connected!");
        Serial.print("IP: ");
        Serial.println(WiFi.localIP());
        networkConnected = true;
    }
}

String getParam(String name) {
    // Read parameter from server, for custom HTML input
    String value;
    if (wm.server->hasArg(name)) {
        value = wm.server->arg(name);
    }
    return value;
}

bool isValidIP(String ip) {
    // Check if not empty after trimming whitespace
    ip.trim();
    return ip.length() > 0;
}

bool isValidPort(String port) {
    // Check if not empty
    if (port.length() == 0) {
        return false;
    }
    
    // Check if all characters are digits
    for (int i = 0; i < port.length(); i++) {
        if (!isDigit(port.charAt(i))) {
            return false;
        }
    }
    
    // Check if port is in valid range (1-65535)
    int portNum = port.toInt();
    if (portNum < 1 || portNum > 65535) {
        return false;
    }
    
    return true;
}

void saveParamCallback() {
    Serial.println("[CALLBACK] saveParamCallback fired");
    String str_taHost = getParam("taHostIP");
    String str_taPort = getParam("taHostPort");
    
    // Trim whitespace
    str_taHost.trim();
    str_taPort.trim();
    
    // Validate IP address (must not be empty)
    if (!isValidIP(str_taHost)) {
        Serial.println("ERROR: IP address field cannot be empty");
        Serial.println("Keeping existing host configuration");
        return;  // Don't save invalid values
    }
    
    // Validate port
    if (!isValidPort(str_taPort)) {
        Serial.println("ERROR: Invalid port format: " + str_taPort);
        Serial.println("Port must be a number between 1 and 65535");
        Serial.println("Keeping existing port configuration");
        return;  // Don't save invalid values
    }
    
    Serial.print("Saving new TallyArbiter host: ");
    Serial.println(str_taHost);
    Serial.print("Saving new TallyArbiter port: ");
    Serial.println(str_taPort);
    
    preferences.begin("tally-arbiter", false);
    preferences.putString("taHost", str_taHost);
    preferences.putString("taPort", str_taPort);
    preferences.end();
    
    // Update the current values
    str_taHost.toCharArray(tallyarbiter_host, 40);
    str_taPort.toCharArray(tallyarbiter_port, 6);
    
    // Redirect to the info/main page after saving
    if (wm.server) {
        wm.server->sendHeader("Location", "/", true);
        wm.server->send(302, "text/plain", "");  // 302 redirect
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
    
    // Flash LED twice to indicate connection
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
    // Flash screen white three times
    for (int i = 0; i < 3; i++) {
        M5.Lcd.fillScreen(TFT_WHITE);
        delay(500);
        M5.Lcd.fillScreen(TFT_BLACK);
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
        M5.Lcd.setTextSize(3);
        
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
            M5.Lcd.setCursor(4, 50);
            M5.Lcd.print(state.deviceName);
            
            // Update LED based on bus type
            updateLED();
            
            // Display update confirmed
        } else {
            // No active tally - black screen, grey text
            M5.Lcd.fillScreen(TFT_BLACK);
            M5.Lcd.setTextColor(TFT_DARKGREY);
            M5.Lcd.setCursor(4, 50);
            M5.Lcd.print(state.deviceName);
            updateLED();
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
    // M5StickC Plus 2 has a built-in red LED on GPIO10 (active LOW)
#if LED_ON_PROGRAM
    // Remove quotes from type string
    String typeStr = state.actualType;
    typeStr.replace("\"", "");
    
    if (typeStr == "program") {
        digitalWrite(10, LOW);  // LED on (active LOW)
    } else {
        digitalWrite(10, HIGH);  // LED off
    }
#else
    // LED disabled - keep it off
    digitalWrite(10, HIGH);  // LED off
#endif
}

void initPower() {
    // M5StickC Plus 2 uses AXP2101 power management
    // M5.begin() should initialize this, but we can set backlight brightness manually
    // The M5StickC Plus 2 library handles power management initialization
    setBacklight(12);  // Set initial brightness
}

void setBacklight(uint8_t brightness) {
    if (brightness > 12) brightness = 12;
    
    // M5StickC Plus 2 backlight control
    // M5StickCPlus2 library uses M5Unified API which may not have direct brightness control
    // For now, we'll skip brightness control as it's not critical
    // TODO: Implement brightness control if M5StickCPlus2 library supports it
    // Possible methods to try:
    // - M5.Power.setBrightness(brightness)
    // - M5.Display.setBrightness(brightness)
    // - Or use GPIO control if available
}

void showSettings() {
    currentScreen = 1;
    Serial.println("Showing settings screen");
    
    // Clear screen and configure for settings
    M5.Lcd.fillScreen(TFT_BLACK);
    M5.Lcd.setCursor(4, 4);
    M5.Lcd.setTextSize(1.5);
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
    M5.Lcd.setCursor(4, 50);
    M5.Lcd.setTextSize(3);
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
    // M5StickC Plus 2 battery voltage calculation
    // M5StickCPlus2 library uses M5Unified API with M5.Power
    // Battery voltage range: ~3.0V (empty) to ~4.2V (full)
    // Note: The exact voltage range may vary - adjust as needed
    
    // Try M5Unified Power API
    // getBatteryVoltage() typically returns voltage in mV, so divide by 1000 to get volts
    float voltage = M5.Power.getBatteryVoltage() / 1000.0;
    
    // If voltage seems too high (likely already in volts), use as-is
    if (voltage > 10.0) {
        voltage = M5.Power.getBatteryVoltage();  // Already in volts
    }
    
    int level = floor(100.0 * ((voltage - 3.0) / (4.2 - 3.0)));
    
    // Clamp to 0-100
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    
    return level;
}

bool isCharging() {
    // Check if device is charging
    // M5StickC Plus 2 uses M5Unified API with M5.Power
    // Check charging status - positive current typically means charging
    float chargeCurrent = M5.Power.getBatteryCurrent();
    // Charging is indicated by positive current
    return chargeCurrent > 0;
}

