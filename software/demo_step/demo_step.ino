// Include the standard Stepper library
#include <Stepper.h>

// Number of steps per output rotation
const int stepsPerRevolution = 200;

// Create an instance of Stepper
// The constructor takes the number of steps per revolution and the pins to
// which the motor is attached, in order: IN1, IN3, IN2, IN4 (for 4-wire stepper)
Stepper myStepper(stepsPerRevolution, D1, D3, D2, D4);

void setup() {
  // Set the motor speed in RPM
  myStepper.setSpeed(60); // 60 RPM (1 rotation per second)
  
  // Initialize the serial port
  Serial.begin(9600);
  delay(500); // Give serial time to initialize
  
  Serial.println("\n\n=== Simple Stepper Motor Test ===");
}

void loop() {
  // Print status message
  Serial.println("Rotating one full revolution clockwise...");
  
  // Rotate one full revolution
  myStepper.step(stepsPerRevolution);
  
  // Print completion message
  Serial.println("Rotation complete");
  Serial.println("Waiting 5 seconds...");
  
  // Wait 5 seconds
  delay(5000);
  
  // Print a separator for readability in the serial monitor
  Serial.println("------------------------------");
}