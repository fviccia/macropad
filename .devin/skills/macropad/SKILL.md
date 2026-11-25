---
name: macropad
description: Arquitectura, pinout, mapa de EEPROM, protocolo Web Serial y guía para extender el firmware (macropad.ino) o el configurador web (index.html) del Programmable Macropad V2.
---

# Programmable Macropad V2 — Guía Técnica y de Extensión

Este proyecto controla un **Programmable Macropad V2** (basado en un **Arduino Pro Micro ATmega32U4**) mediante un firmware con almacenamiento en **EEPROM** (`macropad.ino`) y una aplicación web configuradora sin servidor (`index.html`) que usa la **Web Serial API** (`navigator.serial`).

---

## 1. Hardware y Pinout Calibrado

El microcontrolador es un **ATmega32U4** (5V / 16MHz), compilado con el FQBN `arduino:avr:leonardo` (cuando tiene cargado `HID-Project`, enumera por USB como `/dev/cu.usbmodemHIDEF1`).

### Matriz de 11 Teclas (4 filas × 3 columnas con diodos 1N4148)
- **Pines de Fila (`rowPins`):** `{6, 5, 21, 20}` (`21 = A3`, `20 = A2`)
- **Pines de Columna (`colPins`):** `{7, 8, 9}`
- **Disposición física (vista de frente):**
  ```text
  Fila 1:  [ Encoder ('X') ]   [ Cambiar Layout ('*') ]   [ Botón Extra ('-') ]
  Fila 2:  [      '7'      ]   [         '8'          ]   [        '9'        ]
  Fila 3:  [      '4'      ]   [         '5'          ]   [        '6'        ]
  Fila 4:  [      '1'      ]   [         '2'          ]   [        '3'        ]
  ```
- **Índices internos de botón (`btnIdx` `0..9` en cada Layout):**
  - `0..8`: Teclas `'1'` a `'9'` (`btnIdx = char - '1'`)
  - `9`: Tecla superior derecha `'-'`
  - La tecla `'*'` es fija y rota entre los 4 layouts (`0 → 1 → 2 → 3 → 0`).

### 4 LEDs Indicadores de Layout (abajo del frente)
- **Pines (`States[4]`):** `{10, 16, 14, 15}`
  - `Layout 1 (índice 0)` → Pin `10` (LED más a la izquierda)
  - `Layout 2 (índice 1)` → Pin `16` (2º LED desde la izquierda)
  - `Layout 3 (índice 2)` → Pin `14` (3º LED desde la izquierda)
  - `Layout 4 (índice 3)` → Pin `15` (LED más a la derecha)
  - *Nota:* El Instructable original listaba `{15, 14, 16, 10}`, pero al soldarse viendo el plato desde abajo quedan espejados; `{10, 16, 14, 15}` es el orden correcto de izquierda a derecha en esta unidad.

### Encoder Rotativo con Pulsador (arriba a la izquierda)
- **Pines:** `PIN_DT = 2`, `PIN_CLK = 3` (ambos con interrupción de hardware en ATmega32U4), `PIN_SW = 19 (A1)`.
- **Instanciación:** `Encoder volumeKnob(PIN_CLK, PIN_DT);`
- **Calibración:** Cada diente físico ("click") genera **4 transiciones de cuadratura** (`encStepsPerClick = 4`), y el sentido horario (`CW`) corresponde a `diff > 0` con `encInvert = 1`.

---

## 2. Mapa de Memoria EEPROM (1024 bytes totales en ATmega32U4)

| Rango de Direcciones | Tamaño | Contenido |
|---|---|---|
| `0 .. 3` | 4 bytes | **Magic Header:** `'M'`, `'P'`, `'V'`, `2` (si no coincide al arrancar, `ensureEepromInitialized()` ejecuta `resetDefaults()`). |
| `4 .. 15` | 12 bytes | **`struct GlobalConfig`:**<br>- `4`: `encStepsPerClick` (`uint8_t`, default `4`)<br>- `5`: `encInvert` (`uint8_t`, default `1`)<br>- `6..7`: `encCwCode` (`uint16_t`, default `MEDIA_VOLUME_UP = 0x00E9`)<br>- `8..9`: `encCcwCode` (`uint16_t`, default `MEDIA_VOLUME_DOWN = 0x00EA`)<br>- `10..11`: `encClickCode` (`uint16_t`, default `MEDIA_VOLUME_MUTE = 0x00E2`)<br>- `12..13`: `encLongClickCode` (`uint16_t`, default `0` = desactivado para que el click corto sea instantáneo al presionar)<br>- `14`: `defaultLayout` (`uint8_t`, `0..3`, default `0`)<br>- `15`: `reserved` (`0`) |
| `16 .. 175` | 160 bytes | **40 Slots de Botón** (`4 layouts × 10 botones × 4 bytes`):<br>Dirección = `16 + ((layout * 10 + btnIdx) * 4)`.<br>Cada botón es `struct ButtonAction { uint8_t type, mods, arg1, arg2; }`. |
| `176 .. 1023` | 848 bytes | **Pool Dinámico de Textos (`ACT_TEXT`):**<br>Strings UTF-8 terminados en `\0` (`0x00`). La web deduplica textos repetidos y calcula el offset (`0..847`) guardado en `arg1 | (arg2 << 8)`. |

### Tipos de Acción (`ActionType` en `ButtonAction.type`)

1. **`ACT_NONE (0)`**: Sin acción.
2. **`ACT_KEY (1)`**: Atajo de teclado o tecla simple.
   - `mods`: Bitmask de modificadores (`0x01` = Left Ctrl, `0x02` = Left Shift, `0x04` = Left Alt/Option, `0x08` = Left GUI/Cmd).
   - `arg1`: `KeyboardKeycode` USB HID (`uint8_t`, ej. `4..29` = `A..Z`, `30..39` = `1..0`, `40` = Enter, `41` = Esc, `44` = Space, `58..69` = `F1..F12`, `104..115` = `F13..F24`).
   - `arg2`: Segunda tecla simultánea opcional (`0` si no se usa).
3. **`ACT_MEDIA (2)`**: Acción multimedia (`ConsumerKeycode` de 16 bits en `arg1 | (arg2 << 8)`).
   - Incluye códigos estándar (`0x00CD` Play/Pause, `0x00E2` Mute, `0x00E9` Vol+, `0x00EA` Vol-, `0x006F` Brillo+, `0x0070` Brillo-) y dos códigos sintéticos para ajuste fino de volumen en macOS:
     - `0xFF01`: `Option + Shift + Volume Up` (¼ de bloque en macOS)
     - `0xFF02`: `Option + Shift + Volume Down` (¼ de bloque en macOS)
4. **`ACT_TEXT (3)`**: Escribir texto o comando desde el pool de EEPROM.
   - `offset = arg1 | (arg2 << 8)` (relativo a `EEPROM_TXT_ADDR = 176`).
   - `mods`: `bit 0 (0x01)` = enviar `Enter` al final; `bit 1 (0x02)` = enviar `Tab` al final.
   - **Independencia de distribución de teclado en macOS:**
     - `typeUtf8TextFromEeprom()` detecta secuencias UTF-8 de 2 bytes (`0xC3` / `0xC2`) para `ñ`, `Ñ`, `á, é, í, ó, ú`, `Á, É, Í, Ó, Ú`, `ü, Ü`, `¿, ¡` y las escribe usando las *dead keys* universales de macOS (`⌥ Option + N` → `N` para `ñ`, que funciona tanto en teclado `ABC`/`US` como en `Spanish`/`Spanish - ISO`).
     - Para los símbolos `-`, `/`, `*`, `+`, envía los códigos del teclado numérico (`KEYPAD_SUBTRACT`, `KEYPAD_DIVIDE`, `KEYPAD_MULTIPLY`, `KEYPAD_ADD`), que producen el mismo carácter sin importar qué idioma de teclado esté activo en el sistema operativo.
5. **`ACT_SEQ (4)`**: Secuencia de 2 atajos separados por 45 ms (ideal para acordes tipo VS Code `Cmd+K → Cmd+S` o `tmux` `Ctrl+B → D`).
   - Paso 1: modificadores `mods & 0x0F` + tecla `arg1`.
   - Paso 2: modificadores `(mods >> 4) & 0x0F` + tecla `arg2`.
6. **`ACT_LAYOUT (5)`**: Cambiar de layout desde cualquier tecla (`arg1 = 0..3` va directo a ese layout, `254` = anterior, `255` = siguiente).

---

## 3. Protocolo Web Serial (`115200` baud, líneas terminadas en `\n`)

### Comandos del Navegador → Arduino
- `PING` → Responde `PONG MPV2`
- `DUMP` → Devuelve el estado completo:
  - `CFG <steps> <invert> <cw> <ccw> <click> <longClick> <defaultLayout> <currentState>`
  - 40 líneas `BTN <layout> <btnIdx> <type> <mods> <arg1> <arg2>`
  - Líneas `TXT <offset> <HEX_BYTES>` (para la porción usada del pool de textos)
  - `END`
- `SET_ENC <steps> <invert> <cw> <ccw> <click> <longClick> <defaultLayout>` → Guarda en EEPROM y responde `OK`.
- `SET_BTN <layout> <btnIdx> <type> <mods> <arg1> <arg2>` → Guarda en EEPROM (`EEPROM.update`) y responde `OK`.
- `SET_TXT <offset> <HEX_BYTES>` → Escribe bytes decodificados desde hex en `EEPROM_TXT_ADDR + offset` y responde `OK`.
- `SET_LAYOUT <0..3>` → Cambia el layout activo y actualiza los LEDs físicos en tiempo real, responde `OK`.
- `RESET_DEFAULTS` → Restaura la EEPROM de fábrica y responde `OK`.

### Eventos en Vivo del Arduino → Navegador
- `EVT KEY <layout> <btnIdx>` (cuando el usuario presiona una tecla física)
- `EVT LAYOUT <layout>` (cuando el usuario presiona `*` o una tecla de cambio de layout)
- `EVT ENC CW` / `EVT ENC CCW` / `EVT ENC CLICK` / `EVT ENC LONG`

---

## 4. Cómo Agregar Nuevas Opciones en el Futuro

### Caso A: Agregar nuevos Presets o Teclas a la Web (Sin tocar ni reflashear el Arduino)
1. Abrir `index.html`.
2. Para agregar un **Preset rápido** (ej. un atajo de una app), agregar una entrada al array `MACOS_PRESETS`:
   ```js
   { name: "🚀 Raycast AI", label: "Raycast AI", type: 1, mods: 8 | 4, arg1: 44, arg2: 0 }
   ```
   - Recordar los bits de `mods`: `1 = Ctrl`, `2 = Shift`, `4 = Option/Alt`, `8 = Cmd`.
   - Los códigos `arg1` están en la tabla `HID_KEYS` de `index.html`.
3. Guardar `index.html` (y hacer `git commit && git push` si se usa desde GitHub Pages). No hace falta compilar ni flashear el Pro Micro.

### Caso B: Modificar el Firmware (`macropad.ino`) y Flashear en una Mac Nueva
1. Si la Mac no tiene las herramientas instaladas, correr:
   ```bash
   brew install arduino-cli
   arduino-cli core update-index
   arduino-cli core install arduino:avr
   arduino-cli lib install "Keypad" "Encoder" "Bounce2" "HID-Project"
   ```
2. Asegurarse de que ninguna pestaña de Chrome tenga el puerto Web Serial abierto (`lsof /dev/cu.usbmodem*` debe dar vacío).
3. Ejecutar:
   ```bash
   ./flash.sh
   ```
   *(Nota: Flashear un sketch nuevo en el ATmega32U4 **no borra la EEPROM**, por lo que tus layouts y textos guardados se mantienen intactos a menos que cambies la versión del header `MPV2`).*
