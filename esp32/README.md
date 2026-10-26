# Moonmane for ESP32-S3-Touch-AMOLED-1.8

A horse-themed virtual pet for the Waveshare ESP32-S3-Touch-AMOLED-1.8 (V2 board: CO5300 display, CST820 touch). It runs on a 32×20 dot screen and is paced like the 1996 Tamagotchi.

## Flashing with the Arduino IDE

1. Install the **esp32 by Espressif Systems** core, version **3.x**. Use Boards Manager.
2. Install **GFX Library for Arduino** (by Moon On Our Nation), version **1.6.x**. Use Library Manager.
3. Open `Moonmane/Moonmane.ino`.
4. In the **Tools** menu:
   - Board: **ESP32S3 Dev Module**
   - USB CDC On Boot: **Enabled**
   - Flash Size: **16MB (128Mb)**
   - PSRAM: **OPI PSRAM** (required for the screen buffer)
   - Partition Scheme: **16M Flash (3MB APP/9.9MB FATFS)**, or any scheme with at least 1.5 MB for the app
5. Plug the board in by USB-C, pick its port, and click **Upload**.

On first boot, the board sets its clock from the time the sketch was compiled. To set it yourself, press **C** with no icon lit to show the clock, then press **B**:
- **A** adds 1 and **C** subtracts 1.
- **B** moves from hours (24-hour) to minutes, then saves.

If an upload won't start, hold **BOOT** while plugging in the USB cable, then upload again.

## Controls

| | Touch | Hardware |
|---|---|---|
| **A**: next icon / move arrow / guess left | A button | BOOT, short press |
| **B**: use icon / pick / guess right | B button | PWR, short press |
| **C**: back / clock | C button | BOOT, hold |
| Mute or unmute | Hold C for 2 seconds | |
| Screen off | | Hold PWR for 3 seconds |
| Save and power off | | Hold PWR for 5 seconds. Press PWR to turn it back on. |

While plugged into USB, the board may start right back up after powering off, because the charger keeps it powered.

The screen turns off after 30 seconds without input. Any touch or button wakes it, and the first press only wakes the screen. When your horse needs you, it beeps and turns the screen on for 15 seconds. On battery, the board sleeps between ticks while the screen is off. On USB, it stays awake so uploads keep working.

## Pacing

| Stage | How long | Sleep |
|---|---|---|
| Egg | 5 minutes | |
| Foal | 65 minutes. Loses a hunger heart every 3 min and a happy heart every 4 min. | Naps from minute 40 to 45 |
| Yearling | About 3 days. Loses a heart every 60–70 min. | 8 PM – 9 AM |
| Arabian / Pony / Mule | Lifetime about 12–16 days | 9 PM–9 AM / 8 PM–8 AM / 10 PM–9 AM |

- Your horse ages one year each morning when it wakes up.
- Ignoring a call for 15 minutes is a care mistake. Calls happen for an empty hunger or happy meter, the lights left on at bedtime, or a fake call for attention.
- Two or fewer care mistakes as a yearling grows into an Arabian, three to five grows into a Pony, and more grows into a Mule.
- Care mistakes also shorten its life.
- A horse dies after 12 hours of untreated sickness or 12 hours with an empty hunger meter.
- A foal can't die.

If the board is switched off, up to 2 hours of that time is played out when it powers back on, with no deaths or care mistakes. Any time beyond that counts as a pause. Progress is saved to flash every 10 minutes and at every milestone.

## Settings

The values at the top of `app.cpp` can be changed:
- `SCREEN_TIMEOUT_MS`
- `BRIGHTNESS`
- `VOLUME`
- `USE_LIGHT_SLEEP`
- `MAX_CATCHUP_S`
- `PWR_SLEEP_MS`, `PWR_OFF_MS`
- `BOOT_TEST`: set to 1 to flash red, green and blue at startup and log each startup step to the Serial Monitor

Stage lengths and heart timers are at the top of `game.cpp`.

## Files

- `Moonmane/Moonmane.ino`: intentionally empty; the Arduino IDE opens the project from it.
- `Moonmane/app.cpp`: board setup for display, touch, buttons, clock, sound, sleep and saving.
- `Moonmane/game.cpp`, `game.h`: the game. It has no hardware code.
- `Moonmane/sprites.h`: horses, icons and the 3×5 font.
- `Moonmane/es8311.*`: Espressif's ES8311 audio codec driver (Apache-2.0), from Waveshare's examples.
- `tools/host_test.cpp`: runs whole lifetimes on a desktop in under a second.

  ```
  g++ -std=c++17 -I../Moonmane host_test.cpp ../Moonmane/game.cpp -o host_test && ./host_test
  ```
