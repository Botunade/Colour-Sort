#include "Adafruit_TCS34725.h"
#include <Arduino.h>
#include <ESP32Servo.h>
#include <LiquidCrystal_I2C.h>
#include <Wire.h>

// ============================================================================
// COLOR SORTING SYSTEM - FINAL FIRMWARE
//
//
// HARDWARE LAYOUT:
//   - Left Rail:  Servo Motors, Catch Bins
//   - Right Rail: HW-201 IR Sensors (looking across the belt)
// ============================================================================

#define SERVO_RED_PIN 13
#define SERVO_GREEN_PIN 12
#define SERVO_BLUE_PIN 14
#define CONVEYOR_RELAY 26

#define IR_ENTRY_PIN 32
#define IR_RED_PIN 33
#define IR_GREEN_PIN 25
#define IR_BLUE_PIN 34

#define RELAY_ON HIGH
#define RELAY_OFF LOW

Servo servoRed;
Servo servoGreen;
Servo servoBlue;

Adafruit_TCS34725 tcs =
    Adafruit_TCS34725(TCS34725_INTEGRATIONTIME_50MS, TCS34725_GAIN_4X);
LiquidCrystal_I2C lcd(0x27, 16, 2);

// --- System Lifetime Counters ---
int totalRed = 0;
int totalGreen = 0;
int totalBlue = 0;

// --- Multi-Box Queue Architecture ---
#define MAX_BOXES 10
enum ColorResult { NONE, RED_OBJ, GREEN_OBJ, BLUE_OBJ };

struct BoxRecord {
  ColorResult color;
  int zone;
  unsigned long clearTimer; // Replaced yellowTimer with generic clear timer
};

BoxRecord conveyorQueue[MAX_BOXES];
int boxCount = 0;

// --- Timers & Edge Detection ---
bool lastRedIR = HIGH;
bool lastGreenIR = HIGH;
bool lastBlueIR = HIGH;
unsigned long lastEntryTrigger = 0;

bool scanPending = false;
unsigned long scanTimeout = 0;
unsigned long lastFastRead = 0;

bool redDropping = false;
unsigned long redDropEnd = 0;
bool greenDropping = false;
unsigned long greenDropEnd = 0;
bool blueDropping = false;
unsigned long blueDropEnd = 0;

// --- LCD Management Variables ---
unsigned long lastLcdUpdate = 0;
String lcdStatus = "Sys: Ready";
unsigned long lcdStatusTimeout = 0;

// --- Helper: Update Status Message ---
void setStatus(String msg, int timeoutMs = 2000) {
  lcdStatus = msg;
  lcdStatusTimeout = millis() + timeoutMs;
}

// --- Differential Color Detection Logic ---
ColorResult detectColor(int R, int G, int B, uint16_t C) {
  if (C < 280)
    return NONE;

  // RED BOX
  if (R >= 170 && G <= 50 && B <= 50)
    return RED_OBJ;
  // GREEN BOX
  if (G > R && G >= 105 && B <= 48)
    return GREEN_OBJ;
  // BLUE BOX
  if (R <= 62 && (B - R) >= 35 && B > R && G > R)
    return BLUE_OBJ;

  return NONE;
}

// --- Queue Management ---
void removeBox(int index) {
  for (int i = index; i < boxCount - 1; i++) {
    conveyorQueue[i] = conveyorQueue[i + 1];
  }
  boxCount--;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Wire.begin();

  // Initialize LCD
  Wire.beginTransmission(0x27);
  if (Wire.endTransmission() == 0) {
    lcd.init();
    lcd.backlight();
    lcd.setCursor(0, 0);
    lcd.print("System Booting..");
  }

  Serial.println(F("\n=================================================="));
  Serial.println(F("  ESP32 FIFO SORTER: STOP-AND-SCAN FIRMWARE       "));
  Serial.println(F("=================================================="));

  pinMode(CONVEYOR_RELAY, OUTPUT);
  digitalWrite(CONVEYOR_RELAY, RELAY_OFF);

  // Inputs (Sensors)
  pinMode(IR_ENTRY_PIN, INPUT_PULLUP);
  pinMode(IR_RED_PIN, INPUT_PULLUP);
  pinMode(IR_GREEN_PIN, INPUT_PULLUP);
  pinMode(IR_BLUE_PIN, INPUT); // Requires physical pull-up!

  if (tcs.begin()) {
    Serial.println(F("[INIT] TCS34725 Color Sensor Found!"));
  } else {
    Serial.println(F("[ERROR] No TCS34725 found. Check I2C wiring."));
    lcd.setCursor(0, 1);
    lcd.print("SENSOR ERROR!   ");
    while (1)
      ;
  }

  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);

  servoRed.setPeriodHertz(50);
  servoGreen.setPeriodHertz(50);
  servoBlue.setPeriodHertz(50);

  servoRed.attach(SERVO_RED_PIN, 500, 2400);
  servoGreen.attach(SERVO_GREEN_PIN, 500, 2400);
  servoBlue.attach(SERVO_BLUE_PIN, 500, 2400);

  // Idle State = 90 Degrees
  servoRed.write(90);
  servoGreen.write(90);
  servoBlue.write(90);
}

void loop() {
  bool currEntryIR = digitalRead(IR_ENTRY_PIN);
  bool currRedIR = digitalRead(IR_RED_PIN);
  bool currGreenIR = digitalRead(IR_GREEN_PIN);
  bool currBlueIR = digitalRead(IR_BLUE_PIN);

  // ========================================================================
  // 1. ENTRY IR & SCANNING
  // ========================================================================
  if (currEntryIR == LOW && millis() - lastEntryTrigger > 2000) {
    lastEntryTrigger = millis();
    scanPending = true;
    scanTimeout = millis() + 1500;

    Serial.println(F("\n[ENTRY] IR Triggered! Belt Paused. Scanning..."));
    setStatus("Scanning...");
  }

  if (scanPending) {
    if (millis() - lastFastRead > 50) {
      lastFastRead = millis();

      uint16_t r_raw, g_raw, b_raw, c;
      tcs.getRawData(&r_raw, &g_raw, &b_raw, &c);

      int nR = 0, nG = 0, nB = 0;
      if (c > 0) {
        nR = (r_raw * 255) / c;
        nG = (g_raw * 255) / c;
        nB = (b_raw * 255) / c;
      }

      ColorResult detected = detectColor(nR, nG, nB, c);

      if (detected != NONE && boxCount < MAX_BOXES) {
        conveyorQueue[boxCount].color = detected;
        conveyorQueue[boxCount].zone = 0;
        boxCount++;

        String colorName = "UNKNWN";
        if (detected == RED_OBJ)
          colorName = "RED";
        if (detected == GREEN_OBJ)
          colorName = "GREEN";
        if (detected == BLUE_OBJ)
          colorName = "BLUE";

        Serial.printf("[SUCCESS] >> %s << Box locked! Belt Resuming.\n",
                      colorName.c_str());
        setStatus("Found: " + colorName, 2000);
        scanPending = false;
      }
    }

    if (millis() > scanTimeout && scanPending) {
      scanPending = false;
      Serial.println(F("[TIMEOUT] Scan window closed. Belt Resuming."));
      setStatus("Scan Failed", 2000);
    }
  }

  // ========================================================================
  // 2. BIN IR TRIGGERS (Zone Tracking / Shift Register)
  // ========================================================================
  if (currRedIR == LOW && lastRedIR == HIGH) {
    for (int i = 0; i < boxCount; i++) {
      if (conveyorQueue[i].zone == 0) {
        if (conveyorQueue[i].color == RED_OBJ) {
          Serial.println(F("[BIN 1] RED box caught! Dragging..."));
          setStatus("Dropped: RED");
          totalRed++;
          servoRed.write(180);
          redDropping = true;
          redDropEnd = millis() + 1000;
          removeBox(i);
        } else {
          conveyorQueue[i].zone = 1;
        }
        break;
      }
    }
  }

  if (currGreenIR == LOW && lastGreenIR == HIGH) {
    for (int i = 0; i < boxCount; i++) {
      if (conveyorQueue[i].zone == 1) {
        if (conveyorQueue[i].color == GREEN_OBJ) {
          Serial.println(F("[BIN 2] GREEN box caught! Dragging..."));
          setStatus("Dropped: GREEN");
          totalGreen++;
          servoGreen.write(180);
          greenDropping = true;
          greenDropEnd = millis() + 1000;
          removeBox(i);
        } else {
          conveyorQueue[i].zone = 2;
        }
        break;
      }
    }
  }

  if (currBlueIR == LOW && lastBlueIR == HIGH) {
    for (int i = 0; i < boxCount; i++) {
      if (conveyorQueue[i].zone == 2) {
        if (conveyorQueue[i].color == BLUE_OBJ) {
          Serial.println(F("[BIN 3] BLUE box caught! Dragging..."));
          setStatus("Dropped: BLUE");
          totalBlue++;
          servoBlue.write(180);
          blueDropping = true;
          blueDropEnd = millis() + 1000;
          removeBox(i);
        } else {
          Serial.println(F("[BIN 3] Unmatched Box passing to End of Line."));
          setStatus("Passing: END");
          conveyorQueue[i].zone = 3;
          conveyorQueue[i].clearTimer = millis() + 8000;
        }
        break;
      }
    }
  }

  // ========================================================================
  // 3. SERVO RESETS & END OF LINE TIMEOUTS
  // ========================================================================
  if (redDropping && millis() > redDropEnd) {
    servoRed.write(90);
    redDropping = false;
  }
  if (greenDropping && millis() > greenDropEnd) {
    servoGreen.write(90);
    greenDropping = false;
  }
  if (blueDropping && millis() > blueDropEnd) {
    servoBlue.write(90);
    blueDropping = false;
  }

  for (int i = 0; i < boxCount; i++) {
    if (conveyorQueue[i].zone == 3 && millis() > conveyorQueue[i].clearTimer) {
      Serial.println(F("[END] Box cleared the belt."));
      removeBox(i);
      break;
    }
  }

  // ========================================================================
  // 4. DYNAMIC SERVO POSITIONING (Look-Ahead Catch Logic)
  // ========================================================================
  if (!redDropping) {
    bool cR = false;
    for (int i = 0; i < boxCount; i++) {
      if (conveyorQueue[i].zone == 0 && conveyorQueue[i].color == RED_OBJ)
        cR = true;
    }
    servoRed.write(cR ? 0 : 90);
  }

  if (!greenDropping) {
    bool cG = false;
    for (int i = 0; i < boxCount; i++) {
      if ((conveyorQueue[i].zone == 0 || conveyorQueue[i].zone == 1) &&
          conveyorQueue[i].color == GREEN_OBJ)
        cG = true;
    }
    servoGreen.write(cG ? 0 : 90);
  }

  if (!blueDropping) {
    bool cB = false;
    for (int i = 0; i < boxCount; i++) {
      if ((conveyorQueue[i].zone <= 2) && conveyorQueue[i].color == BLUE_OBJ)
        cB = true;
    }
    servoBlue.write(cB ? 0 : 90);
  }

  // ========================================================================
  // 5. MOTOR CONTROL (Stop-and-Scan Enabled)
  // ========================================================================
  if (scanPending) {
    digitalWrite(CONVEYOR_RELAY, RELAY_OFF);
  } else if (boxCount > 0) {
    digitalWrite(CONVEYOR_RELAY, RELAY_ON);
  } else {
    digitalWrite(CONVEYOR_RELAY, RELAY_OFF);
  }

  // ========================================================================
  // 6. DYNAMIC LCD DASHBOARD (Non-blocking, updates 4Hz)
  // ========================================================================
  if (millis() - lastLcdUpdate >= 250) {
    lastLcdUpdate = millis();

    if (millis() > lcdStatusTimeout) {
      lcdStatus = (boxCount > 0) ? "Belt Running..." : "Sys: Ready     ";
    }

    // --- Render Line 1 (Status) ---
    char line1[17];
    snprintf(line1, sizeof(line1), "%-16s", lcdStatus.c_str());
    lcd.setCursor(0, 0);
    lcd.print(line1);

    // --- Render Line 2 (Data) ---
    char line2[17];
    if (boxCount > 0) {
      int qR = 0, qG = 0, qB = 0;
      for (int i = 0; i < boxCount; i++) {
        if (conveyorQueue[i].color == RED_OBJ)
          qR++;
        else if (conveyorQueue[i].color == GREEN_OBJ)
          qG++;
        else if (conveyorQueue[i].color == BLUE_OBJ)
          qB++;
      }
      snprintf(line2, sizeof(line2), "Q: %dR %dG %dB      ", qR, qG, qB);
    } else {
      int cycle = (millis() / 2500) % 2;
      if (cycle == 0) {
        snprintf(line2, sizeof(line2), "Tot: R:%-3d G:%-3d", totalRed,
                 totalGreen);
      } else {
        snprintf(line2, sizeof(line2), "Tot: B:%-3d      ", totalBlue);
      }
    }

    lcd.setCursor(0, 1);
    lcd.print(line2);
  }

  // Update IR Edges
  lastRedIR = currRedIR;
  lastGreenIR = currGreenIR;
  lastBlueIR = currBlueIR;
}