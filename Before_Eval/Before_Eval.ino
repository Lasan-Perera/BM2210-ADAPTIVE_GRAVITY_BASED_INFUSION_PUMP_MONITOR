#include <Keypad.h>
#include <TM1637Display.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include "esp_system.h"

const byte ROWS = 3;
const byte COLS = 4;

char keys[ROWS][COLS] = {
  {'1','4','7','*'},
  {'2','5','8','0'},
  {'3','6','9','#'}
};

byte rowPins[ROWS] = {27, 26, 25};
byte colPins[COLS] = {33, 32, 14, 12};

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

#define CLK 2
#define DIO 15
TM1637Display display(CLK, DIO);

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SDA_PIN 21
#define SCL_PIN 22
#define BUZZER_PIN 13
Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

#define DROP_PIN 4
volatile unsigned long dropCount = 0;
volatile unsigned long lastDropTime = 0;  // microseconds

// new alarm stages
const unsigned long deviationDelay = 5000;     // 5 seconds
const unsigned long patternDuration = 3000;    // 3 seconds

bool patternPhase = false;
bool continuousPhase = false;

unsigned long melodyPrev = 0;
unsigned long panicPrev = 0;

bool inMelody = false;
bool inPanic = false;

int melodyStep = 0;
int panicStep = 0;

unsigned long medicineVolume = 0;  // mL
unsigned long durationMinutes = 0;
float presetDropRate = 0;  // DPM
unsigned long alarmBlockUntil = 0;


// buzz blinking
unsigned long previousBlinkTime = 0;
bool buzzState = false;
const unsigned long blinkInterval = 250; // ms

// display updating
unsigned long previousDisplayMillis = 0;
const unsigned long displayInterval = 500;

// Beeping overtime correction
unsigned long overLimitStart = 0;  
bool alarmActive = false;           
const unsigned long overLimitDelay = 10000; 

#define NUM_SAMPLES 5
volatile unsigned long dropTimes[NUM_SAMPLES];
volatile int dropIndex = 0;
bool enoughDrops = false;

float currentDPM = 0.0;

// ISR drop detection
void IRAM_ATTR onDropDetected() {
    unsigned long now = micros();

    if (now - lastDropTime >  000) {
        dropTimes[dropIndex] = now;       
        dropIndex = (dropIndex + 1) % NUM_SAMPLES;
        dropCount++;
        lastDropTime = now;

        if (dropCount >= NUM_SAMPLES) enoughDrops = true; // start DPM calculation
    }
}

// keyboard input
unsigned long getUserInput(String prompt) {
    String input = "";
    oled.clearDisplay();
    oled.setTextSize(2);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0,0);
    oled.println(prompt);
    oled.display();

    while (true) {
        char key = keypad.getKey();
        if (key) {
            if (key >= '0' && key <= '9') {
                input += key;
            } else if (key == '*') { // backspace
                if (input.length() > 0) input.remove(input.length() - 1);
            } else if (key == '#') { // confirm
                if (input.length() > 0) return input.toInt();
            }

            // Display current input
            oled.clearDisplay();
            oled.setTextSize(2);
            oled.setCursor(0,0);
            oled.println(prompt);
            oled.setTextSize(3);
            oled.setCursor(0,30);
            oled.println(input);
            oled.display();
        }
    }
}

// --- Calculate instantaneous DPM using last few drops ---
float calculateInstantDPM() {
    if (!enoughDrops) return 0; // wait until 5 drops detected

    // Find oldest and newest drop in buffer
    unsigned long oldest = dropTimes[dropIndex]; // circular buffer
    unsigned long newest = dropTimes[(dropIndex + NUM_SAMPLES - 1) % NUM_SAMPLES];

    unsigned long dt = newest - oldest; // total microseconds between 5 drops
    if (dt == 0) return 0;

    float dpm = (NUM_SAMPLES - 1) * 60.0 * 1000000.0 / dt; // extrapolate to drops per minute
    return dpm;
}

void playMelodyPattern() {
    static int sequence[] = {1,0,1,0,0,1,1,0};   // melody feel
    static int len = 8;

    if (millis() - melodyPrev > 200) {   // slow beautiful
        melodyPrev = millis();
        melodyStep = (melodyStep + 1) % len;
        digitalWrite(BUZZER_PIN, sequence[melodyStep]);
    }
}

void playPanicPattern() {
    unsigned long interval = max(40, 200 - panicStep*10); // accelerates
    if (millis() - panicPrev > interval) {
        panicPrev = millis();
        panicStep++;
        digitalWrite(BUZZER_PIN, !digitalRead(BUZZER_PIN));
    }
}


void setup() {
    Serial.begin(115200);
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
    
    // --- TM1637 ---
    display.setBrightness(0x0f);
    display.clear();

    // --- OLED ---
    Wire.begin(SDA_PIN, SCL_PIN);
    if(!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("OLED init failed");
        for(;;);
    }
    oled.clearDisplay();
    oled.display();
    alarmBlockUntil = millis() + 30000;  // 5 minutes


    // --- Drop Sensor Interrupt ---
    pinMode(DROP_PIN, INPUT_PULLUP); // active LOW
    attachInterrupt(digitalPinToInterrupt(DROP_PIN), onDropDetected, FALLING);

    // --- Get medicine volume and duration from user ---
    medicineVolume = getUserInput("Med Vol   (mL):");
    durationMinutes = getUserInput("Duration  (min):");

    // Calculate preset drop rate: drops per minute
    presetDropRate = (medicineVolume * 20.0) / durationMinutes;

    oled.clearDisplay();
    oled.setTextSize(2);
    oled.setCursor(0,0);
    oled.println("Preset DPM");
    oled.setTextSize(3);
    oled.setCursor(0,30);
    oled.println(presetDropRate);
    oled.display();

    delay(3000); // show preset rate for 3 seconds
}

void loop() {
    // --- Keypad handling ---
    char key = keypad.getKey();
    if (key == '*') {
      oled.clearDisplay();
      oled.setTextSize(2);
      oled.setCursor(0,0);
      oled.println("Rebooting...");
      oled.display();
      digitalWrite(BUZZER_PIN, LOW);
      delay(500);       
      esp_restart();
    }
    if (key == '#') {
    dropCount = 0;  // reset counter
    display.clear();
    oled.clearDisplay();
    oled.setTextSize(2);
    oled.setCursor(0,0);
    oled.println("Reset Done");
    digitalWrite(BUZZER_PIN, LOW);
    oled.display();
    delay(100);
}

    unsigned long currentMillis = millis();

    // --- Calculate instantaneous current DPM ---
    currentDPM = calculateInstantDPM();
    float percentageError = 0;

    if(presetDropRate > 0){
        percentageError = fabs(currentDPM - presetDropRate) / presetDropRate * 100.0;
    }

    // --- Update displays every 0.5 second ---
    if (currentMillis - previousDisplayMillis >= displayInterval) {
        // TM1637: current DPM
        display.showNumberDec((int)currentDPM, false);

        // OLED: preset DPM and current DPM
        oled.clearDisplay();
        oled.setTextSize(1);
        oled.setCursor(0,0);
        oled.println("Preset DPM:");
        oled.setTextSize(3);
        oled.setCursor(0,18);
        oled.println((int)presetDropRate);
        oled.setTextSize(1);
        oled.setCursor(0,40);
        oled.println("Current DPM:");
        oled.setTextSize(2);
        oled.setCursor(0,50);
        oled.println((int)currentDPM);
        oled.display();

        previousDisplayMillis = currentMillis;
    }

    if(millis() < alarmBlockUntil){
        digitalWrite(BUZZER_PIN, LOW);
        oled.clearDisplay();
        oled.setTextSize(2);
        oled.setCursor(0,44);
        oled.println("ADJUSTING");
        oled.display();

        return;
    }


    // not enough drops → do nothing
    if(!enoughDrops){
        digitalWrite(BUZZER_PIN, LOW);
        return;
    }

    if(percentageError < 10){
        // NORMAL
        digitalWrite(BUZZER_PIN, LOW);
    }
    else if(percentageError < 20){
        // WARNING
        // beep with pauses
        if(millis() - melodyPrev > 500){
            melodyPrev = millis();
            digitalWrite(BUZZER_PIN, !digitalRead(BUZZER_PIN));
        }
    }
    else {
        // CRITICAL
        // panic alarm
        playPanicPattern();
    }



}
