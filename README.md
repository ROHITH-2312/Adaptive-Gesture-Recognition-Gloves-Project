# 🖐️ Adaptive Sign Language to Speech Glove with On-Device Learning & Gemini AI

[![C/C++](https://img.shields.io/badge/Language-C%20%2F%20C%2B%2B-blue.svg)](https://en.wikipedia.org/wiki/C_(programming_language))
[![Python 3.10+](https://img.shields.io/badge/Python-3.10%2B-yellow.svg)](https://www.python.org/)
[![Hardware](https://img.shields.io/badge/Target-Arduino%20Uno%20(ATmega328P)-00979D.svg)](https://www.arduino.cc/)
[![Simulator](https://img.shields.io/badge/Simulation-Wokwi-brightgreen.svg)](https://wokwi.com/)
[![Cloud AI](https://img.shields.io/badge/LLM-Google%20Gemini%202.5%20Flash-orange.svg)](https://ai.google.dev/)

An embedded assistive communication system combining a wearable **7-dimensional kinematic gesture glove** running on an 8-bit Arduino Uno with a host **Google Gemini LLM Bridge** to translate American Sign Language (ASL) gloss tokens into natural, grammatically complete English sentences.

---

## 📌 Problem & Novelty

### The Problem
Over 70 million deaf individuals worldwide face a severe daily communication barrier because most hearing people cannot interpret sign language. Existing "gesture gloves" typically suffer from three major shortcomings:
1. **Accelerometer-only sensing**: They track arm swing rather than finger curl/hand-shape.
2. **Hardcoded static thresholds**: They are tuned for one person's hand and cannot adapt to individual anatomy or natural hand tremors.
3. **Isolated word output**: They print disconnected words (e.g., `["WATER", "NEED"]`) rather than grammatically correct spoken English sentences.

### Our Solution
* **7D Spatial Kinematic Vector**: 4 finger flex sensors ($A0\text{--}A3$) + MPU6050 6-DOF IMU (wrist Pitch, Roll, Gyro magnitude).
* **On-Device Adaptive Calibration**: Captures individual user hand variance (`maxSpread`) and computes a personal confidence radius stored permanently in **EEPROM**.
* **Zero Heavy ML on MCU**: Uses an isotropic 7D Euclidean Nearest-Neighbor Classifier in pure embedded C running in $< 200\text{ ms}$ within 2 KB RAM.
* **Edge-to-Cloud Gemini AI Reconstruction**: A host Python bridge takes accumulated sign sequences and reconstructs them into fluent English (*"I need water."*) in $\sim 1.2\text{ seconds}$ with zero thinking latency.

---

## 🏛️ System Architecture

```
┌────────────────────────────────────────────────────────────────────────┐
│                        ARDUINO GLOVE (Edge Layer)                      │
│                                                                        │
│   [4x Flex Sensors (A0-A3)] ──► [3-Sample ADC Avg + 1.5% Deadzone]     │
│   [MPU6050 IMU (A4-A5)]     ──► [Pitch/Roll atan2 + 0-100 Normalizer]  │
│                                           │                            │
│                                           ▼                            │
│                           [7D Feature Vector Space]                    │
│                                           │                            │
│        ┌──────────────────────────────────┴────────────────────┐       │
│        ▼ (Calibrate Mode)                                      ▼ (Detect Mode)
│  [10-Sample Calibration]                           [7D Nearest-Neighbor Match]
│  [Compute Adaptive Radius]                         [Roll Wrap-Around: |dr|>50]
│  [Save to EEPROM (0x474D)]                         [Threshold: d <= 1.8*Radius]
│                                                                │       │
│                                           ┌────────────────────┘       │
│                                           ▼                            │
│                             [Word Buffer (up to 8 signs)]              │
│                                           │ (Hold CONFIRM 2s)          │
└───────────────────────────────────────────┼────────────────────────────┘
                                            │ UART / USB Serial (9600 baud)
                                            │ "WORD:WATER", "SEND"
┌───────────────────────────────────────────┼────────────────────────────┐
│                        HOST PC (Linguistic Cloud Layer)                │
│                                           ▼                            │
│                                    [bridge.py]                         │
│                                           │                            │
│                   ┌───────────────────────┴──────────────────────┐     │
│                   ▼                                              ▼     │
│       [Google Gemini 2.5 Flash API]                  [Local Grammar Fallback]
│       (thinking_budget=0, ~1.2s latency)             (Zero-Freeze Insurance)  │
│                   │                                              │     │
│                   └───────────────────────┬──────────────────────┘     │
│                                           ▼                            │
│                             "SENTENCE:I need water."                   │
│                                           │                            │
│       [gestures.txt (Backup)] ◄───────────┼──────────► [translations.txt]
└───────────────────────────────────────────┼────────────────────────────┘
                                            │ UART / USB Serial
                                            ▼
                               [16x2 I2C LCD Display]
                             Line 1: "I need water."
                             (Locked until CONFIRM tap)
```

---

## 🔌 Hardware Connections & Pinout

| Module / Sensor | Arduino Uno Pin | Protocol | Role |
|---|---|---|---|
| **Finger 1 (Index)** | **A0** | Analog In | Normalized finger bend ($0\text{--}100\%$) |
| **Finger 2 (Middle)** | **A1** | Analog In | Normalized finger bend ($0\text{--}100\%$) |
| **Finger 3 (Ring)** | **A2** | Analog In | Normalized finger bend ($0\text{--}100\%$) |
| **Finger 4 (Pinky)** | **A3** | Analog In | Normalized finger bend ($0\text{--}100\%$) |
| **MPU6050 IMU** | **A4 (SDA), A5 (SCL)** | I2C (`0x68`) | Wrist Pitch, Roll, Gyro magnitude |
| **16x2 LCD Display** | **A4 (SDA), A5 (SCL)** | I2C (`0x27`) | System status, signs, and English sentence |
| **MODE Button** | **Pin 2** | Digital In (`INPUT_PULLUP`) | State toggle: `IDLE` $\rightarrow$ `CALIBRATE` $\rightarrow$ `DETECT` |
| **CONFIRM Button** | **Pin 3** | Digital In (`INPUT_PULLUP`) | Save / Send to Gemini (hold 2s) / Reset |
| **Piezo Buzzer** | **Pin 8** | Digital Out | Tone feedback (Match, Unknown, Victory jingle) |
| **Status LED + 220 Ω** | **Pin 13** | Digital Out | Active indicator (ON in Calibrate/Detect) |

---

## 📡 Serial Protocol (UART @ 9600 Baud)

| Command | Direction | Function |
|---|:---:|---|
| `WORD:<label>` | Arduino &rarr; PC | Emits a recognized sign into the sequence buffer. |
| `SEND` | Arduino &rarr; PC | Triggers Gemini AI sentence reconstruction (sent on 2-second hold). |
| `SENTENCE:<text>` | PC &rarr; Arduino | Translated English sentence displayed across the 16x2 LCD. |
| `RESET_BUFFER` | Arduino &rarr; PC | Clears the active word buffer on state change. |
| `GEST:<label>,...` | Both Ways | Bidirectional synchronization of raw 7D signature vectors. |
| `DUMP` / `CLEAR_GEST` | PC &rarr; Arduino | Export all gestures to PC / wipe EEPROM library. |

---

## 🚀 Getting Started

### 1. Clone the Repository
```bash
git clone https://github.com/your-username/sign-language-recognition-glove.git
cd sign-language-recognition-glove
```

### 2. Configure Your Gemini API Key
Create a `.env` file in the root directory (or copy from `.env.example`):
```bash
cp .env.example .env
```
Open `.env` and add your free Google AI Studio API key:
```env
GEMINI_API_KEY=your_actual_gemini_api_key_here
GEMINI_MODEL=gemini-2.5-flash
```

### 3. Install Python Dependencies
```bash
pip install -r requirements.txt
```

---

## 🎮 Running the Simulation (Wokwi)

1. Open this repository in **VS Code** with the **Wokwi** and **PlatformIO** extensions installed.
2. Open `diagram.json` and press `F1` &rarr; **`Wokwi: Start Simulator`** (or click Play).
3. Open a terminal and run the Python AI bridge:
   ```bash
   python bridge.py
   ```
4. The bridge connects automatically via virtual RFC2217 serial on `localhost:4000`:
   ```text
   [Bridge]   Connected via RFC2217 to Wokwi simulation!
   [Gemini]   Model: gemini-2.5-flash — Ready
   ```

---

## 🎬 How to Use & Demonstrate

1. **Calibration Mode**:
   * Tap the **MODE** button &rarr; LCD displays `CALIBRATE MODE`.
   * Type the gesture label in the terminal (e.g. `WATER`), hold your hand pose, and tap **CONFIRM**.
   * The buzzer chirps, saves to EEPROM, and auto-syncs to `gestures.txt`.
2. **Detection Mode**:
   * Tap **MODE** again &rarr; LCD enters `DETECT MODE`.
   * Form signs in sequence: e.g. `WATER` &rarr; `NEED`.
   * LCD row 1 shows the current sign; row 2 tracks `2 words buffered`.
3. **AI Translation**:
   * In `DETECT MODE`, **hold the CONFIRM button for 2 seconds**.
   * LCD displays `Asking Gemini... Hold tight!`.
   * In $\sim 1.2\text{ s}$, Gemini returns:
     ```text
     I need water.
     ```
   * The sentence stays locked on screen for comfortable reading. Tap **CONFIRM** to dismiss and resume live detection!

---

## 📊 Experimental Results

Tested across 180 controlled physical trials (6 calibrated gestures $\times$ 30 repetitions with $\pm 6\%$ finger curl and $\pm 6^\circ$ wrist variance):

| Metric | Score | Details |
|---|:---:|---|
| **Overall Classification Accuracy** | **97.8%** | 176 / 180 successful classifications |
| **Wrong-Word Error Rate** | **0.0%** | Zero false classifications |
| **Precision** | **100.0%** | When a word is output, it is always correct |
| **Safe Unknown Rejections** | **2.2%** | 4 / 180 ambiguous poses safely rejected as `"Unknown"` |
| **Gemini AI Translation Latency** | **$\sim 1.2\text{ s}$** | Achieved via `thinking_budget: 0` |

---

## 👥 Authors & Team Credits

* **Harieesh S** — *Persistence & Finite State Machine*
* **Rithik S V** — *Sensing & Calibration Subsystem*
* **Rohith Sivakumar** — *Classification Engine & 7D Euclidean Metric*
* **Shyam Dharshan Soundararajan** — *Output Systems & Gemini AI Cloud Bridge*

---

## 📄 License
This project is open-source under the [MIT License](LICENSE).
