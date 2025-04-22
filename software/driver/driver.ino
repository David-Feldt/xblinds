#include <Wire.h>
#include <RTClib.h>
#include <Encoder.h>

#define dirPin D5  // GPIO14
#define stepPin D6 // GPIO12
#define stepsPerRevolution 200
#define SDA_PIN D4
#define SCL_PIN D3
#define ENCODER_CLK D2
#define ENCODER_DT D1

RTC_DS3231 rtc;
Encoder myEnc(ENCODER_CLK, ENCODER_DT);
long oldPosition = -999;

void setup() {
  // Initialize serial for logging
  Serial.begin(115200);
  delay(1000); // Give time for Serial monitor to connect

  // Initialize I2C for RTC
  Wire.begin(SDA_PIN, SCL_PIN);
  
  if (!rtc.begin()) {
    Serial.println("Couldn't find RTC");
    while (1);
  }

  // If RTC lost power, set the time
  if (rtc.lostPower()) {
    Serial.println("RTC lost power, setting the time!");
    // When time needs to be set on a new device, or after power loss, the
    // following line sets the RTC to the date & time this sketch was compiled
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  // Declare pins as output
  pinMode(stepPin, OUTPUT);
  pinMode(dirPin, OUTPUT);

  Serial.println("Stepper, RTC, and Encoder initialized.");
}

void stepMotor(int steps, int delayMicrosec) {
  for (int i = 0; i < steps; i++) {
    digitalWrite(stepPin, HIGH);
    delayMicroseconds(delayMicrosec);
    digitalWrite(stepPin, LOW);
    delayMicroseconds(delayMicrosec);
  }
}

void loop() {
  // Get current time from RTC
  DateTime now = rtc.now();
  
  Serial.print("Current time: ");
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
  Serial.print(now.second(), DEC);
  Serial.println();

  // Read encoder position
  long newPosition = myEnc.read();
  if (newPosition != oldPosition) {
    oldPosition = newPosition;
    Serial.print("Encoder position: ");
    Serial.println(newPosition);
  }

  Serial.println("Direction: Clockwise | Speed: Slow | Revolutions: 1");
  digitalWrite(dirPin, HIGH);
  stepMotor(stepsPerRevolution, 2000);
  delay(1000);

  Serial.println("Direction: Counterclockwise | Speed: Medium | Revolutions: 1");
  digitalWrite(dirPin, LOW);
  stepMotor(stepsPerRevolution, 1000);
  delay(1000);

  Serial.println("Direction: Clockwise | Speed: Fast | Revolutions: 5");
  digitalWrite(dirPin, HIGH);
  stepMotor(5 * stepsPerRevolution, 500);
  delay(1000);

  Serial.println("Direction: Counterclockwise | Speed: Fast | Revolutions: 5");
  digitalWrite(dirPin, LOW);
  stepMotor(5 * stepsPerRevolution, 500);
  delay(1000);

  Serial.println("Cycle complete.\n");
}