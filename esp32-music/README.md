# MoonmaneMusic for ESP32-S3-Touch-AMOLED-1.8

This version of Moonmane adds an MP3/WAV player. Songs come from the micro SD card and play through the board's speaker. The plain version in [`../esp32`](../esp32) is unchanged.

Both versions use the same save, so your horse carries over when you switch between them.

## Flashing

Use the same settings as the plain version:

1. Install the esp32 core 3.x and **GFX Library for Arduino** 1.6.x.
2. Open `MoonmaneMusic/MoonmaneMusic.ino`.
3. In the **Tools** menu:
   - Board: **Waveshare ESP32-S3-Touch-AMOLED-1.8**, or **ESP32S3 Dev Module**
   - USB CDC On Boot: **Enabled**
   - Flash Size: **16MB**
   - PSRAM: **OPI PSRAM**
   - Partition Scheme: anything with at least 2 MB for the app, for example **16M Flash (3MB APP/9.9MB FATFS)**
4. Upload.

## Putting music on the card

1. Format a micro SD card as **FAT32**.
2. Make a folder named **Music** at the top of the card.
3. Copy **MP3** or **WAV** files into it. One level of subfolders works, for example `Music/Album/Song.mp3`. Up to 500 songs are listed, sorted by name.

Supported formats:
- **MP3:** any bitrate, mono or stereo, sample rates from 8 to 48 kHz.
- **WAV:** 16-bit or 8-bit PCM.

Tags and cover art in MP3s are skipped, and the player shows the file name.

## Using the player

Tap the **horseshoe** in the top-right corner to open the player. Tap it again to go back to your horse.

- **Previous / play-pause / next:** the three round buttons
- **Volume:** the − and + buttons, in steps of 5. The volume is remembered.
- **SONGS, or tap the song title:** a list of songs, 7 per page. Use **UP** and **DOWN** to page, and tap a song to play it.

Hardware buttons in the player:
- **BOOT:** next song (next page in the list)
- **PWR:** play or pause
- **Hold BOOT:** back

Holding PWR still turns the screen off at 3 seconds and powers off at 5 seconds.

When a song ends, the next one starts, and the list loops. The last song you played is remembered, so **play** picks up there after a restart.

Your horse keeps living while music plays. Its beeps play over the song, and the music dips while they sound. When it needs you, the top bar shows **HORSE NEEDS YOU** with a flashing call icon. Tap the horseshoe to go back and take care of it. Holding C to mute only mutes the beeps, not the music.

Music keeps playing with the screen off. The board stays awake while music plays, so expect the battery to run down much faster than with the game alone.

## Not supported

Bluetooth headphones and speakers won't work. The ESP32-S3 only has Bluetooth Low Energy, and Bluetooth audio (A2DP) needs Bluetooth Classic.

## Files

- `MoonmaneMusic/app.cpp`: board setup, the game screen, the player and song list screens, buttons and touch.
- `MoonmaneMusic/audio.cpp`: the speaker, SD card, song list and beep mixing.
- `MoonmaneMusic/decoder.cpp`: MP3 and WAV decoding.
- `MoonmaneMusic/minimp3.h`: the MP3 decoder, [minimp3](https://github.com/lieff/minimp3) (public domain, CC0).
- `MoonmaneMusic/game.cpp`, `game.h`, `sprites.h`: the game, the same as the plain version.
- `MoonmaneMusic/es8311.*`: Espressif's ES8311 codec driver (Apache-2.0).
- `tools/decoder_test.cpp`: decodes MP3 and WAV files on a desktop.
- `tools/host_test.cpp`: runs whole horse lifetimes on a desktop.
