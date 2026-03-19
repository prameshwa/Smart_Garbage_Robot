#include <Wire.h>
#include "BluetoothSerial.h"
#include <Arduino.h>
#include <ESP32Servo.h>
#include <FirebaseESP32.h>
BluetoothSerial serialBT;

//Bluetooth signal Store in this variable
char btSignal;

// Firebase objects
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// line following IR Sensors
#define IR_LEFT   32
#define IR_CENTER 33
#define IR_RIGHT  25

// L298N Motor Pins
#define IN1 26
#define IN2 27
#define IN3 14
#define IN4 12
#define ENA 13
#define ENB 15

int baseSpeed = 80;

// Hand Detection
#define IR_LID_TRIGGER 19
#define IR_BIN_FULL 18  // IR sensor to detect bin full

// --- Ultrasonic sensor pins ---
#define TRIG_PIN 23
#define ECHO_PIN 22

#define SERVO_PIN 34      // Servo for ultrasonic scanning

// lid open
#define SERVO_LID_PIN 21

// WIFI details
#define WIFI_SSID "############"
#define WIFI_PASSWORD "#############"

// Firebase details
#define DATABASE_SECRET "5RFHDdIUeC6lkYc11G0JWv1U4vCdKFwlez7vjzXZ"
#define DATABASE_URL "https://smartbin-92a3e-default-rtdb.asia-southeast1.firebasedatabase.app/"

// --- Obstacle detection distance ---
int OBSTACLE_DISTANCE = 25;  // cm - detect at 15cm

Servo scanServo;  //create servo object for ultrasonic scanning
Servo lidServo;   //create servo object for lid control
bool binFull = false;  // Track bin status
bool lidOpen = false;  // Track lid status
unsigned long lidOpenTime = 0;  // Track when lid was opened

// Lid servo positions
#define LID_CLOSED 0    // Closed position (0 degrees)
#define LID_OPEN 90     // Open position (90 degrees) - adjust based on your mechanism
#define LID_OPEN_DURATION 6000  // Keep lid open for 6 seconds

void setup()
{
    Serial.begin(115200);

    //Bluetooth Name
    serialBT.begin("Smart Bin");

    // Connect to WiFi
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) 
    {
        Serial.print(".");
        delay(300);
    }
    Serial.println();
    Serial.println("Connected to WiFi");

    // Firebase configuration
    config.database_url = DATABASE_URL;
    config.signer.tokens.legacy_token = DATABASE_SECRET;

    Firebase.begin(&config, &auth);
    Firebase.reconnectWiFi(true);
    Serial.println("Connected to Firebase");

    pinMode(IR_LEFT, INPUT);
    pinMode(IR_CENTER, INPUT);
    pinMode(IR_RIGHT, INPUT);

    pinMode(IN1, OUTPUT);
    pinMode(IN2, OUTPUT);
    pinMode(IN3, OUTPUT);
    pinMode(IN4, OUTPUT);
    pinMode(ENA, OUTPUT);
    pinMode(ENB, OUTPUT);

    pinMode(IR_BIN_FULL, INPUT);  // Bin full detection sensor
    pinMode(IR_LID_TRIGGER, INPUT);  // NEW: Lid trigger sensor
  
  // Ultrasonic setup
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  
  // Servo setup
  scanServo.attach(SERVO_PIN);
  scanServo.write(90); // center position
  
  lidServo.attach(SERVO_LID_PIN);  // NEW: Attach lid servo
  lidServo.write(LID_CLOSED);      // Start with lid closed
  delay(500);
  
  // Random seed for random turning decisions
  randomSeed(analogRead(0));

}

void loop()
{
    if (Firebase.RTDB.getInt(&fbdo, "/smartbin/mode"))
    {
        int mode = fbdo.intData();

        if (mode == 1)
        {
            // FIRST: Check lid trigger (automatic lid opening) - HIGH PRIORITY
            checkLidTrigger();
            
            // SECOND: Check bin status
            checkBinStatus();
            
            // If bin is full OR lid is operating, don't move
            if (binFull || lidOpen) {
                stopMotor();
                delay(100);  // Short delay
                return;  // Skip rest of loop - don't move motors
            }
            
            // Normal operation if bin is not full
            long distance = getDistance();
            
            // Check if obstacle is detected
            if (distance < OBSTACLE_DISTANCE && distance > 0) {
                // OBSTACLE DETECTED!
                stopMotor();
                Serial.println("");
                Serial.println("⚠️⚠️⚠️ OBSTACLE DETECTED! ⚠️⚠️⚠️");
                Serial.print("Distance: ");
                Serial.print(distance);
                Serial.println(" cm");
                Serial.println("⏸️  Stopped. Wave hand near sensor to open lid for trash.");
                Serial.println("⏱️  Waiting for 5 seconds...");
                
                // Wait for 5 seconds - during this time, hand sensor can open lid
                unsigned long stopStartTime = millis();
                while (millis() - stopStartTime < 5000) {
                checkLidTrigger();  // Check if hand is waved to open lid
                delay(50);
                }
                
                // Make sure lid is closed before continuing
                if (lidOpen) {
                Serial.println("🚪 Closing lid before moving...");
                lidServo.write(LID_CLOSED);
                lidOpen = false;
                delay(1000);
                }
                
                // REVERSE from the object
                Serial.println("⬅️  REVERSING from obstacle...");
                backward();
                delay(500);  // Reverse for 0.5 second
                
                stopMotor();
                delay(300);
                
                // Scan to find best direction
                int direction = scanDirection();
                
                // Add randomness - 30% chance to turn opposite direction
                if (random(0, 100) < 30) {
                direction = !direction;
                Serial.println("🎲 Random decision - Turning opposite direction!");
                }
                
                if (direction == 0) {
                Serial.println("⬅️  Turning LEFT...");
                turnLeft();
                delay(800);  // Turn duration for 90 degree turn
                } else {
                Serial.println("➡️  Turning RIGHT...");
                turnRight();
                delay(800);  // Turn duration for 90 degree turn
                }
                
                stopMotor();
                delay(200);
                
                Serial.println("▶️  Continuing forward...");
                Serial.println("");
            } 
            else {
                // NO OBSTACLE - Keep moving forward
                forward();
            }
        }
        else if (mode == 2)
        {
            int left   = digitalRead(IR_LEFT);
            int center = digitalRead(IR_CENTER);
            int right  = digitalRead(IR_RIGHT);

            // Print IR sensor values to Serial Monitor
            Serial.print("Left: ");
            Serial.print(left);
            Serial.print("  Center: ");
            Serial.print(center);
            Serial.print("  Right: ");
            Serial.println(right);

            // Black line = HIGH, White surface = LOW
            if (center == HIGH && left == LOW && right == LOW) {
                forward();
            }
            else if (left == HIGH && center == LOW) {
                turnRight();
            }
            else if (right == HIGH && center == LOW) {
                turnLeft();
            }
            else if (left == HIGH && center == HIGH && right == LOW) {
                turnRight();
            }
            else if (right == HIGH && center == HIGH && left == LOW) {
                turnLeft();
            }
            else {
                stopMotor();
            }
        }
        else if (mode == 3)
        {
            while (serialBT.available()) {
            btSignal = serialBT.read();
            //Serial.println(btSignal);

            if (btSignal == '0') baseSpeed = 100;
            if (btSignal == '1') baseSpeed = 110;
            if (btSignal == '2') baseSpeed = 120;
            if (btSignal == '3') baseSpeed = 130;
            if (btSignal == '4') baseSpeed = 140;
            if (btSignal == '5') baseSpeed = 150;
            if (btSignal == '6') baseSpeed = 180;
            if (btSignal == '7') baseSpeed = 200;
            if (btSignal == '8') baseSpeed = 220;
            if (btSignal == '9') baseSpeed = 240;
            if (btSignal == 'q') baseSpeed = 255;
            

            //to see the incoming signal in serial monitor
                Serial.println(btSignal);
                
            //backward
                if (btSignal == 'B') {
                backward();
                }

            //forward
                else if (btSignal == 'F') {
                forward();
                }

            //LEFT
                else if (btSignal == 'L') {
                turnLeft();
                }

            //RIGHT
                else if (btSignal == 'R') {
                turnRight();
                }

            //STOP
                else if (btSignal == 'S') {
                stopMotor();
                }
            }
        }
    }
    
}

// --- Check if bin is full ---
void checkBinStatus() {
  int binSensor = digitalRead(IR_BIN_FULL);
  
  // IR sensor output: LOW = object detected (bin full), HIGH = no object (bin empty)
  if (binSensor == LOW) {
    if (!binFull) {  // Only print when status changes
      binFull = true;
      Serial.println("");
      Serial.println("🗑️🚨🚨🚨 BIN IS FULL! 🚨🚨🚨🗑️");
      Serial.println("⚠️  Please empty the bin!");
      Serial.println("");

      Firebase.RTDB.setString(&fbdo, "/smartbin/binStatus", "Full");
      
      // Stop robot when bin is full
      stopMotor();
      
      // Blink or beep pattern (optional - blink motors as alert)
      for (int i = 0; i < 5; i++) {
        delay(200);
        // You could add LED or buzzer here
      }
    }
  } else {
    if (binFull) {  // Bin was full, now empty
      binFull = false;
      Serial.println("Bin is empty now. Continuing operation...");
      Firebase.RTDB.setString(&fbdo, "/smartbin/binStatus", "Not Full");
    }
  }
}

// --- Check for hand near bin and control lid ---
void checkLidTrigger() {
  int lidSensor = digitalRead(IR_LID_TRIGGER);
  
  // Debug output
  static unsigned long lastDebug = 0;
  if (millis() - lastDebug > 500) {
    Serial.print("Lid Sensor Value: ");
    Serial.print(lidSensor);
    Serial.print(" | Lid Status: ");
    Serial.println(lidOpen ? "OPEN" : "CLOSED");
    lastDebug = millis();
  }
  
  // If lid is currently open, check if 6 seconds have passed
  if (lidOpen) {
    unsigned long elapsedTime = millis() - lidOpenTime;

    Firebase.RTDB.setString(&fbdo, "/smartbin/lidStatus", "Open");

    if (elapsedTime >= LID_OPEN_DURATION) {
      // 6 seconds passed - close the lid
      lidOpen = false;
      Serial.println("🚪 Timer expired - Closing lid...");
      lidServo.write(LID_CLOSED);
      delay(1000);  // Wait for lid to fully close
      Serial.println("✅ Lid closed. Ready for next trigger.");
    }
    // While lid is open, do nothing else
    return;
  }
  //right
  // Lid is closed - check if hand is detected
  // Try LOW first - change to HIGH if it doesn't work
  if (lidSensor == LOW) {
    // Hand detected - open lid ONCE
    Serial.println("👋 HAND DETECTED! Opening lid for 6 seconds...");
    
    lidServo.write(LID_OPEN);
    delay(800);  // Wait for servo to move
    
    lidOpen = true;
    lidOpenTime = millis();  // Start timer
    
    Serial.println("⏱️ Timer started. Lid will close in 4 seconds...");

    Firebase.RTDB.setString(&fbdo, "/smartbin/lidStatus", "Closed");
    
    // Wait until hand is removed to prevent re-triggering
    while (digitalRead(IR_LID_TRIGGER) == LOW) {
      delay(50);  // Wait for hand to move away
    }
    Serial.println("✋ Hand removed.");
  }
}

// --- Reliable ultrasonic function ---
long getDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  
  unsigned long startTime = micros();
  while (digitalRead(ECHO_PIN) == LOW) {
    if (micros() - startTime > 30000) return 999;
  }
  
  startTime = micros();
  while (digitalRead(ECHO_PIN) == HIGH) {
    if (micros() - startTime > 30000) return 999;
  }
  
  long duration = micros() - startTime;
  return (duration * 0.034 / 2); // cm
}

// --- Servo-based scanning function ---
int scanDirection() {
  int leftDist, rightDist;
  
  Serial.println("🔍 Scanning with servo...");
  
  // Look left (90 degrees from center)
  scanServo.write(180);  // Turn 90° left
  delay(600);
  leftDist = getDistance();
  
  // Look right (90 degrees from center)
  scanServo.write(0);    // Turn 90° right
  delay(500);
  rightDist = getDistance();
  
  // Return to center
  scanServo.write(90);
  delay(300);
  
  Serial.print("Left Distance: ");
  Serial.print(leftDist);
  Serial.print(" cm | Right Distance: ");
  Serial.print(rightDist);
  Serial.println(" cm");
  
  // Decide which direction has more space
  if (leftDist > rightDist) {
    Serial.println("✅ Left side is clearer - Turning LEFT");
    return 0;  // Turn left
  } else {
    Serial.println("✅ Right side is clearer - Turning RIGHT");
    return 1;  // Turn right
  }
}

// Motor Functions
void forward() {
  analogWrite(ENA, baseSpeed);
  analogWrite(ENB, baseSpeed);
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, HIGH);
  digitalWrite(IN4, LOW);
}

void turnLeft() {
  analogWrite(ENA, baseSpeed);
  analogWrite(ENB, baseSpeed);
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, HIGH);
  digitalWrite(IN4, LOW);
}

void turnRight() {
  analogWrite(ENA, baseSpeed);
  analogWrite(ENB, baseSpeed);
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, HIGH);
}

void stopMotor() {
  analogWrite(ENA, baseSpeed);
  analogWrite(ENB, baseSpeed);
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
}

void backward() {
    analogWrite(ENA, baseSpeed);
    analogWrite(ENB, baseSpeed);
    digitalWrite(IN1, LOW);
    digitalWrite(IN2, HIGH);
    digitalWrite(IN3, LOW);
    digitalWrite(IN4, HIGH);
}

