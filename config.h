#pragma once

// ---------- LCD1602 / PCF8574T ----------
// Generic ESP32-C3 Dev Module defaults. Change these when another board uses
// different I2C pins.
static constexpr int I2C_SDA_PIN = 8;
static constexpr int I2C_SCL_PIN = 9;
// PCF8574T is run above its original 100 kHz standard-mode rating to shorten
// CGRAM animation writes. If a particular backpack is unstable, use 100000.
static constexpr uint32_t I2C_CLOCK_HZ = 250000;

static constexpr uint8_t LCD_COLUMNS = 16;
static constexpr uint8_t LCD_ROWS = 2;
static constexpr uint8_t LCD_PCF8574_ADDRESS = 0x27;
static constexpr uint8_t LCD_PCF8574_RS_MASK = 0x01;  // P0
static constexpr uint8_t LCD_PCF8574_EN_MASK = 0x04;  // P2
static constexpr uint8_t LCD_PCF8574_BL_MASK = 0x08;  // P3, active high
// P1 is RW and is always kept low. P4..P7 carry D4..D7.

// Set to 0 to hide the lower-right two-digit seconds display.
#define SHOW_SECONDS 1
#if SHOW_SECONDS != 0 && SHOW_SECONDS != 1
#error "SHOW_SECONDS must be 0 or 1"
#endif

// ---------- Wi-Fi profile ----------
#define NETWORK_MODE_ENTERPRISE_EAP 1
#define NETWORK_MODE_WPA2_PERSONAL  2
#define NETWORK_MODE NETWORK_MODE_WPA2_PERSONAL

#if NETWORK_MODE != NETWORK_MODE_ENTERPRISE_EAP && NETWORK_MODE != NETWORK_MODE_WPA2_PERSONAL
#error "NETWORK_MODE must be NETWORK_MODE_ENTERPRISE_EAP or NETWORK_MODE_WPA2_PERSONAL"
#endif

// ---------- Time and NTP ----------
static constexpr char TIMEZONE[] = "CST-8";
static constexpr char NTP_PRIMARY[] = "ntp.aliyun.com";
static constexpr char NTP_SECONDARY[] = "time.cloudflare.com";
static constexpr char NTP_TERTIARY[] = "cn.pool.ntp.org";

static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 30000;
static constexpr uint32_t WIFI_RETRY_DELAY_MS = 15000;
static constexpr uint32_t WIFI_STATUS_HOLD_MS = 3000;
static constexpr uint32_t NTP_FIRST_SYNC_TIMEOUT_MS = 20000;
static constexpr uint32_t NTP_RETRY_DELAY_MS = 5UL * 60UL * 1000UL;
static constexpr uint32_t TIME_SAVE_MIN_INTERVAL_SEC = 24UL * 60UL * 60UL;
// Each 5x8 pixel row remains visible long enough to distinguish all eight
// rows on a PCF8574T LCD. Lower values can appear to skip the lower half.
static constexpr uint32_t PIXEL_SCAN_FRAME_MS = 60;
// Seconds use a shorter frame because only two CGRAM glyphs are rewritten.
static constexpr uint32_t SECOND_SCAN_FRAME_MS = PIXEL_SCAN_FRAME_MS / 2U;

// 2024-01-01 UTC. Earlier values are treated as unsynchronised.
static constexpr time_t MIN_VALID_EPOCH = 1704067200;

