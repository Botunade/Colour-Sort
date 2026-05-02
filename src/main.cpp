#include <Wire.h>
#include <Adafruit_TCS34725.h>
#include <ESP32Servo.h>
#include <LiquidCrystal_I2C.h>
#include <queue>

// PIN CONFIGURATION
#define INTAKE_IR 4
#define RELAY_PIN 5
#define BIN_RED_IR 16
#define BIN_GRN_IR 17
#define BIN_BLU_IR 18

#define SERVO_RED_PIN 13
#define SERVO_GRN_PIN 12
#define SERVO_BLU_PIN 14

enum BoxColor { NONE, RED, GREEN, BLUE, YELLOW };
std::queue<BoxColor> beltQueue;
int totals[] = {0, 0, 0, 0};

Adafruit_TCS34725 tcs = Adafruit_TCS34725(TCS34725_INTEGRATIONTIME_2_4MS, TCS34725_GAIN_16X);
LiquidCrystal_I2C lcd(0x27, 16, 4);
Servo sRed, sGrn, sBlu;

// DEBOUNCE TIMERS
unsigned long lastIntakeTime = 0;
unsigned long debounceDelay = 200; // 200ms debounce for IR sensors

BoxColor identify(uint16_t r, uint16_t g, uint16_t b);
void handleSort(int irPin, BoxColor target, Servo &srv, const char* colorName);
void updateLCD(BoxColor last);

void setup() {
  Serial.begin(115200);
  Serial.println("Initializing System...");

  pinMode(INTAKE_IR, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BIN_RED_IR, INPUT); pinMode(BIN_GRN_IR, INPUT); pinMode(BIN_BLU_IR, INPUT);

  digitalWrite(RELAY_PIN, LOW);

  // Servo Homing to 0 Degrees
  Serial.println("Homing Servos...");
  sRed.attach(SERVO_RED_PIN); sRed.write(0);
  sGrn.attach(SERVO_GRN_PIN); sGrn.write(0);
  sBlu.attach(SERVO_BLU_PIN); sBlu.write(0);

  lcd.init();
  lcd.backlight();
  if (tcs.begin()) {
    Serial.println("TCS34725 Sensor found");
  } else {
    Serial.println("No TCS34725 found ... check your connections");
    while (1);
  }

  lcd.print("SYSTEM READY");
  Serial.println("System Ready - Waiting for objects...");
}

void loop() {
  // 1. Motor Wake-up (with basic debouncing)
  if (digitalRead(INTAKE_IR) == LOW && (millis() - lastIntakeTime > debounceDelay)) {
    Serial.println("Object detected at Intake. Activating conveyor.");
    digitalWrite(RELAY_PIN, HIGH);
    lastIntakeTime = millis();
  }

  // 2. Color Scanning (with multi-sample filtering)
  uint16_t r, g, b, c;
  tcs.getRawData(&r, &g, &b, &c);

  if (c > 1500) {
    Serial.println("Object beneath color sensor. Taking average scan...");

    // Take an average of 3 readings to filter out noise
    uint32_t r_sum = r, g_sum = g, b_sum = b;
    for(int i=0; i<2; i++) {
        delay(10); // small delay between samples
        tcs.getRawData(&r, &g, &b, &c);
        r_sum += r; g_sum += g; b_sum += b;
    }
    r = r_sum / 3; g = g_sum / 3; b = b_sum / 3;

    BoxColor detected = identify(r, g, b);
    if (detected != NONE) {
      beltQueue.push(detected);
      updateLCD(detected);
      Serial.print("Classified as: ");
      switch(detected) {
        case RED: Serial.println("RED"); break;
        case GREEN: Serial.println("GREEN"); break;
        case BLUE: Serial.println("BLUE"); break;
        case YELLOW: Serial.println("YELLOW"); break;
        default: break;
      }
      Serial.print("Queue size: "); Serial.println(beltQueue.size());
    } else {
      Serial.println("Could not confidently classify color.");
    }
    delay(600); // Cool down before next scan
  }

  // 3. Sorting Actuation
  handleSort(BIN_RED_IR, RED, sRed, "RED");
  handleSort(BIN_GRN_IR, GREEN, sGrn, "GREEN");
  handleSort(BIN_BLU_IR, BLUE, sBlu, "BLUE");
}

BoxColor identify(uint16_t r, uint16_t g, uint16_t b) {
  if (r > g && r > b) return RED;
  if (g > r && g > b) return GREEN;
  if (b > r && b > g) return BLUE;
  if (r > 2000 && g > 2000) return YELLOW;
  return NONE;
}

void handleSort(int irPin, BoxColor target, Servo &srv, const char* colorName) {
  if (digitalRead(irPin) == LOW && !beltQueue.empty()) {
    if (beltQueue.front() == target) {
      Serial.print("Match at "); Serial.print(colorName); Serial.println(" bin! Initiating Catch and Sweep.");

      srv.write(90);
      delay(300); // Catch
      Serial.println("  -> Caught object (90 deg)");

      srv.write(180);
      delay(500); // Sweep
      Serial.println("  -> Swept object (180 deg)");

      srv.write(0);
      // Reset
      Serial.println("  -> Servo returned to home (0 deg)");

      beltQueue.pop();
      totals[target-1]++;
      updateLCD(target);
      Serial.print("Current queue size: "); Serial.println(beltQueue.size());

      // Basic debounce for bin IR to avoid double triggers
      delay(500);
    } else {
        // Optional debugging: We saw something, but it's not our color.
        // Uncommenting this could be noisy if the sensor is held down.
        // Serial.print("Object passed "); Serial.print(colorName); Serial.println(" bin, ignoring.");
    }
  }
}

void updateLCD(BoxColor last) {
  lcd.setCursor(0, 0); lcd.print("SYS: RUNNING    ");
  lcd.setCursor(0, 1); lcd.print("SCAN: ");
  lcd.print(last == RED ? "RED  " : last == GREEN ? "GREEN" : "BLUE ");
  lcd.setCursor(0, 2);
  lcd.print("R:"); lcd.print(totals[0]);
  lcd.print(" G:"); lcd.print(totals[1]);
  lcd.print(" B:"); lcd.print(totals[2]);
  lcd.setCursor(0, 3);
  lcd.print("TOTAL COUNT: "); lcd.print(totals[0]+totals[1]+totals[2]);
}
