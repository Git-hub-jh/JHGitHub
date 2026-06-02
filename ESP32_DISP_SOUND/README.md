# ESP32_DISP_SOUND

ESP32-S3 project for a 240x240 ST7789 IPS LCD and an I2S speaker.

## Features

- Shows Tainan weather from Taiwan CWA data.
- Draws simple white weather icons on the LCD.
- Plays a startup sound through an I2S speaker.

## LCD wiring

- SCL: GPIO21
- SDA / DIN: GPIO47
- RES: GPIO45
- DC: GPIO40
- BLK: GPIO42

## I2S speaker wiring

- DIN: GPIO7
- BCLK: GPIO15
- LRC: GPIO16

## Wi-Fi

The sketch uses the configured local Wi-Fi credentials in the `.ino` file.
