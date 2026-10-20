/*
  BIKE HUD - Chronos-only edition, navigation toast + smarter call timeout

  ESP32 + 128x64 SSD1306 OLED + ChronosESP32
  OLED SDA: GPIO20; SCL: GPIO21; common I2C address 0x3C / 0x3D

  The Chronos app remains the ONLY place to change 12/24-hour time.
  No Wi-Fi, web interface, saved local setting, or forced set24Hour().
  Clock extraction deliberately follows the user's ORIGINAL working sketch:
      watch.getHourC(), watch.getTime(":"), watch.getTime("%M"),
      watch.getAmPmC(true)
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ChronosESP32.h>

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET   -1
#define OLED_SDA_PIN 20
#define OLED_SCL_PIN 21

constexpr uint32_t MIN_FRAME_MS       = 65;     // OLED redraw throttle
constexpr uint32_t CLOCK_POLL_MS      = 500;    // catches app format/time changes
constexpr uint32_t BATTERY_POLL_MS    = 3000;
constexpr uint32_t NOTIF_DURATION_MS  = 5000;
constexpr uint32_t CALL_STALE_MS      = 120000; // fallback only when no call-end event arrives
constexpr uint32_t ANIMATION_MS       = 500;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
ChronosESP32 watch("EN NE");

Navigation navData;
Notification lastNotif;

bool navActive = false;
uint32_t navCRC = 0xFFFFFFFFUL;
bool bleConnected = false;

// A ringing call has priority over ordinary notifications.
enum class Overlay : uint8_t { NONE, NOTIFICATION, RINGING };
Overlay overlay = Overlay::NONE;
bool missedCall = false;
uint32_t overlayStarted = 0;

String clockDigits = "--:--";
String clockMeridiem = "";
bool clockReceived = false;
uint8_t phoneBattery = 255;  // 255 = unknown
bool phoneCharging = false;

bool needRedraw = true;
uint32_t lastFrame = 0;
uint32_t lastClockPoll = 0;
uint32_t lastBatteryPoll = 0;
uint32_t lastAnimation = 0;

String lowerCopy(String s) {
  s.toLowerCase();
  return s;
}

String cleanText(String s) {
  s.replace("\r", " ");
  s.replace("\n", " ");
  s.replace("\t", " ");
  // SSD1306's built-in font does not support these Unicode symbols.
  s.replace("→", ">");
  s.replace("←", "<");
  s.replace("↗", "/");
  s.replace("↖", "\\");
  s.replace("–", "-");
  s.replace("—", "-");
  s.trim();
  return s;
}

String crop(String s, uint8_t limit) {
  s = cleanText(s);
  if (s.length() <= limit) return s;
  if (limit <= 1) return ".";
  return s.substring(0, limit - 1) + ".";
}

void centered(String s, int y, uint8_t size = 1) {
  display.setTextSize(size);
  uint8_t maxChars = SCREEN_WIDTH / (size * 6);
  s = crop(s, maxChars);
  int x = (SCREEN_WIDTH - static_cast<int>(s.length()) * size * 6) / 2;
  display.setCursor(x < 0 ? 0 : x, y);
  display.print(s);
}

void wrapped(String s, int x, int y, uint8_t charsPerLine,
             uint8_t lines, uint8_t lineHeight = 10) {
  s = cleanText(s);
  size_t p = 0;
  for (uint8_t line = 0; line < lines && p < s.length(); line++) {
    while (p < s.length() && s.charAt(p) == ' ') p++;
    if (p >= s.length()) break;
    String remaining = s.substring(p);
    if (remaining.length() <= charsPerLine) {
      display.setCursor(x, y + line * lineHeight);
      display.print(remaining);
      return;
    }
    if (line == lines - 1) {
      display.setCursor(x, y + line * lineHeight);
      display.print(crop(remaining, charsPerLine));
      return;
    }
    size_t take = charsPerLine;
    int spaceAt = remaining.lastIndexOf(' ', charsPerLine);
    if (spaceAt > 2 && spaceAt <= charsPerLine) take = spaceAt;
    display.setCursor(x, y + line * lineHeight);
    display.print(remaining.substring(0, take));
    p += take;
  }
}

// IMPORTANT: Retain the original app-driven Chronos time-reading method.
// No set24Hour(true/false) anywhere in this firmware.
void updateClock() {
  String h = String(watch.getHourC());
  String t = h + watch.getTime(":") + watch.getTime("%M");
  String ampm = watch.getAmPmC(true);
  if (t != clockDigits || ampm != clockMeridiem) {
    clockDigits = t;
    clockMeridiem = ampm;
    needRedraw = true;
  }
}

void connectionCallback(bool connected) {
  bleConnected = connected;
  if (!connected) {
    if (overlay == Overlay::RINGING) overlay = Overlay::NONE;
    navActive = false;
    navCRC = 0xFFFFFFFFUL;
    phoneBattery = 255;
    phoneCharging = false;
    clockReceived = false;
    clockDigits = "--:--";
    clockMeridiem = "";
  }
  needRedraw = true;
}

// Chronos app controls which apps forward notifications; do not filter or
// change that selection here. Preserve the existing call classification.
void notificationCallback(Notification n) {
  String app = lowerCopy(n.app);
  String title = lowerCopy(n.title);
  String message = lowerCopy(n.message);

  bool phoneApp = app == "phone" || app == "calls" || app == "call" ||
                  app.indexOf("dialer") >= 0 || app.indexOf("telephone") >= 0;
  bool missed = title.indexOf("missed call") >= 0 ||
                message.indexOf("missed call") >= 0 ||
                (phoneApp && title.indexOf("missed") >= 0);
  bool ended = title.indexOf("call ended") >= 0 ||
               title.indexOf("call declined") >= 0 ||
               title.indexOf("call rejected") >= 0 ||
               message.indexOf("call ended") >= 0;
  bool incoming = title.indexOf("incoming call") >= 0 ||
                  message.indexOf("incoming call") >= 0 ||
                  title.indexOf("is calling") >= 0 ||
                  message.indexOf("is calling") >= 0 ||
                  ((title.indexOf("incoming") >= 0 || message.indexOf("incoming") >= 0) &&
                   (title.indexOf("call") >= 0 || message.indexOf("call") >= 0)) ||
                  (phoneApp && !missed && !ended);
  if (missed) incoming = false;

  if (ended && overlay == Overlay::RINGING) {
    overlay = Overlay::NONE;
    needRedraw = true;
    return;
  }
  if (overlay == Overlay::RINGING && !incoming && !missed) return;

  lastNotif = n;
  missedCall = missed;
  overlay = incoming ? Overlay::RINGING : Overlay::NOTIFICATION;
  overlayStarted = millis();
  needRedraw = true;
}

void configCallback(Config type, uint32_t a, uint32_t b) {
  (void)b;
  switch (type) {
    case CF_NAV_DATA:
      navActive = (a != 0);
      if (navActive) {
        navData = watch.getNavigation();
        navCRC = navData.iconCRC;
      } else {
        navCRC = 0xFFFFFFFFUL;
      }
      needRedraw = true;
      break;
    case CF_NAV_ICON:
      if (a == 2) {
        Navigation updated = watch.getNavigation();
        if (navCRC != updated.iconCRC || !navData.icon) {
          navCRC = updated.iconCRC;
          navData = updated;
          needRedraw = true;
        }
      }
      break;
    case CF_TIME:
      clockReceived = true;
      updateClock();
      needRedraw = true;
      break;
    case CF_PBAT:
      phoneBattery = watch.getPhoneBattery();
      phoneCharging = watch.isPhoneCharging();
      needRedraw = true;
      break;
    default:
      break;
  }
}

void statusBar() {
  display.setTextSize(1);
  display.setCursor(1, 1);
  // Bottom clock is the primary time; keep this bar short to avoid overlap.
  String shortClock = clockReceived ? clockDigits : "--:--";
  display.print(crop(shortClock, 7));

  if (bleConnected) display.fillCircle(77, 4, 2, SSD1306_WHITE);
  else display.drawCircle(77, 4, 2, SSD1306_WHITE);
  display.setCursor(83, 1);
  display.print("BT");

  String bat = (bleConnected && phoneBattery <= 100)
                 ? String(phoneBattery) + "%" : "--%";
  if (phoneCharging && phoneBattery <= 100) bat = "+" + bat;
  display.setCursor(SCREEN_WIDTH - static_cast<int>(bat.length()) * 6, 1);
  display.print(bat);
  display.drawFastHLine(0, 10, SCREEN_WIDTH, SSD1306_WHITE);
}

void clockScreen() {
  statusBar();
  centered(clockReceived ? clockDigits : "--:--", 23, 3);
  if (clockReceived) {
    // The Chronos library supplies the meridiem for the phone's clock mode.
    if (clockMeridiem.length()) centered(clockMeridiem, 51);
  } else {
    centered("SYNC WITH CHRONOS", 51);
  }
  display.drawFastHLine(18, 61, 92, SSD1306_WHITE);
}

void navScreen() {
  statusBar();
  display.drawFastVLine(55, 12, 52, SSD1306_WHITE);
  if (navData.icon != nullptr) {
    display.drawBitmap(3, 14, navData.icon, 48, 48, SSD1306_WHITE);
  } else {
    display.drawRoundRect(6, 21, 44, 33, 4, SSD1306_WHITE);
    display.setTextSize(2);
    display.setCursor(9, 29);
    display.print("NAV");
  }
  display.setTextSize(1);
  wrapped(navData.directions.length() ? navData.directions : "Waiting for route",
          59, 14, 11, 5);
}

String callerName() {
  String title = cleanText(lastNotif.title);
  String low = lowerCopy(title);
  if (!title.length() || low == "incoming call" || low == "call") {
    String msg = cleanText(lastNotif.message);
    return msg.length() ? msg : "Unknown";
  }
  return title;
}

void fullCallScreen() {
  centered("INCOMING CALL", 3);
  display.drawFastHLine(8, 16, 112, SSD1306_WHITE);
  centered(callerName(), 23, 2);
  bool pulse = ((millis() / ANIMATION_MS) % 2) != 0;
  display.drawCircle(64, 46, 7, SSD1306_WHITE);
  if (pulse) display.fillCircle(64, 46, 4, SSD1306_WHITE);
  else display.drawCircle(64, 46, 4, SSD1306_WHITE);
  centered("ANSWER ON PHONE", 56);
}

void compactCallScreen() {
  statusBar();
  display.setTextSize(1);
  wrapped(navData.directions.length() ? navData.directions : "Navigation active",
          2, 13, 20, 2);
  display.drawFastHLine(0, 34, SCREEN_WIDTH, SSD1306_WHITE);
  display.setCursor(2, 39);
  display.print("CALL:");
  display.setCursor(38, 39);
  display.print(crop(callerName(), 14));
  centered(((millis() / ANIMATION_MS) % 2) ? "(( RINGING ))" : "( RINGING )", 53);
}

void notificationScreen() {
  statusBar();
  display.setTextSize(1);
  display.setCursor(2, 13);
  display.print(missedCall ? "MISSED CALL" : crop(lastNotif.app, 20));
  display.drawFastHLine(2, 23, 124, SSD1306_WHITE);
  display.setCursor(2, 27);
  display.print(crop(lastNotif.title, 20));
  wrapped(lastNotif.message, 2, 40, 20, 2);
  uint32_t elapsed = millis() - overlayStarted;
  uint32_t remain = (elapsed >= NOTIF_DURATION_MS) ? 0 : NOTIF_DURATION_MS - elapsed;
  int progress = (remain * static_cast<uint32_t>(SCREEN_WIDTH)) / NOTIF_DURATION_MS;
  if (progress > 0) display.fillRect(0, 61, progress, 3, SSD1306_WHITE);
}

// Keep the 48x48 turn icon and the top three direction lines visible.
// Only the lower-right portion becomes a compact notification toast.
void navNotificationScreen() {
  navScreen();
  // The divider remains at x=55; never paint over the turn icon (x=3..50).
  display.fillRect(57, 42, 71, 22, SSD1306_BLACK);
  display.drawFastHLine(57, 42, 71, SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(59, 44);
  String source = missedCall ? "MISSED CALL" : lastNotif.app;
  display.print(crop(source.length() ? source : "NOTICE", 11));
  display.setCursor(59, 54);
  String preview = lastNotif.title.length() ? lastNotif.title : lastNotif.message;
  display.print(crop(preview, 11));
  const uint32_t elapsed = millis() - overlayStarted;
  const uint32_t remain = (elapsed >= NOTIF_DURATION_MS)
                            ? 0 : NOTIF_DURATION_MS - elapsed;
  const int barWidth = (remain * 71UL) / NOTIF_DURATION_MS;
  if (barWidth > 0) display.drawFastHLine(57, 63, barWidth, SSD1306_WHITE);
}

void drawScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);
  if (overlay == Overlay::RINGING) {
    if (navActive) compactCallScreen();
    else fullCallScreen();
  } else if (overlay == Overlay::NOTIFICATION) {
    if (navActive) navNotificationScreen();
    else notificationScreen();
  } else if (navActive) {
    navScreen();
  } else {
    clockScreen();
  }
  display.display();
}

void setup() {
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3D)) {
      while (true) delay(250); // no OLED at either address
    }
  }

  display.setTextColor(SSD1306_WHITE);
  display.clearDisplay();
  centered("BIKE HUD", 14, 2);
  display.drawFastHLine(18, 40, 92, SSD1306_WHITE);
  centered("CONNECT CHRONOS", 47);
  display.display();
  delay(1000);

  watch.setConnectionCallback(connectionCallback);
  watch.setNotificationCallback(notificationCallback);
  watch.setConfigurationCallback(configCallback);
  watch.begin();

  // Intentional: do NOT call watch.set24Hour().
  // The phone's Chronos app remains responsible for clock preferences.
  // Intentional: do NOT call watch.setBattery(80), which advertises a fake value.
  needRedraw = true;
}

void loop() {
  watch.loop();  // always run BLE loop, even when OLED doesn't need updating
  const uint32_t now = millis();

  if (now - lastClockPoll >= CLOCK_POLL_MS) {
    lastClockPoll = now;
    if (clockReceived) updateClock();
  }
  if (bleConnected && now - lastBatteryPoll >= BATTERY_POLL_MS) {
    lastBatteryPoll = now;
    uint8_t b = watch.getPhoneBattery();
    bool charge = watch.isPhoneCharging();
    if (b != phoneBattery || charge != phoneCharging) {
      phoneBattery = b;
      phoneCharging = charge;
      needRedraw = true;
    }
  }
  if (overlay != Overlay::NONE) {
    // Chronos may send an explicit call-ended notification. If not, clear
    // only a *stale* ringing screen rather than hiding it after 45 seconds.
    // Repeated incoming-call notifications reset overlayStarted automatically.
    uint32_t lifetime = (overlay == Overlay::RINGING)
                          ? CALL_STALE_MS : NOTIF_DURATION_MS;
    if (now - overlayStarted >= lifetime) {
      overlay = Overlay::NONE;
      needRedraw = true;
    } else if (now - lastAnimation >= ANIMATION_MS) {
      lastAnimation = now;
      needRedraw = true;
    }
  }

  if (needRedraw && now - lastFrame >= MIN_FRAME_MS) {
    lastFrame = now;
    needRedraw = false;
    drawScreen();
  }
}
