#include <Wire.h>
#include <RTClib.h>
#include <Encoder.h>
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>

// Pin Definitions
#define DIR_PIN D5      // Direction pin for stepper motor
#define STEP_PIN D6     // Step pin for stepper motor
#define SDA_PIN D4      // I2C SDA pin for RTC
#define SCL_PIN D3      // I2C SCL pin for RTC
#define ENCODER_CLK D2  // Encoder clock pin
#define ENCODER_DT D1   // Encoder data pin
#define ENCODER_BTN D7  // Encoder button pin

// Motor settings
#define STEPS_PER_REVOLUTION 200
#define MICROSTEPS 16
#define MAX_SPEED 2000          // Slower maximum speed for reliability
#define MIN_SPEED 4000          // Slower minimum speed for reliability
#define STEP_DELAY 100          // Base delay between steps in microseconds

// EEPROM addresses
#define EEPROM_SIZE 512
#define MIN_POS_ADDR 0
#define MAX_POS_ADDR 4
#define CURRENT_POS_ADDR 8
#define ALARM_COUNT_ADDR 12
#define ALARM_DATA_START 16     // Each alarm takes 3 bytes (hour, minute, action)

// Blind states
#define BLIND_UP 0
#define BLIND_DOWN 1
#define BLIND_CUSTOM 2

// WiFi settings
//Old
const char* ssid = "Feldtfam";
const char* password = "Nectarine03";
//New 
// const char* ssid = "New Stadium Guests";
// const char* password = "combo#2please";

// Global objects
RTC_DS3231 rtc;
Encoder myEncoder(ENCODER_CLK, ENCODER_DT);
ESP8266WebServer server(80);

// Global variables
long currentPosition = 0;
long minPosition = 0;
long maxPosition = 1000;
int blindState = BLIND_UP;
bool setupMode = false;
bool btnPressed = false;
unsigned long lastEncoderMove = 0;
unsigned long lastBtnCheck = 0;
unsigned long lastTimeCheck = 0;

// Add these global variables after other global variables
unsigned long lastStepTime = 0;
bool isMoving = false;
long targetPosition = 0;
int stepDelay = 1000; // Default step delay in microseconds

// Add after other global variables
bool isConnected = true;

// Alarm structure
struct Alarm {
  uint8_t hour;
  uint8_t minute;
  uint8_t action; // 0 = up, 1 = down
  bool enabled;
};

#define MAX_ALARMS 10
Alarm alarms[MAX_ALARMS];
int alarmCount = 0;

// Add these global variables
bool webSetupMode = false;
long setupMinPosition = 0;
long setupMaxPosition = 1000;

void setup() {
  // Initialize serial for debugging
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nSmart Blinds Controller Starting...");
  
  // Initialize EEPROM
  EEPROM.begin(EEPROM_SIZE);
  
  // Initialize I2C for RTC
  Wire.begin(SDA_PIN, SCL_PIN);
  
  // Initialize RTC
  if (!rtc.begin()) {
    Serial.println("Couldn't find RTC");
    while (1);
  }

  // If RTC lost power, set the time
  if (rtc.lostPower()) {
    Serial.println("RTC lost power, setting the time!");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
  
  // Set pin modes
  pinMode(STEP_PIN, OUTPUT);
  pinMode(DIR_PIN, OUTPUT);
  pinMode(ENCODER_BTN, INPUT_PULLUP);
  
  // Load saved values from EEPROM
  loadSettingsFromEEPROM();
  
  // Initialize WiFi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    
    // Initialize mDNS
    if (MDNS.begin("blinds")) {
      Serial.println("mDNS responder started");
      Serial.println("You can now access the device at http://blinds.local");
      
      // Add service to mDNS
      MDNS.addService("http", "tcp", 80);
    } else {
      Serial.println("Error setting up mDNS responder!");
    }
    
    // Setup web server routes
    setupWebServer();
  } else {
    Serial.println("\nWiFi connection failed");
  }
  
  // Check if button is pressed during startup for setup mode
  if (digitalRead(ENCODER_BTN) == LOW) {
    Serial.println("Button pressed at startup - entering setup mode");
    delay(2000); // Wait to ensure it's a deliberate press
    if (digitalRead(ENCODER_BTN) == LOW) {
      enterSetupMode();
    }
  }
  
  Serial.println("System Ready");
}

void loop() {
  // Handle mDNS updates
  MDNS.update();
  
  // Handle web server requests
  server.handleClient();
  
  // Update motor position
  updateMotor();
  
  // Check encoder for manual control
  checkEncoder();
  
  // Check button presses
  checkButton();
  
  // Check if any alarms need to be triggered
  if (millis() - lastTimeCheck > 30000) {
    checkAlarms();
    lastTimeCheck = millis();
  }
  
  // Handle setup mode if active
  if (setupMode) {
    handleSetupMode();
  }
  
  // Yield to prevent watchdog timer issues
  yield();
}

void loadSettingsFromEEPROM() {
  // Read min and max positions
  EEPROM.get(MIN_POS_ADDR, minPosition);
  EEPROM.get(MAX_POS_ADDR, maxPosition);
  EEPROM.get(CURRENT_POS_ADDR, currentPosition);
  
  // Read alarm count
  EEPROM.get(ALARM_COUNT_ADDR, alarmCount);
  
  // Sanity check on loaded values
  if (alarmCount < 0 || alarmCount > MAX_ALARMS) {
    alarmCount = 0;
  }
  
  if (minPosition > maxPosition || minPosition < -10000 || maxPosition > 10000) {
    // Default values if EEPROM has invalid data
    minPosition = 0;
    maxPosition = 1000;
    currentPosition = 0;
  }
  
  // Load all alarms
  for (int i = 0; i < alarmCount; i++) {
    int addr = ALARM_DATA_START + (i * 4); // 4 bytes per alarm
    alarms[i].hour = EEPROM.read(addr);
    alarms[i].minute = EEPROM.read(addr + 1);
    alarms[i].action = EEPROM.read(addr + 2);
    alarms[i].enabled = EEPROM.read(addr + 3) == 1;
    
    // Validate values
    if (alarms[i].hour > 23 || alarms[i].minute > 59 || alarms[i].action > 1) {
      // Invalid alarm data, disable it
      alarms[i].enabled = false;
    }
  }
  
  // Initialize encoder with current position
  myEncoder.write(currentPosition);
  
  Serial.println("Settings loaded from EEPROM:");
  Serial.print("Min Position: ");
  Serial.println(minPosition);
  Serial.print("Max Position: ");
  Serial.println(maxPosition);
  Serial.print("Current Position: ");
  Serial.println(currentPosition);
  Serial.print("Alarm Count: ");
  Serial.println(alarmCount);
}

void saveSettingsToEEPROM() {
  EEPROM.put(MIN_POS_ADDR, minPosition);
  EEPROM.put(MAX_POS_ADDR, maxPosition);
  EEPROM.put(CURRENT_POS_ADDR, currentPosition);
  EEPROM.put(ALARM_COUNT_ADDR, alarmCount);
  
  // Save all alarms
  for (int i = 0; i < alarmCount; i++) {
    int addr = ALARM_DATA_START + (i * 4);
    EEPROM.write(addr, alarms[i].hour);
    EEPROM.write(addr + 1, alarms[i].minute);
    EEPROM.write(addr + 2, alarms[i].action);
    EEPROM.write(addr + 3, alarms[i].enabled ? 1 : 0);
  }
  
  EEPROM.commit();
  Serial.println("Settings saved to EEPROM");
}

void checkEncoder() {
  long newPosition = myEncoder.read();
  
  // If position has changed
  if (newPosition != currentPosition) {
    lastEncoderMove = millis();
    
    // Limit position to min/max
    if (newPosition < minPosition) newPosition = minPosition;
    if (newPosition > maxPosition) newPosition = maxPosition;
    
    // If position has really changed after limits
    if (newPosition != currentPosition) {
      Serial.print("Moving to position: ");
      Serial.println(newPosition);
      
      moveBlindToPosition(newPosition);
      currentPosition = newPosition;
      
      // Save current position to EEPROM
      EEPROM.put(CURRENT_POS_ADDR, currentPosition);
      EEPROM.commit();
      
      // Update blind state
      if (currentPosition == minPosition) {
        blindState = BLIND_UP;
      } else if (currentPosition == maxPosition) {
        blindState = BLIND_DOWN;
      } else {
        blindState = BLIND_CUSTOM;
      }
    }
  }
}

void checkButton() {
  // Debounce button
  if (millis() - lastBtnCheck < 50) return;
  lastBtnCheck = millis();
  
  bool currentBtnState = digitalRead(ENCODER_BTN) == LOW;
  
  if (currentBtnState && !btnPressed) {
    btnPressed = true;
    Serial.println("Button pressed");
    
    // If not in setup mode, toggle between up and down
    if (!setupMode) {
      if (blindState == BLIND_UP) {
        moveBlindDown();
      } else {
        moveBlindUp();
      }
    }
  } else if (!currentBtnState && btnPressed) {
    btnPressed = false;
    Serial.println("Button released");
  }
}

void moveBlindToPosition(long newTargetPosition) {
  targetPosition = newTargetPosition;
  isMoving = true;
  
  // Set direction
  bool direction = targetPosition > currentPosition;
  digitalWrite(DIR_PIN, direction ? HIGH : LOW);
  
  // Set initial speed
  stepDelay = MAX_SPEED;
  
  // Small delay to ensure direction is set
  delayMicroseconds(100);
}

void updateMotor() {
  if (!isMoving) return;
  
  unsigned long currentTime = micros();
  
  // Check if it's time for the next step
  if (currentTime - lastStepTime >= stepDelay) {
    lastStepTime = currentTime;
    
    // Take a single step with proper timing
    digitalWrite(STEP_PIN, HIGH);
    delayMicroseconds(STEP_DELAY); // Fixed delay for reliable stepping
    digitalWrite(STEP_PIN, LOW);
    
    // Update position
    if (targetPosition > currentPosition) {
      currentPosition++;
    } else if (targetPosition < currentPosition) {
      currentPosition--;
    }
    
    // Update encoder position to match motor position
    myEncoder.write(currentPosition);
    
    // Check if we've reached the target
    if (currentPosition == targetPosition) {
      isMoving = false;
      // Save current position to EEPROM
      EEPROM.put(CURRENT_POS_ADDR, currentPosition);
      EEPROM.commit();
      
      // Update blind state
      if (currentPosition == minPosition) {
        blindState = BLIND_UP;
      } else if (currentPosition == maxPosition) {
        blindState = BLIND_DOWN;
      } else {
        blindState = BLIND_CUSTOM;
      }
    }
    
    // Simpler speed control
    if (isMoving) {
      long stepsRemaining = abs(targetPosition - currentPosition);
      if (stepsRemaining > 100) {
        stepDelay = MAX_SPEED;
      } else if (stepsRemaining < 20) {
        stepDelay = MIN_SPEED;
      } else {
        stepDelay = (MAX_SPEED + MIN_SPEED) / 2;
      }
    }
  }
}

void moveBlindUp() {
  Serial.println("Moving blind up");
  moveBlindToPosition(minPosition);
}

void moveBlindDown() {
  Serial.println("Moving blind down");
  moveBlindToPosition(maxPosition);
}

void enterSetupMode() {
  setupMode = true;
  Serial.println("Entering setup mode");
  Serial.println("Use encoder to set MIN position, then press button");
  
  // Reset encoder
  currentPosition = 0;
  myEncoder.write(0);
}

void handleSetupMode() {
  if (server.hasArg("action")) {
    String action = server.arg("action");
    
    if (action == "start") {
      webSetupMode = true;
      setupMinPosition = minPosition;
      setupMaxPosition = maxPosition;
      server.send(200, "text/plain", "Setup mode started");
    } 
    else if (action == "stop") {
      webSetupMode = false;
      server.send(200, "text/plain", "Setup mode stopped");
    }
    else if (action == "save") {
      if (server.hasArg("min") && server.hasArg("max")) {
        setupMinPosition = server.arg("min").toInt();
        setupMaxPosition = server.arg("max").toInt();
        
        // Validate values
        if (setupMinPosition >= setupMaxPosition) {
          server.send(400, "text/plain", "Invalid values: min must be less than max");
          return;
        }
        
        // Save to EEPROM
        minPosition = setupMinPosition;
        maxPosition = setupMaxPosition;
        EEPROM.put(MIN_POS_ADDR, minPosition);
        EEPROM.put(MAX_POS_ADDR, maxPosition);
        EEPROM.commit();
        
        webSetupMode = false;
        server.send(200, "text/plain", "Settings saved");
      } else {
        server.send(400, "text/plain", "Missing parameters");
      }
    }
    else if (action == "status") {
      String json = "{";
      json += "\"setupMode\":" + String(webSetupMode ? "true" : "false") + ",";
      json += "\"minPosition\":" + String(setupMinPosition) + ",";
      json += "\"maxPosition\":" + String(setupMaxPosition);
      json += "}";
      server.send(200, "application/json", json);
    }
  } else {
    server.send(400, "text/plain", "Missing action parameter");
  }
}

void moveStepperBySteps(long steps) {
  // Set direction
  bool direction = steps > 0;
  digitalWrite(DIR_PIN, direction ? HIGH : LOW);
  
  // Calculate absolute steps
  long absSteps = abs(steps);
  
  // Move motor
  for (long i = 0; i < absSteps; i++) {
    digitalWrite(STEP_PIN, HIGH);
    delayMicroseconds(1000);
    digitalWrite(STEP_PIN, LOW);
    delayMicroseconds(1000);
  }
}

void checkAlarms() {
  DateTime now = rtc.now();
  
  // Debug print current time
  Serial.print("Current time: ");
  Serial.print(now.hour());
  Serial.print(":");
  if (now.minute() < 10) Serial.print("0");
  Serial.println(now.minute());
  
  // Check each alarm
  for (int i = 0; i < alarmCount; i++) {
    if (alarms[i].enabled && 
        alarms[i].hour == now.hour() && 
        alarms[i].minute == now.minute()) {
      
      Serial.print("Alarm triggered: ");
      Serial.print(alarms[i].hour);
      Serial.print(":");
      if (alarms[i].minute < 10) Serial.print("0");
      Serial.print(alarms[i].minute);
      Serial.print(" - Action: ");
      Serial.println(alarms[i].action == 0 ? "UP" : "DOWN");
      
      // Perform action
      if (alarms[i].action == 0) {
        moveBlindUp();
      } else {
        moveBlindDown();
      }
    }
  }
}

void setupWebServer() {
  // Root page
  server.on("/", HTTP_GET, handleRoot);
  
  // API endpoints
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/moveup", HTTP_GET, handleMoveUp);
  server.on("/api/movedown", HTTP_GET, handleMoveDown);
  server.on("/api/moveto", HTTP_GET, handleMoveTo);
  server.on("/api/alarms", HTTP_GET, handleGetAlarms);
  server.on("/api/alarms", HTTP_POST, handleSetAlarms);
  server.on("/api/time", HTTP_GET, handleGetTime);
  server.on("/api/time", HTTP_POST, handleSetTime);
  server.on("/api/setup", HTTP_GET, handleSetupMode);  // New endpoint
  
  // Start server
  server.begin();
  Serial.println("HTTP server started");
}

void handleRoot() {
  String html = "<!DOCTYPE html>\n";
  html += "<html>\n";
  html += "<head>\n";
  html += "  <title>Smart Blinds Controller</title>\n";
  html += "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
  html += "  <style>\n";
  html += "    body { font-family: Arial, sans-serif; margin: 0; padding: 20px; }\n";
  html += "    h1 { color: #333; }\n";
  html += "    .btn { background-color: #4CAF50; border: none; color: white; padding: 15px 32px; text-align: center; text-decoration: none; display: inline-block; font-size: 16px; margin: 4px 2px; cursor: pointer; }\n";
  html += "    .btn-up { background-color: #2196F3; }\n";
  html += "    .btn-down { background-color: #f44336; }\n";
  html += "    .slider { width: 100%; }\n";
  html += "    .alarm-item { margin-bottom: 10px; border: 1px solid #ddd; padding: 10px; }\n";
  html += "    .setup-mode { background-color: #ffeb3b; padding: 10px; margin: 10px 0; }\n";
  html += "    .setup-mode input { width: 100px; margin: 0 10px; }\n";
  html += "    .position-display { background-color: #e3f2fd; padding: 20px; margin: 20px 0; border-radius: 5px; text-align: center; }\n";
  html += "    .position-display h2 { margin: 0; color: #1976d2; }\n";
  html += "    .position-display p { margin: 10px 0; font-size: 18px; }\n";
  html += "    .position-bar { width: 100%; height: 20px; background-color: #e0e0e0; border-radius: 10px; margin: 10px 0; }\n";
  html += "    .position-fill { height: 100%; background-color: #4CAF50; border-radius: 10px; transition: width 0.3s; }\n";
  html += "    .percentage-controls { margin: 20px 0; }\n";
  html += "    .button-group { display: flex; justify-content: space-between; gap: 10px; }\n";
  html += "    .percentage-btn { flex: 1; padding: 15px; font-size: 16px; }\n";
  html += "  </style>\n";
  html += "</head>\n";
  html += "<body>\n";
  html += "  <h1>Smart Blinds Controller</h1>\n";
  
  // Position Display Section
  html += "  <div class=\"position-display\">\n";
  html += "    <h2>Current Position</h2>\n";
  html += "    <p id=\"positionText\">Loading...</p>\n";
  html += "    <div class=\"position-bar\">\n";
  html += "      <div class=\"position-fill\" id=\"positionFill\" style=\"width: 0%\"></div>\n";
  html += "    </div>\n";
  html += "    <p id=\"positionDetails\">Loading details...</p>\n";
  html += "  </div>\n";
  
  // Replace slider and up/down buttons with percentage buttons
  html += "  <div class=\"percentage-controls\">\n";
  html += "    <h2>Move Blinds</h2>\n";
  html += "    <div class=\"button-group\">\n";
  html += "      <button class=\"btn percentage-btn\" onclick=\"moveToPercentage(0)\">0%</button>\n";
  html += "      <button class=\"btn percentage-btn\" onclick=\"moveToPercentage(25)\">25%</button>\n";
  html += "      <button class=\"btn percentage-btn\" onclick=\"moveToPercentage(50)\">50%</button>\n";
  html += "      <button class=\"btn percentage-btn\" onclick=\"moveToPercentage(75)\">75%</button>\n";
  html += "      <button class=\"btn percentage-btn\" onclick=\"moveToPercentage(100)\">100%</button>\n";
  html += "    </div>\n";
  html += "  </div>\n";
  
  // Rest of the existing HTML...
  html += "  <div class=\"setup-mode\">\n";
  html += "    <h2>Setup Mode</h2>\n";
  html += "    <div id=\"setupStatus\">Loading setup status...</div>\n";
  html += "    <div id=\"setupControls\" style=\"display: none;\">\n";
  html += "      <p>Set the minimum and maximum positions:</p>\n";
  html += "      <input type=\"number\" id=\"minPosition\" placeholder=\"Min Position\">\n";
  html += "      <input type=\"number\" id=\"maxPosition\" placeholder=\"Max Position\">\n";
  html += "      <button class=\"btn\" onclick=\"saveSetup()\">Save Settings</button>\n";
  html += "      <button class=\"btn\" onclick=\"stopSetup()\">Cancel</button>\n";
  html += "    </div>\n";
  html += "    <button class=\"btn\" id=\"startSetupBtn\" onclick=\"startSetup()\">Enter Setup Mode</button>\n";
  html += "  </div>\n";
  
  html += "  <div>\n";
  html += "    <button class=\"btn btn-up\" onclick=\"moveUp()\">Move Up</button>\n";
  html += "    <button class=\"btn btn-down\" onclick=\"moveDown()\">Move Down</button>\n";
  html += "  </div>\n";
  
  html += "  <div>\n";
  html += "    <h2>Manual Position</h2>\n";
  html += "    <input type=\"range\" min=\"0\" max=\"100\" value=\"0\" class=\"slider\" id=\"positionSlider\">\n";
  html += "    <button class=\"btn\" onclick=\"setPosition()\">Set Position</button>\n";
  html += "  </div>\n";
  
  // Add improved JavaScript for position updates
  html += "  <script>\n";
  html += "    let isConnected = true;\n";
  html += "    let retryCount = 0;\n";
  html += "    const MAX_RETRIES = 5;\n";
  html += "    const POLL_INTERVAL = 500; // Poll every 500ms\n";
  html += "    \n";
  html += "    // Load initial data\n";
  html += "    window.onload = function() {\n";
  html += "      fetchStatus();\n";
  html += "      fetchSetupStatus();\n";
  // Start polling
  html += "      startPolling();\n";
  html += "    };\n";
  html += "    \n";
  html += "    function startPolling() {\n";
  html += "      setInterval(fetchStatus, POLL_INTERVAL);\n";
  html += "    }\n";
  html += "    \n";
  html += "    function fetchStatus() {\n";
  html += "      if (!isConnected) return;\n";
  html += "      \n";
  html += "      fetch('/api/status')\n";
  html += "        .then(response => {\n";
  html += "          if (!response.ok) {\n";
  html += "            throw new Error('Network response was not ok');\n";
  html += "          }\n";
  html += "          return response.json();\n";
  html += "        })\n";
  html += "        .then(data => {\n";
  html += "          retryCount = 0; // Reset retry count on success\n";
  html += "          updateUI(data);\n";
  html += "        })\n";
  html += "        .catch(error => {\n";
  html += "          console.error('Error fetching status:', error);\n";
  html += "          retryCount++;\n";
  html += "          if (retryCount >= MAX_RETRIES) {\n";
  html += "            isConnected = false;\n";
  html += "            showConnectionError();\n";
  // Try to reconnect after 5 seconds
  html += "            setTimeout(reconnect, 5000);\n";
  html += "          }\n";
  html += "        });\n";
  html += "    }\n";
  html += "    \n";
  html += "    function updateUI(data) {\n";
  // Update position text
  html += "      document.getElementById('positionText').innerHTML = \n";
  html += "        'Position: ' + data.position + ' (' + data.percentage.toFixed(1) + '%)';\n";
  html += "      \n";
  // Update position bar
  html += "      document.getElementById('positionFill').style.width = data.percentage + '%';\n";
  html += "      \n";
  // Update position details
  html += "      let stateText = data.state == 0 ? 'Up' : data.state == 1 ? 'Down' : 'Custom';\n";
  html += "      let movingText = data.isMoving ? ' (Moving)' : '';\n";
  html += "      document.getElementById('positionDetails').innerHTML = \n";
  html += "        'State: ' + stateText + movingText + '<br>' +\n";
  html += "        'Min: ' + data.minPos + ' | Max: ' + data.maxPos;\n";
  html += "      \n";
  // Update slider
  html += "      document.getElementById('positionSlider').value = data.percentage;\n";
  html += "    }\n";
  html += "    \n";
  html += "    function showConnectionError() {\n";
  html += "      document.getElementById('positionText').innerHTML = 'Connection lost. Reconnecting...';\n";
  html += "      document.getElementById('positionDetails').innerHTML = 'Please wait...';\n";
  html += "    }\n";
  html += "    \n";
  html += "    function reconnect() {\n";
  html += "      isConnected = true;\n";
  html += "      retryCount = 0;\n";
  html += "      fetchStatus();\n";
  html += "    }\n";
  
  // Add setup mode functions
  html += "    function startSetup() {\n";
  html += "      fetch('/api/setup?action=start')\n";
  html += "        .then(response => response.text())\n";
  html += "        .then(() => {\n";
  html += "          document.getElementById('setupControls').style.display = 'block';\n";
  html += "          document.getElementById('startSetupBtn').style.display = 'none';\n";
  html += "          fetchSetupStatus();\n";
  html += "        })\n";
  html += "        .catch(error => console.error('Error starting setup:', error));\n";
  html += "    }\n";
  html += "    \n";
  html += "    function stopSetup() {\n";
  html += "      fetch('/api/setup?action=stop')\n";
  html += "        .then(response => response.text())\n";
  html += "        .then(() => {\n";
  html += "          document.getElementById('setupControls').style.display = 'none';\n";
  html += "          document.getElementById('startSetupBtn').style.display = 'block';\n";
  html += "          fetchSetupStatus();\n";
  html += "        })\n";
  html += "        .catch(error => console.error('Error stopping setup:', error));\n";
  html += "    }\n";
  html += "    \n";
  html += "    function saveSetup() {\n";
  html += "      const minPos = document.getElementById('minPosition').value;\n";
  html += "      const maxPos = document.getElementById('maxPosition').value;\n";
  html += "      \n";
  html += "      fetch('/api/setup?action=save&min=' + minPos + '&max=' + maxPos)\n";
  html += "        .then(response => response.text())\n";
  html += "        .then(() => {\n";
  html += "          document.getElementById('setupControls').style.display = 'none';\n";
  html += "          document.getElementById('startSetupBtn').style.display = 'block';\n";
  html += "          fetchSetupStatus();\n";
  html += "          fetchStatus(); // Refresh position display\n";
  html += "        })\n";
  html += "        .catch(error => console.error('Error saving setup:', error));\n";
  html += "    }\n";
  html += "    \n";
  html += "    function fetchSetupStatus() {\n";
  html += "      fetch('/api/setup?action=status')\n";
  html += "        .then(response => response.json())\n";
  html += "        .then(data => {\n";
  html += "          document.getElementById('setupStatus').innerHTML = \n";
  html += "            'Setup Mode: ' + (data.setupMode ? 'Active' : 'Inactive') + '<br>' +\n";
  html += "            'Min Position: ' + data.minPosition + '<br>' +\n";
  html += "            'Max Position: ' + data.maxPosition;\n";
  html += "          \n";
  html += "          if (data.setupMode) {\n";
  html += "            document.getElementById('setupControls').style.display = 'block';\n";
  html += "            document.getElementById('startSetupBtn').style.display = 'none';\n";
  html += "            document.getElementById('minPosition').value = data.minPosition;\n";
  html += "            document.getElementById('maxPosition').value = data.maxPosition;\n";
  html += "          } else {\n";
  html += "            document.getElementById('setupControls').style.display = 'none';\n";
  html += "            document.getElementById('startSetupBtn').style.display = 'block';\n";
  html += "          }\n";
  html += "        })\n";
  html += "        .catch(error => console.error('Error fetching setup status:', error));\n";
  html += "    }\n";
  
  // Add move functions
  html += "    function moveUp() {\n";
  html += "      fetch('/api/moveup')\n";
  html += "        .then(response => response.text())\n";
  html += "        .then(() => {\n";
  html += "          console.log('Moving up');\n";
  html += "          fetchStatus(); // Refresh position display\n";
  html += "        })\n";
  html += "        .catch(error => console.error('Error moving up:', error));\n";
  html += "    }\n";
  html += "    \n";
  html += "    function moveDown() {\n";
  html += "      fetch('/api/movedown')\n";
  html += "        .then(response => response.text())\n";
  html += "        .then(() => {\n";
  html += "          console.log('Moving down');\n";
  html += "          fetchStatus(); // Refresh position display\n";
  html += "        })\n";
  html += "        .catch(error => console.error('Error moving down:', error));\n";
  html += "    }\n";
  
  // Add JavaScript for the new function
  html += "    function moveToPercentage(percentage) {\n";
  html += "      fetch('/api/moveto?percentage=' + percentage)\n";
  html += "        .then(response => response.text())\n";
  html += "        .then(() => {\n";
  html += "          console.log('Moving to ' + percentage + '%');\n";
  html += "          fetchStatus(); // Refresh position display\n";
  html += "        })\n";
  html += "        .catch(error => console.error('Error moving to percentage:', error));\n";
  html += "    }\n";
  
  // Rest of the existing JavaScript...
  html += "  </script>\n";
  html += "</body>\n";
  html += "</html>\n";
  
  server.send(200, "text/html", html);
}

void handleStatus() {
  String json = "{";
  json += "\"position\":" + String(currentPosition) + ",";
  json += "\"minPos\":" + String(minPosition) + ",";
  json += "\"maxPos\":" + String(maxPosition) + ",";
  json += "\"state\":" + String(blindState) + ",";
  json += "\"isMoving\":" + String(isMoving ? "true" : "false") + ",";
  json += "\"targetPosition\":" + String(targetPosition) + ",";
  json += "\"percentage\":" + String((currentPosition - minPosition) * 100 / (maxPosition - minPosition));
  json += "}";
  
  server.send(200, "application/json", json);
}

void handleMoveUp() {
  moveBlindUp();
  server.send(200, "text/plain", "Moving up");
}

void handleMoveDown() {
  moveBlindDown();
  server.send(200, "text/plain", "Moving down");
}

void handleMoveTo() {
  if (server.hasArg("percentage")) {
    int percentage = server.arg("percentage").toInt();
    
    // Validate percentage
    if (percentage < 0 || percentage > 100) {
      server.send(400, "text/plain", "Invalid percentage value");
      return;
    }
    
    // Calculate position based on percentage
    long range = maxPosition - minPosition;
    long newTargetPosition = minPosition + (range * percentage / 100);
    Serial.print("Moving to position percentage: ");
    Serial.println(newTargetPosition);
    
    moveBlindToPosition(newTargetPosition);
    server.send(200, "text/plain", "Moving to position");
  } else {
    server.send(400, "text/plain", "Missing percentage parameter");
  }
}

void handleGetAlarms() {
  String json = "{\"alarms\":[";
  
  for (int i = 0; i < alarmCount; i++) {
    if (i > 0) json += ",";
    
    json += "{";
    json += "\"hour\":" + String(alarms[i].hour) + ",";
    json += "\"minute\":" + String(alarms[i].minute) + ",";
    json += "\"action\":" + String(alarms[i].action) + ",";
    json += "\"enabled\":" + String(alarms[i].enabled ? "true" : "false");
    json += "}";
  }
  
  json += "]}";
  
  server.send(200, "application/json", json);
}

void handleSetAlarms() {
  if (server.hasArg("plain")) {
    String json = server.arg("plain");
    
    // Extract the action type
    int actionStartIndex = json.indexOf("\"action\":\"") + 10;
    int actionEndIndex = json.indexOf("\"", actionStartIndex);
    String action = json.substring(actionStartIndex, actionEndIndex);
    
    if (action == "add") {
      // Add new alarm
      if (alarmCount >= MAX_ALARMS) {
        server.send(400, "text/plain", "Maximum number of alarms reached");
        return;
      }
      
      // Extract hour
      int hourStartIndex = json.indexOf("\"hour\":") + 7;
      int hourEndIndex = json.indexOf(",", hourStartIndex);
      int hour = json.substring(hourStartIndex, hourEndIndex).toInt();
      
      // Extract minute
      int minuteStartIndex = json.indexOf("\"minute\":") + 9;
      int minuteEndIndex = json.indexOf(",", minuteStartIndex);
      int minute = json.substring(minuteStartIndex, minuteEndIndex).toInt();
      
      // Extract alarm action
      int alarmActionStartIndex = json.indexOf("\"alarmAction\":") + 13;
      int alarmActionEndIndex = json.indexOf(",", alarmActionStartIndex);
      if (alarmActionEndIndex == -1) {
        alarmActionEndIndex = json.indexOf("}", alarmActionStartIndex);
      }
      int alarmAction = json.substring(alarmActionStartIndex, alarmActionEndIndex).toInt();
      
      // Create new alarm
      alarms[alarmCount].hour = hour;
      alarms[alarmCount].minute = minute;
      alarms[alarmCount].action = alarmAction;
      alarms[alarmCount].enabled = true;
      
      alarmCount++;
      
      // Save to EEPROM
      saveSettingsToEEPROM();
      
      server.send(200, "text/plain", "Alarm added");
    } 
    else if (action == "delete") {
      // Delete alarm
      int indexStartIndex = json.indexOf("\"index\":") + 8;
      int indexEndIndex = json.indexOf("}", indexStartIndex);
      int index = json.substring(indexStartIndex, indexEndIndex).toInt();
      
      if (index < 0 || index >= alarmCount) {
        server.send(400, "text/plain", "Invalid alarm index");
        return;
      }
      
      // Remove alarm by shifting all alarms after it
      for (int i = index; i < alarmCount - 1; i++) {
        alarms[i] = alarms[i + 1];
      }
      
      alarmCount--;
      
      // Save to EEPROM
      saveSettingsToEEPROM();
      
      server.send(200, "text/plain", "Alarm deleted");
    } 
    else if (action == "toggle") {
      // Toggle alarm enabled state
      int indexStartIndex = json.indexOf("\"index\":") + 8;
      int indexEndIndex = json.indexOf("}", indexStartIndex);
      int index = json.substring(indexStartIndex, indexEndIndex).toInt();
      
      if (index < 0 || index >= alarmCount) {
        server.send(400, "text/plain", "Invalid alarm index");
        return;
      }
      
      // Toggle enabled state
      alarms[index].enabled = !alarms[index].enabled;
      
      // Save to EEPROM
      saveSettingsToEEPROM();
      
      server.send(200, "text/plain", "Alarm toggled");
    } 
    else {
      server.send(400, "text/plain", "Unknown action");
    }
  } else {
    server.send(400, "text/plain", "Missing request body");
  }
}

void handleGetTime() {
  DateTime now = rtc.now();
  
  String dateTimeStr = String(now.year()) + "-";
  dateTimeStr += String(now.month() < 10 ? "0" : "") + String(now.month()) + "-";
  dateTimeStr += String(now.day() < 10 ? "0" : "") + String(now.day()) + " ";
  dateTimeStr += String(now.hour() < 10 ? "0" : "") + String(now.hour()) + ":";
  dateTimeStr += String(now.minute() < 10 ? "0" : "") + String(now.minute()) + ":";
  dateTimeStr += String(now.second() < 10 ? "0" : "") + String(now.second());
  
  String json = "{\"datetime\":\"" + dateTimeStr + "\"}";
  
  server.send(200, "application/json", json);
}

void handleSetTime() {
  if (server.hasArg("plain")) {
    String json = server.arg("plain");
    
    // Extract datetime string
    int dtStartIndex = json.indexOf("\"datetime\":\"") + 12;
    int dtEndIndex = json.indexOf("\"", dtStartIndex);
    String dateTimeStr = json.substring(dtStartIndex, dtEndIndex);
    
    // Parse datetime string (format: YYYY-MM-DDTHH:MM)
    int year = dateTimeStr.substring(0, 4).toInt();
    int month = dateTimeStr.substring(5, 7).toInt();
    int day = dateTimeStr.substring(8, 10).toInt();
    int hour = dateTimeStr.substring(11, 13).toInt();
    int minute = dateTimeStr.substring(14, 16).toInt();
    
    // Set RTC time
    rtc.adjust(DateTime(year, month, day, hour, minute, 0));
    
    server.send(200, "text/plain", "Time set");
  } else {
    server.send(400, "text/plain", "Missing request body");
  }
}