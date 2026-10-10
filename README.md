# ESP32-S3 Touch Chess

A complete chess game for the **Guition JC3248W535** (ESP32-S3, 3.5" 320×480
capacitive touchscreen). Everything runs on the board: no phone, Wi-Fi or
server needed.

## Features
- Full rules: castling, en passant, promotion, check, checkmate, stalemate,
  50-move rule, threefold repetition, insufficient material
- **1 player** vs. the built-in engine (Easy / Medium / Hard), play as White or Black
- **2 players** pass-and-play with optional board rotation
- **Chess clock** for 2-player games (3 / 5 / 10 / 15 min per player)
- Legal-move highlights, last-move and check indicators, undo, board flip
- Engine: alpha-beta search with quiescence and piece-square evaluation,
  running in its own FreeRTOS task so the UI stays responsive

## Hardware
Guition JC3248W535 (ESP32-S3 N16R8, AXS15231B QSPI display + touch).

## Build
1. Install the latest **GFX Library for Arduino** (Arduino_GFX) and the ESP32 board package (arduino-esp32).
2. Arduino IDE settings:
   - Board: **ESP32S3 Dev Module**
   - Flash Size: **16MB**
   - PSRAM: **OPI PSRAM** (required for the screen buffer)
   - USB CDC On Boot: Enabled
3. Open `chess_jc3248w535.ino` (keep `chess_types.h` in the same folder) and upload.

## Touch calibration
If taps land in the wrong place, set `TOUCH_DEBUG` to 1, watch the Serial
Monitor, and adjust `TOUCH_SWAP_XY`, `TOUCH_INV_X` and `TOUCH_INV_Y` at the
top of the sketch.

## Ideas / TODO
- Piece bitmaps instead of letters
- Clock increment, resign and draw buttons
- Opening book, transposition table

## License
MIT
