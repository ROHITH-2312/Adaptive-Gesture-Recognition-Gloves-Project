/*
  GESTURE SIGN LANGUAGE GLOVE - v13 (GEMINI AI + FILE PERSISTENCE)
  ---------------------------------------------------------------
  Key Features:
  - Gemini AI Integration via Serial bridge (bridge.py on PC)
  - Bi-directional Serial Protocol to save/load gestures to/from .txt file
  - Text File Persistence:
      * PC -> Arduino: "GEST:label,f1,f2,f3,f4,pitch,roll,gyro,radius" (loads gesture)
      * Arduino -> PC: "GEST:..." emitted on calibration (auto-saved to gestures.txt)
      * PC -> Arduino: "DUMP" or "DUMP_GEST" (Arduino dumps all gestures)
      * PC -> Arduino: "CLEAR_GEST" (clears gesture library in RAM and EEPROM)
  - EEPROM Non-volatile Storage on ATmega328P
  - Fixed LCD sentence display overwrite bug (4s viewing window)
  - Fixed Roll angle 360-degree boundary wrap in distance computation
  - Fixed CONFIRM button hold re-triggering lock
  - Fixed Gesture jitter/duplicate registration
  - Zero-flicker padded LCD updates
  - Safe string handling and null-termination

  Hardware:
    - 4x Potentiometer (Index/Middle/Ring/Pinky) -> A0, A1, A2, A3
    - MPU6050  -> I2C (SDA->A4, SCL->A5), addr 0x68
    - 16x2 LCD -> I2C (SDA->A4, SCL->A5), addr 0x27
    - MODE Button    -> Pin 2 (INPUT_PULLUP)
    - CONFIRM Button -> Pin 3 (INPUT_PULLUP)
    - Status LED     -> Pin 13
    - Buzzer         -> Pin 8
*/

#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>

// ---------- Pins ----------
#define FINGER1_PIN A0
#define FINGER2_PIN A1
#define FINGER3_PIN A2
#define FINGER4_PIN A3
#define MODE_BUTTON     2
#define CONFIRM_BUTTON  3
#define STATUS_LED      13
#define BUZZER_PIN      8

// ---------- Constants ----------
#define NUM_FINGERS           4
#define MAX_GESTURES          8
#define LABEL_LEN             12
#define EEPROM_MAGIC          0x474D   // "GM" v13 magic key
#define EEPROM_START_ADDR     0
#define NUM_CALIB_SAMPLES     20
#define THRESHOLD_SAFETY_FACTOR 1.8f
#define MIN_RADIUS            12.0f
#define POT_RAW_MAX           1023.0f
#define GYRO_NORM_MAX         250.0f
#define PITCH_MIN             -90.0f
#define PITCH_MAX              90.0f
#define ROLL_MIN              -180.0f
#define ROLL_MAX               180.0f
#define ANALOG_SAMPLES        3
#define NOISE_DEADZONE        1.5f
#define DETECTION_INTERVAL_MS 200
#define MODE_SWITCH_INTERVAL  350
#define CONFIRM_HOLD_MS       2000    // hold CONFIRM 2s in DETECT to send to Gemini
#define SERIAL_BUF_LEN        96      // max incoming line buffer length (+null)
#define SENTENCE_VIEW_MS      4000    // Keep AI sentence visible on LCD before resuming detection
#define UNKNOWN_RESET_MS      1000    // Time in unknown state required before repeating same word

// ---------- Data Structure ----------
struct GestureSignature {
  char  label[LABEL_LEN];
  float fingerValues[NUM_FINGERS];
  float pitch;
  float roll;
  float gyroMag;
  float radius;
  bool  isSet;
};

GestureSignature library[MAX_GESTURES];
int gestureCount = 0;

// ---------- System State ----------
enum SystemState { IDLE, CALIBRATE, DETECT };
SystemState currentState = IDLE;

// ---------- Hardware Objects ----------
Adafruit_MPU6050    mpu;
LiquidCrystal_I2C   lcd(0x27, 16, 2);
bool mpuAvailable = false;

// ---------- Button Timers ----------
unsigned long lastModePress    = 0;
unsigned long lastConfirmPress = 0;
bool libFullDisplayed = false;

// ---------- Detection State ----------
unsigned long lastDetectionCheck = 0;
float lastFilteredFingers[NUM_FINGERS] = {0, 0, 0, 0};
bool  filterInitialized  = false;
char  lastDisplayedResult[LABEL_LEN] = "";
char  lastAddedGesture[LABEL_LEN] = "";
unsigned long unknownStateStartTime = 0;

// ---------- LLM Word Buffer ----------
char wordBuffer[MAX_GESTURES][LABEL_LEN];
int  wordCount           = 0;
bool waitingForSentence  = false;
unsigned long sentenceDisplayUntil = 0;
bool showingSentence     = false;
unsigned long waitingStartTime = 0;

// ---------- Incoming Serial Buffer ----------
char serialInBuf[SERIAL_BUF_LEN];
int  serialInIdx = 0;

// ---------- Calibrate Label Buffer ----------
char calibInputBuf[LABEL_LEN] = "";
int  calibInputIdx = 0;

// ---------- Forward Declarations ----------
void showIdleScreen();
void showCalibrateScreen();
void showDetectScreen();
void updateDetectCountDisplay();
void lcdPrintPadded(uint8_t col, uint8_t row, const char *str);
void lcdPrintPadded(uint8_t col, uint8_t row, const __FlashStringHelper *ifsh);
void readRawNormalizedSnapshot(float outFingers[NUM_FINGERS], float &outPitch,
                               float &outRoll, float &outGyroMag);
float signatureDistance(float fA[NUM_FINGERS], float pA, float rA, float gA,
                        float fB[NUM_FINGERS], float pB, float rB, float gB);
void saveLibraryToEEPROM();
void loadLibraryFromEEPROM();
void clearEEPROM();
void processSerialInput();
void handleSerialCommand(char *line);
bool parseGestureLine(char *line, GestureSignature &outG);
void printGesture(const GestureSignature &g);
void dumpGestures();
void displaySentenceOnLCD(const char *sentence);

// ==========================================================================
//  SETUP
// ==========================================================================
void setup() {
  Serial.begin(9600);
  pinMode(MODE_BUTTON,    INPUT_PULLUP);
  pinMode(CONFIRM_BUTTON, INPUT_PULLUP);
  pinMode(STATUS_LED,     OUTPUT);
  pinMode(BUZZER_PIN,     OUTPUT);

  Wire.begin();
  delay(100);

  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcdPrintPadded(0, 0, F("Glove v13 + AI"));
  lcdPrintPadded(0, 1, F("Initializing..."));

  mpuAvailable = mpu.begin();
  if (mpuAvailable) {
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    mpu.setGyroRange(MPU6050_RANGE_250_DEG);
  } else {
    lcdPrintPadded(0, 0, F("MPU Not Found"));
    lcdPrintPadded(0, 1, F("Flex Only Mode"));
    delay(1000);
  }

  loadLibraryFromEEPROM();

  lcd.clear();
  lcdPrintPadded(0, 0, F("Ready! Gestures:"));
  char countBuf[16];
  snprintf(countBuf, sizeof(countBuf), "%d / %d loaded", gestureCount, MAX_GESTURES);
  lcdPrintPadded(0, 1, countBuf);
  delay(1000);

  showIdleScreen();

  Serial.println(F("=== Gesture Glove v13 + Gemini AI + Text Persistence ==="));
  Serial.println(F("Commands:"));
  Serial.println(F("  Upload: GEST:label,f1,f2,f3,f4,pitch,roll,gyro,radius"));
  Serial.println(F("  DUMP -> Dump loaded gestures in text format"));
  Serial.println(F("  CLEAR_GEST -> Clear all stored gestures"));
  Serial.println(F("  In CALIBRATE mode: type label, hold pose, press CONFIRM"));
  Serial.println(F("  In DETECT mode: hold CONFIRM 2s to translate with Gemini"));
  dumpGestures();
}

// ==========================================================================
//  MAIN LOOP
// ==========================================================================
void loop() {
  processSerialInput();
  handleModeButton();

  switch (currentState) {
    case IDLE:
      digitalWrite(STATUS_LED, LOW);
      handleIdleMode();
      break;

    case CALIBRATE:
      digitalWrite(STATUS_LED, HIGH);
      handleCalibration();
      break;

    case DETECT:
      digitalWrite(STATUS_LED, HIGH);
      handleDetection();
      break;
  }
}

// ==========================================================================
//  LCD UTILITY: Flicker-free fixed length line printing
// ==========================================================================
void lcdPrintPadded(uint8_t col, uint8_t row, const char *str) {
  lcd.setCursor(col, row);
  uint8_t len = 0;
  if (str) {
    while (str[len] && len < (16 - col)) {
      lcd.write(str[len]);
      len++;
    }
  }
  while (len < (16 - col)) {
    lcd.write(' ');
    len++;
  }
}

void lcdPrintPadded(uint8_t col, uint8_t row, const __FlashStringHelper *ifsh) {
  lcd.setCursor(col, row);
  int written = lcd.print(ifsh);
  while (written < (16 - col)) {
    lcd.write(' ');
    written++;
  }
}

// ==========================================================================
//  IDLE MODE  (CONFIRM = reset library)
// ==========================================================================
void handleIdleMode() {
  int confirmReading = digitalRead(CONFIRM_BUTTON);
  static int lastConfirmState = HIGH;

  if (confirmReading == LOW && lastConfirmState == HIGH) {
    if (millis() - lastConfirmPress > MODE_SWITCH_INTERVAL) {
      lastConfirmPress = millis();
      clearEEPROM();
      lcdPrintPadded(0, 0, F("Library Cleared!"));
      lcdPrintPadded(0, 1, F("0 Gestures"));
      tone(BUZZER_PIN, 600, 200);
      delay(1000);
      showIdleScreen();
      Serial.println(F("[EEPROM] Library reset."));
      Serial.println(F("GESTURES_CLEARED"));
    }
  }
  lastConfirmState = confirmReading;
}

// ==========================================================================
//  MODE BUTTON
// ==========================================================================
void handleModeButton() {
  static int lastSteadyState = HIGH;
  static int lastFlickerableState = HIGH;
  static unsigned long lastDebounceTime = 0;
  const unsigned long debounceDelay = 50;

  int currentReading = digitalRead(MODE_BUTTON);

  if (currentReading != lastFlickerableState) {
    lastDebounceTime = millis();
    lastFlickerableState = currentReading;
  }

  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (lastSteadyState == HIGH && currentReading == LOW) {
      lastSteadyState = LOW;
      currentState = (SystemState)((currentState + 1) % 3);

      // Reset state on mode change
      filterInitialized      = false;
      libFullDisplayed       = false;
      lastDisplayedResult[0] = '\0';
      lastAddedGesture[0]    = '\0';
      wordCount              = 0;
      waitingForSentence     = false;
      sentenceDisplayUntil   = 0;
      showingSentence        = false;
      waitingStartTime       = 0;
      calibInputBuf[0]       = '\0';
      calibInputIdx          = 0;
      serialInBuf[0]         = '\0';
      serialInIdx            = 0;

      Serial.println(F("RESET_BUFFER"));

      switch (currentState) {
        case IDLE:
          showIdleScreen();
          Serial.println(F("\n[MODE] -> IDLE (CONFIRM to reset library)"));
          break;
        case CALIBRATE:
          showCalibrateScreen();
          Serial.println(F("\n[MODE] -> CALIBRATE (type label in Serial, hold pose, press CONFIRM)"));
          break;
        case DETECT:
          showDetectScreen();
          Serial.println(F("\n[MODE] -> DETECT (hold CONFIRM 2s to send words to Gemini)"));
          break;
      }
    } else if (lastSteadyState == LOW && currentReading == HIGH) {
      lastSteadyState = HIGH;
    }
  }
}

void showIdleScreen() {
  lcdPrintPadded(0, 0, F("IDLE MODE"));
  char buf[17];
  snprintf(buf, sizeof(buf), "Gest:%d MODE/CNFM", gestureCount);
  lcdPrintPadded(0, 1, buf);
}

void showCalibrateScreen() {
  if (gestureCount >= MAX_GESTURES) {
    lcdPrintPadded(0, 0, F("Library Full!"));
    lcdPrintPadded(0, 1, F("Max 8 Gestures"));
    libFullDisplayed = true;
  } else {
    if (calibInputIdx > 0) {
      char buf[17];
      snprintf(buf, sizeof(buf), "CAL: %s", calibInputBuf);
      lcdPrintPadded(0, 0, buf);
    } else {
      lcdPrintPadded(0, 0, F("CALIBRATE MODE"));
    }
    lcdPrintPadded(0, 1, F("Hold+Press CNFM"));
  }
}

void showDetectScreen() {
  lcdPrintPadded(0, 0, F("DETECT MODE"));
  updateDetectCountDisplay();
}

void updateDetectCountDisplay() {
  char buf[17];
  if (wordCount == 1) {
    snprintf(buf, sizeof(buf), "1 word buffered");
  } else {
    snprintf(buf, sizeof(buf), "%d words buffered", wordCount);
  }
  lcdPrintPadded(0, 1, buf);
}

// ==========================================================================
//  SENSOR READINGS
// ==========================================================================
int readAnalogAveraged(int pin) {
  long sum = 0;
  for (int i = 0; i < ANALOG_SAMPLES; i++) sum += analogRead(pin);
  return (int)(sum / ANALOG_SAMPLES);
}

void readRawNormalizedSnapshot(float outFingers[NUM_FINGERS], float &outPitch,
                               float &outRoll, float &outGyroMag) {
  float newF[NUM_FINGERS];
  newF[0] = (readAnalogAveraged(FINGER1_PIN) / POT_RAW_MAX) * 100.0f;
  newF[1] = (readAnalogAveraged(FINGER2_PIN) / POT_RAW_MAX) * 100.0f;
  newF[2] = (readAnalogAveraged(FINGER3_PIN) / POT_RAW_MAX) * 100.0f;
  newF[3] = (readAnalogAveraged(FINGER4_PIN) / POT_RAW_MAX) * 100.0f;

  if (!filterInitialized) {
    for (int i = 0; i < NUM_FINGERS; i++) lastFilteredFingers[i] = newF[i];
    filterInitialized = true;
  }
  for (int i = 0; i < NUM_FINGERS; i++) {
    if (fabs(newF[i] - lastFilteredFingers[i]) >= NOISE_DEADZONE)
      lastFilteredFingers[i] = newF[i];
    outFingers[i] = lastFilteredFingers[i];
  }

  if (mpuAvailable) {
    sensors_event_t a, gyroEvent, temp;
    mpu.getEvent(&a, &gyroEvent, &temp);

    float rawPitch = atan2(a.acceleration.y,
                           sqrt(a.acceleration.x * a.acceleration.x +
                                a.acceleration.z * a.acceleration.z)) * 180.0f / PI;
    float rawRoll  = atan2(-a.acceleration.x, a.acceleration.z) * 180.0f / PI;

    outPitch = ((rawPitch - PITCH_MIN) / (PITCH_MAX - PITCH_MIN)) * 100.0f;
    outRoll  = ((rawRoll  - ROLL_MIN)  / (ROLL_MAX  - ROLL_MIN))  * 100.0f;
    outPitch = constrain(outPitch, 0.0f, 100.0f);
    outRoll  = constrain(outRoll,  0.0f, 100.0f);

    float gMag = sqrt(gyroEvent.gyro.x * gyroEvent.gyro.x +
                      gyroEvent.gyro.y * gyroEvent.gyro.y +
                      gyroEvent.gyro.z * gyroEvent.gyro.z) * (180.0f / PI);
    outGyroMag = min((gMag / GYRO_NORM_MAX) * 100.0f, 100.0f);
  } else {
    outPitch = 50.0f;
    outRoll  = 50.0f;
    outGyroMag = 0.0f;
  }
}

// Distance computation with wrap-around fix for Roll (0..100 maps to -180..+180 deg)
float signatureDistance(float fingersA[NUM_FINGERS], float pA, float rA, float gA,
                        float fingersB[NUM_FINGERS], float pB, float rB, float gB) {
  float sum = 0.0f;
  for (int i = 0; i < NUM_FINGERS; i++) {
    float d = fingersA[i] - fingersB[i];
    sum += d * d;
  }
  float dp = pA - pB;
  float dr = fabs(rA - rB);
  if (dr > 50.0f) dr = 100.0f - dr; // Circular wrap around -180 / +180 deg
  float dg = gA - gB;

  sum += dp * dp + dr * dr + dg * dg;
  return sqrt(sum);
}

// ==========================================================================
//  CALIBRATION MODE
// ==========================================================================
void handleCalibration() {
  if (gestureCount >= MAX_GESTURES) {
    if (!libFullDisplayed) {
      showCalibrateScreen();
    }
    return;
  }

  int confirmReading = digitalRead(CONFIRM_BUTTON);
  static int lastConfirmState = HIGH;

  if (confirmReading == LOW && lastConfirmState == HIGH) {
    if (millis() - lastConfirmPress > MODE_SWITCH_INTERVAL) {
      lastConfirmPress = millis();

      char label[LABEL_LEN];
      if (calibInputIdx > 0) {
        strncpy(label, calibInputBuf, LABEL_LEN - 1);
        label[LABEL_LEN - 1] = '\0';
      } else {
        snprintf(label, sizeof(label), "GEST_%d", gestureCount + 1);
      }

      lcdPrintPadded(0, 0, F("Sampling..."));
      lcdPrintPadded(0, 1, label);

      Serial.println();
      Serial.print(F(">>> Calibrating '")); Serial.print(label); Serial.println(F("'... Keep pose steady! <<<"));

      const int SAMPLES = 10;
      float sumF[NUM_FINGERS] = {0};
      float sumP = 0, sumR = 0, sumG = 0;
      float minF[NUM_FINGERS], maxF[NUM_FINGERS];
      float minP = 999.0f, maxP = -999.0f;
      float minR = 999.0f, maxR = -999.0f;
      float minG = 999.0f, maxG = -999.0f;

      for (int i = 0; i < NUM_FINGERS; i++) {
        minF[i] = 999.0f;
        maxF[i] = -999.0f;
      }

      for (int s = 0; s < SAMPLES; s++) {
        float f[NUM_FINGERS]; float p, r, g;
        readRawNormalizedSnapshot(f, p, r, g);
        for (int i = 0; i < NUM_FINGERS; i++) {
          sumF[i] += f[i];
          if (f[i] < minF[i]) minF[i] = f[i];
          if (f[i] > maxF[i]) maxF[i] = f[i];
        }
        sumP += p;
        if (p < minP) minP = p;
        if (p > maxP) maxP = p;

        sumR += r;
        if (r < minR) minR = r;
        if (r > maxR) maxR = r;

        sumG += g;
        if (g < minG) minG = g;
        if (g > maxG) maxG = g;

        delay(80);
      }

      GestureSignature ng;
      strncpy(ng.label, label, LABEL_LEN - 1);
      ng.label[LABEL_LEN - 1] = '\0';
      for (int i = 0; i < NUM_FINGERS; i++) ng.fingerValues[i] = sumF[i] / SAMPLES;
      ng.pitch   = sumP / SAMPLES;
      ng.roll    = sumR / SAMPLES;
      ng.gyroMag = sumG / SAMPLES;

      // Calculate spread on the fly without giant stack arrays
      float maxSpread = 0.0f;
      for (int i = 0; i < NUM_FINGERS; i++) {
        float d = maxF[i] - minF[i];
        if (d > maxSpread) maxSpread = d;
      }
      float dp = maxP - minP;
      if (dp > maxSpread) maxSpread = dp;
      float dr = maxR - minR;
      if (dr > maxSpread) maxSpread = dr;
      float dg = maxG - minG;
      if (dg > maxSpread) maxSpread = dg;

      ng.radius = max(maxSpread * 1.5f, MIN_RADIUS);
      ng.isSet  = true;

      // Update in place if label exists, otherwise append
      int targetIdx = gestureCount;
      for (int i = 0; i < gestureCount; i++) {
        if (strcmp(library[i].label, ng.label) == 0) {
          targetIdx = i;
          break;
        }
      }
      if (targetIdx == gestureCount && gestureCount < MAX_GESTURES) {
        gestureCount++;
      }
      library[targetIdx] = ng;
      saveLibraryToEEPROM();
      tone(BUZZER_PIN, 1500, 150);

      // Emit GEST line for bridge / file logger
      printGesture(ng);

      char buf0[17], buf1[17];
      snprintf(buf0, sizeof(buf0), "Saved: %s", ng.label);
      snprintf(buf1, sizeof(buf1), "Total: %d/%d", gestureCount, MAX_GESTURES);
      lcdPrintPadded(0, 0, buf0);
      lcdPrintPadded(0, 1, buf1);

      Serial.print(F("[EEPROM] Saved '")); Serial.print(ng.label);
      Serial.print(F("' (Total: ")); Serial.print(gestureCount);
      Serial.println(F("/8)"));
      Serial.println(F("Copy line above to gestures.txt to keep permanently across simulator resets."));

      delay(1200);
      calibInputBuf[0] = '\0';
      calibInputIdx = 0;
      serialInBuf[0] = '\0';
      serialInIdx = 0;
      showCalibrateScreen();
    }
  }
  lastConfirmState = confirmReading;
}

// ==========================================================================
//  DETECTION MODE (WITH GEMINI AI WORD BUFFER & FIXES)
// ==========================================================================
void handleDetection() {
  // Hold AI sentence on LCD until dismissed by pressing CONFIRM button
  if (showingSentence) {
    int confirmReading = digitalRead(CONFIRM_BUTTON);
    static int lastDismissState = HIGH;
    if (confirmReading == LOW && lastDismissState == HIGH) {
      showingSentence = false;
      showDetectScreen();
      lastDisplayedResult[0] = '\0';
      lastAddedGesture[0]    = '\0';
      tone(BUZZER_PIN, 1200, 80);
      delay(200);
    }
    lastDismissState = confirmReading;
    return;
  }

  // ── CONFIRM hold: trigger Gemini send ────────────────────────────────────
  int confirmReading = digitalRead(CONFIRM_BUTTON);
  static bool confirmHoldActive    = false;
  static bool confirmHoldTriggered = false;
  static unsigned long confirmHoldStart = 0;

  if (confirmReading == LOW) {
    if (!confirmHoldActive && !confirmHoldTriggered) {
      confirmHoldActive = true;
      confirmHoldStart  = millis();
    } else if (confirmHoldActive && (millis() - confirmHoldStart >= CONFIRM_HOLD_MS)) {
      confirmHoldActive    = false;
      confirmHoldTriggered = true; // Lock until physical button is released

      if (wordCount > 0) {
        serialInBuf[0] = '\0';
        serialInIdx = 0;
        Serial.println(F("SEND"));
        waitingForSentence = true;
        waitingStartTime = millis();
        lcdPrintPadded(0, 0, F("Asking Gemini..."));
        lcdPrintPadded(0, 1, F("Hold tight!     "));
      } else {
        lcdPrintPadded(0, 1, F("No words yet!   "));
        delay(800);
        updateDetectCountDisplay();
      }
    }
  } else {
    confirmHoldActive    = false;
    confirmHoldTriggered = false;
  }

  // If waiting for LLM reply, pause recognition with automatic 6-second timeout safety
  if (waitingForSentence) {
    if (millis() - waitingStartTime > 6000) {
      waitingForSentence = false;
      lcdPrintPadded(0, 0, F("Retrying AI...  "));
      lcdPrintPadded(0, 1, F("Hold CNFM again "));
      delay(1200);
      updateDetectCountDisplay();
    }
    return;
  }

  // ── Rate-limited gesture detection ───────────────────────────────────────
  if (millis() - lastDetectionCheck < DETECTION_INTERVAL_MS) return;
  lastDetectionCheck = millis();

  float liveF[NUM_FINGERS]; float liveP, liveR, liveG;
  readRawNormalizedSnapshot(liveF, liveP, liveR, liveG);

  float minDist  = 999999.0f;
  int   bestIdx  = -1;

  for (int i = 0; i < gestureCount; i++) {
    float d = signatureDistance(liveF, liveP, liveR, liveG,
                                library[i].fingerValues, library[i].pitch,
                                library[i].roll, library[i].gyroMag);
    if (d < minDist) { minDist = d; bestIdx = i; }
  }

  bool matched = (bestIdx >= 0) &&
                 (minDist <= library[bestIdx].radius * THRESHOLD_SAFETY_FACTOR);

  const char *resultText = (!matched || gestureCount == 0)
                           ? "Unknown"
                           : library[bestIdx].label;

  // Handle Unknown state timing (prevents momentary jitter between same gesture)
  if (strcmp(resultText, "Unknown") == 0) {
    if (unknownStateStartTime == 0) {
      unknownStateStartTime = millis();
    }
    if (millis() - unknownStateStartTime > UNKNOWN_RESET_MS) {
      lastAddedGesture[0] = '\0'; // Allow signing the same gesture again after rest
    }
  } else {
    unknownStateStartTime = 0;
  }

  // Only update display when result text changes
  if (strcmp(resultText, lastDisplayedResult) != 0) {
    strncpy(lastDisplayedResult, resultText, LABEL_LEN - 1);
    lastDisplayedResult[LABEL_LEN - 1] = '\0';

    lcdPrintPadded(0, 0, resultText);

    if (strcmp(resultText, "Unknown") != 0) {
      // Add to buffer if it's a new gesture or hand rested
      if (strcmp(resultText, lastAddedGesture) != 0) {
        if (wordCount < MAX_GESTURES) {
          strncpy(wordBuffer[wordCount], resultText, LABEL_LEN - 1);
          wordBuffer[wordCount][LABEL_LEN - 1] = '\0';
          wordCount++;

          strncpy(lastAddedGesture, resultText, LABEL_LEN - 1);
          lastAddedGesture[LABEL_LEN - 1] = '\0';

          Serial.print(F("WORD:"));
          Serial.println(resultText);
        }
        tone(BUZZER_PIN, 1500, 80);
      }
    } else {
      tone(BUZZER_PIN, 800, 60);
    }

    updateDetectCountDisplay();
  }
}

// ==========================================================================
//  UNIFIED SERIAL INPUT PROCESSING (Save/Load + AI Sentence + Typing)
// ==========================================================================
void processSerialInput() {
  while (Serial.available() > 0) {
    char c = Serial.read();

    if (c == '\r') continue;

    if (c == '\n') {
      if (serialInIdx > 0) {
        serialInBuf[serialInIdx] = '\0';
        handleSerialCommand(serialInBuf);
        serialInIdx = 0;
      }
    } else if (c == '\b' || c == 127) {
      if (serialInIdx > 0) {
        serialInIdx--;
        serialInBuf[serialInIdx] = '\0';
        if (currentState == CALIBRATE && calibInputIdx > 0) {
          calibInputIdx--;
          calibInputBuf[calibInputIdx] = '\0';
          Serial.print(F("\b \b"));
        }
      }
    } else if (serialInIdx < SERIAL_BUF_LEN - 1) {
      serialInBuf[serialInIdx++] = c;
      serialInBuf[serialInIdx] = '\0';

      // Echo typing in CALIBRATE mode if typing a label
      if (currentState == CALIBRATE &&
          strncmp(serialInBuf, "GEST:", 5) != 0 &&
          strncmp(serialInBuf, "SENTENCE:", 9) != 0 &&
          c >= 32 && c <= 126) {
        if (calibInputIdx < LABEL_LEN - 1) {
          char uc = toupper(c);
          calibInputBuf[calibInputIdx++] = uc;
          calibInputBuf[calibInputIdx] = '\0';
          Serial.print(uc);
        }
      }
    }
  }
}

void handleSerialCommand(char *line) {
  // 1. Gemini AI Translated Sentence (match anywhere on line)
  char *sPos = strstr(line, "SENTENCE:");
  if (sPos != NULL) {
    const char *sentence = sPos + 9;
    displaySentenceOnLCD(sentence);
    showingSentence      = true;  // Keep sentence on LCD until CONFIRM button is pressed
    waitingForSentence   = false;
    wordCount            = 0;
    lastDisplayedResult[0] = '\0';
    lastAddedGesture[0]    = '\0';

    tone(BUZZER_PIN, 1800, 100); delay(120);
    tone(BUZZER_PIN, 2200, 150);
    Serial.print(F("[AI] ")); Serial.println(sentence);
    return;
  }

  // 2. Load Gesture: GEST:<label>,... or <label>,<f0>,<f1>,<f2>,<f3>,<pitch>,<roll>,<gyro>,<radius>
  char *gestPayload = NULL;
  if (strncmp(line, "GEST:", 5) == 0) {
    gestPayload = line + 5;
  } else if (strchr(line, ',') != NULL) {
    gestPayload = line;
  }

  if (gestPayload != NULL) {
    GestureSignature ng;
    if (parseGestureLine(gestPayload, ng)) {
      int targetIdx = gestureCount;
      for (int i = 0; i < gestureCount; i++) {
        if (strcmp(library[i].label, ng.label) == 0) {
          targetIdx = i;
          break;
        }
      }
      if (targetIdx == gestureCount) {
        if (gestureCount < MAX_GESTURES) {
          gestureCount++;
        } else {
          Serial.print(F("[ERROR] Library full, cannot add '"));
          Serial.print(ng.label); Serial.println(F("'"));
          return;
        }
      }
      library[targetIdx] = ng;
      saveLibraryToEEPROM();
      Serial.print(F("[GEST] Loaded: ")); Serial.print(ng.label);
      Serial.print(F(" (")); Serial.print(gestureCount);
      Serial.println(F("/8)"));

      if (currentState == IDLE) showIdleScreen();
      else if (currentState == CALIBRATE) showCalibrateScreen();
      else if (currentState == DETECT) showDetectScreen();
    } else {
      Serial.println(F("[ERROR] Malformed GEST line."));
    }
    return;
  }

  // 3. Dump Gestures to Serial
  if (strcmp(line, "DUMP") == 0 || strcmp(line, "DUMP_GEST") == 0) {
    dumpGestures();
    return;
  }

  // 4. Clear Gesture Library
  if (strcmp(line, "CLEAR_GEST") == 0) {
    clearEEPROM();
    Serial.println(F("[GEST] Library cleared."));
    Serial.println(F("GESTURES_CLEARED"));
    if (currentState == IDLE) showIdleScreen();
    else if (currentState == CALIBRATE) showCalibrateScreen();
    else if (currentState == DETECT) showDetectScreen();
    return;
  }

  // 5. In CALIBRATE mode: Enter pressed for typed label
  if (currentState == CALIBRATE) {
    Serial.println();
    Serial.print(F("Label set to: "));
    Serial.println(calibInputBuf[0] == '\0' ? "(Auto)" : calibInputBuf);
    showCalibrateScreen();
    return;
  }
}

// Parse comma-separated gesture signature safely without sscanf float dependency
bool parseGestureLine(char *line, GestureSignature &outG) {
  char *token = strtok(line, ",");
  if (!token) return false;
  strncpy(outG.label, token, LABEL_LEN - 1);
  outG.label[LABEL_LEN - 1] = '\0';

  for (int i = 0; i < NUM_FINGERS; i++) {
    token = strtok(NULL, ",");
    if (!token) return false;
    outG.fingerValues[i] = atof(token);
  }

  token = strtok(NULL, ",");
  if (!token) return false;
  outG.pitch = atof(token);

  token = strtok(NULL, ",");
  if (!token) return false;
  outG.roll = atof(token);

  token = strtok(NULL, ",");
  if (!token) return false;
  outG.gyroMag = atof(token);

  token = strtok(NULL, ",");
  if (!token) return false;
  outG.radius = atof(token);

  outG.isSet = true;
  return true;
}

void printGesture(const GestureSignature &g) {
  Serial.print(F("GEST:"));
  Serial.print(g.label);
  for (int i = 0; i < NUM_FINGERS; i++) {
    Serial.print(',');
    Serial.print(g.fingerValues[i], 1);
  }
  Serial.print(',');
  Serial.print(g.pitch, 1);
  Serial.print(',');
  Serial.print(g.roll, 1);
  Serial.print(',');
  Serial.print(g.gyroMag, 1);
  Serial.print(',');
  Serial.println(g.radius, 1);
}

void dumpGestures() {
  Serial.print(F("GEST_COUNT:"));
  Serial.println(gestureCount);
  for (int i = 0; i < gestureCount; i++) {
    printGesture(library[i]);
  }
  Serial.println(F("GEST_END"));
}

void displaySentenceOnLCD(const char *sentence) {
  int len = strlen(sentence);
  char line1[17];
  char line2[17];

  if (len <= 16) {
    strncpy(line1, sentence, 16);
    line1[len] = '\0';
    line2[0] = '\0';
  } else {
    // Word-wrap: find last space on or before index 16
    int split = 16;
    while (split > 0 && sentence[split] != ' ') {
      split--;
    }
    if (split == 0) split = 16; // Single unbroken word fallback

    int i = 0;
    for (; i < split && i < len; i++) line1[i] = sentence[i];
    line1[i] = '\0';

    int start2 = (sentence[split] == ' ') ? split + 1 : split;
    int j = 0;
    for (i = start2; i < len && j < 16; i++, j++) line2[j] = sentence[i];
    line2[j] = '\0';
  }

  lcdPrintPadded(0, 0, line1);
  lcdPrintPadded(0, 1, line2);
}

// ==========================================================================
//  EEPROM PERSISTENCE
// ==========================================================================
void saveLibraryToEEPROM() {
  int addr = EEPROM_START_ADDR;
  uint16_t magic = EEPROM_MAGIC;
  EEPROM.put(addr, magic);         addr += sizeof(magic);
  EEPROM.put(addr, gestureCount); addr += sizeof(gestureCount);
  for (int i = 0; i < gestureCount; i++) {
    EEPROM.put(addr, library[i]);
    addr += sizeof(GestureSignature);
  }
}

void loadLibraryFromEEPROM() {
  int addr = EEPROM_START_ADDR;
  uint16_t magic = 0;
  EEPROM.get(addr, magic); addr += sizeof(magic);
  if (magic != EEPROM_MAGIC) {
    gestureCount = 0;
    saveLibraryToEEPROM();
    return;
  }

  int count = 0;
  EEPROM.get(addr, count); addr += sizeof(count);
  if (count < 0 || count > MAX_GESTURES) {
    gestureCount = 0;
    return;
  }

  gestureCount = 0;
  for (int i = 0; i < count; i++) {
    GestureSignature g;
    EEPROM.get(addr, g); addr += sizeof(GestureSignature);
    if (g.isSet) library[gestureCount++] = g;
  }
}

void clearEEPROM() {
  gestureCount = 0;
  saveLibraryToEEPROM();
}
