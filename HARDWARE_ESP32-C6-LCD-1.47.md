# Waveshare ESP32-C6-LCD-1.47

Board notes collected on 2026-10-01. The manufacturer's model name is
**ESP32-C6-LCD-1.47** 

## Board overview

Waveshare describes this as a compact ESP32-C6 development board with a
1.47-inch, 172 x 320 color display. Its product listing advertises a 160 MHz
single-core processor, Wi-Fi 6, Bluetooth, and a colorful RGB LED. Waveshare's
demo documentation also demonstrates the board's TF (microSD) card slot.

The display's brightness should be kept at 50% or below during normal use.
Waveshare warns that extended full-brightness operation can heat the display
and cause image artifacts.

## ESP32-C6 silicon

The board uses the ESP32-C6 family. According to Espressif's chip
documentation, relevant capabilities include:

- High-performance 32-bit RISC-V processor, up to 160 MHz; the SoC also has a
  low-power RISC-V processor.
- 2.4 GHz Wi-Fi 6 (802.11ax), with 802.11b/g/n compatibility.
- Bluetooth Low Energy 5.3.
- IEEE 802.15.4 radio for Thread and Zigbee.
- On-chip ROM, SRAM, security peripherals, USB Serial/JTAG controller, and
  common interfaces including SPI, I2C, UART, and ADC.

These are **SoC** capabilities, not a guarantee that every pin or peripheral
is exposed on this particular board. The Waveshare Arduino demo identifies
the LCD controller as ST7789 and provides the following board wiring:

| Function | ESP32-C6 GPIO |
| --- | ---: |
| LCD SPI MISO | 5 |
| LCD SPI MOSI | 6 |
| LCD SPI SCLK | 7 |
| LCD chip select | 14 |
| LCD data/command | 15 |
| LCD reset | 21 |
| LCD backlight PWM | 22 |
| Onboard RGB LED (addressable) | 8 |

The fitted flash capacity is not confirmed by the sources reviewed here.

## Firmware and documentation

- [Waveshare product page](https://www.waveshare.com/esp32-c6-lcd-1.47.htm)
- [Waveshare board documentation](https://docs.waveshare.com/ESP32-C6-LCD-1.47)
- [Waveshare Arduino setup and examples](https://docs.waveshare.com/ESP32-C6-LCD-1.47/Arduino)
  - Waveshare specifies **esp32 by Espressif Systems 3.0.0 or later** for this
    board.
  - The board example bundle is
    [ESP32-C6-LCD-1.47-Demo.zip](https://files.waveshare.com/wiki/ESP32-C6-LCD-1.47/ESP32-C6-LCD-1.47-Demo.zip).
- [Waveshare ESP-IDF setup](https://docs.waveshare.com/ESP32-C6-LCD-1.47/ESP-IDF)
- [Espressif ESP32-C6 datasheet](https://documentation.espressif.com/esp32-c6_datasheet_en.html)
- [Espressif ESP32-C6 technical reference manual (PDF)](https://documentation.espressif.com/esp32-c6_technical_reference_manual_en.pdf)
- [ESP-IDF ESP32-C6 getting-started guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/get-started/index.html)
- [Espressif Arduino-ESP32 core](https://github.com/espressif/arduino-esp32)

## This workspace and connected board

At the time of discovery, macOS exposed `/dev/cu.usbmodem1101`, consistent
with a USB serial device being connected. The model identification in these
notes comes from the model supplied by the user and manufacturer references;
a chip-identification handshake did not complete.

The default relay build remains configured for the **ESP32-C3**:

- `platformio.ini` selects `esp32-c3-devkitm-1`.
- `scripts/build_upload.sh` uses the Arduino target `esp32:esp32:esp32c3`.

The separate `esp32-c6-lcd-147` PlatformIO environment builds the relay for
the C6 target, initializes its ST7789 display, shows Wi-Fi and BLE status, and
uses the onboard RGB LED as a low-brightness status indicator. Build and upload
using the C6-specific command in the project README; the default upload still
targets the C3.
