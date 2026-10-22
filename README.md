# NachoDrums

**Módulo de batería electrónica MIDI** basado en Arduino Mega 2560 con pantalla LCD I2C.  
Convierte pads piezoeléctricos en señales MIDI de baja latencia (<10ms) para usar con cualquier DAW o sintetizador.

---

## Características

- 12 pads piezoeléctricos independientes (A0–A11)
- Detección de golpe de ultra-baja latencia (<10ms)
- Velocity dinámica mapeada al pico de señal del piezo
- Hi-Hat con pedal analógico (CC MIDI #4) y detección de posición (abierto / pedal / cerrado)
- Salida MIDI por USB (mocoLUFA en el ATmega16U2) o DIN-5 opcional
- Pantalla LCD I2C 16x2 con menú completo de configuración
- Configuración guardada en EEPROM con checksum
- Note Off no bloqueante
- Sin ningún `delay()` en el path crítico

---

## Hardware necesario

| Componente | Cantidad | Notas |
|---|---|---|
| Arduino Mega 2560 | 1 | |
| Pantalla LCD I2C 16x2 | 1 | Dirección 0x27 |
| Piezoeléctrico 27mm | 12 | Para los pads |
| Sensor analógico pedal Hi-Hat | 1 | Potenciómetro o FSR |
| Resistencia 1 MΩ | 12 | En paralelo con cada piezo |
| Resistencia 1 KΩ | 12 | En serie entre piezo y pin analógico |
| Diodo zener 5.1V 1W | 12 | Protección del ADC (ej: 1N4733A) |
| Resistencia 220 Ω | 1 | Para el LED indicador |
| LED | 1 | Indicador de golpe |
| Pulsadores normalmente abiertos | 7 | Para el menú |
| Conector DIN-5 | 1 | Salida MIDI opcional; el firmware actual usa USB |
| Resistencia 220 Ω | 1 | Para el circuito MIDI DIN-5 |

---

## Pines

### Pads piezoeléctricos
| Pad | Pin | Nombre por defecto |
|---|---|---|
| 1 | A0 | Caja |
| 2 | A1 | Tom 10" |
| 3 | A2 | Tom 12" |
| 4 | A3 | Tom Piso |
| 5 | A4 | Bombo |
| 6 | A5 | Hi-hat |
| 7 | A6 | Crash |
| 8 | A7 | Ride |
| 9 | A8 | Cowbell |
| 10 | A9 | Tom Alt |
| 11 | A10 | Pad 10 |
| 12 | A11 | Pad 11 |

### Pedal Hi-Hat
| Pin | Función |
|---|---|
| A12 | Sensor analógico del pedal |

### Botones
| Pin | Función |
|---|---|
| 22 | NEXT (siguiente ítem del menú) |
| 23 | UP (incrementar valor) |
| 24 | DOWN (decrementar valor) |
| 26 | PAD+ (siguiente pad) |
| 27 | TOGGLE (activar/desactivar pad) |
| 28 | PAD− (pad anterior) |
| 30 | CANCEL (cancelar calibración) |

### Otros
| Pin | Función |
|---|---|
| 25 | LED indicador de golpe |
| 1 (TX0) | MIDI hacia el ATmega16U2 (mocoLUFA → USB-MIDI) |
| 18 (TX1) | Libre; hay que espejar los bytes para usar DIN-5 |
| 20 (SDA) | LCD I2C datos |
| 21 (SCL) | LCD I2C reloj |

---

## Circuito de protección por pad

Cada pad requiere el siguiente circuito para proteger el ADC del Arduino:

```
Piezo (+) ──┬── 1KΩ ──── Pin Ax
            │                │
           1MΩ            Zener 5.1V
            │                │
Piezo (−) ──┴────────────── GND
```

> ⚠️ **Importante:** No omitas el diodo zener. Los piezos de 27mm pueden generar picos de 8–15V que dañan el ADC del Arduino de forma permanente.

---

## Salida MIDI

### USB (recomendado)

El firmware escribe en **TX0 (pin 1)**, que va al ATmega16U2. Para que la PC lo vea como instrumento MIDI hay que flashear el 16U2 con **mocoLUFA** (o HIDuino). Sin ese firmware, el USB funciona solo como puerto serie y el DAW no ve nada.

> ⚠️ Con mocoLUFA instalado el puerto serie normal desaparece: para volver a cargar sketches hay que entrar en modo DFU puenteando los pads del 16U2 (o usar un programador ISP).

### DIN-5 (opcional)

El firmware no escribe en `Serial1`, así que para usar el conector hay que duplicar cada byte a `Serial1` (TX1, pin 18). El circuito estándar es:

```
TX1 (pin 18) ── 220Ω ── DIN-5 pin 5
+5V          ── 220Ω ── DIN-5 pin 4
                         DIN-5 pin 2 ── GND
                         DIN-5 pin 3 ── sin conectar
```

---

## Instalación

### Librerías necesarias
Instala desde el gestor de librerías del IDE de Arduino:
- `LiquidCrystal_I2C` de Frank de Brabander

### Pasos
1. Clona o descarga este repositorio
2. Abre `NachoDrums.ino` en el IDE de Arduino
3. Selecciona la placa: **Arduino Mega or Mega 2560**
4. Selecciona el puerto COM correspondiente
5. Carga el sketch

> Para MIDI por USB, el ATmega16U2 debe tener mocoLUFA instalado. Ver [Salida MIDI](#salida-midi).

---

## Uso del menú

Al encender, la pantalla muestra el nombre del pad actual y sus parámetros:

```
Caja ON
>S50 C80 N:38
```

| Parámetro | Descripción |
|---|---|
| S (Sens) | Umbral de detección. Subir si dispara solo, bajar si no detecta golpes suaves |
| C (Cool) | Tiempo de enfriamiento en ms. Subir si detecta doble golpe |
| N (Note) | Nota MIDI enviada (0–127) |

### Botones
| Botón | Función |
|---|---|
| PAD+ / PAD− | Navegar entre pads |
| NEXT | Cambiar entre S / C / N. En calibración: Ab / Cr / Empezar |
| UP / DOWN | Modificar el valor seleccionado (mantener para repetición) |
| TOGGLE | Activar o desactivar el pad actual |
| CANCEL | Cancelar calibración y restaurar los valores anteriores |

### Calibración Hi-Hat
1. Navega hasta el menú **CALIBRAR HIHAT**
2. Con NEXT elegí **Empezar** (podés ajustar `Ab` y `Cr` a mano antes)
3. Cierra el pedal completamente → presiona NEXT
4. Abre el pedal completamente → presiona NEXT
5. Listo — los valores se guardan automáticamente en EEPROM

Si el recorrido del pedal es menor a 80 counts, la pantalla muestra **Rango muy corto** y no guarda. `CANCEL` cancela y restaura los valores previos.

### Factory Reset
Navega hasta **Factory Reset** y presiona NEXT para restaurar todos los valores por defecto.

---

## Ajuste de latencia

Si sientes que la respuesta no es inmediata, estos son los parámetros a ajustar en el código:

| Constante | Valor por defecto | Efecto |
|---|---|---|
| `PEAK_WINDOW_MS` | 8 ms | Ventana máxima de búsqueda de pico. Bajar para menos latencia |
| `NOTE_OFF_MS` | 15 ms | Tiempo hasta enviar Note Off |
| `DEBOUNCE_MS` | 180 ms | Tiempo de debounce de botones |

---

## Notas MIDI por defecto

| Pad | Nota | Instrumento GM |
|---|---|---|
| Caja | 38 | Acoustic Snare |
| Tom 10" | 48 | Hi-Mid Tom |
| Tom 12" | 47 | Low-Mid Tom |
| Tom Piso | 43 | High Floor Tom |
| Bombo | 36 | Bass Drum 1 |
| Hi-Hat | 42/44/46 | Closed/Pedal/Open HH |
| Crash | 49 | Crash Cymbal 1 |
| Ride | 51 | Ride Cymbal 1 |
| Cowbell | 56 | Cowbell |
| Tom Alt | 45 | Low Tom |

---

## Licencia

Este proyecto está bajo la licencia **MIT**. Podés usarlo, modificarlo y distribuirlo libremente con atribución.

---

## Autor

**NachoDV** — Proyecto DIY de batería electrónica MIDI  
Construido con Arduino Mega 2560
