#include <EEPROM.h>
#include <Keypad.h>
#include <Encoder.h>
#include <Bounce2.h>
#include "HID-Project.h"

// ============================================================================
// Hardware Pinout (Programmable Macropad V2 - Arduino Pro Micro ATmega32U4)
// ============================================================================
const byte ROWS = 4;
const byte COLS = 3;
char keys[COLS][ROWS] = {
  {'X', '7', '4', '1'},
  {'*', '8', '5', '2'},
  {'-', '9', '6', '3'}
};
byte rowPins[ROWS] = {6, 5, 21, 20};
byte colPins[COLS] = {7, 8, 9};
Keypad kpd = Keypad(makeKeymap(keys), colPins, rowPins, COLS, ROWS);

const int numStates = 4;
const int States[numStates] = {10, 16, 14, 15};
int currentState = 0;

const int PIN_SW  = 19;
const int PIN_DT  = 2;
const int PIN_CLK = 3;
Encoder volumeKnob(PIN_CLK, PIN_DT);
Bounce encoderButton = Bounce();
long oldPosition = 0;

// ============================================================================
// EEPROM Layout (1024 bytes total on ATmega32U4)
//   0..3    : Magic 'M','P','V', 2
//   4..15   : Global & Encoder settings (12 bytes)
//   16..175 : 4 Layouts x 10 Buttons x 4 bytes (160 bytes)
//             Button index 0..8 = Keys '1'..'9', index 9 = Key '-'
//   176..1023: Text Pool (848 bytes) for ACT_TEXT null-terminated strings
// ============================================================================
const int EEPROM_MAGIC_ADDR   = 0;
const int EEPROM_CFG_ADDR     = 4;
const int EEPROM_BTNS_ADDR    = 16;
const int EEPROM_TXT_ADDR     = 176;
const int EEPROM_TXT_SIZE     = 848;
const int NUM_LAYOUTS         = 4;
const int NUM_BUTTONS         = 10; // 0..8 = '1'..'9', 9 = '-'

enum ActionType : uint8_t {
  ACT_NONE   = 0,
  ACT_KEY    = 1, // mods bitmask + arg1 (HID keycode) + arg2 (optional 2nd HID keycode)
  ACT_MEDIA  = 2, // uint16_t ConsumerKeycode in arg1 | (arg2 << 8)
  ACT_TEXT   = 3, // uint16_t text offset in arg1 | (arg2 << 8); mods bit0=Enter, bit1=Tab
  ACT_SEQ    = 4, // 2-step chord: step1=(mods&0x0F)+arg1, step2=(mods>>4)+arg2
  ACT_LAYOUT = 5  // arg1: 0..3 = go to layout, 254 = prev, 255 = next
};

// Modifier bitmask for ACT_KEY and ACT_SEQ
// bit 0 (0x01): Left Ctrl
// bit 1 (0x02): Left Shift
// bit 2 (0x04): Left Alt / Option
// bit 3 (0x08): Left GUI / Command

struct GlobalConfig {
  uint8_t  encStepsPerClick;
  uint8_t  encInvert;
  uint16_t encCwCode;
  uint16_t encCcwCode;
  uint16_t encClickCode;
  uint16_t encLongClickCode;
  uint8_t  defaultLayout;
  uint8_t  reserved;
};

struct ButtonAction {
  uint8_t type;
  uint8_t mods;
  uint8_t arg1;
  uint8_t arg2;
};

GlobalConfig cfg;
String serialLine = "";

int buttonIndexFromChar(char c) {
  if (c >= '1' && c <= '9') return c - '1';
  if (c == '-') return 9;
  return -1;
}

int btnEepromAddr(int layout, int btnIdx) {
  return EEPROM_BTNS_ADDR + ((layout * NUM_BUTTONS + btnIdx) * 4);
}

void readButtonAction(int layout, int btnIdx, ButtonAction &act) {
  int addr = btnEepromAddr(layout, btnIdx);
  act.type = EEPROM.read(addr);
  act.mods = EEPROM.read(addr + 1);
  act.arg1 = EEPROM.read(addr + 2);
  act.arg2 = EEPROM.read(addr + 3);
}

void writeButtonAction(int layout, int btnIdx, const ButtonAction &act) {
  int addr = btnEepromAddr(layout, btnIdx);
  EEPROM.update(addr,     act.type);
  EEPROM.update(addr + 1, act.mods);
  EEPROM.update(addr + 2, act.arg1);
  EEPROM.update(addr + 3, act.arg2);
}

void loadGlobalConfig() {
  EEPROM.get(EEPROM_CFG_ADDR, cfg);
  if (cfg.encStepsPerClick == 0 || cfg.encStepsPerClick > 16) {
    cfg.encStepsPerClick = 4;
  }
  if (cfg.defaultLayout >= NUM_LAYOUTS) {
    cfg.defaultLayout = 0;
  }
}

void saveGlobalConfig() {
  EEPROM.put(EEPROM_CFG_ADDR, cfg);
}

void resetDefaults() {
  cfg.encStepsPerClick = 4;
  cfg.encInvert        = 1; // Matches user's preferred clockwise=up direction
  cfg.encCwCode        = MEDIA_VOLUME_UP;
  cfg.encCcwCode       = MEDIA_VOLUME_DOWN;
  cfg.encClickCode     = MEDIA_VOLUME_MUTE;
  cfg.encLongClickCode = 0; // Immediate click action without waiting for long press
  cfg.defaultLayout    = 0;
  cfg.reserved         = 0;
  saveGlobalConfig();

  ButtonAction emptyAct = {ACT_NONE, 0, 0, 0};
  for (int l = 0; l < NUM_LAYOUTS; l++) {
    for (int b = 0; b < NUM_BUTTONS; b++) {
      writeButtonAction(l, b, emptyAct);
    }
  }

  // Layout 1 & 4 default: keys '1'..'9' type 1..9 (HID KEY_1=30 .. KEY_9=38)
  for (int b = 0; b < 9; b++) {
    ButtonAction numAct = {ACT_KEY, 0, (uint8_t)(KEY_1 + b), 0};
    writeButtonAction(0, b, numAct);
    writeButtonAction(3, b, numAct);
  }

  // Layout 2 default: key '6' (idx 5) = Enter, key '7' (idx 6) = Esc
  ButtonAction enterAct = {ACT_KEY, 0, KEY_ENTER, 0};
  ButtonAction escAct   = {ACT_KEY, 0, KEY_ESC, 0};
  writeButtonAction(1, 5, enterAct);
  writeButtonAction(1, 6, escAct);

  // Layout 3 default: inverted numpad rows (1->7, 2->8, 3->9, 4->4, 5->5, 6->6, 7->1, 8->2, 9->3)
  const uint8_t l3Keys[9] = {KEY_7, KEY_8, KEY_9, KEY_4, KEY_5, KEY_6, KEY_1, KEY_2, KEY_3};
  for (int b = 0; b < 9; b++) {
    ButtonAction act = {ACT_KEY, 0, l3Keys[b], 0};
    writeButtonAction(2, b, act);
  }

  // Clear first byte of text pool
  EEPROM.update(EEPROM_TXT_ADDR, 0);

  EEPROM.update(EEPROM_MAGIC_ADDR,     'M');
  EEPROM.update(EEPROM_MAGIC_ADDR + 1, 'P');
  EEPROM.update(EEPROM_MAGIC_ADDR + 2, 'V');
  EEPROM.update(EEPROM_MAGIC_ADDR + 3, 2);
}

void ensureEepromInitialized() {
  if (EEPROM.read(EEPROM_MAGIC_ADDR)     != 'M' ||
      EEPROM.read(EEPROM_MAGIC_ADDR + 1) != 'P' ||
      EEPROM.read(EEPROM_MAGIC_ADDR + 2) != 'V' ||
      EEPROM.read(EEPROM_MAGIC_ADDR + 3) != 2) {
    resetDefaults();
  } else {
    loadGlobalConfig();
  }
}

// ============================================================================
// LED & Layout State Management
// ============================================================================
void StartAnimation() {
  int waitTime = 120;
  for (int i = 0; i < numStates; i++) {
    digitalWrite(States[i], HIGH);
    delay(waitTime);
  }
  for (int i = 0; i < numStates; i++) {
    digitalWrite(States[i], LOW);
    delay(waitTime);
  }
}

void setActiveLayout(int newLayout, bool notifySerial) {
  if (newLayout < 0) newLayout = NUM_LAYOUTS - 1;
  if (newLayout >= NUM_LAYOUTS) newLayout = 0;
  digitalWrite(States[currentState], LOW);
  currentState = newLayout;
  digitalWrite(States[currentState], HIGH);
  if (notifySerial) {
    Serial.print(F("EVT LAYOUT "));
    Serial.println(currentState);
  }
}

// ============================================================================
// Action Execution
// ============================================================================
void pressModifiers(uint8_t mods) {
  if (mods & 0x01) Keyboard.press(KEY_LEFT_CTRL);
  if (mods & 0x02) Keyboard.press(KEY_LEFT_SHIFT);
  if (mods & 0x04) Keyboard.press(KEY_LEFT_ALT);
  if (mods & 0x08) Keyboard.press(KEY_LEFT_GUI);
  if (mods & 0x10) Keyboard.press(KEY_RIGHT_CTRL);
  if (mods & 0x20) Keyboard.press(KEY_RIGHT_SHIFT);
  if (mods & 0x40) Keyboard.press(KEY_RIGHT_ALT);
  if (mods & 0x80) Keyboard.press(KEY_RIGHT_GUI);
}

void executeConsumerCode(uint16_t code) {
  if (code == 0) return;
  // Special codes 0xFF01 / 0xFF02: macOS Fine Volume (Option + Shift + VolUp/VolDown)
  if (code == 0xFF01 || code == 0xFF02) {
    Keyboard.press(KEY_LEFT_ALT);
    Keyboard.press(KEY_LEFT_SHIFT);
    delay(5);
    Consumer.write(code == 0xFF01 ? MEDIA_VOLUME_UP : MEDIA_VOLUME_DOWN);
    delay(5);
    Keyboard.releaseAll();
    return;
  }
  Consumer.write((ConsumerKeycode)code);
}

void sendDeadKeyChar(KeyboardKeycode deadKey, KeyboardKeycode baseKey, bool shiftBase) {
  Keyboard.press(KEY_LEFT_ALT);
  Keyboard.press(deadKey);
  delay(8);
  Keyboard.releaseAll();
  delay(8);
  if (shiftBase) Keyboard.press(KEY_LEFT_SHIFT);
  Keyboard.press(baseKey);
  delay(8);
  Keyboard.releaseAll();
  delay(6);
}

void typeUtf8TextFromEeprom(uint16_t offset) {
  uint16_t i = offset;
  while (i < EEPROM_TXT_SIZE) {
    uint8_t b1 = EEPROM.read(EEPROM_TXT_ADDR + i);
    if (b1 == 0 || b1 == 0xFF) break;
    i++;

    // Handle 2-byte UTF-8 Latin-1 Supplement (0xC2 / 0xC3) for macOS ABC layout
    if ((b1 == 0xC3 || b1 == 0xC2) && i < EEPROM_TXT_SIZE) {
      uint8_t b2 = EEPROM.read(EEPROM_TXT_ADDR + i);
      if (b2 != 0 && b2 != 0xFF) i++;

      if (b1 == 0xC3) {
        switch (b2) {
          case 0xB1: sendDeadKeyChar(KEY_N, KEY_N, false); break; // ñ
          case 0x91: sendDeadKeyChar(KEY_N, KEY_N, true);  break; // Ñ
          case 0xA1: sendDeadKeyChar(KEY_E, KEY_A, false); break; // á
          case 0x81: sendDeadKeyChar(KEY_E, KEY_A, true);  break; // Á
          case 0xA9: sendDeadKeyChar(KEY_E, KEY_E, false); break; // é
          case 0x89: sendDeadKeyChar(KEY_E, KEY_E, true);  break; // É
          case 0xAD: sendDeadKeyChar(KEY_E, KEY_I, false); break; // í
          case 0x8D: sendDeadKeyChar(KEY_E, KEY_I, true);  break; // Í
          case 0xB3: sendDeadKeyChar(KEY_E, KEY_O, false); break; // ó
          case 0x93: sendDeadKeyChar(KEY_E, KEY_O, true);  break; // Ó
          case 0xBA: sendDeadKeyChar(KEY_E, KEY_U, false); break; // ú
          case 0x9A: sendDeadKeyChar(KEY_E, KEY_U, true);  break; // Ú
          case 0xBC: sendDeadKeyChar(KEY_U, KEY_U, false); break; // ü
          case 0x9C: sendDeadKeyChar(KEY_U, KEY_U, true);  break; // Ü
        }
      } else if (b1 == 0xC2) {
        if (b2 == 0xA1) { // ¡ (Option + 1)
          Keyboard.press(KEY_LEFT_ALT);
          Keyboard.press(KEY_1);
          delay(8);
          Keyboard.releaseAll();
          delay(6);
        } else if (b2 == 0xBF) { // ¿ (Option + Shift + /)
          Keyboard.press(KEY_LEFT_ALT);
          Keyboard.press(KEY_LEFT_SHIFT);
          Keyboard.press(KEY_SLASH);
          delay(8);
          Keyboard.releaseAll();
          delay(6);
        }
      }
      continue;
    }

    // Layout-independent symbols via USB HID Keypad codes (universal across US/ABC & Spanish)
    if (b1 == '-') { Keyboard.write(KEYPAD_SUBTRACT); delay(4); continue; }
    if (b1 == '/') { Keyboard.write(KEYPAD_DIVIDE);   delay(4); continue; }
    if (b1 == '*') { Keyboard.write(KEYPAD_MULTIPLY); delay(4); continue; }
    if (b1 == '+') { Keyboard.write(KEYPAD_ADD);      delay(4); continue; }

    Keyboard.write((char)b1);
    delay(4);
  }
}

void executeButtonAction(const ButtonAction &act) {
  switch (act.type) {
    case ACT_NONE:
      break;

    case ACT_KEY:
      pressModifiers(act.mods);
      if (act.arg1 != 0) Keyboard.press((KeyboardKeycode)act.arg1);
      if (act.arg2 != 0) Keyboard.press((KeyboardKeycode)act.arg2);
      delay(12);
      Keyboard.releaseAll();
      break;

    case ACT_MEDIA: {
      uint16_t code = (uint16_t)act.arg1 | ((uint16_t)act.arg2 << 8);
      executeConsumerCode(code);
      break;
    }

    case ACT_TEXT: {
      uint16_t offset = (uint16_t)act.arg1 | ((uint16_t)act.arg2 << 8);
      if (offset < EEPROM_TXT_SIZE) {
        typeUtf8TextFromEeprom(offset);
        if (act.mods & 0x01) {
          Keyboard.write(KEY_ENTER);
        }
        if (act.mods & 0x02) {
          Keyboard.write(KEY_TAB);
        }
      }
      break;
    }

    case ACT_SEQ: {
      uint8_t mods1 = act.mods & 0x0F;
      uint8_t mods2 = (act.mods >> 4) & 0x0F;
      if (mods1 || act.arg1) {
        pressModifiers(mods1);
        if (act.arg1 != 0) Keyboard.press((KeyboardKeycode)act.arg1);
        delay(12);
        Keyboard.releaseAll();
      }
      delay(45);
      if (mods2 || act.arg2) {
        pressModifiers(mods2);
        if (act.arg2 != 0) Keyboard.press((KeyboardKeycode)act.arg2);
        delay(12);
        Keyboard.releaseAll();
      }
      break;
    }

    case ACT_LAYOUT:
      if (act.arg1 == 255) {
        setActiveLayout((currentState + 1) % NUM_LAYOUTS, true);
      } else if (act.arg1 == 254) {
        setActiveLayout((currentState + NUM_LAYOUTS - 1) % NUM_LAYOUTS, true);
      } else if (act.arg1 < NUM_LAYOUTS) {
        setActiveLayout(act.arg1, true);
      }
      break;
  }
}

// ============================================================================
// Web Serial Configuration Protocol
// ============================================================================
uint8_t hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}

void printHexByte(uint8_t b) {
  const char hex[] = "0123456789ABCDEF";
  Serial.print(hex[(b >> 4) & 0x0F]);
  Serial.print(hex[b & 0x0F]);
}

void dumpConfigToSerial() {
  Serial.print(F("CFG "));
  Serial.print(cfg.encStepsPerClick); Serial.print(' ');
  Serial.print(cfg.encInvert);        Serial.print(' ');
  Serial.print(cfg.encCwCode);        Serial.print(' ');
  Serial.print(cfg.encCcwCode);       Serial.print(' ');
  Serial.print(cfg.encClickCode);     Serial.print(' ');
  Serial.print(cfg.encLongClickCode); Serial.print(' ');
  Serial.print(cfg.defaultLayout);    Serial.print(' ');
  Serial.println(currentState);

  for (int l = 0; l < NUM_LAYOUTS; l++) {
    for (int b = 0; b < NUM_BUTTONS; b++) {
      ButtonAction act;
      readButtonAction(l, b, act);
      Serial.print(F("BTN "));
      Serial.print(l);        Serial.print(' ');
      Serial.print(b);        Serial.print(' ');
      Serial.print(act.type); Serial.print(' ');
      Serial.print(act.mods); Serial.print(' ');
      Serial.print(act.arg1); Serial.print(' ');
      Serial.println(act.arg2);
    }
  }

  // Find highest used byte in text pool to avoid dumping unused trailing 0x00/0xFF
  int maxTxtLen = 0;
  for (int l = 0; l < NUM_LAYOUTS; l++) {
    for (int b = 0; b < NUM_BUTTONS; b++) {
      ButtonAction act;
      readButtonAction(l, b, act);
      if (act.type == ACT_TEXT) {
        uint16_t off = (uint16_t)act.arg1 | ((uint16_t)act.arg2 << 8);
        if (off < EEPROM_TXT_SIZE) {
          int i = off;
          while (i < EEPROM_TXT_SIZE) {
            uint8_t ch = EEPROM.read(EEPROM_TXT_ADDR + i);
            i++;
            if (ch == 0 || ch == 0xFF) break;
          }
          if (i > maxTxtLen) maxTxtLen = i;
        }
      }
    }
  }

  const int CHUNK = 32;
  for (int off = 0; off < maxTxtLen; off += CHUNK) {
    int len = maxTxtLen - off;
    if (len > CHUNK) len = CHUNK;
    Serial.print(F("TXT "));
    Serial.print(off);
    Serial.print(' ');
    for (int i = 0; i < len; i++) {
      printHexByte(EEPROM.read(EEPROM_TXT_ADDR + off + i));
    }
    Serial.println();
  }

  Serial.println(F("END"));
}

void handleSerialCommand(String line) {
  line.trim();
  if (line.length() == 0) return;

  if (line == "PING") {
    Serial.println(F("PONG MPV2"));
    return;
  }
  if (line == "DUMP") {
    dumpConfigToSerial();
    return;
  }
  if (line == "RESET_DEFAULTS") {
    resetDefaults();
    setActiveLayout(cfg.defaultLayout, false);
    Serial.println(F("OK"));
    return;
  }
  if (line.startsWith("SET_LAYOUT ")) {
    int l = line.substring(11).toInt();
    setActiveLayout(l, false);
    Serial.println(F("OK"));
    return;
  }
  if (line.startsWith("SET_ENC ")) {
    int p1 = 8;
    int s1 = line.indexOf(' ', p1);
    int s2 = line.indexOf(' ', s1 + 1);
    int s3 = line.indexOf(' ', s2 + 1);
    int s4 = line.indexOf(' ', s3 + 1);
    int s5 = line.indexOf(' ', s4 + 1);
    int s6 = line.indexOf(' ', s5 + 1);
    if (s1 > 0 && s2 > 0 && s3 > 0 && s4 > 0 && s5 > 0 && s6 > 0) {
      cfg.encStepsPerClick = (uint8_t)line.substring(p1, s1).toInt();
      cfg.encInvert        = (uint8_t)line.substring(s1 + 1, s2).toInt();
      cfg.encCwCode        = (uint16_t)line.substring(s2 + 1, s3).toInt();
      cfg.encCcwCode       = (uint16_t)line.substring(s3 + 1, s4).toInt();
      cfg.encClickCode     = (uint16_t)line.substring(s4 + 1, s5).toInt();
      cfg.encLongClickCode = (uint16_t)line.substring(s5 + 1, s6).toInt();
      cfg.defaultLayout    = (uint8_t)line.substring(s6 + 1).toInt();
      if (cfg.encStepsPerClick == 0) cfg.encStepsPerClick = 4;
      saveGlobalConfig();
      volumeKnob.write(0);
      oldPosition = 0;
      Serial.println(F("OK"));
    } else {
      Serial.println(F("ERR SET_ENC"));
    }
    return;
  }
  if (line.startsWith("SET_BTN ")) {
    int p1 = 8;
    int s1 = line.indexOf(' ', p1);
    int s2 = line.indexOf(' ', s1 + 1);
    int s3 = line.indexOf(' ', s2 + 1);
    int s4 = line.indexOf(' ', s3 + 1);
    int s5 = line.indexOf(' ', s4 + 1);
    if (s1 > 0 && s2 > 0 && s3 > 0 && s4 > 0 && s5 > 0) {
      int l = line.substring(p1, s1).toInt();
      int b = line.substring(s1 + 1, s2).toInt();
      ButtonAction act;
      act.type = (uint8_t)line.substring(s2 + 1, s3).toInt();
      act.mods = (uint8_t)line.substring(s3 + 1, s4).toInt();
      act.arg1 = (uint8_t)line.substring(s4 + 1, s5).toInt();
      act.arg2 = (uint8_t)line.substring(s5 + 1).toInt();
      if (l >= 0 && l < NUM_LAYOUTS && b >= 0 && b < NUM_BUTTONS) {
        writeButtonAction(l, b, act);
        Serial.println(F("OK"));
        return;
      }
    }
    Serial.println(F("ERR SET_BTN"));
    return;
  }
  if (line.startsWith("SET_TXT ")) {
    int p1 = 8;
    int s1 = line.indexOf(' ', p1);
    if (s1 > 0) {
      int offset = line.substring(p1, s1).toInt();
      String hexStr = line.substring(s1 + 1);
      int numBytes = hexStr.length() / 2;
      for (int i = 0; i < numBytes; i++) {
        if (offset + i < EEPROM_TXT_SIZE) {
          uint8_t val = (hexNibble(hexStr[i * 2]) << 4) | hexNibble(hexStr[i * 2 + 1]);
          EEPROM.update(EEPROM_TXT_ADDR + offset + i, val);
        }
      }
      Serial.println(F("OK"));
      return;
    }
    Serial.println(F("ERR SET_TXT"));
    return;
  }

  Serial.println(F("ERR UNKNOWN"));
}

// ============================================================================
// Setup & Main Loop
// ============================================================================
void setup() {
  Serial.begin(115200);
  serialLine.reserve(96);

  for (int i = 0; i < numStates; i++) {
    pinMode(States[i], OUTPUT);
    digitalWrite(States[i], LOW);
  }

  pinMode(PIN_CLK, INPUT_PULLUP);
  pinMode(PIN_DT, INPUT_PULLUP);
  encoderButton.attach(PIN_SW, INPUT_PULLUP);
  encoderButton.interval(10);

  ensureEepromInitialized();
  currentState = cfg.defaultLayout;

  Keyboard.begin();
  Consumer.begin();

  StartAnimation();
  digitalWrite(States[currentState], HIGH);
  volumeKnob.write(0);
  oldPosition = 0;
}

void loop() {
  // 1. Process incoming Web Serial configuration commands
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialLine.length() > 0) {
        handleSerialCommand(serialLine);
        serialLine = "";
      }
    } else if (serialLine.length() < 160) {
      serialLine += c;
    }
  }

  // 2. Scan key matrix
  char key = kpd.getKey();
  if (key) {
    if (key == '*') {
      setActiveLayout((currentState + 1) % NUM_LAYOUTS, true);
      delay(80);
    } else {
      int btnIdx = buttonIndexFromChar(key);
      if (btnIdx >= 0) {
        Serial.print(F("EVT KEY "));
        Serial.print(currentState);
        Serial.print(' ');
        Serial.println(btnIdx);

        ButtonAction act;
        readButtonAction(currentState, btnIdx, act);
        executeButtonAction(act);
      }
    }
  }

  // 3. Check encoder button
  if (encoderButton.update()) {
    if (encoderButton.fell()) {
      if (cfg.encLongClickCode == 0) {
        // Immediate click mode (no release delay)
        Serial.println(F("EVT ENC CLICK"));
        executeConsumerCode(cfg.encClickCode);
      } else {
        unsigned long fall = millis();
        while (!encoderButton.update()) {}
        if (encoderButton.rose()) {
          unsigned long rise = millis();
          if (rise - fall > 500) {
            Serial.println(F("EVT ENC LONG"));
            executeConsumerCode(cfg.encLongClickCode);
          } else {
            Serial.println(F("EVT ENC CLICK"));
            executeConsumerCode(cfg.encClickCode);
          }
        }
      }
      Keyboard.releaseAll();
    }
  }

  // 4. Check encoder rotation
  long newPosition = volumeKnob.read() / (long)cfg.encStepsPerClick;
  if (newPosition != oldPosition) {
    long diff = newPosition - oldPosition;
    oldPosition = newPosition;
    bool clockwise = cfg.encInvert ? (diff > 0) : (diff < 0);
    if (clockwise) {
      Serial.println(F("EVT ENC CW"));
      executeConsumerCode(cfg.encCwCode);
    } else {
      Serial.println(F("EVT ENC CCW"));
      executeConsumerCode(cfg.encCcwCode);
    }
    Keyboard.releaseAll();
  }
}
