// MoonmaneMusic for the Waveshare ESP32-S3-Touch-AMOLED-1.8 (V2: CO5300 display, CST816/CST820 touch).
// Moonmane, the horse-themed virtual pet, plus an MP3/WAV player for songs on the SD card.
//
// Controls
//   Touch  : A / B / C buttons under the game screen. Hold C for 2 s to mute or unmute the horse's beeps.
//            The horseshoe in the top-right corner opens and closes the music player.
//   BOOT   : short press = A, hold = C          (in the player: next song, hold = back)
//   PWR    : short press = B, hold 3 s = screen off, hold 5 s = save and power off (press PWR to turn on)
//            (in the player: play / pause)
// Music: put MP3 or WAV files in a folder named Music on a FAT32 micro SD card (one level of
// subfolders is fine). Your horse keeps living while music plays; its beeps play over the song.
//
// Needs: Arduino-ESP32 core 3.x, "GFX Library for Arduino" 1.6.x, Tools > PSRAM = "OPI PSRAM".
//
// All the code is in app.cpp (board and player screens), audio.cpp (sound and SD card),
// decoder.cpp (MP3/WAV) and game.cpp (the game). This file stays empty on purpose: the Arduino IDE
// adds generated declarations to .ino files, which breaks custom types.
