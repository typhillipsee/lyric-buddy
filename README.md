# Lyric Buddy
### Portable Spotify Lyrics Display

Lyric Buddy is a battery-powered ESP32-S3 device that displays synchronized lyrics for songs playing on Spotify. I developed this project to gain hands-on experience with embedded programming, electronics, and custom PCB design.

## Features

- Connects to Wi-Fi and retrieves Spotify playback information.
- Displays lyrics on a 3.5-inch, 480 × 320 SPI TFT screen.
- Supports word-level karaoke highlighting when word timing is available.
- Falls back to line-level synchronized lyrics when supported by the lyrics provider.
- Uses background FreeRTOS tasks to reduce display interruptions during network requests.
- Operates on battery power for portable use.

Lyric availability and timing quality depend on external providers. Lyric Buddy displays lyrics for Spotify playback; it does not stream audio.

## Hardware and Software

| Component | Technology |
| --- | --- |
| Microcontroller | ESP32-S3 |
| Display | 3.5-inch ST7796 SPI TFT |
| Power | Rechargeable battery and voltage boost circuitry |
| Firmware | C++ using the Arduino framework |
| Background processing | FreeRTOS |
| Connectivity | Wi-Fi and HTTPS |
| Playback information | Spotify Web API |
| Lyrics providers | SyncLRC and LRCLIB |
| Graphics | TFT_eSPI |
| JSON parsing | ArduinoJson |
| PCB design | KiCad |

## How It Works

1. The ESP32-S3 connects to Wi-Fi.
2. The firmware uses a refresh token to obtain a Spotify access token.
3. A background task retrieves the current track and playback position.
4. The device requests timed lyrics from external providers.
5. The display updates lyric highlighting based on playback progress.

## Project Status

The prototype supports synchronized lyrics, Spotify integration, and battery-powered operation.

A custom PCB has been designed and ordered. PCB assembly, enclosure development, and firmware refinements are in progress.

The current design focuses on lyrics. Album artwork and physical playback buttons have been dropped from the design. The uploaded firmware may still contain legacy code for those features while cleanup is underway.

## Firmware Setup

1. Open `firmware/DAONE_buttons/DAONE_buttons.ino` in Arduino IDE.
2. Install ESP32 board support.
3. Install the libraries referenced by the sketch, including ArduinoJson and TFT_eSPI. The current source also references TJpg_Decoder for legacy artwork code.
4. Configure TFT_eSPI for the ST7796 display and the actual wiring.
5. Copy `secrets.example.h` to `secrets.h` in the sketch folder.
6. Enter your Wi-Fi and Spotify credentials in `secrets.h`.
7. Select the appropriate ESP32-S3 board settings, compile, and upload.

The sketch currently retains its development filename, `DAONE_buttons.ino`.

A Spotify developer application and a refresh token with the required playback-reading permissions are needed.

**Never upload `secrets.h`.** The repository's `.gitignore` excludes it from normal Git tracking, but GitHub website uploads must be checked manually.

A complete wiring guide and display configuration will be added as hardware documentation is completed.

## Engineering Work

- Integrated an ESP32-S3, SPI TFT display, and battery power hardware.
- Developed firmware combining HTTPS requests, JSON parsing, and synchronized graphics.
- Implemented background network polling with FreeRTOS.
- Reduced display flicker through selective redraws and iterative debugging.
- Prototyped and tested hardware using breadboarding, soldering, serial diagnostics, and multimeter measurements.
- Designed a custom PCB in KiCad to move the project toward an enclosed portable device.

## Planned Improvements

- Assemble and validate the custom PCB.
- Design and build the enclosure.
- Improve lyric timing, text rendering, and replay behavior.
- Complete cleanup of unused button and artwork code.
- Add schematics, PCB files, build photos, and a demonstration video.

## License

This repository uses the MIT License. Third-party services, lyrics, and other content remain subject to their respective terms and rights.
