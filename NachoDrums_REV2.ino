/*
  NachoDrums – REV 2.0 – Arduino Mega 2560
  ==========================================
  ARQUITECTURA REESCRITA DESDE CERO
  - Detección de golpes de ultra-baja latencia (<10ms)
  - LCD y botones nunca bloquean el escaneo de pads
  - EEPROM escribe solo al soltar botón (no en cada tick)
  - Note Off no bloqueante con millis()
  - Calibración Hi-Hat con pedal
  - Menú LCD completo
  - EEPROM con checksum
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>

// ================================================================
//  HARDWARE
// ================================================================
#define NUM_PADS        12
#define HIHAT_PIN       A12

const byte PAD_PIN[NUM_PADS] = {A0,A1,A2,A3,A4,A5,A6,A7,A8,A9,A10,A11};

// Botones
#define BTN_NEXT        22
#define BTN_UP          23
#define BTN_DOWN        24
#define BTN_PAD_NEXT    26
#define BTN_PAD_BACK    28
#define BTN_TOGGLE_PAD  27
#define BTN_CANCEL      30
#define LED_PIN         25

// LCD I2C
#define LCD_ADDR  0x27
#define LCD_COLS  16
#define LCD_ROWS  2
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);

// ================================================================
//  MIDI
// ================================================================
#define MIDI_CH         9    // Canal 10 (0-indexed)

inline void midiNoteOn(byte note, byte vel) {
  Serial.write(0x90 | MIDI_CH);
  Serial.write(note & 0x7F);
  Serial.write(vel  & 0x7F);
}
inline void midiNoteOff(byte note) {
  Serial.write(0x80 | MIDI_CH);
  Serial.write(note & 0x7F);
  Serial.write((byte)0);
}
inline void midiCC(byte cc, byte val) {
  Serial.write(0xB0 | MIDI_CH);
  Serial.write(cc  & 0x7F);
  Serial.write(val & 0x7F);
}

// ================================================================
//  CONSTANTES DE DETECCIÓN
// ================================================================
#define PEAK_WINDOW_MS    8    // Ventana máxima de búsqueda de pico (ms)
#define NOTE_OFF_MS       15   // Tiempo antes de enviar Note Off
#define SENS_MIN          10
#define SENS_MAX          900
#define COOL_MIN          10
#define COOL_MAX          500
#define NOTE_MIN          0
#define NOTE_MAX          127

// Hi-Hat
#define HH_CLOSED_NOTE  42
#define HH_PEDAL_NOTE   44
#define HH_OPEN_NOTE    46
#define HH_HYSTERESIS   20

// ================================================================
//  ESTADO DE CADA PAD
// ================================================================
enum PadState : byte {
  PAD_IDLE,        // Esperando golpe
  PAD_PEAK_HUNT,   // Golpe detectado, buscando pico
  PAD_NOTE_OFF     // Nota enviada, esperando Note Off
};

struct Pad {
  // Configuración (guardada en EEPROM)
  byte          note;
  unsigned int  sens;
  unsigned int  cool;
  bool          active;

  // Estado runtime (no se guarda)
  PadState      state;
  int           peak;
  unsigned long stateStart;   // Cuando entró al estado actual
  unsigned long lastHit;      // Último golpe confirmado
};

Pad pads[NUM_PADS];

const char* PAD_NAME[NUM_PADS] = {
  "Caja","Tom 10\"","Tom 12\"","Tom Piso",
  "Bombo","Hi-hat","Crash","Ride",
  "Cowbell","Tom Alt","Pad 10","Pad 11"
};

const byte NOTE_DEFAULT[NUM_PADS] = {38,48,47,43,36,42,49,51,56,45,39,37};

// ================================================================
//  HI-HAT
// ================================================================
int hhOpenVal   = 700;
int hhClosedVal = 300;

// ================================================================
//  MENÚ LCD
// ================================================================
int  menuPad  = 0;   // 0..NUM_PADS-1 = pads | NUM_PADS = HiHat | NUM_PADS+1 = FactoryReset
int  menuItem = 0;   // Sub-ítem dentro del pad

// ================================================================
//  BOTONES
// ================================================================
#define DEBOUNCE_MS     180
#define HOLD_REPEAT_MS  80    // Repetición al mantener presionado UP/DOWN
#define HOLD_START_MS   400   // Delay antes de activar repetición

struct Button {
  byte pin;
  bool lastState;       // Estado anterior
  bool pressed;         // Flanco confirmado este ciclo
  bool held;            // Se mantiene presionado
  unsigned long pressTime;
  unsigned long lastRepeat;
};

//                         pin           lastState pressed held  pressTime lastRepeat
Button btnNext     = {BTN_NEXT,          true, false, false, 0, 0};
Button btnUp       = {BTN_UP,            true, false, false, 0, 0};
Button btnDown     = {BTN_DOWN,          true, false, false, 0, 0};
Button btnPadNext  = {BTN_PAD_NEXT,      true, false, false, 0, 0};
Button btnPadBack  = {BTN_PAD_BACK,      true, false, false, 0, 0};
Button btnToggle   = {BTN_TOGGLE_PAD,    true, false, false, 0, 0};
Button btnCancel   = {BTN_CANCEL,        true, false, false, 0, 0};

Button* allBtns[] = {&btnNext, &btnUp, &btnDown, &btnPadNext, &btnPadBack, &btnToggle, &btnCancel};
const byte NUM_BTNS = 7;

// ================================================================
//  LCD – ACTUALIZACIÓN NO BLOQUEANTE
// ================================================================
#define LCD_UPDATE_MS   80
unsigned long lastLcdUpdate = 0;
bool          lcdDirty      = true;   // Forzar redibujado

// ================================================================
//  LED
// ================================================================
#define LED_ON_MS   40
unsigned long ledTimer = 0;
bool          ledOn    = false;

// ================================================================
//  CALIBRACIÓN HI-HAT
// ================================================================
enum CalibState : byte {
  CAL_OFF,
  CAL_WAIT_CLOSED,
  CAL_WAIT_OPEN,
  CAL_DONE
};
CalibState calState    = CAL_OFF;
unsigned long calTimer = 0;
int lastHHVal          = 0;

// ================================================================
//  EEPROM CON CHECKSUM
// ================================================================
#define EEPROM_MAGIC  0xAB   // Byte marcador de versión

void eepromSave() {
  int addr = 0;
  byte cs  = EEPROM_MAGIC;
  EEPROM.update(addr++, EEPROM_MAGIC);

  for (int i = 0; i < NUM_PADS; i++) {
    EEPROM.update(addr++, pads[i].note);      cs ^= pads[i].note;
    EEPROM.put(addr, pads[i].sens);           cs ^= (pads[i].sens & 0xFF) ^ (pads[i].sens >> 8);   addr += 2;
    EEPROM.put(addr, pads[i].cool);           cs ^= (pads[i].cool & 0xFF) ^ (pads[i].cool >> 8);   addr += 2;
    EEPROM.update(addr++, pads[i].active ? 1 : 0); cs ^= (pads[i].active ? 1 : 0);
  }
  EEPROM.put(addr, hhOpenVal);   cs ^= (hhOpenVal   & 0xFF) ^ (hhOpenVal   >> 8); addr += 2;
  EEPROM.put(addr, hhClosedVal); cs ^= (hhClosedVal & 0xFF) ^ (hhClosedVal >> 8); addr += 2;
  EEPROM.update(addr, cs);
}

bool eepromLoad() {
  int addr = 0;
  byte cs  = EEPROM_MAGIC;

  if (EEPROM.read(addr++) != EEPROM_MAGIC) return false;

  for (int i = 0; i < NUM_PADS; i++) {
    pads[i].note = EEPROM.read(addr++);              cs ^= pads[i].note;
    EEPROM.get(addr, pads[i].sens);                  cs ^= (pads[i].sens & 0xFF) ^ (pads[i].sens >> 8); addr += 2;
    EEPROM.get(addr, pads[i].cool);                  cs ^= (pads[i].cool & 0xFF) ^ (pads[i].cool >> 8); addr += 2;
    byte onoff = EEPROM.read(addr++);                cs ^= onoff;
    pads[i].active = (onoff == 1);
  }
  EEPROM.get(addr, hhOpenVal);   cs ^= (hhOpenVal   & 0xFF) ^ (hhOpenVal   >> 8); addr += 2;
  EEPROM.get(addr, hhClosedVal); cs ^= (hhClosedVal & 0xFF) ^ (hhClosedVal >> 8); addr += 2;

  if (EEPROM.read(addr) != cs) return false;

  // Sanidad de valores
  for (int i = 0; i < NUM_PADS; i++) {
    if (pads[i].note > NOTE_MAX)          pads[i].note  = NOTE_DEFAULT[i];
    if (pads[i].sens < SENS_MIN || pads[i].sens > SENS_MAX) pads[i].sens = 50;
    if (pads[i].cool < COOL_MIN || pads[i].cool > COOL_MAX) pads[i].cool = 80;
  }
  if (hhOpenVal <= hhClosedVal || hhOpenVal > 1023 || hhClosedVal < 0) {
    hhOpenVal = 700; hhClosedVal = 300;
  }
  return true;
}

void factoryReset() {
  for (int i = 0; i < NUM_PADS; i++) {
    pads[i].note   = NOTE_DEFAULT[i];
    pads[i].sens   = 50;
    pads[i].cool   = 80;
    pads[i].active = (i < 8);   // Primeros 8 activos por defecto
  }
  hhOpenVal   = 700;
  hhClosedVal = 300;
  eepromSave();
}

// ================================================================
//  PROTOTIPOS
// ================================================================
void updateButton(Button& b, unsigned long now);
bool buttonFired(Button& b, unsigned long now, bool allowRepeat);
void scanPads(unsigned long now);
void processNoteOffs(unsigned long now);
void scanHiHat();
void readButtons(unsigned long now);
void handleMenu();
void updateLCD();
void advanceCalib();
void handleCalib();
void updateCalibLCD();
void updateLED(unsigned long now);
void eepromSave();
bool eepromLoad();
void factoryReset();

// ================================================================
//  SETUP
// ================================================================
void setup() {
  Serial.begin(31250);

  lcd.init();
  lcd.backlight();

  // Pines botones
  for (byte i = 0; i < NUM_BTNS; i++)
    pinMode(allBtns[i]->pin, INPUT_PULLUP);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // Inicializar estado runtime de pads
  for (int i = 0; i < NUM_PADS; i++) {
    pads[i].state      = PAD_IDLE;
    pads[i].peak       = 0;
    pads[i].stateStart = 0;
    pads[i].lastHit    = 0;
  }

  // Cargar config
  if (!eepromLoad()) {
    lcd.setCursor(0,0); lcd.print("EEPROM invalida ");
    lcd.setCursor(0,1); lcd.print("Reset fabrica...");
    delay(1500);
    factoryReset();
  }

  lcd.clear();
  lcd.setCursor(3,0); lcd.print("NachoDrums");
  lcd.setCursor(2,1); lcd.print("  REV 2.0  ");
  delay(1000);
  lcd.clear();
  lcdDirty = true;
}

// ================================================================
//  LOOP PRINCIPAL
//  Orden de prioridad: pads → HiHat → NoteOffs → botones → LCD
// ================================================================
void loop() {
  unsigned long now = millis();

  scanPads(now);
  scanHiHat();
  processNoteOffs(now);
  readButtons(now);

  if (calState == CAL_OFF) {
    handleMenu();
    if (now - lastLcdUpdate >= LCD_UPDATE_MS) {
      updateLCD();
      lastLcdUpdate = now;
    }
  } else {
    handleCalib();
    updateCalibLCD();
  }

  updateLED(now);
}

// ================================================================
//  ESCANEO DE PADS  ← NÚCLEO DE BAJA LATENCIA
// ================================================================
void scanPads(unsigned long now) {
  for (int i = 0; i < NUM_PADS; i++) {
    if (!pads[i].active) continue;

    int raw = analogRead(PAD_PIN[i]);

    switch (pads[i].state) {

      case PAD_IDLE:
        // ── Umbral cruzado: comenzar búsqueda de pico ──
        if (raw > (int)pads[i].sens &&
            now - pads[i].lastHit > pads[i].cool) {
          pads[i].state      = PAD_PEAK_HUNT;
          pads[i].peak       = raw;
          pads[i].stateStart = now;
        }
        break;

      case PAD_PEAK_HUNT:
        // ── Actualizar pico mientras la señal sube ──
        if (raw > pads[i].peak) pads[i].peak = raw;

        // Condición de disparo: señal bajó O se agotó la ventana
        if (raw < pads[i].peak / 2 || now - pads[i].stateStart >= PEAK_WINDOW_MS) {
          // Mapear pico → velocidad MIDI 1-127
          int vel = map(constrain(pads[i].peak, pads[i].sens, 1023),
                        pads[i].sens, 1023, 1, 127);
          midiNoteOn(pads[i].note, (byte)vel);

          pads[i].state      = PAD_NOTE_OFF;
          pads[i].stateStart = now;
          pads[i].lastHit    = now;

          // LED (excepto Hi-Hat pad 5)
          if (i != 5) { ledOn = true; ledTimer = now; }
        }
        break;

      case PAD_NOTE_OFF:
        // Gestionado en processNoteOffs()
        break;
    }
  }
}

// ================================================================
//  NOTE OFFS NO BLOQUEANTES
// ================================================================
void processNoteOffs(unsigned long now) {
  for (int i = 0; i < NUM_PADS; i++) {
    if (pads[i].state == PAD_NOTE_OFF &&
        now - pads[i].stateStart >= NOTE_OFF_MS) {
      midiNoteOff(pads[i].note);
      pads[i].state = PAD_IDLE;
    }
  }
}

// ================================================================
//  HI-HAT
// ================================================================
void scanHiHat() {
  if (!pads[5].active) return;

  static int lastCC = -1;
  static byte lastNote = 0;

  int val = analogRead(HIHAT_PIN);
  int cc  = map(constrain(val, hhClosedVal, hhOpenVal), hhClosedVal, hhOpenVal, 0, 127);

  // Solo enviar CC si el valor cambió (tolerancia de ±1 para ignorar ruido ADC)
  if (abs(cc - lastCC) > 1) {
    midiCC(4, (byte)cc);
    lastCC = cc;
  }

  byte nuevaNota;
  if      (val < hhClosedVal + HH_HYSTERESIS) nuevaNota = HH_CLOSED_NOTE;
  else if (val > hhOpenVal   - HH_HYSTERESIS) nuevaNota = HH_OPEN_NOTE;
  else                                         nuevaNota = HH_PEDAL_NOTE;

  // Si la nota cambió, actualizar y refrescar LCD si estamos en el menú Hi-Hat
  if (nuevaNota != lastNote) {
    pads[5].note = nuevaNota;
    lastNote     = nuevaNota;
    if (menuPad == 5) lcdDirty = true;
  }
}

// ================================================================
//  LECTURA DE BOTONES CON REPETICIÓN AL MANTENER
// ================================================================
void updateButton(Button& b, unsigned long now) {
  bool raw = !digitalRead(b.pin);  // LOW = presionado (INPUT_PULLUP)
  b.pressed = false;

  if (raw && !b.lastState) {
    // Flanco de bajada: solo aceptar si pasó el debounce desde el último press
    if (now - b.pressTime >= DEBOUNCE_MS) {
      b.pressed    = true;
      b.held       = true;
      b.pressTime  = now;
      b.lastRepeat = now;
    }
  } else if (!raw) {
    b.held = false;
  }
  b.lastState = raw;
}

// Devuelve true si el botón debe "disparar" este ciclo (press o repetición)
bool buttonFired(Button& b, unsigned long now, bool allowRepeat = false) {
  if (b.pressed) return true;
  if (allowRepeat && b.held) {
    if (now - b.pressTime >= HOLD_START_MS &&
        now - b.lastRepeat >= HOLD_REPEAT_MS) {
      b.lastRepeat = now;
      return true;
    }
  }
  return false;
}

// Bandera para guardar EEPROM al soltar UP/DOWN
bool eepromPending = false;

void readButtons(unsigned long now) {
  for (byte i = 0; i < NUM_BTNS; i++) updateButton(*allBtns[i], now);

  // Detectar soltado de UP/DOWN para guardar EEPROM
  if (eepromPending && !btnUp.held && !btnDown.held) {
    eepromSave();
    eepromPending = false;
  }

  // ── Calibración activa ──
  if (calState != CAL_OFF) {
    if (btnCancel.pressed) {
      calState = CAL_OFF;
      lcdDirty = true;
    }
    if (btnNext.pressed) advanceCalib();
    return;
  }

  // ── Iniciar calibración Hi-Hat ──
  if (menuPad == NUM_PADS && btnNext.pressed) {
    calState  = CAL_WAIT_CLOSED;
    calTimer  = now;
    lastHHVal = analogRead(HIHAT_PIN);
    return;
  }

  // ── Factory Reset ──
  if (menuPad == NUM_PADS + 1 && btnNext.pressed) {
    factoryReset();
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("Reset completo! ");
    lcd.setCursor(0,1); lcd.print("                ");
    delay(1500);
    eepromLoad();
    menuPad  = 0;
    lcdDirty = true;
    return;
  }

  // ── Navegación entre pads ──
  if (btnPadNext.pressed) {
    menuPad++;
    if (menuPad > NUM_PADS + 1) menuPad = 0;
    menuItem = 0; lcdDirty = true;
  }
  if (btnPadBack.pressed) {
    menuPad--;
    if (menuPad < 0) menuPad = NUM_PADS + 1;
    menuItem = 0; lcdDirty = true;
  }

  // ── Sub-ítems ──
  if (btnNext.pressed) {
    menuItem++;
    int maxItem = (menuPad < NUM_PADS) ? 2 : (menuPad == NUM_PADS ? 1 : 0);
    if (menuItem > maxItem) menuItem = 0;
    lcdDirty = true;
  }

  // ── Toggle pad ON/OFF ──
  if (btnToggle.pressed && menuPad < NUM_PADS) {
    pads[menuPad].active = !pads[menuPad].active;
    eepromSave();
    lcdDirty = true;
  }

  // ── UP / DOWN con repetición ──
  bool upFired   = buttonFired(btnUp,   now, true);
  bool downFired = buttonFired(btnDown, now, true);

  if (upFired || downFired) {
    int dir = upFired ? 1 : -1;

    if (menuPad < NUM_PADS) {
      if (menuItem == 0) pads[menuPad].sens = constrain((int)pads[menuPad].sens + dir * 5, SENS_MIN, SENS_MAX);
      if (menuItem == 1) pads[menuPad].cool = constrain((int)pads[menuPad].cool + dir * 10, COOL_MIN, COOL_MAX);
      if (menuItem == 2) pads[menuPad].note = constrain((int)pads[menuPad].note + dir, NOTE_MIN, NOTE_MAX);
    } else if (menuPad == NUM_PADS) {
      if (menuItem == 0) hhOpenVal   = constrain(hhOpenVal   + dir * 5, 0, 1023);
      if (menuItem == 1) hhClosedVal = constrain(hhClosedVal + dir * 5, 0, 1023);
    }
    eepromPending = true;
    lcdDirty = true;
  }
}

// ================================================================
//  MENÚ (solo lógica de actualización de LCD)
// ================================================================
void handleMenu() {
  // Nada que hacer aquí; la lógica está en readButtons()
}

void updateLCD() {
  if (!lcdDirty) return;
  lcdDirty = false;
  lcd.clear();

  if (menuPad < NUM_PADS) {
    // Línea 0: nombre + estado
    lcd.setCursor(0,0);
    lcd.print(PAD_NAME[menuPad]);
    lcd.print(" ");
    lcd.print(pads[menuPad].active ? "ON " : "OFF");

    // Línea 1: S / C / N con cursor >
    lcd.setCursor(0,1);
    if (menuPad == 5) {
      // Hi-Hat: mostrar nota activa en tiempo real en lugar de N fijo
      lcd.print(menuItem==0 ? ">" : " "); lcd.print("S"); lcd.print(pads[5].sens);
      lcd.print(" ");
      lcd.print(menuItem==1 ? ">" : " "); lcd.print("C"); lcd.print(pads[5].cool);
      lcd.print(" N:");
      switch (pads[5].note) {
        case HH_CLOSED_NOTE: lcd.print("42"); break;
        case HH_PEDAL_NOTE:  lcd.print("44"); break;
        case HH_OPEN_NOTE:   lcd.print("46"); break;
        default:             lcd.print(pads[5].note); break;
      }
    } else {
      lcd.print(menuItem==0 ? ">" : " "); lcd.print("S"); lcd.print(pads[menuPad].sens);
      lcd.print(" ");
      lcd.print(menuItem==1 ? ">" : " "); lcd.print("C"); lcd.print(pads[menuPad].cool);
      lcd.print(" ");
      lcd.print(menuItem==2 ? ">" : " "); lcd.print("N"); lcd.print(pads[menuPad].note);
    }

  } else if (menuPad == NUM_PADS) {
    // Línea 0: título
    lcd.setCursor(0,0); lcd.print("CALIBRAR HIHAT  ");

    // Línea 1: valores de calibración
    lcd.setCursor(0,1);
    lcd.print(menuItem==0 ? ">" : " "); lcd.print("Ab:"); lcd.print(hhOpenVal);
    lcd.print(" ");
    lcd.print(menuItem==1 ? ">" : " "); lcd.print("Cr:"); lcd.print(hhClosedVal);

  } else {
    lcd.setCursor(0,0); lcd.print("Factory Reset");
    lcd.setCursor(0,1); lcd.print("NEXT = confirmar");
  }
}

// ================================================================
//  CALIBRACIÓN HI-HAT
// ================================================================
void advanceCalib() {
  switch (calState) {
    case CAL_WAIT_CLOSED:
      hhClosedVal = analogRead(HIHAT_PIN);
      calState    = CAL_WAIT_OPEN;
      calTimer    = millis();
      lastHHVal   = hhClosedVal;
      break;
    case CAL_WAIT_OPEN:
      hhOpenVal = analogRead(HIHAT_PIN);
      calState  = CAL_DONE;
      calTimer  = millis();
      eepromSave();
      break;
    case CAL_DONE:
      calState = CAL_OFF;
      lcdDirty = true;
      break;
    default: break;
  }
}

void handleCalib() {
  // Auto-avance en CAL_DONE tras 2 segundos
  if (calState == CAL_DONE && millis() - calTimer > 2000) {
    calState = CAL_OFF;
    lcdDirty = true;
  }
}

void updateCalibLCD() {
  static CalibState lastCal = CAL_OFF;
  if (calState == lastCal) return;   // Solo redibujar al cambiar estado
  lastCal = calState;

  lcd.clear();
  switch (calState) {
    case CAL_WAIT_CLOSED:
      lcd.setCursor(0,0); lcd.print("Cerrar pedal");
      lcd.setCursor(0,1); lcd.print("Luego NEXT");
      break;
    case CAL_WAIT_OPEN:
      lcd.setCursor(0,0); lcd.print("Abrir pedal");
      lcd.setCursor(0,1); lcd.print("Luego NEXT");
      break;
    case CAL_DONE:
      lcd.setCursor(0,0); lcd.print("Calibrado OK!");
      lcd.setCursor(0,1);
      lcd.print("Cr:"); lcd.print(hhClosedVal);
      lcd.print(" Ab:"); lcd.print(hhOpenVal);
      break;
    default: break;
  }
}

// ================================================================
//  LED
// ================================================================
void updateLED(unsigned long now) {
  if (ledOn) {
    digitalWrite(LED_PIN, HIGH);
    if (now - ledTimer > LED_ON_MS) {
      digitalWrite(LED_PIN, LOW);
      ledOn = false;
    }
  }
}
