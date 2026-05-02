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

BoxColor identify(uint16_t r, uint16_t g, uint16_t b);
void handleSort(int irPin, BoxColor target, Servo &srv);
void updateLCD(BoxColor last);

void setup() {
  pinMode(INTAKE_IR, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BIN_RED_IR, INPUT); pinMode(BIN_GRN_IR, INPUT); pinMode(BIN_BLU_IR, INPUT);

  digitalWrite(RELAY_PIN, LOW);

  // Servo Homing to 0 Degrees
  sRed.attach(SERVO_RED_PIN); sRed.write(0);
  sGrn.attach(SERVO_GRN_PIN); sGrn.write(0);
  sBlu.attach(SERVO_BLU_PIN); sBlu.write(0);

  lcd.init();
  lcd.backlight();
  tcs.begin();
  lcd.print("SYSTEM READY");
}

void loop() {
  // 1. Motor Wake-up
  if (digitalRead(INTAKE_IR) == LOW) digitalWrite(RELAY_PIN, HIGH);

  // 2. Color Scanning
  uint16_t r, g, b, c;
  tcs.getRawData(&r, &g, &b, &c);
  if (c > 1500) {
    BoxColor detected = identify(r, g, b);
    if (detected != NONE) {
      beltQueue.push(detected);
      updateLCD(detected);
    }
    delay(600);
  }

  // 3. Sorting Actuation
  handleSort(BIN_RED_IR, RED, sRed);
  handleSort(BIN_GRN_IR, GREEN, sGrn);
  handleSort(BIN_BLU_IR, BLUE, sBlu);
}

BoxColor identify(uint16_t r, uint16_t g, uint16_t b) {
  if (r > g && r > b) return RED;
  if (g > r && g > b) return GREEN;
  if (b > r && b > g) return BLUE;
  if (r > 2000 && g > 2000) return YELLOW; //
  return NONE;
}

void handleSort(int irPin, BoxColor target, Servo &srv) {
  if (digitalRead(irPin) == LOW && !beltQueue.empty()) {
    if (beltQueue.front() == target) {
      srv.write(90);  delay(300); // Catch
      srv.write(180); delay(500); // Sweep
      srv.write(0);               // Reset
      beltQueue.pop();
      totals[target-1]++;
      updateLCD(target);
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
