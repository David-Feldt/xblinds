// Include the AccelStepper library instead of Stepper
#include <AccelStepper.h>
// Include ESP8266 libraries for WiFi and web server
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
// Include mDNS library
#include <ESP8266mDNS.h>

// Access Point settings
const char* ap_ssid = "blinds";  // Name of the WiFi network the ESP will create
const char* ap_password = "";  // Empty password for open network
IPAddress ap_ip(1, 2, 3, 4);  // Simple IP address that's easy to remember
IPAddress ap_subnet(255, 255, 255, 0);  // Subnet mask

// Number of steps per output rotation
const int stepsPerRevolution = 200;

// Potentiometer settings
const int potPin = A0;  // Analog pin connected to potentiometer
int lastPotValue = 0;   // Last read potentiometer value
int currentPosition = 0; // Current position of the motor in steps
const int potThreshold = 10; // Minimum change in pot value to trigger movement
const int maxRotations = 5;  // Maximum number of rotations (full range)
const int maxSteps = maxRotations * stepsPerRevolution;  // Maximum number of steps

// Create Instance of AccelStepper
// Define pin connections: type, step pin, direction pin (for driver) or 4 pins for direct connection
// For 4-wire stepper motor directly connected to Arduino/ESP8266
AccelStepper myStepper(AccelStepper::FULL4WIRE, D1, D3, D2, D4);

// Create web server on port 80
ESP8266WebServer server(80);

// Add these variables for bounds configuration
int lowerBound = 0;      // Lower bound position (fully closed)
int upperBound = 1000;   // Upper bound position (fully open)
bool setupMode = false;  // Flag to indicate if in setup mode

void setup()
{
	// Configure the stepper motor
	myStepper.setMaxSpeed(200);     // Steps per second
	myStepper.setAcceleration(50);  // Steps per second per second
	myStepper.setSpeed(100);        // Steps per second
	
	// initialize the serial port:
	Serial.begin(9600);
	delay(500); // Give serial time to initialize
	
	Serial.println("\n\n=== ESP8266 Stepper Motor Control - AP Mode ===");
	Serial.println("Firmware version: 1.0");
	
	// Configure Access Pointß
	WiFi.mode(WIFI_AP);  // Set ESP8266 to Access Point mode
	WiFi.softAPConfig(ap_ip, ap_ip, ap_subnet);  // Configure the AP IP settings
	
	// Start the Access Point (open network with no password)
	bool apStarted = WiFi.softAP(ap_ssid);
	
	if (apStarted) {
		Serial.println("Access Point started successfully!");
		Serial.print("Network name: ");
		Serial.println(ap_ssid);
		Serial.println("No password required (open network)");
		Serial.print("AP IP address: ");
		Serial.println(WiFi.softAPIP());
	} else {
		Serial.println("Failed to start Access Point!");
	}
	
	// Define server endpoints
	server.on("/", handleRoot);
	server.on("/clockwise", handleClockwise);
	server.on("/counterclockwise", handleCounterclockwise);
	server.on("/position", handlePosition);
	server.on("/setup", handleSetupMode);
	server.on("/setbounds", handleSetBounds);
	server.onNotFound(handleNotFound);
	
	// Start the server
	server.begin();
	Serial.println("HTTP server started successfully");
	Serial.println("=== Setup complete ===\n");
	
	// Print connection instructions
	Serial.println("To control the blinds:");
	Serial.println("1. Connect to the 'blinds' WiFi network");
	Serial.println("2. No password required");
	Serial.println("3. Open a browser and navigate to http://1.2.3.4");
	
	// Initialize potentiometer reading
	lastPotValue = analogRead(potPin);
	Serial.println("Potentiometer initialized");
}

void loop() 
{
	// First priority: Run the stepper motor if it has steps to move
	if (myStepper.distanceToGo() != 0) {
		myStepper.run();
		// Don't do anything else while the motor is moving
		return;
	}
	
	// Only handle other tasks when the motor is not moving
	server.handleClient();
	handlePotentiometer();
	
	// Small delay to prevent CPU hogging
	delay(1);
}

// Handle potentiometer input
void handlePotentiometer() {
	// Read the potentiometer value (0-1023)
	int potValue = analogRead(potPin);
	
	// Check if the value has changed significantly
	if (abs(potValue - lastPotValue) > potThreshold) {
		// Map potentiometer value to rotation number (0 to maxRotations)
		int targetRotation;
		
		if (setupMode) {
			// In setup mode, use full range
			targetRotation = map(potValue, 0, 1023, 0, maxRotations);
		} else {
			// In normal mode, map to configured bounds (in rotations)
			int lowerRotation = lowerBound / stepsPerRevolution;
			int upperRotation = upperBound / stepsPerRevolution;
			targetRotation = map(potValue, 0, 1023, lowerRotation, upperRotation);
		}
		
		// Convert rotation to steps (ensuring complete rotations only)
		int targetPosition = targetRotation * stepsPerRevolution;
		
		// Calculate steps to move
		int stepsToMove = targetPosition - currentPosition;
		
		if (stepsToMove != 0) {
			Serial.print("Potentiometer value: ");
			Serial.print(potValue);
			Serial.print(" -> Moving to rotation ");
			Serial.print(targetRotation);
			Serial.print(" (position ");
			Serial.print(targetPosition);
			Serial.println(" steps)");
			
			// Move the motor using AccelStepper
			myStepper.moveTo(targetPosition);
			
			// Update current position
			currentPosition = targetPosition;
		}
		
		// Update last value
		lastPotValue = potValue;
	}
	
	// Small delay to prevent too frequent readings
	delay(50);
}

// Add a handler for 404 errors
void handleNotFound() {
	Serial.print("404 Error: ");
	Serial.println(server.uri());
	
	String message = "404 Not Found\n\n";
	message += "URI: ";
	message += server.uri();
	message += "\nMethod: ";
	message += (server.method() == HTTP_GET) ? "GET" : "POST";
	message += "\nArguments: ";
	message += server.args();
	message += "\n";
	
	for (uint8_t i = 0; i < server.args(); i++) {
		message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
	}
	
	server.send(404, "text/plain", message);
}

// Serve the main HTML page
void handleRoot() {
	String html = "<html><head>";
	html += "<title>ESP8266 Stepper Control</title>";
	html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
	html += "<style>";
	html += "body { font-family: Arial; text-align: center; margin: 20px; }";
	html += "button { background-color: #4CAF50; color: white; padding: 15px 32px; ";
	html += "font-size: 16px; margin: 10px; cursor: pointer; border-radius: 8px; }";
	html += ".clockwise { background-color: #2196F3; }";
	html += ".counterclockwise { background-color: #f44336; }";
	html += ".setup { background-color: #FF9800; }";
	html += ".slider-container { margin: 20px 0; }";
	html += "input[type=range] { width: 80%; max-width: 400px; }";
	html += "</style>";
	html += "<script>";
	html += "function updatePosition(val) {";
	html += "  // Convert slider value to rotations";
	html += "  var rotation = Math.round(val / " + String(stepsPerRevolution) + ");";
	html += "  document.getElementById('posValue').innerHTML = rotation;";
	html += "  // Send the rotation value to the server";
	html += "  fetch('/position?pos=' + (rotation * " + String(stepsPerRevolution) + "));";
	html += "}";
	html += "function refreshPosition() {";
	html += "  fetch('/position').then(response => response.text()).then(data => {";
	html += "    document.getElementById('slider').value = data;";
	html += "    var rotation = Math.round(data / " + String(stepsPerRevolution) + ");";
	html += "    document.getElementById('posValue').innerHTML = rotation;";
	html += "  });";
	html += "}";
	html += "window.onload = function() { refreshPosition(); setInterval(refreshPosition, 5000); };";
	html += "</script>";
	html += "</head><body>";
	html += "<h1>Stepper Motor Control</h1>";
	
	// Show different controls based on setup mode
	if (setupMode) {
		html += "<h2>SETUP MODE</h2>";
		html += "<p>Move the blinds to the desired position using the buttons below:</p>";
		html += "<button class='clockwise' onclick='location.href=\"/clockwise\"'>Clockwise</button>";
		html += "<button class='counterclockwise' onclick='location.href=\"/counterclockwise\"'>Counterclockwise</button>";
		html += "<div class='slider-container'>";
		html += "<h3>Set Bounds</h3>";
		html += "<button onclick='location.href=\"/setbounds?bound=lower\"'>Set Lower Bound</button>";
		html += "<button onclick='location.href=\"/setbounds?bound=upper\"'>Set Upper Bound</button>";
		html += "<button onclick='location.href=\"/setup?exit=true\"'>Exit Setup Mode</button>";
		html += "<p>Current position: <span id='posValue'>0</span> rotations</p>";
		html += "</div>";
	} else {
		html += "<button class='clockwise' onclick='location.href=\"/clockwise\"'>Clockwise</button>";
		html += "<button class='counterclockwise' onclick='location.href=\"/counterclockwise\"'>Counterclockwise</button>";
		html += "<button class='setup' onclick='location.href=\"/setup\"'>Setup Mode</button>";
		html += "<div class='slider-container'>";
		html += "<h2>Position Control</h2>";
		html += "<input type='range' min='0' max='" + String(maxRotations * stepsPerRevolution) + "' step='" + String(stepsPerRevolution) + "' value='0' id='slider' oninput='updatePosition(this.value)'>";
		html += "<p>Position: <span id='posValue'>0</span> rotations</p>";
		html += "<p><small>Note: Web slider and physical potentiometer control the same motor</small></p>";
		html += "</div>";
	}
	
	html += "</body></html>";
	
	server.send(200, "text/html", html);
}

// Handle clockwise rotation
void handleClockwise() {
	Serial.println("Command received: Clockwise rotation");
	
	// Just set the target and return immediately - don't wait for completion
	myStepper.move(stepsPerRevolution);
	
	// Redirect back to the main page
	server.sendHeader("Location", "/", true);
	server.send(302, "text/plain", "");
}

// Handle counterclockwise rotation
void handleCounterclockwise() {
	Serial.println("Command received: Counterclockwise rotation");
	
	// Just set the target and return immediately - don't wait for completion
	myStepper.move(-stepsPerRevolution);
	
	// Redirect back to the main page
	server.sendHeader("Location", "/", true);
	server.send(302, "text/plain", "");
}

// Handle position control from web interface
void handlePosition() {
	if (server.hasArg("pos")) {
		int requestedPosition = server.arg("pos").toInt();
		
		// Convert to rotations (rounding to nearest rotation)
		int targetRotation = round(requestedPosition / (float)stepsPerRevolution);
		
		// Convert back to steps (ensuring complete rotations only)
		int newPosition = targetRotation * stepsPerRevolution;
		
		// Map the position to the configured bounds if not in setup mode
		if (!setupMode) {
			// Convert bounds to rotations
			int lowerRotation = lowerBound / stepsPerRevolution;
			int upperRotation = upperBound / stepsPerRevolution;
			
			// Map the rotation within bounds
			targetRotation = map(targetRotation, 0, maxRotations, lowerRotation, upperRotation);
			
			// Convert back to steps
			newPosition = targetRotation * stepsPerRevolution;
		}
		
		Serial.print("Web request: Move to rotation ");
		Serial.print(targetRotation);
		
		// Just set the target and return immediately - don't wait for completion
		myStepper.moveTo(newPosition);
	}
	
	// Return the current position in terms of the slider range
	int sliderPosition = setupMode ? 
		currentPosition : 
		map(currentPosition, lowerBound, upperBound, 0, maxSteps);
	
	server.send(200, "text/plain", String(sliderPosition));
}

// Handle setup mode toggle
void handleSetupMode() {
	if (server.hasArg("exit") && server.arg("exit") == "true") {
		setupMode = false;
		Serial.println("Exiting setup mode");
	} else {
		setupMode = true;
		Serial.println("Entering setup mode");
	}
	
	// Redirect back to the main page
	server.sendHeader("Location", "/", true);
	server.send(302, "text/plain", "");
}

// Handle setting bounds
void handleSetBounds() {
	if (server.hasArg("bound")) {
		String boundType = server.arg("bound");
		
		// Round the current position to the nearest complete rotation
		int rotationPosition = round(currentPosition / (float)stepsPerRevolution) * stepsPerRevolution;
		
		if (boundType == "lower") {
			lowerBound = rotationPosition;
			Serial.print("Lower bound set to: ");
			Serial.print(lowerBound);
			Serial.print(" steps (");
			Serial.print(lowerBound / stepsPerRevolution);
			Serial.println(" rotations)");
		} else if (boundType == "upper") {
			upperBound = rotationPosition;
			Serial.print("Upper bound set to: ");
			Serial.print(upperBound);
			Serial.print(" steps (");
			Serial.print(upperBound / stepsPerRevolution);
			Serial.println(" rotations)");
		}
	}
	
	// Redirect back to the main page
	server.sendHeader("Location", "/", true);
	server.send(302, "text/plain", "");
}