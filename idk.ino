// Include the AccelStepper library
#include <AccelStepper.h>

// Number of steps per output rotation
const int stepsPerRevolution = 200;

// Create Instance of AccelStepper
// Define pin connections: type, step pin, direction pin (for driver) or 4 pins for direct connection
// For 4-wire stepper motor directly connected to Arduino/ESP8266
AccelStepper myStepper(AccelStepper::FULL4WIRE, D1, D3, D2, D4);

void setup() {
  // Configure the stepper motor
  myStepper.setMaxSpeed(200);     // Steps per second
  myStepper.setAcceleration(50);  // Steps per second per second
  myStepper.setSpeed(100);        // Steps per second
  
  // Initialize the serial port
  Serial.begin(9600);
  delay(500); // Give serial time to initialize
  
  Serial.println("\n\n=== ESP8266 Stepper Motor Test ===");
  
  // Move one full rotation clockwise
  Serial.println("Moving one rotation clockwise...");
  myStepper.move(stepsPerRevolution);
}

void loop() {
  // Run the stepper motor if it has steps to move
  if (myStepper.distanceToGo() != 0) {
    myStepper.run();
  } else {
    // When one movement completes, start another after a delay
    static unsigned long lastMoveTime = 0;
    unsigned long currentTime = millis();
    
    if (currentTime - lastMoveTime > 3000) { // 3 second delay between movements
      // Alternate between clockwise and counterclockwise
      static bool clockwise = true;
      
      if (clockwise) {
        Serial.println("Moving one rotation clockwise...");
        myStepper.move(stepsPerRevolution);
      } else {
        Serial.println("Moving one rotation counterclockwise...");
        myStepper.move(-stepsPerRevolution);
      }
      
      clockwise = !clockwise;
      lastMoveTime = currentTime;
    }
  }
}