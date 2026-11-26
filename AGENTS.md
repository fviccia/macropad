# AGENTS.md — Programmable Macropad V2 (Arduino Pro Micro ATmega32U4)

Repositorio del firmware y configurador web del **Programmable Macropad V2** (basado en el diseño de TinyBoat, adaptado con almacenamiento en EEPROM y Web Serial API para macOS).

## Estructura del proyecto

- `macropad.ino`: Firmware para Arduino Pro Micro (`ATmega32U4`, compilado con FQBN `arduino:avr:leonardo`).
- `index.html`: Configurador web autónomo (HTML/CSS/JS sin dependencias externas) que se comunica con el Macropad vía **Web Serial API** (`navigator.serial` a `115200` baud) para leer y grabar la EEPROM en tiempo real sin necesidad de reflashear.
- `flash.sh`: Script para compilar y flashear `macropad.ino` usando `arduino-cli`.
- `.devin/skills/macropad/SKILL.md`: Documentación detallada de arquitectura, mapa de memoria EEPROM, protocolo serie y procedimientos para agregar nuevas funciones o presets.

## Reglas obligatorias antes de modificar el proyecto

1. **Invocar la skill `macropad`** (`.devin/skills/macropad/SKILL.md`) antes de modificar `macropad.ino` o `index.html`.
2. **No cambiar el pinout ni invertir direcciones ya calibradas** sin pedido explícito del usuario:
   - Matriz 4×3: Filas `{6, 5, 21, 20}`, Columnas `{7, 8, 9}`.
   - LEDs de estado (Layout 1 a 4, de izquierda a derecha): `{10, 16, 14, 15}` (espejados respecto al Instructable original porque están soldados desde abajo).
   - Encoder rotativo: `CLK = 3`, `DT = 2`, `SW = 19`, `ENC_STEPS_PER_CLICK = 4`, dirección invertida (`encInvert = 1`).
3. **Sincronización Firmware ↔ Web:**
   - Si solo se agregan **presets** o mejoras visuales en `index.html` usando los `ActionType` existentes (`0..5`), **no hace falta reflashear el Arduino**.
   - Si se modifica `macropad.ino`, siempre verificar compilación con `arduino-cli compile --fqbn arduino:avr:leonardo .` y recordar que para flashear (`./flash.sh`), la pestaña del navegador debe estar desconectada del puerto serie (`lsof /dev/cu.usbmodem*` vacío).
   - Nunca usar `1200` baud en conexiones Web Serial normales (en `ATmega32U4`, abrir y cerrar a `1200` baud dispara el reset al bootloader Caterina).
4. **Despliegue automático en GitHub Pages (`git commit` + `git push`):**
   - Siempre que se realicen cambios en este repositorio (`index.html`, `macropad.ino`, etc.), hacer `git commit` y `git push origin main` automáticamente al finalizar para mantener actualizada la web desplegada en GitHub Pages.
