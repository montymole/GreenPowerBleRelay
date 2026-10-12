#include "status_display.h"
#include "connection_state.h"

#if defined(STATUS_DISPLAY_ENABLED)

#include <Adafruit_NeoPixel.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>

namespace {

constexpr int LCD_MISO_PIN = 5;
constexpr int LCD_MOSI_PIN = 6;
constexpr int LCD_SCLK_PIN = 7;
constexpr int LCD_CS_PIN = 14;
constexpr int LCD_DC_PIN = 15;
constexpr int LCD_RESET_PIN = 21;
constexpr int LCD_BACKLIGHT_PIN = 22;
constexpr int RGB_LED_PIN = 8;
constexpr uint8_t STATUS_LED_BRIGHTNESS = 16;
constexpr uint32_t LCD_BACKLIGHT_DUTY = 358;

Adafruit_ST7789 display(&SPI, LCD_CS_PIN, LCD_DC_PIN, LCD_RESET_PIN);
Adafruit_NeoPixel statusLed(1, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);
// Reuse a small band buffer instead of allocating a full-screen framebuffer.
// Every update replaces the entire band, including its background and margins.
GFXcanvas16 textBand(172, 80);
struct TextCache {
  String value;
  uint16_t color = 0;
  bool valid = false;
};
TextCache previousWifiStatus;
TextCache previousIpAddress;
TextCache previousBleStatus;
TextCache previousOverallStatus;
TextCache previousSsid;

// The 156-pixel content width fits 13 characters at text size 2.
void drawText(int16_t y, const String &value, uint16_t color,
              TextCache &previous, uint8_t rows = 1, uint8_t size = 2) {
  if (previous.valid && value == previous.value && color == previous.color) return;
  const int16_t height = rows * 24 + 8;
  // If buffer allocation failed, retain a complete opaque redraw as a fallback.
  Adafruit_GFX &target = textBand.getBuffer() ? static_cast<Adafruit_GFX &>(textBand)
                                             : static_cast<Adafruit_GFX &>(display);
  const int16_t originY = textBand.getBuffer() ? 0 : y;
  target.fillRect(0, originY, 172, height, ST77XX_BLACK);
  target.setTextWrap(false);
  target.setTextSize(size);
  target.setTextColor(color, ST77XX_BLACK);
  const uint8_t columns = 156 / (6 * size);
  for (uint8_t row = 0; row < rows; ++row) {
    target.setCursor(8, originY + row * 24);
    target.print(value.substring(row * columns, (row + 1) * columns));
  }
  if (textBand.getBuffer()) {
    display.drawRGBBitmap(0, y, textBand.getBuffer(), 172, height);
  }
  previous.value = value;
  previous.color = color;
  previous.valid = true;
}

}  // namespace

void statusDisplayInit() {
  statusLed.begin();
  statusLed.setBrightness(STATUS_LED_BRIGHTNESS);
  statusLed.clear();
  statusLed.show();

  if (!ledcAttach(LCD_BACKLIGHT_PIN, 1000, 10)) {
    Serial.println("[DISPLAY] Unable to configure backlight PWM");
  } else {
    ledcWrite(LCD_BACKLIGHT_PIN, LCD_BACKLIGHT_DUTY);
  }

  SPI.begin(LCD_SCLK_PIN, LCD_MISO_PIN, LCD_MOSI_PIN, LCD_CS_PIN);
  display.init(172, 320);
  display.setRotation(0);
  display.fillScreen(ST77XX_BLACK);
  display.setTextWrap(false);
  previousWifiStatus.valid = previousIpAddress.valid = previousBleStatus.valid = false;
  previousOverallStatus.valid = previousSsid.valid = false;

}

void statusDisplayUpdate(bool wifiConnected, bool wifiConnecting,
                         const String &ssid, const String &ipAddress, bool bleConnected,
                         bool bleReady, bool bleConnecting, bool active, bool hasError) {
  const auto state = relayStatus(wifiConnected, wifiConnecting, bleConnected, bleReady,
                                 bleConnecting, active, hasError);
  const bool busy = state == RelayStatus::Working;
  const bool ready = state == RelayStatus::Ready;
  const String overall = busy ? "Busy" : ready ? "Ready" : "Error";
  const uint16_t overallColor = busy ? ST77XX_CYAN : ready ? ST77XX_GREEN : ST77XX_RED;
  const String wifiStatus = wifiConnected ? "WiFi OK" :
                            wifiConnecting ? "WiFi ..." : "WiFi retry";
  const String bleStatus = bleConnecting ? "BLE ..." :
                           bleConnected && bleReady ? "BLE OK" : "BLE retry";

  drawText(12, overall, overallColor, previousOverallStatus, 1, 3);
  drawText(64, wifiStatus,
           wifiConnected ? ST77XX_GREEN : wifiConnecting ? ST77XX_CYAN : ST77XX_RED,
           previousWifiStatus);
  // Keep the complete network name legible, using up to three large-text lines.
  drawText(96, ssid, ST77XX_WHITE, previousSsid, 3);
  drawText(192, bleStatus,
           bleConnecting ? ST77XX_CYAN : bleConnected && bleReady ? ST77XX_GREEN : ST77XX_RED,
           previousBleStatus);
  String shownIp = wifiConnected ? ipAddress : "--";
  if (shownIp.length() > 13) {
    // Wrap a long IPv4 address after a dot, never in the middle of an octet.
    const int split = shownIp.lastIndexOf('.') + 1;
    String firstLine = shownIp.substring(0, split);
    while (firstLine.length() < 13) firstLine += ' ';
    shownIp = firstLine + shownIp.substring(split);
  }
  drawText(240, shownIp, wifiConnected ? ST77XX_GREEN : ST77XX_RED,
           previousIpAddress, 2);

  // Blue remains on during work; green rises and falls to animate turquoise.
  const uint32_t phase = millis() % 1600;
  const uint8_t green = (phase < 800 ? phase : 1600 - phase) * 220 / 800;
  statusLed.setPixelColor(0, busy ? statusLed.Color(0, green, 255) :
                            ready ? statusLed.Color(0, 255, 0) : statusLed.Color(255, 0, 0));
  statusLed.show();
  display.fillCircle(154, 22, 5, busy ? display.color565(0, green, 255) : overallColor);
}

#else

void statusDisplayInit() {}

void statusDisplayUpdate(bool, bool, const String &, const String &, bool, bool, bool, bool, bool) {}

#endif
