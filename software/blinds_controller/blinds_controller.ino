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
// const char* ssid = "Feldtfam";
// const char* password = "Nectarine03";
//New 
const char* ssid = "New Stadium Guests";
const char* password = "combo#2please";

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
  static int setupStep = 0;
  static bool buttonWasReleased = true;
  
  // Wait until button is released before continuing
  if (digitalRead(ENCODER_BTN) == HIGH) {
    buttonWasReleased = true;
  }
  
  if (setupStep == 0) { // Setting min position
    long encoderPos = myEncoder.read();
    
    // Move motor to encoder position
    if (encoderPos != currentPosition) {
      Serial.print("Setup - Min Position: ");
      Serial.println(encoderPos);
      
      // Move motor
      moveStepperBySteps(encoderPos - currentPosition);
      currentPosition = encoderPos;
    }
    
    // When button is pressed, save min position and move to next step
    if (digitalRead(ENCODER_BTN) == LOW && buttonWasReleased) {
      minPosition = currentPosition;
      Serial.print("Min position set to: ");
      Serial.println(minPosition);
      
      setupStep = 1;
      buttonWasReleased = false;
      
      Serial.println("Use encoder to set MAX position, then press button");
    }
  } 
  else if (setupStep == 1) { // Setting max position
    long encoderPos = myEncoder.read();
    
    // Move motor to encoder position
    if (encoderPos != currentPosition) {
      Serial.print("Setup - Max Position: ");
      Serial.println(encoderPos);
      
      // Move motor
      moveStepperBySteps(encoderPos - currentPosition);
      currentPosition = encoderPos;
    }
    
    // When button is pressed, save max position and exit setup mode
    if (digitalRead(ENCODER_BTN) == LOW && buttonWasReleased) {
      maxPosition = currentPosition;
      Serial.print("Max position set to: ");
      Serial.println(maxPosition);
      
      // Ensure min < max
      if (minPosition > maxPosition) {
        long temp = minPosition;
        minPosition = maxPosition;
        maxPosition = temp;
      }
      
      // Save settings
      saveSettingsToEEPROM();
      
      // Exit setup mode
      setupMode = false;
      setupStep = 0;
      
      // Move to min position (blinds up)
      moveBlindUp();
      
      Serial.println("Setup complete");
    }
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
  html += "  </style>\n";
  html += "</head>\n";
  html += "<body>\n";
  html += "  <h1>Smart Blinds Controller</h1>\n";
  html += "  <div id=\"statusDiv\">Loading status...</div>\n";
  html += "  <div>\n";
  html += "    <button class=\"btn btn-up\" onclick=\"moveUp()\">Move Up</button>\n";
  html += "    <button class=\"btn btn-down\" onclick=\"moveDown()\">Move Down</button>\n";
  html += "  </div>\n";
  html += "  <div>\n";
  html += "    <h2>Manual Position</h2>\n";
  html += "    <input type=\"range\" min=\"0\" max=\"100\" value=\"0\" class=\"slider\" id=\"positionSlider\">\n";
  html += "    <button class=\"btn\" onclick=\"setPosition()\">Set Position</button>\n";
  html += "  </div>\n";
  html += "  <div>\n";
  html += "    <h2>Alarms</h2>\n";
  html += "    <div id=\"alarmsList\">Loading alarms...</div>\n";
  html += "    <h3>Add New Alarm</h3>\n";
  html += "    <div>\n";
  html += "      <input type=\"time\" id=\"newAlarmTime\">\n";
  html += "      <select id=\"newAlarmAction\">\n";
  html += "        <option value=\"0\">Up</option>\n";
  html += "        <option value=\"1\">Down</option>\n";
  html += "      </select>\n";
  html += "      <button class=\"btn\" onclick=\"addAlarm()\">Add Alarm</button>\n";
  html += "    </div>\n";
  html += "  </div>\n";
  html += "  <div>\n";
  html += "    <h2>Set Time</h2>\n";
  html += "    <div id=\"currentTime\">Loading current time...</div>\n";
  html += "    <input type=\"datetime-local\" id=\"newDateTime\">\n";
  html += "    <button class=\"btn\" onclick=\"setTime()\">Set Time</button>\n";
  html += "  </div>\n";
  html += "  <script>\n";
  html += "    // Load initial data\n";
  html += "    window.onload = function() {\n";
  html += "      fetchStatus();\n";
  html += "      fetchAlarms();\n";
  html += "      fetchTime();\n";
  html += "      // Refresh status every 5 seconds\n";
  html += "      setInterval(fetchStatus, 5000);\n";
  html += "      setInterval(fetchTime, 5000);\n";
  html += "    };\n";
  html += "    \n";
  html += "    function fetchStatus() {\n";
  html += "      fetch('/api/status')\n";
  html += "        .then(response => response.json())\n";
  html += "        .then(data => {\n";
  html += "          document.getElementById('statusDiv').innerHTML = \n";
  html += "            '<p>Current Position: ' + data.position + '</p>' +\n";
  html += "            '<p>State: ' + (data.state == 0 ? 'Up' : data.state == 1 ? 'Down' : 'Custom') + '</p>';\n";
  html += "          \n";
  html += "          // Calculate slider percentage\n";
  html += "          let range = data.maxPos - data.minPos;\n";
  html += "          let relativePos = data.position - data.minPos;\n";
  html += "          let percentage = (relativePos / range) * 100;\n";
  html += "          document.getElementById('positionSlider').value = percentage;\n";
  html += "        });\n";
  html += "    }\n";
  html += "    \n";
  html += "    function fetchAlarms() {\n";
  html += "      fetch('/api/alarms')\n";
  html += "        .then(response => response.json())\n";
  html += "        .then(data => {\n";
  html += "          let html = '';\n";
  html += "          data.alarms.forEach((alarm, index) => {\n";
  html += "            html += '<div class=\"alarm-item\">';\n";
  html += "            html += '<p>Time: ' + alarm.hour + ':' + (alarm.minute < 10 ? '0' : '') + alarm.minute;\n";
  html += "            html += ' | Action: ' + (alarm.action == 0 ? 'Up' : 'Down');\n";
  html += "            html += ' | Enabled: ' + (alarm.enabled ? 'Yes' : 'No') + '</p>';\n";
  html += "            html += '<button onclick=\"deleteAlarm(' + index + ')\">Delete</button>';\n";
  html += "            html += '<button onclick=\"toggleAlarm(' + index + ')\">' + (alarm.enabled ? 'Disable' : 'Enable') + '</button>';\n";
  html += "            html += '</div>';\n";
  html += "          });\n";
  html += "          document.getElementById('alarmsList').innerHTML = html;\n";
  html += "        });\n";
  html += "    }\n";
  html += "    \n";
  html += "    function fetchTime() {\n";
  html += "      fetch('/api/time')\n";
  html += "        .then(response => response.json())\n";
  html += "        .then(data => {\n";
  html += "          document.getElementById('currentTime').innerHTML = \n";
  html += "            '<p>Current Time: ' + data.datetime + '</p>';\n";
  html += "          \n";
  html += "          // Format the datetime for the input field\n";
  html += "          let dt = new Date(data.datetime);\n";
  html += "          let year = dt.getFullYear();\n";
  html += "          let month = (dt.getMonth() + 1).toString().padStart(2, '0');\n";
  html += "          let day = dt.getDate().toString().padStart(2, '0');\n";
  html += "          let hours = dt.getHours().toString().padStart(2, '0');\n";
  html += "          let minutes = dt.getMinutes().toString().padStart(2, '0');\n";
  html += "          \n";
  html += "          let formattedDate = year + '-' + month + '-' + day + 'T' + hours + ':' + minutes;\n";
  html += "          document.getElementById('newDateTime').value = formattedDate;\n";
  html += "        });\n";
  html += "    }\n";
  html += "    \n";
  html += "    function moveUp() {\n";
  html += "      fetch('/api/moveup')\n";
  html += "        .then(() => setTimeout(fetchStatus, 500));\n";
  html += "    }\n";
  html += "    \n";
  html += "    function moveDown() {\n";
  html += "      fetch('/api/movedown')\n";
  html += "        .then(() => setTimeout(fetchStatus, 500));\n";
  html += "    }\n";
  html += "    \n";
  html += "    function setPosition() {\n";
  html += "      let percentage = document.getElementById('positionSlider').value;\n";
  html += "      fetch('/api/moveto?percentage=' + percentage)\n";
  html += "        .then(() => setTimeout(fetchStatus, 500));\n";
  html += "    }\n";
  html += "    \n";
  html += "    function addAlarm() {\n";
  html += "      let time = document.getElementById('newAlarmTime').value;\n";
  html += "      let action = document.getElementById('newAlarmAction').value;\n";
  html += "      \n";
  html += "      if (!time) {\n";
  html += "        alert('Please select a time');\n";
  html += "        return;\n";
  html += "      }\n";
  html += "      \n";
  html += "      let [hours, minutes] = time.split(':');\n";
  html += "      \n";
  html += "      let data = {\n";
  html += "        action: 'add',\n";
  html += "        hour: parseInt(hours),\n";
  html += "        minute: parseInt(minutes),\n";
  html += "        alarmAction: parseInt(action),\n";
  html += "        enabled: true\n";
  html += "      };\n";
  html += "      \n";
  html += "      fetch('/api/alarms', {\n";
  html += "        method: 'POST',\n";
  html += "        headers: {\n";
  html += "          'Content-Type': 'application/json'\n";
  html += "        },\n";
  html += "        body: JSON.stringify(data)\n";
  html += "      })\n";
  html += "      .then(() => fetchAlarms());\n";
  html += "    }\n";
  html += "    \n";
  html += "    function deleteAlarm(index) {\n";
  html += "      if (!confirm('Are you sure you want to delete this alarm?')) return;\n";
  html += "      \n";
  html += "      let data = {\n";
  html += "        action: 'delete',\n";
  html += "        index: index\n";
  html += "      };\n";
  html += "      \n";
  html += "      fetch('/api/alarms', {\n";
  html += "        method: 'POST',\n";
  html += "        headers: {\n";
  html += "          'Content-Type': 'application/json'\n";
  html += "        },\n";
  html += "        body: JSON.stringify(data)\n";
  html += "      })\n";
  html += "      .then(() => fetchAlarms());\n";
  html += "    }\n";
  html += "    \n";
  html += "    function toggleAlarm(index) {\n";
  html += "      let data = {\n";
  html += "        action: 'toggle',\n";
  html += "        index: index\n";
  html += "      };\n";
  html += "      \n";
  html += "      fetch('/api/alarms', {\n";
  html += "        method: 'POST',\n";
  html += "        headers: {\n";
  html += "          'Content-Type': 'application/json'\n";
  html += "        },\n";
  html += "        body: JSON.stringify(data)\n";
  html += "      })\n";
  html += "      .then(() => fetchAlarms());\n";
  html += "    }\n";
  html += "    \n";
  html += "    function setTime() {\n";
  html += "      let newDateTime = document.getElementById('newDateTime').value;\n";
  html += "      if (!newDateTime) {\n";
  html += "        alert('Please select a date and time');\n";
  html += "        return;\n";
  html += "      }\n";
  html += "      \n";
  html += "      let data = {\n";
  html += "        datetime: newDateTime\n";
  html += "      };\n";
  html += "      \n";
  html += "      fetch('/api/time', {\n";
  html += "        method: 'POST',\n";
  html += "        headers: {\n";
  html += "          'Content-Type': 'application/json'\n";
  html += "        },\n";
  html += "        body: JSON.stringify(data)\n";
  html += "      })\n";
  html += "      .then(() => fetchTime());\n";
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
  json += "\"targetPosition\":" + String(targetPosition);
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