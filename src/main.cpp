#include <Wire.h>
#include <Adafruit_TCS34725.h>
#include <ESP32Servo.h>
#include <LiquidCrystal_I2C.h>
#include <queue>

// PIN CONFIGURATION
#define INTAKE_IR 4
#define RELAY_PIN 5
const int BIN_IR_PINS[] = {16, 17, 18}; // Red, Green, Blue
const int SERVO_PINS[] = {13, 12, 14};  // Red, Green, Blue

enum BoxColor { NONE, RED, GREEN, BLUE, YELLOW };
std::queue<BoxColor> beltQueue;
int totals[] = {0, 0, 0, 0};

// SERVO STATES
enum ServoState { IDLE, CATCH, SWEEP, RETURNING };
struct Bin {
    Servo servo;
    ServoState state = IDLE;
    unsigned long timer = 0;
    BoxColor color;
    int irPin;
};

Bin bins[3];
Adafruit_TCS34725 tcs = Adafruit_TCS34725(TCS34725_INTEGRATIONTIME_2_4MS, TCS34725_GAIN_16X);
LiquidCrystal_I2C lcd(0x27, 16, 4);

void handleIntake();
void handleColorSensor();
BoxColor identify(uint16_t r, uint16_t g, uint16_t b);
void runBinStateMachine(Bin &b);
void updateLCD(BoxColor last);

void setup() {
    Serial.begin(115200);
    pinMode(INTAKE_IR, INPUT);
    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, LOW);

    // Initialize Bins
    BoxColor binColors[] = {RED, GREEN, BLUE};
    for(int i=0; i<3; i++) {
        bins[i].color = binColors[i];
        bins[i].irPin = BIN_IR_PINS[i];
        pinMode(bins[i].irPin, INPUT);
        bins[i].servo.attach(SERVO_PINS[i]);
        bins[i].servo.write(0);
    }

    lcd.init();
    lcd.backlight();
    if (!tcs.begin()) {
        Serial.println("TCS34725 Error");
        while (1);
    }
    lcd.print("SYSTEM READY");
}

void loop() {
    handleIntake();
    handleColorSensor();
    for(int i=0; i<3; i++) {
        runBinStateMachine(bins[i]);
    }
}

// 1. NON-BLOCKING INTAKE
void handleIntake() {
    if (digitalRead(INTAKE_IR) == LOW) {
        digitalWrite(RELAY_PIN, HIGH);
    }
}

// 2. IMPROVED COLOR IDENTIFICATION
void handleColorSensor() {
    uint16_t r, g, b, c;
    tcs.getRawData(&r, &g, &b, &c);

    static unsigned long lastScan = 0;
    if (c > 1500 && millis() - lastScan > 1000) { // Ambient threshold
        BoxColor detected = identify(r, g, b);
        if (detected != NONE) {
            beltQueue.push(detected);
            updateLCD(detected);
            Serial.printf("Detected: %d | Queue: %d\n", detected, beltQueue.size());
        }
        lastScan = millis();
    }
}

BoxColor identify(uint16_t r, uint16_t g, uint16_t b) {
    // Check for Yellow (High Red and Green)
    if (r > 2000 && g > 2000 && b < 1500) return YELLOW;
    // Check dominant channel
    if (r > g && r > b) return RED;
    if (g > r && g > b) return GREEN;
    if (b > r && b > g) return BLUE;
    return NONE;
}

// 3. ASYNC SERVO STATE MACHINE
void runBinStateMachine(Bin &b) {
    switch (b.state) {
        case IDLE:
            if (digitalRead(b.irPin) == LOW && !beltQueue.empty()) {
                if (beltQueue.front() == b.color) {
                    b.servo.write(90); // Catch
                    b.timer = millis();
                    b.state = CATCH;
                } else if (beltQueue.front() == YELLOW) {
                    beltQueue.pop(); // Fix: Actually pop the yellow item so it doesn't block the queue
                    // Ignore, let it pass to the end
                }
            }
            break;

        case CATCH:
            if (millis() - b.timer > 300) {
                b.servo.write(180); // Sweep
                b.timer = millis();
                b.state = SWEEP;
            }
            break;

        case SWEEP:
            if (millis() - b.timer > 500) {
                b.servo.write(0); // Return
                b.timer = millis();
                b.state = RETURNING;
            }
            break;

        case RETURNING:
            if (millis() - b.timer > 400) {
                beltQueue.pop();
                totals[b.color - 1]++;
                updateLCD(b.color);
                b.state = IDLE;
            }
            break;
    }
}

void updateLCD(BoxColor last) {
    lcd.setCursor(0, 0); lcd.print("SYS: OPERATIONAL");
    lcd.setCursor(0, 1); lcd.print("SCAN: ");
    lcd.print(last == RED ? "RED  " : last == GREEN ? "GREEN" : last == YELLOW ? "YELLW" : "BLUE ");
    lcd.setCursor(0, 2);
    lcd.print("R:"); lcd.print(totals[0]);
    lcd.print(" G:"); lcd.print(totals[1]);
    lcd.print(" B:"); lcd.print(totals[2]);
    lcd.setCursor(0, 3);
    lcd.print("TOTAL COUNT: "); lcd.print(totals[0] + totals[1] + totals[2]);
}
