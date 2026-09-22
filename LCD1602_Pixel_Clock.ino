/*
 * ESP32-C3 + PCF8574T LCD1602 network clock
 *
 * Target: Arduino-ESP32 3.3.12, board "ESP32C3 Dev Module"
 * LCD driver: built-in write-only PCF8574T driver
 *
 * Enterprise Wi-Fi security model:
 *   - WPA2-Enterprise PEAP with inner MSCHAPv2
 *   - ESP-IDF's built-in CA bundle is required
 *   - the configured EAP server certificate domain is strictly checked
 *   - no insecure certificate bypass is attempted
 */

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <stdio.h>
#include <esp_err.h>

#include "config.h"
#include "pcf8574t_lcd.h"
#include "secrets.h"

#if NETWORK_MODE == NETWORK_MODE_ENTERPRISE_EAP && __has_include(<esp_eap_client.h>)
#include <esp_eap_client.h>
#define HAS_EAP_CLIENT_SECURITY_API 1
#else
#define HAS_EAP_CLIENT_SECURITY_API 0
#endif

#if __has_include(<esp_sntp.h>)
#include <esp_sntp.h>
#define HAS_SNTP_STATUS_API 1
#else
#define HAS_SNTP_STATUS_API 0
#endif

#if defined(CONFIG_IDF_TARGET_ESP32C3) && defined(ARDUINO_USB_MODE) && ARDUINO_USB_MODE \
    && defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
// ESP32-C3's USB connector is the fixed USB Serial/JTAG CDC interface.
#define USB_SERIAL_PORT HWCDCSerial
#else
// Fallback for builds that have not enabled USB CDC On Boot yet.
#define USB_SERIAL_PORT Serial
#endif

static Pcf8574tLcd lcd(LCD_PCF8574_ADDRESS,
                       LCD_PCF8574_RS_MASK,
                       LCD_PCF8574_EN_MASK,
                       LCD_PCF8574_BL_MASK);
static Preferences preferences;

static bool lcdReady = false;
static bool ntpConfigured = false;
static bool ntpSynchronized = false;
static bool displayTimeSynchronized = false;
static bool ntpFailureShown = false;
static uint32_t connectStartedAt = 0;
static uint32_t nextWifiAttemptAt = 0;
static uint32_t ntpStartedAt = 0;
static uint32_t nextNtpAttemptAt = 0;
static uint32_t lastClockDrawAt = 0;
static uint32_t clockDisplayAfter = 0;
static uint64_t lastSavedEpoch = 0;
static bool lastColonVisible = true;
static bool clockDigitsInitialized = false;
static bool bigScanActive = false;
static bool bigScanRestorePending = false;
static uint8_t bigScanRow = 0;
static uint8_t bigScanTarget[4] = {};
static uint32_t bigScanFrameAt = 0;
static bool splitFlipActive = false;
static uint8_t splitFlipPhase = 0;
static uint8_t splitFlipMask = 0;
static uint8_t splitFlipPrevious[4] = {};
static uint8_t splitFlipTarget[4] = {};
static uint32_t splitFlipPhaseAt = 0;
#if SHOW_SECONDS
static bool secondsInitialized = false;
static bool secondsScanActive = false;
static uint8_t secondsPrevious[2] = {};
static uint8_t secondsTarget[2] = {};
static uint8_t secondsScanMask = 0;
static uint8_t secondsScanRow = 0;
static uint32_t secondsScanFrameAt = 0;
#endif

enum class NetworkState : uint8_t {
  PasswordMissing,
  SecurityError,
  Connecting,
  Connected,
  Disconnected
};

static NetworkState networkState = NetworkState::Disconnected;

static void onWiFiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  if (event != ARDUINO_EVENT_WIFI_STA_DISCONNECTED) return;

  const wifi_err_reason_t reason =
      static_cast<wifi_err_reason_t>(info.wifi_sta_disconnected.reason);
  USB_SERIAL_PORT.printf("WiFi disconnect reason=%u (%s)\n",
                         static_cast<unsigned>(info.wifi_sta_disconnected.reason),
                         WiFi.STA.disconnectReasonName(reason));
}

// ---------- LCD content ----------
static constexpr uint8_t BIG_TOKEN_COLON = 0;
static constexpr uint8_t BIG_TOKEN_DIGIT_BASE = 1;
static constexpr uint8_t BIG_TOKEN_BLANK = 0x8E;
static constexpr uint8_t LCD_SOLID_BLOCK = 0xFF;
static constexpr uint8_t BIG_GLYPH_COUNT = 6;
static constexpr uint8_t BIG_SCAN_SOLID_GLYPH = 6;
static constexpr uint8_t BIG_SCAN_LINE_GLYPH = 7;
static constexpr uint8_t SECOND_GLYPH_TENS = 6;
static constexpr uint8_t SECOND_GLYPH_ONES = 7;

// CGRAM 0..5 form the shared 3-column x 2-row large-number font.
static constexpr uint8_t BIG_GLYPHS[BIG_GLYPH_COUNT][8] = {
  { B11111, B11111, B11111, B00000, B00000, B00000, B00000, B00000 }, // 0 upper bar
  { B00000, B00000, B00000, B00000, B00000, B11111, B11111, B11111 }, // 1 lower bar
  { B11111, B11111, B11111, B00000, B00000, B00000, B11111, B11111 }, // 2 upper/lower
  { B11100, B11100, B11100, B11100, B11100, B11100, B11100, B11100 }, // 3 left bar
  { B00000, B00000, B00000, B00000, B00000, B11100, B11100, B11100 }, // 4 lower-left
  { B11100, B11100, B11100, B00000, B00000, B00000, B11100, B11100 }, // 5 upper/lower-left
};

// Kept separately so SHOW_SECONDS=0 can restore the original custom colon.
static constexpr uint8_t BIG_COLON_GLYPH[8] = {
  B00000, B00000, B01110, B01110, B01110, B00000, B00000, B00000
};

#if SHOW_SECONDS
// Compact square sci-fi glyphs. The last row is intentionally blank so the
// scan line can travel through a clean 5x8 cell.
static constexpr uint8_t SECOND_DIGITS[10][8] = {
  {B01110, B11011, B11011, B11011, B11011, B11011, B01110, B00000}, // 0
  {B00110, B01110, B00110, B00110, B00110, B00110, B11111, B00000}, // 1
  {B11110, B00011, B00011, B01110, B11000, B11000, B11111, B00000}, // 2
  {B11110, B00011, B00011, B01110, B00011, B00011, B11110, B00000}, // 3
  {B11011, B11011, B11011, B11111, B00011, B00011, B00011, B00000}, // 4
  {B11111, B11000, B11000, B11110, B00011, B00011, B11110, B00000}, // 5
  {B01110, B11000, B11000, B11110, B11011, B11011, B01110, B00000}, // 6
  {B11111, B00011, B00011, B00110, B01100, B01100, B01100, B00000}, // 7
  {B01110, B11011, B11011, B01110, B11011, B11011, B01110, B00000}, // 8
  {B01110, B11011, B11011, B01111, B00011, B00011, B01110, B00000}  // 9
};
#endif

// Token order: ':', then three cells for every digit from 0 to 9.
static constexpr uint8_t BIG_FONT[2][31] = {
  {0x07,
   LCD_SOLID_BLOCK, 0x00, 0x03, BIG_TOKEN_BLANK, 0x03, BIG_TOKEN_BLANK,
   0x02, 0x02, 0x03, 0x02, 0x02, 0x03,
   LCD_SOLID_BLOCK, 0x01, 0x03, LCD_SOLID_BLOCK, 0x02, 0x05,
   LCD_SOLID_BLOCK, 0x02, 0x05, 0x00, 0x00, 0x03,
   LCD_SOLID_BLOCK, 0x02, 0x03, LCD_SOLID_BLOCK, 0x02, 0x03},
  {0x07,
   LCD_SOLID_BLOCK, 0x01, 0x03, BIG_TOKEN_BLANK, 0x03, BIG_TOKEN_BLANK,
   LCD_SOLID_BLOCK, 0x01, 0x04, 0x01, 0x01, 0x03,
   BIG_TOKEN_BLANK, BIG_TOKEN_BLANK, 0x03, 0x01, 0x01, 0x03,
   LCD_SOLID_BLOCK, 0x01, 0x03, BIG_TOKEN_BLANK, BIG_TOKEN_BLANK, 0x03,
   LCD_SOLID_BLOCK, 0x01, 0x03, BIG_TOKEN_BLANK, BIG_TOKEN_BLANK, 0x03}
};

static void writePadded(uint8_t row, const char *text) {
  if (!lcdReady) return;
  lcd.setCursor(0, row);
  uint8_t count = 0;
  while (text && text[count] != '\0' && count < LCD_COLUMNS) {
    lcd.write(static_cast<uint8_t>(text[count++]));
  }
  while (count++ < LCD_COLUMNS) lcd.write(' ');
}

static void showStatus(const char *line1, const char *line2, uint32_t holdMs = 0) {
  if (!lcdReady) return;
  lcd.clear();
  writePadded(0, line1);
  writePadded(1, line2);
  clockDisplayAfter = millis() + holdMs;
  // Status screens interrupt the clock. Resume from the current time instead
  // of replaying a scan that may have become stale while Wi-Fi was reconnecting.
  clockDigitsInitialized = false;
  bigScanActive = false;
  bigScanRestorePending = false;
  splitFlipActive = false;
#if SHOW_SECONDS
  secondsInitialized = false;
  secondsScanActive = false;
#endif
}

static void scanI2C() {
  USB_SERIAL_PORT.printf("I2C scan on SDA=%d SCL=%d:\n", I2C_SDA_PIN, I2C_SCL_PIN);
  uint8_t found = 0;
  for (uint8_t address = 1; address < 127; ++address) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      USB_SERIAL_PORT.printf("  found 0x%02X\n", address);
      ++found;
    }
  }
  if (found == 0) USB_SERIAL_PORT.println("  no I2C device found");
}

// ---------- Time bootstrap and persistence ----------
static int monthNumber(const char *month) {
  static const char *names[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
  };
  for (int i = 0; i < 12; ++i) {
    if (strncmp(month, names[i], 3) == 0) return i + 1;
  }
  return 1;
}

// Howard Hinnant's civil-date conversion, kept local so no timezone-dependent
// mktime() call is needed before the network time has been established.
static int64_t daysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
  const unsigned dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return static_cast<int64_t>(era) * 146097 + static_cast<int>(dayOfEra) - 719468;
}

static time_t buildEpoch() {
  char monthName[4] = {};
  int day = 1;
  int year = 2024;
  int hour = 0;
  int minute = 0;
  int second = 0;
  sscanf(__DATE__, "%3s %d %d", monthName, &day, &year);
  sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second);
  const int month = monthNumber(monthName);
  const int64_t days = daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
  return static_cast<time_t>(days * 86400LL + hour * 3600 + minute * 60 + second);
}

static void setEpoch(time_t epoch) {
  struct timeval now = {};
  now.tv_sec = epoch;
  settimeofday(&now, nullptr);
}

#if !HAS_SNTP_STATUS_API
static bool currentTimeIsValid() {
  return time(nullptr) >= MIN_VALID_EPOCH;
}
#endif

static void restoreLastKnownTime() {
  uint64_t savedEpoch = 0;
  if (preferences.getBytesLength("epoch") == sizeof(savedEpoch)) {
    preferences.getBytes("epoch", &savedEpoch, sizeof(savedEpoch));
  }
  lastSavedEpoch = savedEpoch;

  const time_t compiled = buildEpoch();
  const time_t chosen = (savedEpoch >= static_cast<uint64_t>(MIN_VALID_EPOCH))
                          ? static_cast<time_t>(savedEpoch)
                          : compiled;
  setEpoch(chosen);
  setenv("TZ", TIMEZONE, 1);
  tzset();
  USB_SERIAL_PORT.printf("Initial clock source: %s\n", savedEpoch >= static_cast<uint64_t>(MIN_VALID_EPOCH)
                                               ? "saved NTP time" : "firmware build time");
}

static void saveCurrentTime() {
  const time_t now = time(nullptr);
  if (now < MIN_VALID_EPOCH) return;
  const uint64_t epoch = static_cast<uint64_t>(now);
  if (lastSavedEpoch >= static_cast<uint64_t>(MIN_VALID_EPOCH)
      && epoch >= lastSavedEpoch
      && epoch - lastSavedEpoch < TIME_SAVE_MIN_INTERVAL_SEC) {
    USB_SERIAL_PORT.println("NTP time not written: saved value is less than 24 hours old");
    return;
  }
  preferences.putBytes("epoch", &epoch, sizeof(epoch));
  lastSavedEpoch = epoch;
  USB_SERIAL_PORT.println("NTP time saved to flash");
}

// ---------- Wi-Fi and NTP ----------
#if NETWORK_MODE == NETWORK_MODE_ENTERPRISE_EAP
static bool configureEnterpriseSecurity() {
#if HAS_EAP_CLIENT_SECURITY_API
  const esp_err_t bundleResult = esp_eap_client_use_default_cert_bundle(true);
  if (bundleResult != ESP_OK) {
    USB_SERIAL_PORT.printf("Default EAP CA bundle unavailable: %s\n", esp_err_to_name(bundleResult));
    return false;
  }

  const esp_err_t domainResult = esp_eap_client_set_domain_name(EAP_SERVER_DOMAIN);
  if (domainResult != ESP_OK) {
    USB_SERIAL_PORT.printf("EAP certificate domain setup failed: %s\n", esp_err_to_name(domainResult));
    return false;
  }

  // ESP-IDF's enterprise client defaults this flag to disabled time checking
  // on some builds. Re-enable it so the CA validity window is checked. The
  // build-time / saved-time bootstrap above supplies a sane initial clock.
  const esp_err_t timeCheckResult = esp_eap_client_set_disable_time_check(false);
  if (timeCheckResult != ESP_OK) {
    USB_SERIAL_PORT.printf("Unable to enable EAP certificate time checks: %s\n", esp_err_to_name(timeCheckResult));
    return false;
  }

  // WPA2_AUTH_PEAP in WiFi.begin() selects PEAP for this connection. The
  // public esp_eap_client API in Arduino-ESP32 3.x does not expose a generic
  // esp_eap_client_set_eap_methods() function.
  USB_SERIAL_PORT.printf("EAP PEAP + CA bundle enabled; server domain=%s\n", EAP_SERVER_DOMAIN);
  return true;
#else
  USB_SERIAL_PORT.println("This Arduino-ESP32 core has no esp_eap_client security API");
  return false;
#endif
}
#endif

static void beginWifiConnection() {
#if NETWORK_MODE == NETWORK_MODE_ENTERPRISE_EAP
  if (strlen(EAP_PASSWORD) == 0) {
    networkState = NetworkState::PasswordMissing;
    showStatus("SET EAP PASS", "edit secrets.h");
    USB_SERIAL_PORT.println("EAP password is empty; edit secrets.h before connecting");
    return;
  }
#else
  if (strlen(PERSONAL_WIFI_PASSWORD) == 0) {
    networkState = NetworkState::PasswordMissing;
    showStatus("SET WIFI PASS", "edit secrets.h");
    USB_SERIAL_PORT.println("WPA2-Personal password is empty; edit secrets.h before connecting");
    return;
  }
#endif

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  // The compatibility WiFi object uses (wifioff, eraseap, timeout).
  // Do not erase the saved AP configuration during a normal retry.
  WiFi.disconnect(false, false, 1000);
  delay(100);

#if NETWORK_MODE == NETWORK_MODE_ENTERPRISE_EAP
  // The overload selects PEAP. For PEAP/MSCHAPv2 the username and password
  // parameters are the inner (phase-2) credentials.
  WiFi.begin(ENTERPRISE_WIFI_SSID, WPA2_AUTH_PEAP, EAP_IDENTITY, EAP_USERNAME, EAP_PASSWORD);
  showStatus("WiFi: EAP", "connecting...", WIFI_STATUS_HOLD_MS);
  USB_SERIAL_PORT.printf("Connecting to enterprise SSID: %s (PEAP/MSCHAPv2)\n",
                         ENTERPRISE_WIFI_SSID);
#else
  // Require WPA2 or stronger when using the normal personal-network profile.
  WiFi.setMinSecurity(WIFI_AUTH_WPA2_PSK);
  WiFi.begin(PERSONAL_WIFI_SSID, PERSONAL_WIFI_PASSWORD);
  showStatus(PERSONAL_WIFI_SSID, "connecting...", WIFI_STATUS_HOLD_MS);
  USB_SERIAL_PORT.printf("Connecting to WPA2-Personal SSID: %s\n", PERSONAL_WIFI_SSID);
#endif

  connectStartedAt = millis();
  networkState = NetworkState::Connecting;
}

static void requestNtpSync() {
  if (WiFi.status() != WL_CONNECTED) return;
  // NTP always supplies UTC. configTzTime keeps localtime_r() fixed to
  // China Standard Time instead of replacing TZ with UTC as configTime(0, 0)
  // would do.
  configTzTime(TIMEZONE, NTP_PRIMARY, NTP_SECONDARY, NTP_TERTIARY);
  ntpConfigured = true;
  ntpSynchronized = false;
  ntpFailureShown = false;
  ntpStartedAt = millis();
  showStatus("WiFi connected", "NTP syncing...", WIFI_STATUS_HOLD_MS);
  USB_SERIAL_PORT.printf("NTP sync requested; timezone=%s, primary=%s\n", TIMEZONE, NTP_PRIMARY);
}

static bool sntpHasCompleted() {
#if HAS_SNTP_STATUS_API
  return sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED;
#else
  return false;
#endif
}

// ---------- Large clock rendering ----------
static void writeBigToken(uint8_t row, uint8_t token) {
  const uint8_t code = BIG_FONT[row][token];
  if (code == BIG_TOKEN_BLANK) lcd.write(' ');
  else lcd.write(code);
}

static void writeBigDigit(uint8_t row, uint8_t digit) {
  const uint8_t token = static_cast<uint8_t>(BIG_TOKEN_DIGIT_BASE + digit * 3);
  writeBigToken(row, token + 0);
  writeBigToken(row, token + 1);
  writeBigToken(row, token + 2);
}

static constexpr uint8_t BIG_DIGIT_COLUMNS[4] = {1, 4, 8, 11};

static void writeBigDigitAt(uint8_t row, uint8_t position, uint8_t digit) {
  lcd.setCursor(BIG_DIGIT_COLUMNS[position], row);
  writeBigDigit(row, digit);
}

static void clearBigDigitAt(uint8_t row, uint8_t position) {
  lcd.setCursor(BIG_DIGIT_COLUMNS[position], row);
  lcd.write(' ');
  lcd.write(' ');
  lcd.write(' ');
}

#if SHOW_SECONDS
static void loadSecondGlyph(uint8_t slot, uint8_t digit) {
  lcd.createChar(slot, SECOND_DIGITS[digit]);
}

static void loadSecondScanGlyph(uint8_t slot, uint8_t oldDigit,
                                uint8_t newDigit, uint8_t scanRow) {
  uint8_t glyph[8] = {};
  for (uint8_t row = 0; row < 8; ++row) {
    if (row < scanRow) glyph[row] = SECOND_DIGITS[newDigit][row];
    else if (row == scanRow) glyph[row] = B11111;
    else glyph[row] = SECOND_DIGITS[oldDigit][row];
  }
  lcd.createChar(slot, glyph);
}

static void loadSecondsNormal(const uint8_t seconds[2]) {
  loadSecondGlyph(SECOND_GLYPH_TENS, seconds[0]);
  loadSecondGlyph(SECOND_GLYPH_ONES, seconds[1]);
}

static void updateSecondsScan(const uint8_t actualSeconds[2], uint32_t now) {
  if (!secondsInitialized) {
    memcpy(secondsPrevious, actualSeconds, sizeof(secondsPrevious));
    memcpy(secondsTarget, actualSeconds, sizeof(secondsTarget));
    secondsInitialized = true;
    loadSecondsNormal(actualSeconds);
    return;
  }

  if (!secondsScanActive && memcmp(actualSeconds, secondsTarget, sizeof(secondsTarget)) != 0) {
    memcpy(secondsPrevious, secondsTarget, sizeof(secondsPrevious));
    memcpy(secondsTarget, actualSeconds, sizeof(secondsTarget));
    secondsScanMask = 0;
    for (uint8_t i = 0; i < 2; ++i) {
      if (secondsPrevious[i] != secondsTarget[i]) secondsScanMask |= static_cast<uint8_t>(1U << i);
    }
    secondsScanActive = secondsScanMask != 0;
    secondsScanRow = 0;
    secondsScanFrameAt = now;
    for (uint8_t i = 0; i < 2; ++i) {
      if (secondsScanMask & (1U << i)) {
        loadSecondScanGlyph(i == 0 ? SECOND_GLYPH_TENS : SECOND_GLYPH_ONES,
                            secondsPrevious[i], secondsTarget[i], secondsScanRow);
      } else {
        loadSecondGlyph(i == 0 ? SECOND_GLYPH_TENS : SECOND_GLYPH_ONES, secondsTarget[i]);
      }
    }
  }

  if (!secondsScanActive) return;
  if (now - secondsScanFrameAt < SECOND_SCAN_FRAME_MS) return;

  if (secondsScanRow >= 7) {
    secondsScanActive = false;
    loadSecondsNormal(secondsTarget);
    return;
  }

  ++secondsScanRow;
  secondsScanFrameAt = now;
  for (uint8_t i = 0; i < 2; ++i) {
    if (secondsScanMask & (1U << i)) {
      loadSecondScanGlyph(i == 0 ? SECOND_GLYPH_TENS : SECOND_GLYPH_ONES,
                          secondsPrevious[i], secondsTarget[i], secondsScanRow);
    }
  }
}
#endif

static void loadNormalBigGlyphs() {
  for (uint8_t i = 0; i < BIG_GLYPH_COUNT; ++i) lcd.createChar(i, BIG_GLYPHS[i]);
}

static void loadNormalCgram(const uint8_t seconds[2]) {
  loadNormalBigGlyphs();
#if SHOW_SECONDS
  loadSecondsNormal(seconds);
#else
  lcd.createChar(7, BIG_COLON_GLYPH);
#endif
}

static void loadBigScanCgram(uint8_t scanRow) {
  uint8_t masked[8] = {};
  uint8_t solid[8] = {};
  uint8_t line[8] = {};
  for (uint8_t glyph = 0; glyph < BIG_GLYPH_COUNT; ++glyph) {
    for (uint8_t row = 0; row < 8; ++row) {
      masked[row] = row < scanRow ? BIG_GLYPHS[glyph][row]
                                  : (row == scanRow ? B11111 : B00000);
    }
    lcd.createChar(glyph, masked);
  }
  for (uint8_t row = 0; row < 8; ++row) {
    // ROM 0xFF cannot be changed, so slot 6 temporarily provides a solid
    // block whose pixels are revealed through the current scan row.
    solid[row] = row <= scanRow ? B11111 : B00000;
    // Blank target cells display only the travelling line in slot 7.
    line[row] = row == scanRow ? B11111 : B00000;
  }
  lcd.createChar(BIG_SCAN_SOLID_GLYPH, solid);
  lcd.createChar(BIG_SCAN_LINE_GLYPH, line);
}

static void updateBigScan(const uint8_t actualDigits[4], const uint8_t seconds[2], uint32_t now) {
  if (!clockDigitsInitialized) {
    memcpy(bigScanTarget, actualDigits, sizeof(bigScanTarget));
    clockDigitsInitialized = true;
    bigScanRestorePending = false;
    loadNormalCgram(seconds);
    return;
  }

  if (memcmp(actualDigits, bigScanTarget, sizeof(bigScanTarget)) != 0) {
    memcpy(bigScanTarget, actualDigits, sizeof(bigScanTarget));
    bigScanActive = true;
    bigScanRestorePending = false;
    bigScanRow = 0;
    bigScanFrameAt = now;
    loadBigScanCgram(bigScanRow);
#if SHOW_SECONDS
    secondsScanActive = false;
#endif
  }

  if (!bigScanActive) return;
  if (now - bigScanFrameAt < PIXEL_SCAN_FRAME_MS) return;

  if (bigScanRow >= 7) {
    bigScanActive = false;
    // Restore only CGRAM 0..5 here. DDRAM still contains temporary slot 6 in
    // scanned solid cells and slot 7 in scanned blank cells. Loading seconds
    // into those slots now would flash their digits across HH:MM. The draw
    // below first replaces them with ROM 0xFF/spaces, then restores slots 6/7.
    loadNormalBigGlyphs();
    bigScanRestorePending = true;
#if SHOW_SECONDS
    secondsScanActive = false;
#endif
    return;
  }

  ++bigScanRow;
  bigScanFrameAt = now;
  loadBigScanCgram(bigScanRow);
}

static void writeScanCell(uint8_t code) {
  if (code == BIG_TOKEN_BLANK) lcd.write(BIG_SCAN_LINE_GLYPH);
  // Seconds are hidden during a big scan, leaving slot 6 available for the
  // animated replacement of the otherwise immutable ROM 0xFF block.
  else if (code == LCD_SOLID_BLOCK) lcd.write(BIG_SCAN_SOLID_GLYPH);
  else lcd.write(code);
}

static void writeBigDigitForScan(uint8_t row, uint8_t digit, bool scanning) {
  const uint8_t token = static_cast<uint8_t>(BIG_TOKEN_DIGIT_BASE + digit * 3);
  for (uint8_t i = 0; i < 3; ++i) {
    const uint8_t code = BIG_FONT[row][token + i];
    if (scanning) writeScanCell(code);
    else if (code == BIG_TOKEN_BLANK) lcd.write(' ');
    else lcd.write(code);
  }
}

static void writeCenterColon(bool visible, bool scanning) {
  if (scanning) {
    // Keep the scan line continuous through the colon cell.
    lcd.write(BIG_SCAN_LINE_GLYPH);
  } else if (!visible) {
    lcd.write(' ');
#if SHOW_SECONDS
  } else {
    lcd.write(':');
#else
  } else {
    lcd.write(BIG_FONT[0][BIG_TOKEN_COLON]);
#endif
  }
}

static void readDisplayTime(uint32_t nowMs, uint8_t digits[4], uint8_t seconds[2]) {
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;

  if (displayTimeSynchronized) {
    const time_t epoch = time(nullptr);
    struct tm local = {};
    localtime_r(&epoch, &local);
    hour = static_cast<uint8_t>(local.tm_hour);
    minute = static_cast<uint8_t>(local.tm_min);
    second = static_cast<uint8_t>(local.tm_sec);
  } else {
    // Before the first successful NTP sync, show uptime beginning at 00:00:00.
    // The internal build/saved epoch remains available for TLS certificate checks.
    const uint32_t elapsedSeconds = nowMs / 1000U;
    hour = static_cast<uint8_t>((elapsedSeconds / 3600U) % 24U);
    minute = static_cast<uint8_t>((elapsedSeconds / 60U) % 60U);
    second = static_cast<uint8_t>(elapsedSeconds % 60U);
  }

  digits[0] = static_cast<uint8_t>(hour / 10U);
  digits[1] = static_cast<uint8_t>(hour % 10U);
  digits[2] = static_cast<uint8_t>(minute / 10U);
  digits[3] = static_cast<uint8_t>(minute % 10U);
  seconds[0] = static_cast<uint8_t>(second / 10U);
  seconds[1] = static_cast<uint8_t>(second % 10U);
}

static void drawBigClockPixelScan(uint32_t nowMs) {
  if (!lcdReady) return;
  uint8_t actualDigits[4] = {};
  uint8_t actualSeconds[2] = {};
  readDisplayTime(nowMs, actualDigits, actualSeconds);

  updateBigScan(actualDigits, actualSeconds, nowMs);
#if SHOW_SECONDS
  if (!bigScanActive && !bigScanRestorePending) updateSecondsScan(actualSeconds, nowMs);
#endif

  lcd.setCursor(1, 0);
  for (uint8_t i = 0; i < 4; ++i) {
    writeBigDigitForScan(0, bigScanTarget[i], bigScanActive);
    if (i == 1) writeCenterColon(lastColonVisible, bigScanActive);
  }

  lcd.setCursor(1, 1);
  for (uint8_t i = 0; i < 4; ++i) {
    writeBigDigitForScan(1, bigScanTarget[i], bigScanActive);
    if (i == 1) writeCenterColon(lastColonVisible, bigScanActive);
  }

  lcd.setCursor(0, 0);
  lcd.write(' ');
  lcd.setCursor(0, 1);
  lcd.write(' ');
  lcd.setCursor(14, 0);
  lcd.write(' ');
  lcd.write(' ');

  // At this point every temporary slot-6/7 reference in the HH:MM area has
  // been replaced by ROM 0xFF, a normal space, or the colon. It is now safe
  // to turn slots 6/7 back into seconds glyphs without flashing them in HH:MM.
  if (bigScanRestorePending) {
#if SHOW_SECONDS
    memcpy(secondsPrevious, actualSeconds, sizeof(secondsPrevious));
    memcpy(secondsTarget, actualSeconds, sizeof(secondsTarget));
    secondsInitialized = true;
    secondsScanActive = false;
    secondsScanMask = 0;
    loadSecondsNormal(actualSeconds);
#else
    lcd.createChar(7, BIG_COLON_GLYPH);
#endif
    bigScanRestorePending = false;
  }

  lcd.setCursor(14, 1);
#if SHOW_SECONDS
  if (bigScanActive) {
    lcd.write(' ');
    lcd.write(' ');
  } else {
    lcd.write(SECOND_GLYPH_TENS);
    lcd.write(SECOND_GLYPH_ONES);
  }
#else
  lcd.write(' ');
  lcd.write(' ');
#endif
}

static void drawNormalClockFrame(const uint8_t digits[4], const uint8_t seconds[2]) {
  loadNormalCgram(seconds);
  for (uint8_t position = 0; position < 4; ++position) {
    writeBigDigitAt(0, position, digits[position]);
    writeBigDigitAt(1, position, digits[position]);
  }
  lcd.setCursor(0, 0);
  lcd.write(' ');
  lcd.setCursor(0, 1);
  lcd.write(' ');
  lcd.setCursor(14, 0);
  lcd.write(' ');
  lcd.write(' ');
#if SHOW_SECONDS
  memcpy(secondsPrevious, seconds, sizeof(secondsPrevious));
  memcpy(secondsTarget, seconds, sizeof(secondsTarget));
  secondsInitialized = true;
  secondsScanActive = false;
#endif
}

static void renderSplitFlipPhase() {
  const uint8_t row = splitFlipPhase < 2 ? 0 : 1;
  const bool showNewHalf = (splitFlipPhase & 1U) != 0;
  for (uint8_t position = 0; position < 4; ++position) {
    if ((splitFlipMask & (1U << position)) == 0) continue;
    if (showNewHalf) writeBigDigitAt(row, position, splitFlipTarget[position]);
    else clearBigDigitAt(row, position);
  }
}

static void drawBigClockDirectOrFlip(uint32_t nowMs) {
  uint8_t actualDigits[4] = {};
  uint8_t actualSeconds[2] = {};
  readDisplayTime(nowMs, actualDigits, actualSeconds);

  if (!clockDigitsInitialized) {
    memcpy(splitFlipTarget, actualDigits, sizeof(splitFlipTarget));
    memcpy(splitFlipPrevious, actualDigits, sizeof(splitFlipPrevious));
    drawNormalClockFrame(actualDigits, actualSeconds);
    clockDigitsInitialized = true;
  }

#if HHMM_ANIMATION_MODE == HHMM_ANIMATION_NONE
  if (memcmp(actualDigits, splitFlipTarget, sizeof(splitFlipTarget)) != 0) {
    for (uint8_t position = 0; position < 4; ++position) {
      if (actualDigits[position] == splitFlipTarget[position]) continue;
      writeBigDigitAt(0, position, actualDigits[position]);
      writeBigDigitAt(1, position, actualDigits[position]);
    }
    memcpy(splitFlipTarget, actualDigits, sizeof(splitFlipTarget));
  }
#else
  if (!splitFlipActive && memcmp(actualDigits, splitFlipTarget, sizeof(splitFlipTarget)) != 0) {
    memcpy(splitFlipPrevious, splitFlipTarget, sizeof(splitFlipPrevious));
    memcpy(splitFlipTarget, actualDigits, sizeof(splitFlipTarget));
    splitFlipMask = 0;
    for (uint8_t position = 0; position < 4; ++position) {
      if (splitFlipPrevious[position] != splitFlipTarget[position]) {
        splitFlipMask |= static_cast<uint8_t>(1U << position);
      }
    }
    splitFlipActive = splitFlipMask != 0;
    splitFlipPhase = 0;
    splitFlipPhaseAt = nowMs;
    if (splitFlipActive) renderSplitFlipPhase();
  }

  if (splitFlipActive && nowMs - splitFlipPhaseAt >= HHMM_FLIP_PHASE_MS) {
    splitFlipPhaseAt = nowMs;
    if (splitFlipPhase < 3) {
      ++splitFlipPhase;
      renderSplitFlipPhase();
    } else {
      splitFlipActive = false;
      splitFlipMask = 0;
    }
  }
#endif

  lcd.setCursor(7, 0);
  writeCenterColon(lastColonVisible, false);
  lcd.setCursor(7, 1);
  writeCenterColon(lastColonVisible, false);

#if SHOW_SECONDS
  updateSecondsScan(actualSeconds, nowMs);
  lcd.setCursor(14, 1);
  lcd.write(SECOND_GLYPH_TENS);
  lcd.write(SECOND_GLYPH_ONES);
#else
  lcd.setCursor(14, 1);
  lcd.write(' ');
  lcd.write(' ');
#endif
}

static void drawBigClock(uint32_t nowMs) {
#if HHMM_ANIMATION_MODE == HHMM_ANIMATION_PIXEL_SCAN
  drawBigClockPixelScan(nowMs);
#else
  drawBigClockDirectOrFlip(nowMs);
#endif
}

// ---------- Network state machine ----------
static void updateNetworkAndTime() {
  const uint32_t now = millis();
  const wl_status_t wifiStatus = WiFi.status();

  if (networkState == NetworkState::PasswordMissing || networkState == NetworkState::SecurityError) {
    return;
  }

  if (wifiStatus == WL_CONNECTED) {
    if (networkState != NetworkState::Connected) {
      networkState = NetworkState::Connected;
      ntpConfigured = false;
      ntpSynchronized = false;
      USB_SERIAL_PORT.print("WiFi connected, IP: ");
      USB_SERIAL_PORT.println(WiFi.localIP());
      requestNtpSync();
    }

    if (ntpConfigured && !ntpSynchronized) {
      bool syncComplete = sntpHasCompleted();
#if !HAS_SNTP_STATUS_API
      // Older cores do not expose the SNTP status API. In that case the
      // fallback is deliberately conservative: only a valid clock after the
      // request has had time to complete is accepted.
      syncComplete = currentTimeIsValid() && (now - ntpStartedAt > 3000);
#endif
      if (syncComplete) {
        ntpSynchronized = true;
        displayTimeSynchronized = true;
        saveCurrentTime();
        USB_SERIAL_PORT.println("NTP synchronization complete");
        const String ip = WiFi.localIP().toString();
        showStatus("WiFi + NTP OK", ip.c_str(), WIFI_STATUS_HOLD_MS);
      } else if (!ntpFailureShown && now - ntpStartedAt >= NTP_FIRST_SYNC_TIMEOUT_MS) {
        ntpFailureShown = true;
        nextNtpAttemptAt = now + NTP_RETRY_DELAY_MS;
        USB_SERIAL_PORT.println("NTP synchronization timed out; keeping local clock and retrying later");
      }
    }

    if (ntpFailureShown && now >= nextNtpAttemptAt) requestNtpSync();
    return;
  }

  if (networkState == NetworkState::Connected) {
    networkState = NetworkState::Disconnected;
    nextWifiAttemptAt = now + WIFI_RETRY_DELAY_MS;
    USB_SERIAL_PORT.println("WiFi disconnected; clock continues while reconnecting");
    showStatus("WiFi lost", "reconnecting...", WIFI_STATUS_HOLD_MS);
  }

  if (networkState == NetworkState::Connecting && now - connectStartedAt >= WIFI_CONNECT_TIMEOUT_MS) {
    USB_SERIAL_PORT.println("WiFi connection timed out; will retry");
    WiFi.disconnect();
    networkState = NetworkState::Disconnected;
    nextWifiAttemptAt = now + WIFI_RETRY_DELAY_MS;
    showStatus("WiFi timeout", "retrying...", WIFI_STATUS_HOLD_MS);
    return;
  }

  if (networkState == NetworkState::Disconnected && now >= nextWifiAttemptAt) {
    beginWifiConnection();
  }
}

// ---------- Arduino lifecycle ----------
void setup() {
  USB_SERIAL_PORT.begin(115200);
  delay(300);
  USB_SERIAL_PORT.println();
  USB_SERIAL_PORT.println("ESP32-C3 LCD1602 network clock");
#if HHMM_ANIMATION_MODE == HHMM_ANIMATION_SPLIT_FLIP
  USB_SERIAL_PORT.println("HH:MM animation: split flip (changed digits only)");
#elif HHMM_ANIMATION_MODE == HHMM_ANIMATION_PIXEL_SCAN
  USB_SERIAL_PORT.println("HH:MM animation: pixel scan");
#else
  USB_SERIAL_PORT.println("HH:MM animation: none");
#endif

  preferences.begin("pixel-clock", false);
  restoreLastKnownTime();

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(I2C_CLOCK_HZ);
  scanI2C();
  USB_SERIAL_PORT.printf(
      "LCD driver: direct PCF8574T addr=0x%02X map=P0:RS P1:RW P2:EN P3:BL P4-P7:D4-D7\n",
      LCD_PCF8574_ADDRESS);

  const int lcdStatus = lcd.begin(LCD_COLUMNS, LCD_ROWS);
  if (lcdStatus == 0) {
    lcdReady = true;
    // A visible blink proves that P3 and the PCF8574T output path respond.
    lcd.noBacklight();
    delay(300);
    lcd.backlight();
    lcd.display();
    USB_SERIAL_PORT.println("LCD initialized successfully; WiFi is not required for display");
  } else {
    USB_SERIAL_PORT.printf("LCD init failed, status=%d\n", lcdStatus);
  }

#if NETWORK_MODE == NETWORK_MODE_ENTERPRISE_EAP
  if (!configureEnterpriseSecurity()) {
    networkState = NetworkState::SecurityError;
    showStatus("CERT API ERROR", "update ESP32 core");
    return;
  }

  showStatus("CERT: default CA", "domain checked", WIFI_STATUS_HOLD_MS);
#else
  USB_SERIAL_PORT.printf("WPA2-Personal profile selected; SSID=%s\n", PERSONAL_WIFI_SSID);
#endif
  WiFi.onEvent(onWiFiEvent, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  beginWifiConnection();
}

void loop() {
  updateNetworkAndTime();

  const uint32_t now = millis();
  const bool persistentError = networkState == NetworkState::PasswordMissing
                            || networkState == NetworkState::SecurityError;
  const bool statusHoldFinished = static_cast<int32_t>(now - clockDisplayAfter) >= 0;
#if SHOW_SECONDS
  // Seconds need a regular tick even while no animation is active so a
  // second boundary is detected promptly.
  const uint32_t refreshInterval = SECOND_SCAN_FRAME_MS;
#else
#if HHMM_ANIMATION_MODE == HHMM_ANIMATION_PIXEL_SCAN
  const uint32_t refreshInterval = bigScanActive ? PIXEL_SCAN_FRAME_MS : 500U;
#elif HHMM_ANIMATION_MODE == HHMM_ANIMATION_SPLIT_FLIP
  const uint32_t refreshInterval = splitFlipActive ? HHMM_FLIP_PHASE_MS : 500U;
#else
  const uint32_t refreshInterval = 500U;
#endif
#endif
  if (lcdReady && !persistentError && statusHoldFinished
      && now - lastClockDrawAt >= refreshInterval) {
    lastClockDrawAt = now;
    lastColonVisible = ((now / 500U) % 2U) == 0U;
    drawBigClock(now);
  }

  delay(20);
}
