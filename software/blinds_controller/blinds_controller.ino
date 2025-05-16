#include <Wire.h>
#include <RTClib.h>
#include <Encoder.h>
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPClient.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

// Add watchdog timer
#include <ESP8266WiFiMulti.h>
ESP8266WiFiMulti wifiMulti;

// Server health check variables
unsigned long lastServerCheck = 0;
const unsigned long SERVER_CHECK_INTERVAL = 30000; // Check every 30 seconds
bool serverRunning = false;
int serverRestartCount = 0;
const int MAX_RESTARTS = 5;
const unsigned long RESTART_COOLDOWN = 300000; // 5 minutes between restart attempts
unsigned long lastRestartAttempt = 0;

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
#define MICROSTEPS 1  // Full steps for maximum torque
#define MAX_SPEED 8000          // Even slower maximum speed for maximum torque
#define MIN_SPEED 12000         // Even slower minimum speed for maximum torque
#define STEP_DELAY 1000         // Much longer base delay between steps for maximum torque
#define ENCODER_MULTIPLIER 200   // How many steps per encoder click

// EEPROM addresses
#define EEPROM_SIZE 512
#define MIN_POS_ADDR 0
#define MAX_POS_ADDR 4
#define CURRENT_POS_ADDR 8
#define ALARM_COUNT_ADDR 12
#define ALARM_DATA_START 16     // Each alarm takes 5 bytes (hour, minute, action, enabled, days)

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
  uint8_t days; // Bit field for days (0 = Sunday, 1 = Monday, etc.)
};

#define MAX_ALARMS 10
Alarm alarms[MAX_ALARMS];
int alarmCount = 0;

// Add these global variables
bool webSetupMode = false;
long setupMinPosition = 0;
long setupMaxPosition = 1000;

// Add after other global variables
bool isAPMode = false;
const char* AP_SSID = "blinds";
const char* AP_PASSWORD = ""; // You can change this password

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", -14400, 60000); // EDT timezone (-4 hours)

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
  
  // Initialize WiFi with multiple networks
  wifiMulti.addAP(ssid, password);
  // Add backup network if available
  // wifiMulti.addAP("backup_ssid", "backup_password");
  
  Serial.print("Connecting to WiFi");
  
  int attempts = 0;
  while (wifiMulti.run() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (wifiMulti.run() == WL_CONNECTED) {
    Serial.println("\nWiFi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    isAPMode = false;
    
    // Sync time from NTP on boot
    syncTimeFromNTP();
  } else {
    Serial.println("\nWiFi connection failed, starting Access Point mode");
    startAccessPoint();
  }
  
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
  serverRunning = true;
  
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
  
  // Check server health
  checkServerHealth();
  
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

void checkServerHealth() {
  if (millis() - lastServerCheck >= SERVER_CHECK_INTERVAL) {
    lastServerCheck = millis();
    
    // Try to make a request to the server
    HTTPClient http;
    WiFiClient client;
    String url = "http://" + WiFi.localIP().toString() + "/api/status";
    if (http.begin(client, url)) {
      int httpCode = http.GET();
      http.end();
      
      if (httpCode != 200) {
        Serial.println("Server health check failed. HTTP code: " + String(httpCode));
        serverRunning = false;
        
        // Check if we should attempt a restart
        if (serverRestartCount < MAX_RESTARTS && 
            (millis() - lastRestartAttempt) > RESTART_COOLDOWN) {
          restartServer();
        }
      } else {
        serverRunning = true;
        // Reset restart count if server is healthy
        serverRestartCount = 0;
      }
    } else {
      Serial.println("Failed to begin HTTP request");
      serverRunning = false;
    }
  }
}

void restartServer() {
  Serial.println("Attempting to restart server...");
  lastRestartAttempt = millis();
  serverRestartCount++;
  
  // Stop the current server
  server.stop();
  delay(1000);
  
  // Reinitialize WiFi if needed
  if (!isAPMode) {
    if (wifiMulti.run() != WL_CONNECTED) {
      Serial.println("Reconnecting to WiFi...");
      int attempts = 0;
      while (wifiMulti.run() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(".");
        attempts++;
      }
      
      // If still not connected, switch to AP mode
      if (wifiMulti.run() != WL_CONNECTED) {
        Serial.println("WiFi connection failed, switching to Access Point mode");
        startAccessPoint();
      }
    }
  }
  
  // Restart mDNS
  MDNS.end();
  if (MDNS.begin("blinds")) {
    MDNS.addService("http", "tcp", 80);
  }
  
  // Restart the server
  setupWebServer();
  server.begin();
  serverRunning = true;
  
  Serial.println("Server restart completed. Attempt " + String(serverRestartCount) + " of " + String(MAX_RESTARTS));
}

// Add a function to handle server errors
void handleServerError() {
  if (!serverRunning) {
    Serial.println("Server error detected, attempting recovery...");
    restartServer();
  }
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
    int addr = ALARM_DATA_START + (i * 5); // 5 bytes per alarm
    alarms[i].hour = EEPROM.read(addr);
    alarms[i].minute = EEPROM.read(addr + 1);
    alarms[i].action = EEPROM.read(addr + 2);
    alarms[i].enabled = EEPROM.read(addr + 3) == 1;
    alarms[i].days = EEPROM.read(addr + 4);
    
    // Validate values
    if (alarms[i].hour > 23 || alarms[i].minute > 59 || alarms[i].action > 1) {
      // Invalid alarm data, disable it
      alarms[i].enabled = false;
    }
  }
  
  // Initialize encoder with current position (at 1/ENCODER_MULTIPLIER scale)
  myEncoder.write(currentPosition / ENCODER_MULTIPLIER);
  
  Serial.println("Settings loaded from EEPROM:");
  Serial.print("Min Position: ");
  Serial.println(minPosition);
  Serial.print("Max Position: ");
  Serial.println(maxPosition);
  Serial.print("Current Position: ");
  Serial.println(currentPosition);
  Serial.print("Encoder Position: ");
  Serial.println(currentPosition / ENCODER_MULTIPLIER);
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
    int addr = ALARM_DATA_START + (i * 5); // 5 bytes per alarm (hour, minute, action, enabled, days)
    EEPROM.write(addr, alarms[i].hour);
    EEPROM.write(addr + 1, alarms[i].minute);
    EEPROM.write(addr + 2, alarms[i].action);
    EEPROM.write(addr + 3, alarms[i].enabled ? 1 : 0);
    EEPROM.write(addr + 4, alarms[i].days);
  }
  
  EEPROM.commit();
  Serial.println("Settings saved to EEPROM");
}

void checkEncoder() {
  long encoderPosition = myEncoder.read();
  long expectedEncoderPosition = currentPosition / ENCODER_MULTIPLIER;
  
  // If encoder position doesn't match expected position
  if (encoderPosition != expectedEncoderPosition) {
    lastEncoderMove = millis();
    
    // Calculate the difference in encoder clicks
    long encoderDifference = encoderPosition - expectedEncoderPosition;
    
    // Calculate the new target position
    long newTargetPosition = currentPosition + (encoderDifference * ENCODER_MULTIPLIER);
    
    // Limit position to min/max
    if (newTargetPosition < minPosition) {
      newTargetPosition = minPosition;
      // Update encoder to match the min position
      myEncoder.write(minPosition / ENCODER_MULTIPLIER);
    }
    if (newTargetPosition > maxPosition) {
      newTargetPosition = maxPosition;
      // Update encoder to match the max position
      myEncoder.write(maxPosition / ENCODER_MULTIPLIER);
    }
    
    // If position has really changed after limits
    if (newTargetPosition != currentPosition) {
      Serial.print("Encoder moved. Current: ");
      Serial.print(currentPosition);
      Serial.print(", Target: ");
      Serial.println(newTargetPosition);
      
      moveBlindToPosition(newTargetPosition);
      
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
    delayMicroseconds(STEP_DELAY);  // Much longer delay for maximum torque
    digitalWrite(STEP_PIN, LOW);
    
    // Update position
    if (targetPosition > currentPosition) {
      currentPosition++;
    } else if (targetPosition < currentPosition) {
      currentPosition--;
    }
    
    // Update encoder position to match motor position (at 1/ENCODER_MULTIPLIER scale)
    myEncoder.write(currentPosition / ENCODER_MULTIPLIER);
    
    // Debug output for motor movement
    if (currentPosition % 100 == 0) {  // Print every 100 steps
      Serial.print("Motor position: ");
      Serial.print(currentPosition);
      Serial.print(", Target: ");
      Serial.print(targetPosition);
      Serial.print(", Steps remaining: ");
      Serial.println(abs(targetPosition - currentPosition));
    }
    
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
    
    // Speed control with very gradual acceleration/deceleration
    if (isMoving) {
      long stepsRemaining = abs(targetPosition - currentPosition);
      if (stepsRemaining > 500) {  // Much longer acceleration zone
        stepDelay = MAX_SPEED;
      } else if (stepsRemaining < 200) {  // Much longer deceleration zone
        stepDelay = MIN_SPEED;
      } else {
        // Linear interpolation between MAX_SPEED and MIN_SPEED
        stepDelay = MAX_SPEED + ((MIN_SPEED - MAX_SPEED) * (500 - stepsRemaining) / 300);
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
  int currentDay = now.dayOfTheWeek(); // 0 = Sunday, 1 = Monday, etc.
  
  // Debug print current time
  Serial.print("Current time: ");
  Serial.print(now.day());
  Serial.print("/");
  Serial.print(now.month());
  Serial.print("/");
  Serial.print(now.year());
  Serial.print(" ");
  Serial.print(now.hour());
  Serial.print(":");
  if (now.minute() < 10) Serial.print("0");
  Serial.println(now.minute());
  
  // Check each alarm
  for (int i = 0; i < alarmCount; i++) {
    if (alarms[i].enabled && 
        alarms[i].hour == now.hour() && 
        alarms[i].minute == now.minute() &&
        (alarms[i].days & (1 << currentDay))) { // Check if alarm is set for current day
      
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
  server.on("/api/moveto", HTTP_GET, handleMoveTo);
  server.on("/api/alarms", HTTP_GET, handleGetAlarms);
  server.on("/api/alarms", HTTP_POST, handleSetAlarms);
  server.on("/api/time", HTTP_GET, handleGetTime);
  server.on("/api/time", HTTP_POST, handleSetTime);
  server.on("/api/setup", HTTP_GET, handleSetupMode);
  server.on("/api/sync-ntp", HTTP_GET, handleSyncTimeNTP);
  
  // Add error handling
  server.onNotFound([]() {
    handleServerError();
    server.send(404, "text/plain", "Not found");
  });
  
  // Start server
  server.begin();
  Serial.println("HTTP server started");
}

void handleRoot() {
  DateTime now = rtc.now();
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
  html += "    <div class=\"setup-controls\" style=\"margin-top: 20px; padding-top: 20px; border-top: 1px solid #ccc;\">\n";
  html += "      <h3>Position Limits</h3>\n";
  html += "      <div style=\"display: flex; align-items: center; gap: 10px; margin-bottom: 10px;\">\n";
  html += "        <label>Min Position:</label>\n";
  html += "        <input type=\"number\" id=\"minPosition\" style=\"width: 100px;\" value=\"0\">\n";
  html += "        <label>Max Position:</label>\n";
  html += "        <input type=\"number\" id=\"maxPosition\" style=\"width: 100px;\" value=\"1000\">\n";
  html += "        <button class=\"btn\" onclick=\"saveSetup()\">Save Limits</button>\n";
  html += "      </div>\n";
  html += "    </div>\n";
  html += "  </div>\n";
  
  // Percentage buttons
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
  
  // Add Alarms Section
  html += "  <div class=\"alarms-section\" style=\"margin-top: 20px; padding: 20px; background-color: #f5f5f5; border-radius: 5px;\">\n";
  html += "    <h2>Alarms</h2>\n";
  html += "<p>Current time: " + String(now.day()) + "/" + String(now.month()) + "/" + String(now.year()) + " " + String(now.hour()) + ":" + String(now.minute()) + "</p>\n";
  html += "    <div id=\"alarmsList\">Loading alarms...</div>\n";
  html += "    <div class=\"add-alarm\" style=\"margin-top: 20px;\">\n";
  html += "      <h3>Add New Alarm</h3>\n";
  html += "      <div style=\"display: grid; grid-template-columns: repeat(2, 1fr); gap: 10px;\">\n";
  html += "        <div>\n";
  html += "          <label>Time:</label>\n";
  html += "          <input type=\"time\" id=\"alarmTime\" style=\"width: 100%;\">\n";
  html += "        </div>\n";
  html += "        <div>\n";
  html += "          <label>Action:</label>\n";
  html += "          <select id=\"alarmAction\" style=\"width: 100%;\">\n";
  html += "            <option value=\"0\">Open Blinds</option>\n";
  html += "            <option value=\"1\">Close Blinds</option>\n";
  html += "          </select>\n";
  html += "        </div>\n";
  html += "        <div style=\"grid-column: span 2;\">\n";
  html += "          <label>Days:</label>\n";
  html += "          <div style=\"display: flex; gap: 10px; margin-top: 5px;\">\n";
  html += "            <label><input type=\"checkbox\" class=\"day-checkbox\" value=\"0\"> Sun</label>\n";
  html += "            <label><input type=\"checkbox\" class=\"day-checkbox\" value=\"1\"> Mon</label>\n";
  html += "            <label><input type=\"checkbox\" class=\"day-checkbox\" value=\"2\"> Tue</label>\n";
  html += "            <label><input type=\"checkbox\" class=\"day-checkbox\" value=\"3\"> Wed</label>\n";
  html += "            <label><input type=\"checkbox\" class=\"day-checkbox\" value=\"4\"> Thu</label>\n";
  html += "            <label><input type=\"checkbox\" class=\"day-checkbox\" value=\"5\"> Fri</label>\n";
  html += "            <label><input type=\"checkbox\" class=\"day-checkbox\" value=\"6\"> Sat</label>\n";
  html += "          </div>\n";
  html += "        </div>\n";
  html += "        <div style=\"grid-column: span 2;\">\n";
  html += "          <button class=\"btn\" onclick=\"addAlarm()\">Add Alarm</button>\n";
  html += "        </div>\n";
  html += "      </div>\n";
  html += "    </div>\n";
  html += "  </div>\n";
  
  // Add network status to the page
  html += "  <div style=\"margin-top: 20px; padding: 10px; background-color: #f8f9fa; border-radius: 5px;\">\n";
  html += "    <h3>Network Status</h3>\n";
  if (isAPMode) {
    html += "    <p>Mode: Access Point</p>\n";
    html += "    <p>SSID: " + String(AP_SSID) + "</p>\n";
    html += "    <p>Password: " + String(AP_PASSWORD) + "</p>\n";
    html += "    <p>IP Address: " + WiFi.softAPIP().toString() + "</p>\n";
  } else {
    html += "    <p>Mode: WiFi Client</p>\n";
    html += "    <p>Connected to: " + WiFi.SSID() + "</p>\n";
    html += "    <p>IP Address: " + WiFi.localIP().toString() + "</p>\n";
  }
  html += "  </div>\n";
  
  // Add time settings section
  html += "  <div style=\"margin-top: 20px; padding: 10px; background-color: #f0f4c3; border-radius: 5px;\">\n";
  html += "    <h3>Time Settings</h3>\n";
  html += "    <p>Current time: " + String(now.day()) + "/" + String(now.month()) + "/" + String(now.year()) + " " + 
           String(now.hour()) + ":" + (now.minute() < 10 ? "0" : "") + String(now.minute()) + "</p>\n";
  html += "    <div style=\"display: flex; flex-wrap: wrap; gap: 10px;\">\n";
  html += "      <div style=\"flex: 1;\">\n";
  html += "        <button class=\"btn\" onclick=\"syncTimeNTP()\"" + String(isAPMode ? " disabled" : "") + ">Sync with Internet Time</button>\n";
  html += "      </div>\n";
  html += "      <div style=\"flex: 1;\">\n";
  html += "        <label for=\"customTime\">Or set custom time:</label>\n";
  html += "        <input type=\"datetime-local\" id=\"customTime\" style=\"width: 100%;\">\n";
  html += "        <button class=\"btn\" onclick=\"setCustomTime()\" style=\"margin-top: 10px;\">Set Custom Time</button>\n";
  html += "      </div>\n";
  html += "    </div>\n";
  html += "  </div>\n";
  
  // Remove all setup mode related JavaScript
  html += "  <script>\n";
  html += "    let isConnected = true;\n";
  html += "    let retryCount = 0;\n";
  html += "    const MAX_RETRIES = 5;\n";
  html += "    const POLL_INTERVAL = 500; // Poll every 500ms\n";
  html += "    let isEditingMin = false;\n";
  html += "    let isEditingMax = false;\n";
  html += "    \n";
  html += "    // Load initial data\n";
  html += "    window.onload = function() {\n";
  html += "      fetchStatus();\n";
  html += "      fetchAlarms();\n";
  html += "      startPolling();\n";
  html += "      \n";
  html += "      // Add event listeners for input focus\n";
  html += "      document.getElementById('minPosition').addEventListener('focus', function() { isEditingMin = true; });\n";
  html += "      document.getElementById('minPosition').addEventListener('blur', function() { isEditingMin = false; });\n";
  html += "      document.getElementById('maxPosition').addEventListener('focus', function() { isEditingMax = true; });\n";
  html += "      document.getElementById('maxPosition').addEventListener('blur', function() { isEditingMax = false; });\n";
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
  html += "            setTimeout(reconnect, 5000);\n";
  html += "          }\n";
  html += "        });\n";
  html += "    }\n";
  html += "    \n";
  html += "    function updateUI(data) {\n";
  html += "      document.getElementById('positionText').innerHTML = \n";
  html += "        'Position: ' + data.position + ' (' + data.percentage.toFixed(1) + '%)';\n";
  html += "      document.getElementById('positionFill').style.width = data.percentage + '%';\n";
  html += "      let stateText = data.state == 0 ? 'Up' : data.state == 1 ? 'Down' : 'Custom';\n";
  html += "      let movingText = data.isMoving ? ' (Moving)' : '';\n";
  html += "      document.getElementById('positionDetails').innerHTML = \n";
  html += "        'State: ' + stateText + movingText + '<br>' +\n";
  html += "        'Min: ' + data.minPos + ' | Max: ' + data.maxPos;\n";
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
  html += "    \n";
  html += "    function saveSetup() {\n";
  html += "      const minPos = document.getElementById('minPosition').value;\n";
  html += "      const maxPos = document.getElementById('maxPosition').value;\n";
  html += "      \n";
  html += "      // If either input is empty, use the current values from the server\n";
  html += "      fetch('/api/status')\n";
  html += "        .then(response => response.json())\n";
  html += "        .then(data => {\n";
  html += "          const finalMinPos = minPos === '' ? data.minPos : minPos;\n";
  html += "          const finalMaxPos = maxPos === '' ? data.maxPos : maxPos;\n";
  html += "          \n";
  html += "          return fetch('/api/setup?action=save&min=' + finalMinPos + '&max=' + finalMaxPos);\n";
  html += "        })\n";
  html += "        .then(response => response.text())\n";
  html += "        .then(() => {\n";
  html += "          // Clear the input boxes after saving\n";
  html += "          document.getElementById('minPosition').value = '';\n";
  html += "          document.getElementById('maxPosition').value = '';\n";
  html += "          fetchStatus(); // Refresh position display\n";
  html += "        })\n";
  html += "        .catch(error => console.error('Error saving setup:', error));\n";
  html += "    }\n";
  html += "    \n";
  html += "    function moveToPercentage(percentage) {\n";
  html += "      fetch('/api/moveto?percentage=' + percentage)\n";
  html += "        .then(response => response.text())\n";
  html += "        .then(() => {\n";
  html += "          console.log('Moving to ' + percentage + '%');\n";
  html += "          fetchStatus(); // Refresh position display\n";
  html += "        })\n";
  html += "        .catch(error => console.error('Error moving to percentage:', error));\n";
  html += "    }\n";
  html += "    \n";
  html += "    function fetchAlarms() {\n";
  html += "      fetch('/api/alarms')\n";
  html += "        .then(response => response.json())\n";
  html += "        .then(data => {\n";
  html += "          const alarmsList = document.getElementById('alarmsList');\n";
  html += "          if (data.alarms.length === 0) {\n";
  html += "            alarmsList.innerHTML = '<p>No alarms set</p>';\n";
  html += "            return;\n";
  html += "          }\n";
  html += "          \n";
  html += "          let html = '<div class=\"alarms-grid\">';\n";
  html += "          data.alarms.forEach((alarm, index) => {\n";
  html += "            const time = new Date();\n";
  html += "            time.setHours(alarm.hour, alarm.minute);\n";
  html += "            const timeStr = time.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });\n";
  html += "            const actionStr = alarm.action === 0 ? 'Open' : 'Close';\n";
  html += "            const daysStr = alarm.days.map(day => ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'][day]).join(', ');\n";
  html += "            \n";
  html += "            html += `<div class=\"alarm-item\" style=\"padding: 10px; border: 1px solid #ddd; margin-bottom: 10px; border-radius: 5px;\">`;\n";
  html += "            html += `<div style=\"display: flex; justify-content: space-between; align-items: center;\">`;\n";
  html += "            html += `<div>`;\n";
  html += "            html += `<strong>${timeStr}</strong> - ${actionStr} Blinds<br>`;\n";
  html += "            html += `<small>${daysStr}</small>`;\n";
  html += "            html += `</div>`;\n";
  html += "            html += `<div>`;\n";
  html += "            html += `<button class=\"btn\" onclick=\"toggleAlarm(${index})\" style=\"margin-right: 5px;\">${alarm.enabled ? 'Disable' : 'Enable'}</button>`;\n";
  html += "            html += `<button class=\"btn\" onclick=\"deleteAlarm(${index})\">Delete</button>`;\n";
  html += "            html += `</div>`;\n";
  html += "            html += `</div>`;\n";
  html += "            html += `</div>`;\n";
  html += "          });\n";
  html += "          html += '</div>';\n";
  html += "          alarmsList.innerHTML = html;\n";
  html += "        })\n";
  html += "        .catch(error => console.error('Error fetching alarms:', error));\n";
  html += "    }\n";
  html += "    \n";
  html += "    function addAlarm() {\n";
  html += "      const timeInput = document.getElementById('alarmTime');\n";
  html += "      const actionSelect = document.getElementById('alarmAction');\n";
  html += "      const dayCheckboxes = document.querySelectorAll('.day-checkbox:checked');\n";
  html += "      \n";
  html += "      if (!timeInput.value || dayCheckboxes.length === 0) {\n";
  html += "        alert('Please select a time and at least one day');\n";
  html += "        return;\n";
  html += "      }\n";
  html += "      \n";
  html += "      const [hours, minutes] = timeInput.value.split(':');\n";
  html += "      const days = Array.from(dayCheckboxes).map(cb => parseInt(cb.value));\n";
  html += "      \n";
  html += "      const alarmData = {\n";
  html += "        action: 'add',\n";
  html += "        hour: parseInt(hours),\n";
  html += "        minute: parseInt(minutes),\n";
  html += "        alarmAction: parseInt(actionSelect.value),\n";
  html += "        days: days\n";
  html += "      };\n";
  html += "      \n";
  html += "      fetch('/api/alarms', {\n";
  html += "        method: 'POST',\n";
  html += "        headers: {\n";
  html += "          'Content-Type': 'application/json'\n";
  html += "        },\n";
  html += "        body: JSON.stringify(alarmData)\n";
  html += "      })\n";
  html += "        .then(response => response.text())\n";
  html += "        .then(() => {\n";
  html += "          timeInput.value = '';\n";
  html += "          actionSelect.value = '0';\n";
  html += "          document.querySelectorAll('.day-checkbox').forEach(cb => cb.checked = false);\n";
  html += "          fetchAlarms();\n";
  html += "        })\n";
  html += "        .catch(error => console.error('Error adding alarm:', error));\n";
  html += "    }\n";
  html += "    \n";
  html += "    function toggleAlarm(index) {\n";
  html += "      fetch('/api/alarms', {\n";
  html += "        method: 'POST',\n";
  html += "        headers: {\n";
  html += "          'Content-Type': 'application/json'\n";
  html += "        },\n";
  html += "        body: JSON.stringify({ action: 'toggle', index: index })\n";
  html += "      })\n";
  html += "        .then(response => response.text())\n";
  html += "        .then(() => fetchAlarms())\n";
  html += "        .catch(error => console.error('Error toggling alarm:', error));\n";
  html += "    }\n";
  html += "    \n";
  html += "    function deleteAlarm(index) {\n";
  html += "      if (!confirm('Are you sure you want to delete this alarm?')) return;\n";
  html += "      \n";
  html += "      fetch('/api/alarms', {\n";
  html += "        method: 'POST',\n";
  html += "        headers: {\n";
  html += "          'Content-Type': 'application/json'\n";
  html += "        },\n";
  html += "        body: JSON.stringify({ action: 'delete', index: index })\n";
  html += "      })\n";
  html += "        .then(response => response.text())\n";
  html += "        .then(() => fetchAlarms())\n";
  html += "        .catch(error => console.error('Error deleting alarm:', error));\n";
  html += "    }\n";
  html += "    \n";
  html += "    function syncTimeNTP() {\n";
  html += "      fetch('/api/sync-ntp')\n";
  html += "        .then(response => response.text())\n";
  html += "        .then(() => {\n";
  html += "          alert('Time synchronized with NTP server');\n";
  html += "          window.location.reload();\n";
  html += "        })\n";
  html += "        .catch(error => console.error('Error syncing time:', error));\n";
  html += "    }\n";
  html += "    \n";
  html += "    function setCustomTime() {\n";
  html += "      const customTime = document.getElementById('customTime').value;\n";
  html += "      if (!customTime) {\n";
  html += "        alert('Please select a date and time');\n";
  html += "        return;\n";
  html += "      }\n";
  html += "      \n";
  html += "      fetch('/api/time', {\n";
  html += "        method: 'POST',\n";
  html += "        headers: {\n";
  html += "          'Content-Type': 'application/json'\n";
  html += "        },\n";
  html += "        body: JSON.stringify({ datetime: customTime })\n";
  html += "      })\n";
  html += "        .then(response => response.text())\n";
  html += "        .then(() => {\n";
  html += "          alert('Time set successfully');\n";
  html += "          window.location.reload();\n";
  html += "        })\n";
  html += "        .catch(error => console.error('Error setting time:', error));\n";
  html += "    }\n";
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
  json += "\"percentage\":" + String((currentPosition - minPosition) * 100 / (maxPosition - minPosition)) + ",";
  json += "\"networkMode\":" + String(isAPMode ? "\"AP\"" : "\"WiFi\"") + ",";
  json += "\"ipAddress\":" + String(isAPMode ? "\"" + WiFi.softAPIP().toString() + "\"" : "\"" + WiFi.localIP().toString() + "\"");
  json += "}";
  
  server.send(200, "application/json", json);
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
    json += "\"enabled\":" + String(alarms[i].enabled ? "true" : "false") + ",";
    
    // Convert days bit field to array
    json += "\"days\":[";
    bool firstDay = true;
    for (int day = 0; day < 7; day++) {
      if (alarms[i].days & (1 << day)) {
        if (!firstDay) json += ",";
        json += String(day);
        firstDay = false;
      }
    }
    json += "]";
    
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
      
      // Extract days
      int daysStartIndex = json.indexOf("\"days\":[") + 8;
      int daysEndIndex = json.indexOf("]", daysStartIndex);
      String daysStr = json.substring(daysStartIndex, daysEndIndex);
      
      // Parse days array
      uint8_t days = 0;
      int currentIndex = 0;
      while (currentIndex < daysStr.length()) {
        int nextComma = daysStr.indexOf(",", currentIndex);
        if (nextComma == -1) nextComma = daysStr.length();
        int day = daysStr.substring(currentIndex, nextComma).toInt();
        days |= (1 << day); // Set the bit for this day
        currentIndex = nextComma + 1;
      }
      
      // Create new alarm
      alarms[alarmCount].hour = hour;
      alarms[alarmCount].minute = minute;
      alarms[alarmCount].action = alarmAction;
      alarms[alarmCount].enabled = true;
      alarms[alarmCount].days = days;
      
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

void startAccessPoint() {
  // Configure AP mode
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  
  IPAddress IP = WiFi.softAPIP();
  Serial.println("Access Point Started");
  Serial.print("AP IP address: ");
  Serial.println(IP);
  Serial.print("SSID: ");
  Serial.println(AP_SSID);
  Serial.print("Password: ");
  Serial.println(AP_PASSWORD);
  
  isAPMode = true;
}

void syncTimeFromNTP() {
  if (isAPMode) {
    // Cannot sync time in AP mode
    Serial.println("Cannot sync time from NTP in AP mode");
    return;
  }
  
  Serial.println("Syncing time from NTP server...");
  timeClient.begin();
  
  if (timeClient.update()) {
    Serial.println("NTP time sync successful");
    // Get the UTC time
    unsigned long epochTime = timeClient.getEpochTime();
    
    // Convert to DateTime for RTC
    time_t rawtime = epochTime;
    struct tm * ptm = gmtime(&rawtime);
    
    DateTime ntpTime(ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday, 
                   ptm->tm_hour, ptm->tm_min, ptm->tm_sec);
    
    // Set RTC time
    rtc.adjust(ntpTime);
    
    // Log the new time
    DateTime now = rtc.now();
    Serial.print("RTC time updated to: ");
    Serial.print(now.year(), DEC);
    Serial.print('/');
    Serial.print(now.month(), DEC);
    Serial.print('/');
    Serial.print(now.day(), DEC);
    Serial.print(' ');
    Serial.print(now.hour(), DEC);
    Serial.print(':');
    Serial.print(now.minute(), DEC);
    Serial.print(':');
    Serial.println(now.second(), DEC);
  } else {
    Serial.println("Failed to get time from NTP server");
  }
  
  timeClient.end();
}

void handleSyncTimeNTP() {
  if (isAPMode) {
    server.send(400, "text/plain", "Cannot sync time from NTP in AP mode");
    return;
  }
  
  syncTimeFromNTP();
  server.send(200, "text/plain", "Time synced from NTP");
}