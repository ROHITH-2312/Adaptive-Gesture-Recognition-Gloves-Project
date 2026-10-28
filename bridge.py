#!/usr/bin/env python3
"""
bridge.py — Gesture Glove ↔ Gemini AI & File Persistence Bridge
===============================================================
Features:
  1. Serial Bridge between Arduino Gesture Glove and Gemini AI
  2. Text File Persistence:
     - Saves & loads calibrated gestures to/from 'gestures.txt'
     - Auto-syncs gestures to Arduino on connect if Arduino is empty
     - Auto-saves any newly calibrated gestures from Arduino to 'gestures.txt'
     - Saves full translation logs to 'translations.txt'
  3. Interactive Commands (type in terminal while running):
     - 'l' or 'load'    : Load 'gestures.txt' and send to Arduino
     - 's' or 'save'    : Request Arduino to dump gestures and save to 'gestures.txt'
     - 'h' or 'history' : Show recent translation history from 'translations.txt'
     - 'c' or 'clear'   : Clear gestures on Arduino and backup 'gestures.txt'
     - '?' or 'help'    : Show command help
     - 'q' or 'quit'    : Exit bridge

Connection Priority:
  1. Wokwi RFC2217 (rfc2217://localhost:4000) — instant for Wokwi simulation
  2. Real Arduino hardware (USB serial port: /dev/ttyACM*, /dev/ttyUSB*, COM*)
  3. Wokwi VS Code Linux PTY (/dev/pts/N)
"""

import datetime
import glob
import os
import queue
import re
import socket
import sys
import threading
import time

import serial
import serial.tools.list_ports
from google import genai
from google.genai import types

# ── Configuration ──────────────────────────────────────────────────────────
def _load_env_file(env_path):
    if os.path.exists(env_path):
        with open(env_path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#") and "=" in line:
                    k, v = line.split("=", 1)
                    k, v = k.strip(), v.strip().strip('"').strip("'")
                    if k and k not in os.environ:
                        os.environ[k] = v

_load_env_file(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".env"))

API_KEY      = os.environ.get("GEMINI_API_KEY", "")
GEMINI_MODEL = os.environ.get("GEMINI_MODEL", "gemini-2.5-flash")
SERIAL_PORT  = os.environ.get("ARDUINO_PORT", None)  # None = auto-detect
BAUD_RATE    = 9600
WOKWI_RFC_PORT = 4000

GESTURES_FILE     = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gestures.txt")
TRANSLATIONS_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "translations.txt")
# ───────────────────────────────────────────────────────────────────────────


# ===========================================================================
#  FILE PERSISTENCE: GESTURES & TRANSLATIONS
# ===========================================================================

def load_gestures_from_file(filepath: str = GESTURES_FILE) -> list[str]:
    """
    Reads gestures from gestures.txt.
    Returns list of formatted lines like 'GEST:LABEL,f1,f2,f3,f4,p,r,g,rad'.
    """
    if not os.path.exists(filepath):
        return []

    gestures = []
    with open(filepath, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            raw = line[5:] if line.startswith("GEST:") else line
            parts = [p.strip() for p in raw.split(",")]
            if len(parts) != 9:
                continue
            try:
                # Ensure all 8 sensor values are valid numbers
                [float(p) for p in parts[1:]]
            except ValueError:
                continue
            label = parts[0].upper()
            if label in ("LABEL", "GESTURE"):
                continue
            gestures.append(f"GEST:{raw}")
    return gestures


def save_gestures_to_file(gestures: list[str], filepath: str = GESTURES_FILE) -> None:
    """
    Saves a list of 'GEST:...' lines to gestures.txt with a descriptive header.
    """
    with open(filepath, "w", encoding="utf-8") as f:
        f.write("# Gesture Sign Language Glove - Calibrated Sign Library\n")
        f.write("# Format: LABEL,FINGER1,FINGER2,FINGER3,FINGER4,PITCH,ROLL,GYROMAG,RADIUS\n")
        for g in gestures:
            raw = g[5:] if g.startswith("GEST:") else g
            parts = [p.strip() for p in raw.split(",")]
            if len(parts) == 9:
                try:
                    [float(p) for p in parts[1:]]
                    f.write(f"{raw}\n")
                except ValueError:
                    pass
        f.flush()
        os.fsync(f.fileno())
    print(f"[File]     Saved gestures to '{os.path.basename(filepath)}'")


def update_gesture_in_file(gesture_line: str, filepath: str = GESTURES_FILE) -> None:
    """
    Updates or appends a single gesture in gestures.txt.
    """
    raw = gesture_line[5:] if gesture_line.startswith("GEST:") else gesture_line
    parts = [p.strip() for p in raw.split(",")]
    if len(parts) != 9:
        return
    try:
        [float(p) for p in parts[1:]]
    except ValueError:
        return

    label = parts[0].strip().upper()
    if label in ("LABEL", "GESTURE"):
        return

    existing_lines = []
    updated = False

    if os.path.exists(filepath):
        with open(filepath, "r", encoding="utf-8") as f:
            for line in f:
                stripped = line.strip()
                if not stripped or stripped.startswith("#"):
                    existing_lines.append(line)
                    continue
                cur_raw = stripped[5:] if stripped.startswith("GEST:") else stripped
                cur_parts = [p.strip() for p in cur_raw.split(",")]
                if len(cur_parts) != 9:
                    continue
                try:
                    [float(p) for p in cur_parts[1:]]
                except ValueError:
                    continue

                cur_label = cur_parts[0].strip().upper()
                if cur_label == label:
                    existing_lines.append(f"{raw}\n")
                    updated = True
                else:
                    existing_lines.append(f"{cur_raw}\n")

    if not updated:
        if not existing_lines:
            existing_lines.append("# Gesture Sign Language Glove - Calibrated Sign Library\n")
            existing_lines.append("# Format: LABEL,FINGER1,FINGER2,FINGER3,FINGER4,PITCH,ROLL,GYROMAG,RADIUS\n")
        existing_lines.append(f"{raw}\n")

    with open(filepath, "w", encoding="utf-8") as f:
        f.writelines(existing_lines)
        f.flush()
        os.fsync(f.fileno())

    action = "Updated" if updated else "Saved"
    print(f"[File]     >>> {action.upper()} gesture '{label}' into '{os.path.basename(filepath)}' <<<")


def log_translation(words: list[str], sentence: str, filepath: str = TRANSLATIONS_FILE) -> None:
    """
    Appends translation result with timestamp to translations.txt.
    """
    ts = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    entry = f"[{ts}] Signs: {words} -> English: \"{sentence}\"\n"
    with open(filepath, "a", encoding="utf-8") as f:
        f.write(entry)
    print(f"[File]     Logged translation to '{os.path.basename(filepath)}'")


def view_translation_history(filepath: str = TRANSLATIONS_FILE, count: int = 5) -> None:
    """
    Prints recent translations from translations.txt.
    """
    if not os.path.exists(filepath):
        print(f"[History]  No translation history found in '{os.path.basename(filepath)}'.")
        return

    with open(filepath, "r", encoding="utf-8") as f:
        lines = [line.strip() for line in f if line.strip()]

    print(f"\n--- Recent Translations (last {min(count, len(lines))}) ---")
    for line in lines[-count:]:
        print(f"  {line}")
    print("------------------------------------------\n")


# ===========================================================================
#  SERIAL CONNECTION HELPERS
# ===========================================================================

def is_port_open(host: str, port: int, timeout: float = 0.2) -> bool:
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except (OSError, ConnectionRefusedError):
        return False


def try_rfc2217() -> serial.SerialBase | None:
    if not is_port_open("localhost", WOKWI_RFC_PORT):
        return None
    url = f"rfc2217://localhost:{WOKWI_RFC_PORT}"
    try:
        print(f"[Bridge]   Checking Wokwi RFC2217 on {url} …")
        ser = serial.serial_for_url(url, baudrate=BAUD_RATE, timeout=0.1)
        print(f"[Bridge]   Connected via RFC2217 to Wokwi simulation!")
        return ser
    except Exception as e:
        print(f"[Bridge]   RFC2217 connect failed: {e}")
        return None


def find_usb_arduino() -> str | None:
    for p in serial.tools.list_ports.comports():
        desc = (p.description or "") + (p.manufacturer or "")
        if any(kw in desc for kw in ("Arduino", "CH340", "CP210", "FTDI")):
            return p.device
        if any(kw in p.device for kw in ("ttyUSB", "ttyACM")):
            return p.device
    return None


def input_with_timeout(prompt: str, timeout: float = 3.0, default: str = "") -> str:
    print(prompt, end="", flush=True)
    try:
        import select
        if sys.platform != "win32" and sys.stdin.isatty():
            rlist, _, _ = select.select([sys.stdin], [], [], timeout)
            if rlist:
                return sys.stdin.readline().strip()
            print(f" (auto-selected default: [{default}])")
            return default
    except Exception:
        pass
    return default


def find_wokwi_pty() -> str | None:
    if sys.platform != "linux":
        return None

    pts = glob.glob("/dev/pts/[0-9]*")
    pts = [p for p in pts if int(os.path.basename(p)) > 2]
    if not pts:
        return None

    pts.sort(key=lambda p: os.stat(p).st_mtime, reverse=True)

    print("\n[Bridge]   Found PTY devices:")
    for i, p in enumerate(pts[:6]):
        t = datetime.datetime.fromtimestamp(os.stat(p).st_mtime).strftime("%H:%M:%S")
        print(f"           [{i}] {p}  (last active: {t})")

    choice = input_with_timeout("           Select PTY index (press Enter for [0]): ", timeout=3.0, default="0")
    try:
        idx = int(choice) if choice else 0
        return pts[idx] if 0 <= idx < len(pts) else pts[0]
    except Exception:
        return pts[0]


def connect() -> serial.SerialBase:
    # 1. Manual override via config or environment
    if SERIAL_PORT:
        print(f"[Serial]   Connecting to {SERIAL_PORT} …")
        if SERIAL_PORT.startswith("rfc2217"):
            ser = serial.serial_for_url(SERIAL_PORT, baudrate=BAUD_RATE, timeout=0.1)
        else:
            ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=0.1)
        print("[Serial]   Connected!")
        return ser

    print(f"[Bridge]   Waiting for Wokwi simulation (localhost:{WOKWI_RFC_PORT}) or USB Arduino...")
    print(f"[Bridge]   -> In VS Code: Press F1 -> 'Wokwi: Start Simulator' (or click Play)")

    dot_count = 0
    while True:
        # Check Wokwi RFC2217 port 4000
        ser = try_rfc2217()
        if ser:
            return ser

        # Check real hardware USB Arduino
        port = find_usb_arduino()
        if port:
            print(f"\n[Serial]   Connecting to hardware Arduino at {port} …")
            try:
                ser = serial.Serial(port, BAUD_RATE, timeout=0.1)
                time.sleep(2)  # Wait for Arduino bootloader reset
                print("[Serial]   Connected!")
                return ser
            except Exception as e:
                print(f"[Serial]   Failed to open {port}: {e}")

        time.sleep(0.5)
        dot_count += 1
        if dot_count % 10 == 0:
            print(f"[Bridge]   Still waiting for Wokwi simulation to start on port {WOKWI_RFC_PORT}...")


# ===========================================================================
#  GESTURE SYNC HELPERS
# ===========================================================================

def send_gestures_to_arduino(ser: serial.SerialBase, filepath: str = GESTURES_FILE) -> int:
    """
    Reads gestures.txt and uploads all gestures to Arduino via Serial.
    """
    gestures = load_gestures_from_file(filepath)
    if not gestures:
        print(f"[Sync]     No gestures found in '{os.path.basename(filepath)}'.")
        return 0

    print(f"[Sync]     Uploading {len(gestures)} gestures to Arduino from '{os.path.basename(filepath)}'...")
    for g in gestures:
        line = g if g.startswith("GEST:") else f"GEST:{g}"
        ser.write(f"{line}\n".encode("utf-8"))
        try:
            ser.flush()
        except Exception:
            pass
        time.sleep(0.08)  # Give Arduino EEPROM write time

    print(f"[Sync]     Upload complete ({len(gestures)} gestures).")
    return len(gestures)


# ===========================================================================
#  GEMINI LLM TRANSLATOR
# ===========================================================================

def build_prompt(words: list[str]) -> str:
    return f"""You are an expert ASL (American Sign Language) to English translator.

ASL grammar is different from spoken English: it drops articles (a, the),
auxiliary verbs (is, am, are, was, were), and uses topic-comment structure.
Your task: take the detected ASL sign words (in sequence) and produce the most
natural, grammatically correct English sentence.

Strict constraints:
- The output will be displayed on a 16x2 character LCD screen.
- Keep the sentence concise and under 32 characters whenever possible.
- Output ONLY the final English sentence. No quotes, no markdown, no explanation.
- If the words imply a question, end with "?"
- Do not invent words not implied by the signs.

Few-shot examples:
  ["HELLO", "NAME", "SHYAM"]   → Hello, my name is Shyam.
  ["ME", "WANT", "WATER"]      → I want water.
  ["WANT", "FISH"]             → I want fish.
  ["FISH", "WANT"]             → I want fish.
  ["WHERE", "BATHROOM"]        → Where is the bathroom?
  ["THANK", "YOU", "HELP"]     → Thank you for helping.
  ["YOU", "OKAY"]              → Are you okay?
  ["HELP", "ME", "PLEASE"]     → Please help me.
  ["ME", "HUNGRY"]             → I am hungry.
  ["GOOD", "MORNING"]          → Good morning.

Detected signs (in order): {", ".join(words)}"""


def grammatical_fallback(words: list[str]) -> str:
    w_upper = [w.strip().upper() for w in words if w.strip()]
    if not w_upper:
        return "Hello!"

    joined = " ".join(w_upper)
    idiom_map = {
        "WATER NEED": "I need water.",
        "NEED WATER": "I need water.",
        "WATER WANT": "I want water.",
        "WANT WATER": "I want water.",
        "WANT FISH": "I want fish.",
        "FISH WANT": "I want fish.",
        "NEED FISH": "I need fish.",
        "FISH NEED": "I need fish.",
        "WATER": "Water, please.",
        "NEED": "I need help.",
        "WANT": "I want that.",
        "HELLO": "Hello!",
        "THANK YOU": "Thank you very much.",
        "HELP ME": "Please help me.",
        "HELP": "Help me please.",
        "ME HUNGRY": "I am hungry.",
        "HUNGRY": "I am hungry.",
    }
    if joined in idiom_map:
        return idiom_map[joined]

    text = " ".join(w.lower() for w in w_upper)
    if w_upper[0] in ("WANT", "NEED", "LIKE", "LOVE", "EAT", "HAVE", "SEE"):
        return f"I {text}."
    if "WANT" in w_upper:
        noun = " ".join(w.lower() for w in w_upper if w != "WANT")
        return f"I want {noun}."
    if "NEED" in w_upper:
        noun = " ".join(w.lower() for w in w_upper if w != "NEED")
        return f"I need {noun}."

    return f"{text.capitalize()}."


def translate(client: genai.Client, words: list[str]) -> str:
    try:
        config = types.GenerateContentConfig(
            thinking_config=types.ThinkingConfig(thinking_budget=0),
            max_output_tokens=30,
            temperature=0.1,
        )
        response = client.models.generate_content(
            model=GEMINI_MODEL,
            contents=build_prompt(words),
            config=config,
        )
        raw_text = response.text.strip().strip('"').strip("'")
        sentence = " ".join(raw_text.split())
        if sentence and "error" not in sentence.lower():
            return sentence
    except Exception as e:
        print(f"[Gemini Notice] Fallback used: {e}")

    return grammatical_fallback(words)


# ===========================================================================
#  INTERACTIVE CONSOLE THREAD
# ===========================================================================

def console_input_loop(cmd_queue: queue.Queue):
    """
    Reads user commands from terminal stdin without blocking serial read loop.
    """
    if not sys.stdin.isatty():
        return

    while True:
        try:
            line = sys.stdin.readline()
            if not line:
                break
            cmd = line.strip().lower()
            if cmd:
                cmd_queue.put(cmd)
        except Exception:
            break


# ===========================================================================
#  MAIN LOOP
# ===========================================================================

def main():
    print("=" * 60)
    print("  Gesture Sign Glove ↔ Gemini AI & File Bridge (v13)")
    print("=" * 60)

    # 1. Init Gemini Client
    global API_KEY
    if not API_KEY:
        print("[Gemini]   No GEMINI_API_KEY found in .env or environment.")
        API_KEY = input("           Enter your Gemini API key: ").strip()
    client = genai.Client(api_key=API_KEY)
    print(f"[Gemini]   Model: {GEMINI_MODEL} — Ready")
    print(f"[Config]   Gestures file:     '{GESTURES_FILE}'")
    print(f"[Config]   Translations file: '{TRANSLATIONS_FILE}'")

    # 2. Connect to Serial
    ser = connect()

    # 3. Start background user command listener
    cmd_queue = queue.Queue()
    input_thread = threading.Thread(target=console_input_loop, args=(cmd_queue,), daemon=True)
    input_thread.start()

    print("\n[Controls] Type in terminal:")
    print("  'l' / 'load'    -> Load gestures.txt to Arduino glove")
    print("  's' / 'save'    -> Save/dump Arduino gestures to gestures.txt")
    print("  'h' / 'history' -> View translation history")
    print("  'c' / 'clear'   -> Clear Arduino gestures")
    print("  '?' / 'help'    -> Show controls")
    print("  'q' / 'quit'    -> Exit")
    print("=" * 60 + "\n")

    words: list[str] = []
    dump_collector: list[str] = []
    is_collecting_dump = False
    startup_sync_done = False
    startup_timer = time.time()

    # Request initial dump from Arduino to check gesture count
    ser.write(b"DUMP\n")

    while True:
        try:
            # Check for console commands
            while not cmd_queue.empty():
                cmd = cmd_queue.get_nowait()
                if cmd in ("l", "load"):
                    send_gestures_to_arduino(ser, GESTURES_FILE)
                elif cmd in ("s", "save", "dump"):
                    print("[Bridge]   Requesting gesture dump from Arduino...")
                    dump_collector.clear()
                    is_collecting_dump = True
                    ser.write(b"DUMP\n")
                elif cmd in ("h", "history"):
                    view_translation_history(TRANSLATIONS_FILE)
                elif cmd in ("c", "clear"):
                    ser.write(b"CLEAR_GEST\n")
                    if os.path.exists(GESTURES_FILE):
                        with open(GESTURES_FILE, "w", encoding="utf-8") as f:
                            f.write("# Gesture Sign Language Glove - Calibrated Sign Library\n")
                            f.write("# Format: LABEL,FINGER1,FINGER2,FINGER3,FINGER4,PITCH,ROLL,GYROMAG,RADIUS\n")
                    print(f"[Bridge]   Cleared gestures on Arduino and emptied '{os.path.basename(GESTURES_FILE)}'.")
                elif cmd in ("?", "help"):
                    print("\n[Commands] 'load' (l), 'save' (s), 'history' (h), 'clear' (c), 'quit' (q)\n")
                elif cmd in ("q", "quit", "exit"):
                    print("[Bridge]   Exiting...")
                    ser.close()
                    return

            # Read incoming serial line
            raw = ser.readline()
            if not raw:
                if not startup_sync_done and (time.time() - startup_timer > 2.0):
                    startup_sync_done = True
                continue

            line = raw.decode("utf-8", errors="ignore").strip()
            if not line:
                continue

            print(f"[Arduino]  {line}")

            # ── Gesture Count and Dump Handling ───────────────────────────────
            if line.startswith("GEST_COUNT:"):
                try:
                    count = int(line.split(":")[1])
                    if count == 0 and os.path.exists(GESTURES_FILE):
                        saved = load_gestures_from_file(GESTURES_FILE)
                        if saved:
                            print(f"[Bridge]   Arduino has 0 gestures. Auto-uploading {len(saved)} saved gesture(s) from '{os.path.basename(GESTURES_FILE)}'...")
                            send_gestures_to_arduino(ser, GESTURES_FILE)
                    else:
                        dump_collector.clear()
                        is_collecting_dump = True
                except Exception as e:
                    print(f"[Bridge]   Sync check error: {e}")

            elif line.startswith("GEST:"):
                update_gesture_in_file(line, GESTURES_FILE)
                if is_collecting_dump:
                    dump_collector.append(line)

            elif line == "GEST_END":
                if is_collecting_dump and dump_collector:
                    save_gestures_to_file(dump_collector, GESTURES_FILE)
                    dump_collector.clear()
                is_collecting_dump = False

            # ── Word buffer handling ──────────────────────────────────────────
            elif line.startswith("WORD:"):
                word = line[5:].strip().upper()
                if word:
                    words.append(word)
                    print(f"[Buffer]   {words}")

            elif line == "RESET_BUFFER":
                words.clear()
                print("[Buffer]   Buffer cleared.")

            elif line == "GESTURES_CLEARED":
                words.clear()
                if os.path.exists(GESTURES_FILE):
                    with open(GESTURES_FILE, "w", encoding="utf-8") as f:
                        f.write("# Gesture Sign Language Glove - Calibrated Sign Library\n")
                        f.write("# Format: LABEL,FINGER1,FINGER2,FINGER3,FINGER4,PITCH,ROLL,GYROMAG,RADIUS\n")
                print(f"[Bridge]   Gestures cleared and '{os.path.basename(GESTURES_FILE)}' emptied.")

            # ── LLM Translation Trigger ───────────────────────────────────────
            elif line == "SEND":
                if not words:
                    print("[Bridge]   SEND received but buffer is empty — ignoring.")
                    continue

                print(f"\n[Gemini]   Translating signs: {words}")
                sentence = translate(client, words)
                print(f'[Gemini]   English: "{sentence}"\n')

                # Send back to Arduino for LCD display
                msg = f"SENTENCE:{sentence}\n".encode("utf-8")
                ser.write(msg)
                try:
                    ser.flush()
                except Exception:
                    pass
                print(f"[Bridge]   Sent to Arduino LCD: SENTENCE:{sentence}")

                # Persist translation to translations.txt
                log_translation(words, sentence, TRANSLATIONS_FILE)

                words.clear()

        except (serial.SerialException, OSError) as e:
            print(f"[Serial]   Disconnected ({e}). Waiting to reconnect...")
            try:
                ser.close()
            except Exception:
                pass
            time.sleep(1.0)
            ser = connect()
            startup_sync_done = False
            startup_timer = time.time()
            ser.write(b"DUMP\n")
        except KeyboardInterrupt:
            print("\n[Bridge]   Shutting down.")
            ser.close()
            break
        except Exception as e:
            print(f"[Error]    {e}")
            time.sleep(0.5)


if __name__ == "__main__":
    main()
