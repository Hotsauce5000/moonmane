// Stablemate for the Waveshare ESP32-S3-Touch-AMOLED-1.8 (V2: CO5300 display, CST816/CST820 touch).
// A horse-themed virtual pet on a 32x20 dot "LCD", paced like the 1996 Tamagotchi.
//
// Controls
//   Touch  : the A / B / C buttons drawn under the screen. Hold C for 2 s to mute or unmute.
//   BOOT   : short press = A, hold = C
//   PWR    : short press = B, hold 3 s = screen off, hold 5 s = save and power off (press PWR to turn on)
// The screen turns off after SCREEN_TIMEOUT_MS. Any touch or button wakes it; a call from your
// horse wakes it too.
//
// Needs: Arduino-ESP32 core 3.x, "GFX Library for Arduino" 1.6.x, Tools > PSRAM = "OPI PSRAM".
//
// All the code is in app.cpp (board) and game.cpp (game). This file stays empty on purpose:
// the Arduino IDE adds generated declarations to .ino files, which breaks custom types.
