// Include the standard Stepper library
#include <Stepper.h>

// Number of steps per output rotation
const int stepsPerRevolution = 200;

// Create an instance of Stepper
// The constructor takes the number of steps per revolution and the pins to
// which the motor is attached, in order: IN1, IN3, IN2, IN4 (for 4-wire stepper)
Stepper myStepper(stepsPerRevolution, D1, D3, D2, D4);

String inputString = "";         // String to hold incoming serial data
boolean stringComplete = false;  // Whether the string is complete

void setup() {
  // Set the motor speed in RPM
  myStepper.setSpeed(60); // 60 RPM (1 rotation per second)
  
  // Initialize the serial port
  Serial.begin(9600);
  delay(500); // Give serial time to initialize
  
  Serial.println("\n\n=== ESP8266 Stepper Motor Test (Stepper.h) ===");
  Serial.println("Enter number of steps to move (positive for clockwise, negative for counterclockwise)");
  Serial.println("Examples: '200' for one rotation clockwise, '-200' for one rotation counterclockwise");
}

void loop() {
  // Process serial input when a complete command is received
  if (stringComplete) {
    // Convert the input string to an integer
    int steps = inputString.toInt();
    
    if (steps != 0) {
      // Report the movement
      if (steps > 0) {
        Serial.print("Moving ");
        Serial.print(steps);
        Serial.println(" steps clockwise...");
      } else {
        Serial.print("Moving ");
        Serial.print(-steps);
        Serial.println(" steps counterclockwise...");
      }
      
      // Move the stepper motor
      myStepper.step(steps);
      
      Serial.println("Movement complete");
    } else {
      // If conversion failed or zero was entered
      Serial.println("Invalid input. Please enter a number of steps (e.g., 200 or -200)");
    }
    
    // Clear the input string and flag for the next command
    inputString = "";
    stringComplete = false;
    
    // Prompt for next command
    Serial.println("\nEnter number of steps to move:");
  }
  
  // Read serial input
  while (Serial.available()) {
    char inChar = (char)Serial.read();
    
    // Add character to input string
    if (inChar != '\n' && inChar != '\r') {
      inputString += inChar;
    }
    
    // Mark string as complete when newline received
    if (inChar == '\n') {
      stringComplete = true;
    }
  }
}