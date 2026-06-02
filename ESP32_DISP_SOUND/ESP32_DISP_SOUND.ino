#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <driver/i2s.h>

// Common SPI wiring for small ST7789 IPS LCD modules.
// Change only these pins if your LCD board uses different labels/wiring.
static constexpr int TFT_MOSI = 47;  // LCD SDA / DIN
static constexpr int TFT_SCLK = 21;  // LCD SCL / SCK
static constexpr int TFT_CS   = -1;  // LCD CS, -1 if the module has no CS pin
static constexpr int TFT_DC   = 40;  // LCD DC / A0
static constexpr int TFT_RST  = 45;  // LCD RES / RST
static constexpr int TFT_BL   = 42;  // LCD BLK / BL / LED

static constexpr int I2S_DIN  = 7;
static constexpr int I2S_BCLK = 15;
static constexpr int I2S_LRC  = 16;
static constexpr int I2S_SAMPLE_RATE = 22050;

static constexpr int TFT_W = 240;
static constexpr int TFT_H = 240;
static constexpr int TFT_XSTART = 0;
static constexpr int TFT_YSTART = 0;

SPIClass lcdSpi(FSPI);

static const char* WIFI_SSIDS[] = {"M1_IoT"};
static const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
static constexpr size_t WIFI_COUNT = sizeof(WIFI_SSIDS) / sizeof(WIFI_SSIDS[0]);

static constexpr const char* WEATHER_URL =
  "https://www.cwa.gov.tw/Data/js/fcst/W50_Data.js";

static uint32_t lastWeatherFetch = 0;
static bool hasWeather = false;

struct WeatherData {
  char date[16];
  char condition[14];
  float tMax;
  float tMin;
  int rainPct;
  int code;
};

WeatherData weather = {"--", "Loading", 0, 0, 0, -1};

bool audioReady = false;

void initAudio() {
  i2s_config_t config = {};
  config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  config.sample_rate = I2S_SAMPLE_RATE;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 4;
  config.dma_buf_len = 256;
  config.use_apll = false;
  config.tx_desc_auto_clear = true;
  config.fixed_mclk = 0;

  i2s_pin_config_t pins = {};
  pins.bck_io_num = I2S_BCLK;
  pins.ws_io_num = I2S_LRC;
  pins.data_out_num = I2S_DIN;
  pins.data_in_num = I2S_PIN_NO_CHANGE;

  audioReady = i2s_driver_install(I2S_NUM_0, &config, 0, nullptr) == ESP_OK &&
               i2s_set_pin(I2S_NUM_0, &pins) == ESP_OK &&
               i2s_zero_dma_buffer(I2S_NUM_0) == ESP_OK;
}

void playTone(int frequency, int durationMs, int volume = 22000) {
  if (!audioReady || frequency <= 0 || durationMs <= 0) return;

  const int samples = (I2S_SAMPLE_RATE * durationMs) / 1000;
  const int halfPeriod = max(1, I2S_SAMPLE_RATE / (frequency * 2));
  int16_t frame[2];
  size_t written = 0;

  for (int i = 0; i < samples; i++) {
    int16_t sample = ((i / halfPeriod) % 2 == 0) ? volume : -volume;
    frame[0] = sample;
    frame[1] = sample;
    i2s_write(I2S_NUM_0, frame, sizeof(frame), &written, portMAX_DELAY);
  }

  frame[0] = 0;
  frame[1] = 0;
  for (int i = 0; i < I2S_SAMPLE_RATE / 50; i++) {
    i2s_write(I2S_NUM_0, frame, sizeof(frame), &written, portMAX_DELAY);
  }
}

void playStartupSound() {
  playTone(523, 140);
  delay(50);
  playTone(659, 140);
  delay(50);
  playTone(784, 220);
}

void selectLcd() {
  if (TFT_CS >= 0) digitalWrite(TFT_CS, LOW);
}

void deselectLcd() {
  if (TFT_CS >= 0) digitalWrite(TFT_CS, HIGH);
}

void spiWrite(uint8_t value) {
  for (int bit = 7; bit >= 0; bit--) {
    digitalWrite(TFT_SCLK, LOW);
    digitalWrite(TFT_MOSI, (value & (1 << bit)) ? HIGH : LOW);
    digitalWrite(TFT_SCLK, HIGH);
  }
}

void spiWrite16(uint16_t value) {
  spiWrite(value >> 8);
  spiWrite(value & 0xFF);
}

static const uint8_t font5x7[][5] = {
  {0x00,0x00,0x00,0x00,0x00}, // space
  {0x00,0x00,0x5f,0x00,0x00}, // !
  {0x00,0x07,0x00,0x07,0x00}, // "
  {0x14,0x7f,0x14,0x7f,0x14}, // #
  {0x24,0x2a,0x7f,0x2a,0x12}, // $
  {0x23,0x13,0x08,0x64,0x62}, // %
  {0x36,0x49,0x55,0x22,0x50}, // &
  {0x00,0x05,0x03,0x00,0x00}, // '
  {0x00,0x1c,0x22,0x41,0x00}, // (
  {0x00,0x41,0x22,0x1c,0x00}, // )
  {0x14,0x08,0x3e,0x08,0x14}, // *
  {0x08,0x08,0x3e,0x08,0x08}, // +
  {0x00,0x50,0x30,0x00,0x00}, // ,
  {0x08,0x08,0x08,0x08,0x08}, // -
  {0x00,0x60,0x60,0x00,0x00}, // .
  {0x20,0x10,0x08,0x04,0x02}, // /
  {0x3e,0x51,0x49,0x45,0x3e}, // 0
  {0x00,0x42,0x7f,0x40,0x00}, // 1
  {0x42,0x61,0x51,0x49,0x46}, // 2
  {0x21,0x41,0x45,0x4b,0x31}, // 3
  {0x18,0x14,0x12,0x7f,0x10}, // 4
  {0x27,0x45,0x45,0x45,0x39}, // 5
  {0x3c,0x4a,0x49,0x49,0x30}, // 6
  {0x01,0x71,0x09,0x05,0x03}, // 7
  {0x36,0x49,0x49,0x49,0x36}, // 8
  {0x06,0x49,0x49,0x29,0x1e}, // 9
  {0x00,0x36,0x36,0x00,0x00}, // :
  {0x00,0x56,0x36,0x00,0x00}, // ;
  {0x08,0x14,0x22,0x41,0x00}, // <
  {0x14,0x14,0x14,0x14,0x14}, // =
  {0x00,0x41,0x22,0x14,0x08}, // >
  {0x02,0x01,0x51,0x09,0x06}, // ?
  {0x32,0x49,0x79,0x41,0x3e}, // @
  {0x7e,0x11,0x11,0x11,0x7e}, // A
  {0x7f,0x49,0x49,0x49,0x36}, // B
  {0x3e,0x41,0x41,0x41,0x22}, // C
  {0x7f,0x41,0x41,0x22,0x1c}, // D
  {0x7f,0x49,0x49,0x49,0x41}, // E
  {0x7f,0x09,0x09,0x09,0x01}, // F
  {0x3e,0x41,0x49,0x49,0x7a}, // G
  {0x7f,0x08,0x08,0x08,0x7f}, // H
  {0x00,0x41,0x7f,0x41,0x00}, // I
  {0x20,0x40,0x41,0x3f,0x01}, // J
  {0x7f,0x08,0x14,0x22,0x41}, // K
  {0x7f,0x40,0x40,0x40,0x40}, // L
  {0x7f,0x02,0x0c,0x02,0x7f}, // M
  {0x7f,0x04,0x08,0x10,0x7f}, // N
  {0x3e,0x41,0x41,0x41,0x3e}, // O
  {0x7f,0x09,0x09,0x09,0x06}, // P
  {0x3e,0x41,0x51,0x21,0x5e}, // Q
  {0x7f,0x09,0x19,0x29,0x46}, // R
  {0x46,0x49,0x49,0x49,0x31}, // S
  {0x01,0x01,0x7f,0x01,0x01}, // T
  {0x3f,0x40,0x40,0x40,0x3f}, // U
  {0x1f,0x20,0x40,0x20,0x1f}, // V
  {0x3f,0x40,0x38,0x40,0x3f}, // W
  {0x63,0x14,0x08,0x14,0x63}, // X
  {0x07,0x08,0x70,0x08,0x07}, // Y
  {0x61,0x51,0x49,0x45,0x43}, // Z
  {0x00,0x7f,0x41,0x41,0x00}, // [
  {0x02,0x04,0x08,0x10,0x20}, // backslash
  {0x00,0x41,0x41,0x7f,0x00}, // ]
  {0x04,0x02,0x01,0x02,0x04}, // ^
  {0x40,0x40,0x40,0x40,0x40}, // _
  {0x00,0x01,0x02,0x04,0x00}, // `
  {0x20,0x54,0x54,0x54,0x78}, // a
  {0x7f,0x48,0x44,0x44,0x38}, // b
  {0x38,0x44,0x44,0x44,0x20}, // c
  {0x38,0x44,0x44,0x48,0x7f}, // d
  {0x38,0x54,0x54,0x54,0x18}, // e
  {0x08,0x7e,0x09,0x01,0x02}, // f
  {0x0c,0x52,0x52,0x52,0x3e}, // g
  {0x7f,0x08,0x04,0x04,0x78}, // h
  {0x00,0x44,0x7d,0x40,0x00}, // i
  {0x20,0x40,0x44,0x3d,0x00}, // j
  {0x7f,0x10,0x28,0x44,0x00}, // k
  {0x00,0x41,0x7f,0x40,0x00}, // l
  {0x7c,0x04,0x18,0x04,0x78}, // m
  {0x7c,0x08,0x04,0x04,0x78}, // n
  {0x38,0x44,0x44,0x44,0x38}, // o
  {0x7c,0x14,0x14,0x14,0x08}, // p
  {0x08,0x14,0x14,0x18,0x7c}, // q
  {0x7c,0x08,0x04,0x04,0x08}, // r
  {0x48,0x54,0x54,0x54,0x20}, // s
  {0x04,0x3f,0x44,0x40,0x20}, // t
  {0x3c,0x40,0x40,0x20,0x7c}, // u
  {0x1c,0x20,0x40,0x20,0x1c}, // v
  {0x3c,0x40,0x30,0x40,0x3c}, // w
  {0x44,0x28,0x10,0x28,0x44}, // x
  {0x0c,0x50,0x50,0x50,0x3c}, // y
  {0x44,0x64,0x54,0x4c,0x44}, // z
};

void lcdCommand(uint8_t cmd) {
  digitalWrite(TFT_DC, LOW);
  selectLcd();
  spiWrite(cmd);
  deselectLcd();
}

void lcdData(uint8_t data) {
  digitalWrite(TFT_DC, HIGH);
  selectLcd();
  spiWrite(data);
  deselectLcd();
}

void lcdData16(uint16_t data) {
  digitalWrite(TFT_DC, HIGH);
  selectLcd();
  spiWrite16(data);
  deselectLcd();
}

void setWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  x0 += TFT_XSTART;
  x1 += TFT_XSTART;
  y0 += TFT_YSTART;
  y1 += TFT_YSTART;

  lcdCommand(0x2A);
  lcdData(x0 >> 8); lcdData(x0);
  lcdData(x1 >> 8); lcdData(x1);
  lcdCommand(0x2B);
  lcdData(y0 >> 8); lcdData(y0);
  lcdData(y1 >> 8); lcdData(y1);
  lcdCommand(0x2C);
}

void fillRect(int x, int y, int w, int h, uint16_t color) {
  if (x < 0 || y < 0 || x >= TFT_W || y >= TFT_H) return;
  if (x + w > TFT_W) w = TFT_W - x;
  if (y + h > TFT_H) h = TFT_H - y;

  setWindow(x, y, x + w - 1, y + h - 1);
  digitalWrite(TFT_DC, HIGH);
  selectLcd();
  for (int32_t i = 0; i < (int32_t)w * h; i++) {
    spiWrite16(color);
  }
  deselectLcd();
}

void drawFastHLine(int x, int y, int w, uint16_t color) {
  fillRect(x, y, w, 1, color);
}

void drawFastVLine(int x, int y, int h, uint16_t color) {
  fillRect(x, y, 1, h, color);
}

void fillCircle(int cx, int cy, int r, uint16_t color) {
  for (int y = -r; y <= r; y++) {
    for (int x = -r; x <= r; x++) {
      if (x * x + y * y <= r * r) fillRect(cx + x, cy + y, 1, 1, color);
    }
  }
}

void drawCircle(int cx, int cy, int r, uint16_t color) {
  int x = r;
  int y = 0;
  int err = 0;
  while (x >= y) {
    fillRect(cx + x, cy + y, 1, 1, color);
    fillRect(cx + y, cy + x, 1, 1, color);
    fillRect(cx - y, cy + x, 1, 1, color);
    fillRect(cx - x, cy + y, 1, 1, color);
    fillRect(cx - x, cy - y, 1, 1, color);
    fillRect(cx - y, cy - x, 1, 1, color);
    fillRect(cx + y, cy - x, 1, 1, color);
    fillRect(cx + x, cy - y, 1, 1, color);
    y++;
    if (err <= 0) {
      err += 2 * y + 1;
    }
    if (err > 0) {
      x--;
      err -= 2 * x + 1;
    }
  }
}

void drawLine(int x0, int y0, int x1, int y1, uint16_t color) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  while (true) {
    fillRect(x0, y0, 1, 1, color);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

void drawSunIcon(int cx, int cy) {
  const uint16_t sun = 0xFFFF;
  fillCircle(cx, cy, 28, sun);
  for (int i = 0; i < 8; i++) {
    float a = i * PI / 4.0;
    int x0 = cx + cos(a) * 40;
    int y0 = cy + sin(a) * 40;
    int x1 = cx + cos(a) * 58;
    int y1 = cy + sin(a) * 58;
    drawLine(x0, y0, x1, y1, sun);
    drawLine(x0 + 1, y0, x1 + 1, y1, sun);
  }
}

void drawCloudIcon(int cx, int cy) {
  const uint16_t cloud = 0xFFFF;
  fillCircle(cx - 30, cy + 10, 24, cloud);
  fillCircle(cx, cy - 8, 32, cloud);
  fillCircle(cx + 34, cy + 8, 26, cloud);
  fillRect(cx - 54, cy + 8, 112, 34, cloud);
}

void drawUmbrellaIcon(int cx, int cy) {
  const uint16_t top = 0xFFFF;
  const uint16_t handle = 0xFFFF;
  for (int y = 0; y <= 44; y++) {
    int half = 14 + y;
    if (half > 0) fillRect(cx - half, cy + y, half * 2, 1, top);
  }
  fillRect(cx - 58, cy + 44, 116, 5, top);
  drawFastVLine(cx, cy + 48, 54, handle);
  drawFastVLine(cx + 1, cy + 48, 54, handle);
  drawCircle(cx + 14, cy + 102, 14, handle);
  fillRect(cx + 2, cy + 88, 16, 14, 0x0000);
  for (int x = -42; x <= 42; x += 28) {
    drawLine(cx + x, cy + 44, cx + x + 12, cy + 56, top);
  }
}

bool isRainCode(int code) {
  return (code >= 51 && code <= 67) || (code >= 80 && code <= 82) || (code >= 95 && code <= 99);
}

bool isClearCode(int code) {
  return code == 0 || code == 1;
}

void drawWeatherIcon(int code, int cx, int cy) {
  if (isRainCode(code)) {
    drawUmbrellaIcon(cx, cy - 40);
  } else if (isClearCode(code)) {
    drawSunIcon(cx, cy);
  } else {
    drawCloudIcon(cx, cy);
  }
}

void drawChar(int x, int y, char c, uint16_t color, uint16_t bg, uint8_t scale) {
  if (c < 32 || c > 122) c = ' ';
  const uint8_t *glyph = font5x7[c - 32];

  for (int col = 0; col < 5; col++) {
    uint8_t bits = glyph[col];
    for (int row = 0; row < 8; row++) {
      uint16_t pixelColor = (bits & (1 << row)) ? color : bg;
      fillRect(x + col * scale, y + row * scale, scale, scale, pixelColor);
    }
  }
  fillRect(x + 5 * scale, y, scale, 8 * scale, bg);
}

void drawText(int x, int y, const char *text, uint16_t color, uint16_t bg, uint8_t scale) {
  while (*text) {
    drawChar(x, y, *text++, color, bg, scale);
    x += 6 * scale;
  }
}

void lcdInit() {
  if (TFT_CS >= 0) {
    pinMode(TFT_CS, OUTPUT);
    digitalWrite(TFT_CS, HIGH);
  }
  pinMode(TFT_DC, OUTPUT);
  pinMode(TFT_RST, OUTPUT);

  if (TFT_BL >= 0) {
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
  }

  pinMode(TFT_SCLK, OUTPUT);
  pinMode(TFT_MOSI, OUTPUT);
  digitalWrite(TFT_SCLK, HIGH);
  digitalWrite(TFT_MOSI, LOW);

  digitalWrite(TFT_RST, HIGH);
  delay(50);
  digitalWrite(TFT_RST, LOW);
  delay(50);
  digitalWrite(TFT_RST, HIGH);
  delay(120);

  // ST7789 init sequence for 240x240 IPS modules.
  lcdCommand(0x01); // Software reset
  delay(150);
  lcdCommand(0x11); // Sleep out
  delay(150);
  lcdCommand(0x36); lcdData(0x00); // Memory data access control, portrait text direction
  lcdCommand(0x3A); lcdData(0x55); // 16-bit RGB565
  lcdCommand(0x21); // Inversion on, common for IPS ST7789 panels
  lcdCommand(0x29); // Display on
  delay(50);
}

const char* weatherText(int code) {
  if (code == 0) return "Clear";
  if (code == 1 || code == 2 || code == 3) return "Cloudy";
  if (code == 45 || code == 48) return "Fog";
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return "Rain";
  if (code >= 71 && code <= 77) return "Snow";
  if (code >= 95 && code <= 99) return "Storm";
  return "Weather";
}

String extractBetween(const String& text, const char* startToken, const char* endToken, int from = 0) {
  int start = text.indexOf(startToken, from);
  if (start < 0) return "";
  start += strlen(startToken);
  int end = text.indexOf(endToken, start);
  if (end < 0) return "";
  return text.substring(start, end);
}

int extractPercentNear(const String& text, int from) {
  int percent = text.indexOf("%", from);
  if (percent < 0) return 0;

  int start = percent - 1;
  while (start >= 0 && isDigit(text[start])) start--;
  return text.substring(start + 1, percent).toInt();
}

bool extractTempRange(const String& text, int from, float& tMin, float& tMax) {
  int tempAt = text.indexOf("氣溫", from);
  if (tempAt < 0) return false;

  int nums[2] = {0, 0};
  int count = 0;
  for (int i = tempAt; i < text.length() && count < 2; i++) {
    if (!isDigit(text[i])) continue;
    int start = i;
    while (i < text.length() && isDigit(text[i])) i++;
    nums[count++] = text.substring(start, i).toInt();
  }

  if (count < 2) return false;
  tMin = nums[0];
  tMax = nums[1];
  return true;
}

int cwaWeatherCode(const String& text) {
  if (text.indexOf("雷") >= 0) return 95;
  if (text.indexOf("雨") >= 0) return 80;
  if (text.indexOf("陰") >= 0 || text.indexOf("雲") >= 0) return 3;
  if (text.indexOf("晴") >= 0) return 0;
  return 3;
}

const char* cwaWeatherText(int code) {
  if (code >= 95) return "PM Storm";
  if (isRainCode(code)) return "PM Rain";
  if (isClearCode(code)) return "Sunny";
  return "Cloudy";
}

bool parseCwaTainanWeather(const String& payload) {
  int start = payload.indexOf("'67':{");
  if (start < 0) return false;

  int end = payload.indexOf("},'", start + 1);
  if (end < 0) return false;
  String block = payload.substring(start, end);

  String title = extractBetween(block, "'Title':'", "'");
  String content = extractBetween(block, "'Content':[\n    '", "'");
  String dataTime = extractBetween(block, "'DataTime':'", "'");

  int tomorrow = content.indexOf("明天白天");
  if (tomorrow < 0) tomorrow = content.indexOf("明白天");
  if (tomorrow < 0) tomorrow = content.indexOf("明天");
  if (tomorrow < 0) tomorrow = 0;

  int sentenceEnd = content.indexOf("。", tomorrow);
  String tomorrowText = sentenceEnd > tomorrow ? content.substring(tomorrow, sentenceEnd) : content;
  if (tomorrowText.length() == 0) tomorrowText = title;

  float tMin = 0;
  float tMax = 0;
  if (!extractTempRange(content, tomorrow, tMin, tMax)) return false;

  int rainPct = extractPercentNear(content, tomorrow);
  int code = cwaWeatherCode(tomorrowText);

  strncpy(weather.date, dataTime.c_str(), sizeof(weather.date) - 1);
  weather.date[sizeof(weather.date) - 1] = '\0';
  strncpy(weather.condition, cwaWeatherText(code), sizeof(weather.condition) - 1);
  weather.condition[sizeof(weather.condition) - 1] = '\0';
  weather.tMax = tMax;
  weather.tMin = tMin;
  weather.rainPct = rainPct;
  weather.code = code;
  return true;
}

void showScreen(const char* line1, const char* line2, const char* line3, const char* line4) {
  fillRect(0, 0, TFT_W, 210, 0x0000);
  drawText(12, 20, line1, 0xFFFF, 0x0000, 2);
  drawText(12, 64, line2, 0xFFE0, 0x0000, 2);
  drawText(12, 108, line3, 0x07FF, 0x0000, 2);
  drawText(12, 152, line4, 0xF81F, 0x0000, 2);
}

void drawWeather() {
  char tempLine[18];
  char rainLine[18];

  snprintf(tempLine, sizeof(tempLine), "%.0f-%.0f C", weather.tMin, weather.tMax);
  snprintf(rainLine, sizeof(rainLine), "Rain %d%%", weather.rainPct);

  fillRect(0, 0, TFT_W, TFT_H, 0x0000);
  drawText(12, 12, "Tainan", 0xFFFF, 0x0000, 3);
  drawText(12, 50, weather.condition, 0xFFE0, 0x0000, 2);
  drawWeatherIcon(weather.code, 120, 128);
  drawText(12, 178, tempLine, 0x07FF, 0x0000, 2);
  drawText(12, 208, rainLine, 0xF81F, 0x0000, 2);
}

bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(300);

  for (size_t i = 0; i < WIFI_COUNT; i++) {
    char line[18];
    snprintf(line, sizeof(line), "WiFi %s", WIFI_SSIDS[i]);
    showScreen("Connecting", line, "", "");

    WiFi.begin(WIFI_SSIDS[i], WIFI_PASSWORD);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 16000) {
      delay(300);
    }

    if (WiFi.status() == WL_CONNECTED) {
      showScreen("WiFi OK", WiFi.localIP().toString().c_str(), "Fetching", "weather");
      return true;
    }
  }

  showScreen("WiFi failed", "Check SSID", "or password", "");
  return false;
}

bool fetchWeather() {
  if (WiFi.status() != WL_CONNECTED && !connectWiFi()) return false;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  showScreen("Fetching", "CWA Tainan", "forecast", "");

  http.setTimeout(12000);
  if (!http.begin(client, WEATHER_URL)) {
    showScreen("HTTP begin", "failed", "", "");
    return false;
  }

  int status = http.GET();
  if (status != HTTP_CODE_OK) {
    char codeLine[18];
    snprintf(codeLine, sizeof(codeLine), "HTTP %d", status);
    showScreen("Weather", "fetch failed", codeLine, "");
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  if (!parseCwaTainanWeather(payload)) {
    showScreen("CWA parse", "failed", "", "");
    return false;
  }

  hasWeather = true;
  lastWeatherFetch = millis();
  drawWeather();
  return true;
}

void setup() {
  lcdInit();
  initAudio();
  fillRect(0, 0, TFT_W, TFT_H, 0x0000);
  showScreen("Weather LCD", "Booting", "", "");
  playStartupSound();
  connectWiFi();
  fetchWeather();
}

void loop() {
  if (TFT_BL >= 0) digitalWrite(TFT_BL, HIGH);

  if (!hasWeather || millis() - lastWeatherFetch > 30UL * 60UL * 1000UL) {
    fetchWeather();
  }

  delay(500);
}
