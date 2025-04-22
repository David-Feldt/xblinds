Upload to ESP8266
arduino-cli upload -p /dev/cu.usbserial-110 --fqbn esp8266:esp8266:d1_mini stepper.ino

Compile
arduino-cli compile --fqbn esp8266:esp8266:d1_mini stepper.ino


Monitor
arduino-cli monitor -p /dev/cu.usbserial-110 -c baudrate=9600ß