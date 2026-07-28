#include <Arduino.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <WebServer.h>
#include <WiFi.h>
#include <qrcodegen.h>

namespace {

constexpr uint8_t START_BUTTON_PIN = 0;
constexpr uint8_t SET_BUTTON_PIN = 47;
constexpr uint8_t BACKLIGHT_PIN = 10;
constexpr uint8_t BACKLIGHT_PWM_CHANNEL = 0;
constexpr uint16_t BACKLIGHT_PWM_FREQUENCY = 5000;
constexpr uint8_t BACKLIGHT_PWM_BITS = 8;

constexpr uint32_t DEFAULT_WORK_SECONDS = 25U * 60U;
constexpr uint32_t SHORT_BREAK_SECONDS = 5U * 60U;
constexpr uint32_t LONG_BREAK_SECONDS = 20U * 60U;
constexpr uint16_t DEFAULT_WORK_LIMIT_MINUTES = 60;
constexpr uint8_t DEFAULT_BRIGHTNESS_PERCENT = 90;
constexpr uint8_t DEFAULT_ADJUSTMENT_MINUTES = 5;
constexpr bool DEFAULT_AUTO_START = false;
constexpr uint32_t SET_LONG_PRESS_MS = 2000;
constexpr uint32_t DEBOUNCE_MS = 30;
constexpr uint32_t TIMES_UP_MS = 5000;
constexpr uint32_t BACKLIGHT_IDLE_MS = 10U * 60U * 1000U;
constexpr uint32_t HOTSPOT_LIFETIME_MS = 3U * 60U * 1000U;
constexpr char AP_SSID[] = "MiniPomodoro";

constexpr uint16_t BG = TFT_BLACK;
constexpr uint16_t BRIGHT_GREEN = 0x07E0;
constexpr uint16_t DIM_GREEN = 0x03E0;
// This old-panel batch has B/R channels swapped. TFT blue values therefore
// produce the requested red on the physical display.
constexpr uint16_t BRIGHT_RED = 0x001F;
constexpr uint16_t DIM_RED = 0x000F;
constexpr uint16_t BRIGHT_WHITE = 0xFFFF;
constexpr uint16_t DIM_WHITE = 0x7BEF;
constexpr int BOTTOM_BAR_Y = 112;
constexpr int BOTTOM_BAR_HEIGHT = 11;

enum class Phase : uint8_t { Work, Rest };

class Button {
 public:
  Button(uint8_t pin, uint32_t longPressMs)
      : pin_(pin), longPressMs_(longPressMs) {}

  void begin() {
    pinMode(pin_, INPUT_PULLUP);
    rawPressed_ = stablePressed_ = (digitalRead(pin_) == LOW);
    changedAt_ = millis();
  }

  void update(uint32_t now) {
    shortEvent_ = longEvent_ = false;
    const bool pressed = (digitalRead(pin_) == LOW);
    if (pressed != rawPressed_) {
      rawPressed_ = pressed;
      changedAt_ = now;
    }
    if (pressed != stablePressed_ && now - changedAt_ >= DEBOUNCE_MS) {
      stablePressed_ = pressed;
      if (stablePressed_) {
        pressedAt_ = now;
        longFired_ = false;
      } else if (!longFired_) {
        shortEvent_ = true;
      }
    }
    if (stablePressed_ && !longFired_ &&
        now - pressedAt_ >= longPressMs_) {
      longFired_ = true;
      longEvent_ = true;
    }
  }

  bool takeShortPress() {
    const bool value = shortEvent_;
    shortEvent_ = false;
    return value;
  }

  bool takeLongPress() {
    const bool value = longEvent_;
    longEvent_ = false;
    return value;
  }

  bool isPressed() const { return stablePressed_; }

 private:
  uint8_t pin_;
  uint32_t longPressMs_;
  bool rawPressed_ = false;
  bool stablePressed_ = false;
  bool longFired_ = false;
  bool shortEvent_ = false;
  bool longEvent_ = false;
  uint32_t changedAt_ = 0;
  uint32_t pressedAt_ = 0;
};

TFT_eSPI tft;
TFT_eSprite screen(&tft);
Preferences preferences;
WebServer webServer(80);
// IO0 only has a short-press action. UINT32_MAX prevents a held press from
// becoming a separate long-press event.
Button startButton(START_BUTTON_PIN, UINT32_MAX);
Button setButton(SET_BUTTON_PIN, SET_LONG_PRESS_MS);

Phase phase = Phase::Work;
bool running = false;
bool countdownActive = false;
bool stopped = true;
bool longBreak = false;
bool waitingAfterBreak = false;
bool backlightSleeping = false;
bool hotspotActive = false;
bool qrVisible = false;
bool chordActive = false;
uint8_t completedWorkSessions = 0;
uint8_t brightnessPercent = DEFAULT_BRIGHTNESS_PERCENT;
uint8_t adjustmentMinutes = DEFAULT_ADJUSTMENT_MINUTES;
uint16_t workLimitMinutes = DEFAULT_WORK_LIMIT_MINUTES;
uint32_t shortBreakSeconds = SHORT_BREAK_SECONDS;
uint32_t longBreakSeconds = LONG_BREAK_SECONDS;
bool autoStartAfterBreak = DEFAULT_AUTO_START;
uint32_t configuredWorkSeconds = DEFAULT_WORK_SECONDS;
uint32_t totalSeconds = DEFAULT_WORK_SECONDS;
uint32_t remainingSeconds = DEFAULT_WORK_SECONDS;
uint32_t nextTickAt = 0;
uint32_t timesUpUntil = 0;
uint32_t lastDrawnSeconds = UINT32_MAX;
bool lastTimesUpVisible = false;
uint32_t lastActivityAt = 0;
uint32_t hotspotStartedAt = 0;

constexpr uint8_t DIGIT_SEGMENTS[10] = {
    0b0111111, 0b0000110, 0b1011011, 0b1001111, 0b1100110,
    0b1101101, 0b1111101, 0b0000111, 0b1111111, 0b1101111};

void setBacklight(uint8_t percent) {
  percent = constrain(percent, 10, 100);
  // The T-QT backlight is active-low: duty 0 is full brightness.
  const uint8_t duty =
      255U - static_cast<uint8_t>((static_cast<uint16_t>(percent) * 255U) /
                                  100U);
  ledcWrite(BACKLIGHT_PWM_CHANNEL, duty);
}

void wakeBacklight(uint32_t now) {
  backlightSleeping = false;
  lastActivityAt = now;
  if (!backlightSleeping) setBacklight(brightnessPercent);
  lastDrawnSeconds = UINT32_MAX;
}

void loadSettings() {
  preferences.begin("tomato", false);
  brightnessPercent =
      preferences.getUChar("brightness", DEFAULT_BRIGHTNESS_PERCENT);
  adjustmentMinutes =
      preferences.getUChar("adjustStep", DEFAULT_ADJUSTMENT_MINUTES);
  workLimitMinutes =
      preferences.getUShort("workLimit", DEFAULT_WORK_LIMIT_MINUTES);
  const uint16_t workMinutes =
      preferences.getUShort("workMinutes", DEFAULT_WORK_SECONDS / 60U);
  const uint16_t breakMinutes =
      preferences.getUShort("breakMinutes", SHORT_BREAK_SECONDS / 60U);
  const uint16_t longMinutes =
      preferences.getUShort("longMinutes", LONG_BREAK_SECONDS / 60U);
  autoStartAfterBreak =
      preferences.getBool("autoStart", DEFAULT_AUTO_START);

  brightnessPercent =
      constrain(brightnessPercent, static_cast<uint8_t>(10),
                static_cast<uint8_t>(100));
  adjustmentMinutes =
      constrain(adjustmentMinutes, static_cast<uint8_t>(1),
                static_cast<uint8_t>(60));
  workLimitMinutes =
      constrain(workLimitMinutes, static_cast<uint16_t>(5),
                static_cast<uint16_t>(240));
  configuredWorkSeconds =
      constrain(workMinutes, static_cast<uint16_t>(5), workLimitMinutes) * 60U;
  shortBreakSeconds =
      constrain(breakMinutes, static_cast<uint16_t>(1),
                static_cast<uint16_t>(120)) *
      60U;
  longBreakSeconds =
      constrain(longMinutes, static_cast<uint16_t>(1),
                static_cast<uint16_t>(240)) *
      60U;
  totalSeconds = remainingSeconds = configuredWorkSeconds;
}

void saveSettings() {
  preferences.putUChar("brightness", brightnessPercent);
  preferences.putUChar("adjustStep", adjustmentMinutes);
  preferences.putUShort("workLimit", workLimitMinutes);
  preferences.putUShort("workMinutes", configuredWorkSeconds / 60U);
  preferences.putUShort("breakMinutes", shortBreakSeconds / 60U);
  preferences.putUShort("longMinutes", longBreakSeconds / 60U);
  preferences.putBool("autoStart", autoStartAfterBreak);
}

void restoreDefaultSettings() {
  brightnessPercent = DEFAULT_BRIGHTNESS_PERCENT;
  adjustmentMinutes = DEFAULT_ADJUSTMENT_MINUTES;
  workLimitMinutes = DEFAULT_WORK_LIMIT_MINUTES;
  configuredWorkSeconds = DEFAULT_WORK_SECONDS;
  shortBreakSeconds = SHORT_BREAK_SECONDS;
  longBreakSeconds = LONG_BREAK_SECONDS;
  autoStartAfterBreak = DEFAULT_AUTO_START;
  saveSettings();
  if (!backlightSleeping) setBacklight(brightnessPercent);
}

String settingsPage(const char* message = "") {
  String html;
  html.reserve(5200);
  html += F(
      "<!doctype html><html lang='zh-Hant'><head>"
      "<meta charset='utf-8'><meta name='viewport' "
      "content='width=device-width,initial-scale=1'>"
      "<title>Tomato Clock</title><style>"
      "body{font-family:system-ui;background:#101410;color:#e8f5e9;"
      "max-width:560px;margin:auto;padding:24px}"
      "h1{color:#54e66b}form{background:#1b221c;padding:20px;"
      "border-radius:14px}label{display:block;margin:16px 0 6px}"
      "input[type=number],input[type=range]{width:100%;box-sizing:border-box}"
      "input[type=number]{padding:10px;background:#0d110e;color:white;"
      "border:1px solid #456;border-radius:7px}"
      ".row{display:flex;gap:12px;margin-top:22px}.row>*{flex:1}"
      "button{padding:12px;border:0;border-radius:8px;font-weight:700}"
      ".save{background:#42d65c}.reset{background:#ddd}"
      ".msg{color:#7ff493}</style></head><body><h1>Tomato Clock</h1>");
  if (message[0]) {
    html += F("<p class='msg'>");
    html += message;
    html += F("</p>");
  }
  html += F("<form method='post' action='/save'>"
            "<label>背光亮度：<output id='bv'>");
  html += brightnessPercent;
  html += F(
      "%</output></label><input name='brightness' type='range' min='10' "
      "max='100' step='10' value='");
  html += brightnessPercent;
  html += F("' oninput=\"bv.value=this.value+'%'\">"
            "<label>工作時間倒數上限（分鐘）</label>"
            "<input name='limit' type='number' min='5' max='240' value='");
  html += workLimitMinutes;
  html += F("'><label>工作時間（分鐘）</label>"
            "<input name='work' type='number' min='5' max='");
  html += workLimitMinutes;
  html += F("' value='");
  html += configuredWorkSeconds / 60U;
  html += F("'><label>一般休息（分鐘）</label>"
            "<input name='break' type='number' min='1' max='120' value='");
  html += shortBreakSeconds / 60U;
  html += F("'><label>長休息（分鐘）</label>"
            "<input name='longbreak' type='number' min='1' max='240' value='");
  html += longBreakSeconds / 60U;
  html += F(
      "'><label><input name='auto' type='checkbox' value='1' ");
  if (autoStartAfterBreak) html += F("checked ");
  html += F(
      "> 休息結束後自動開始工作倒數</label>"
      "<div class='row'><button class='save' type='submit'>Save</button>"
      "<button class='reset' type='submit' formaction='/reset'>Reset</button>"
      "</div></form><p>Hotspot 會在開機 3 分鐘後自動關閉。</p>"
      "</body></html>");
  return html;
}

void handleWebSave() {
  workLimitMinutes =
      constrain(webServer.arg("limit").toInt(), 5L, 240L);
  const uint16_t workMinutes =
      constrain(webServer.arg("work").toInt(), 5L,
                static_cast<long>(workLimitMinutes));
  brightnessPercent =
      constrain(webServer.arg("brightness").toInt(), 10L, 100L);
  shortBreakSeconds =
      constrain(webServer.arg("break").toInt(), 1L, 120L) * 60U;
  longBreakSeconds =
      constrain(webServer.arg("longbreak").toInt(), 1L, 240L) * 60U;
  autoStartAfterBreak = webServer.hasArg("auto");
  configuredWorkSeconds = workMinutes * 60U;
  saveSettings();
  if (!backlightSleeping) setBacklight(brightnessPercent);
  if (!countdownActive && phase == Phase::Work) {
    totalSeconds = remainingSeconds = configuredWorkSeconds;
    lastDrawnSeconds = UINT32_MAX;
  }
  webServer.send(200, "text/html; charset=utf-8",
                 settingsPage("設定已儲存"));
}

String pomodoroSettingsPage(const char* message = "") {
  String html;
  html.reserve(4800);
  html += F(
      "<!doctype html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>Pomodoro Timer</title><style>"
      "body{font-family:system-ui;background:#101410;color:#e8f5e9;"
      "max-width:560px;margin:auto;padding:24px}"
      "h1{color:#54e66b}form{background:#1b221c;padding:20px;"
      "border-radius:14px}label{display:block;margin:16px 0 6px}"
      "input[type=number],input[type=range]{width:100%;box-sizing:border-box}"
      "input[type=number]{padding:10px;background:#0d110e;color:white;"
      "border:1px solid #456;border-radius:7px}"
      ".row{display:flex;gap:12px;margin-top:22px}.row>*{flex:1}"
      "button{padding:12px;border:0;border-radius:8px;font-weight:700}"
      ".save{background:#42d65c}.reset{background:#ddd}"
      ".msg{color:#7ff493}</style></head><body>"
      "<h1>Pomodoro Timer</h1>");
  if (message[0]) {
    html += F("<p class='msg'>");
    html += message;
    html += F("</p>");
  }
  html += F("<form method='post' action='/save'>"
            "<label>Backlight: <output id='bv'>");
  html += brightnessPercent;
  html += F("%</output></label><input name='brightness' type='range' "
            "min='10' max='100' step='10' value='");
  html += brightnessPercent;
  html += F("' oninput=\"bv.value=this.value+'%'\">"
            "<label>Right-button adjustment step (minutes)</label>"
            "<input name='step' type='number' min='1' max='60' value='");
  html += adjustmentMinutes;
  html += F("'><label>Maximum work time (minutes)</label>"
            "<input name='limit' type='number' min='5' max='240' value='");
  html += workLimitMinutes;
  html += F("'><label>Work time (minutes)</label>"
            "<input name='work' type='number' min='5' max='");
  html += workLimitMinutes;
  html += F("' value='");
  html += configuredWorkSeconds / 60U;
  html += F("'><label>Short break (minutes)</label>"
            "<input name='break' type='number' min='1' max='120' value='");
  html += shortBreakSeconds / 60U;
  html += F("'><label>Long break (minutes)</label>"
            "<input name='longbreak' type='number' min='1' max='240' value='");
  html += longBreakSeconds / 60U;
  html += F("'><label><input name='auto' type='checkbox' value='1' ");
  if (autoStartAfterBreak) html += F("checked ");
  html += F(
      "> Auto-start work after a break</label><div class='row'>"
      "<button class='save' type='submit'>Save</button>"
      "<button class='reset' type='submit' formaction='/reset'>Reset</button>"
      "</div></form>"
      "<h2>使用說明 / Instructions</h2>"
      "<ul>"
      "<li><b>左鍵 / Left:</b> 開始、暫停或繼續倒數。休息結束的"
      "配色互換的 00:00 畫面中，按下後會立即開始下一次工作。</li>"
      "<li><b>右鍵 / Right:</b> 工作尚未開始時短按增加、長按 2 秒"
      "減少工作時間，調整後會自動保存並於下次開機使用；倒數流程中"
      "長按 2 秒重設整個循環。</li>"
      "<li><b>左鍵＋右鍵 / Left + Right:</b> Hotspot 啟用期間顯示"
      "連線 QR Code 與 Web IP。</li>"
      "</ul>"
      "<h2>設定說明 / Settings</h2>"
      "<ul>"
      "<li>Backlight：10 段背光亮度。</li>"
      "<li>Right-button adjustment step：右鍵每次增減的分鐘數。</li>"
      "<li>Maximum work time：可設定的工作時間上限。</li>"
      "<li>Work/Short break/Long break：各階段分鐘數。</li>"
      "<li>Auto-start：休息結束後直接開始工作，不停留在配色互換的"
      " 00:00 畫面。</li>"
      "<li>Save 會保存到裝置；Reset 會回復所有預設值。</li>"
      "</ul>"
      "<p>The hotspot turns off three minutes after boot.</p>"
      "</body></html>");
  return html;
}

void handlePomodoroWebSave() {
  workLimitMinutes =
      constrain(webServer.arg("limit").toInt(), 5L, 240L);
  const uint16_t workMinutes =
      constrain(webServer.arg("work").toInt(), 5L,
                static_cast<long>(workLimitMinutes));
  brightnessPercent =
      constrain(webServer.arg("brightness").toInt(), 10L, 100L);
  adjustmentMinutes =
      constrain(webServer.arg("step").toInt(), 1L, 60L);
  shortBreakSeconds =
      constrain(webServer.arg("break").toInt(), 1L, 120L) * 60U;
  longBreakSeconds =
      constrain(webServer.arg("longbreak").toInt(), 1L, 240L) * 60U;
  autoStartAfterBreak = webServer.hasArg("auto");
  configuredWorkSeconds = workMinutes * 60U;
  saveSettings();
  if (!backlightSleeping) setBacklight(brightnessPercent);
  if (!countdownActive && phase == Phase::Work) {
    totalSeconds = remainingSeconds = configuredWorkSeconds;
    lastDrawnSeconds = UINT32_MAX;
  }
  webServer.send(200, "text/html; charset=utf-8",
                 pomodoroSettingsPage("Settings saved."));
}

void startHotspot() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID);
  webServer.on("/", HTTP_GET, []() {
    webServer.send(200, "text/html; charset=utf-8",
                   pomodoroSettingsPage());
  });
  webServer.on("/save", HTTP_POST, handlePomodoroWebSave);
  webServer.on("/reset", HTTP_POST, []() {
    restoreDefaultSettings();
    if (!countdownActive && phase == Phase::Work) {
      totalSeconds = remainingSeconds = configuredWorkSeconds;
      lastDrawnSeconds = UINT32_MAX;
    }
    webServer.send(200, "text/html; charset=utf-8",
                   pomodoroSettingsPage("Defaults restored."));
  });
  webServer.onNotFound([]() {
    webServer.sendHeader("Location", "http://192.168.4.1/", true);
    webServer.send(302, "text/plain", "");
  });
  webServer.begin();
  hotspotStartedAt = millis();
  hotspotActive = true;
}

void stopHotspot() {
  webServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  hotspotActive = false;
  if (qrVisible) {
    qrVisible = false;
    tft.fillScreen(BG);
    lastDrawnSeconds = UINT32_MAX;
  }
}

uint16_t backgroundColor() {
  if (!waitingAfterBreak) return BG;
  return longBreak ? DIM_WHITE : DIM_RED;
}

uint16_t brightColor() {
  if (waitingAfterBreak) return BG;
  if (phase == Phase::Work) return BRIGHT_GREEN;
  return longBreak ? BRIGHT_WHITE : BRIGHT_RED;
}

uint16_t dimColor() {
  if (waitingAfterBreak) return BG;
  if (phase == Phase::Work) return DIM_GREEN;
  return longBreak ? DIM_WHITE : DIM_RED;
}

void drawSegmentDigit(int x, int y, int width, int height, uint8_t digit,
                      uint16_t color) {
  const int thickness = max(2, width / 5);
  const int half = height / 2;
  const int verticalHeight = half - thickness;
  const uint8_t segments = DIGIT_SEGMENTS[digit];

  auto horizontal = [&](int sy) {
    screen.fillRoundRect(x + thickness / 2, sy, width - thickness, thickness,
                         thickness / 2, color);
  };
  auto vertical = [&](int sx, int sy) {
    screen.fillRoundRect(sx, sy + thickness / 2, thickness,
                         verticalHeight - thickness / 2, thickness / 2, color);
  };

  if (segments & (1 << 0)) horizontal(y);
  if (segments & (1 << 1)) vertical(x + width - thickness, y);
  if (segments & (1 << 2)) vertical(x + width - thickness, y + half);
  if (segments & (1 << 3)) horizontal(y + height - thickness);
  if (segments & (1 << 4)) vertical(x, y + half);
  if (segments & (1 << 5)) vertical(x, y);
  if (segments & (1 << 6)) horizontal(y + half - thickness / 2);
}

void drawSessionBar() {
  screen.fillRect(0, 0, 128, 7, backgroundColor());
  const uint8_t currentWork =
      completedWorkSessions < 4 ? completedWorkSessions + 1 : 4;
  const uint8_t filled =
      longBreak ? 0
                : (phase == Phase::Work ? currentWork
                                        : completedWorkSessions);
  constexpr int x0 = 2;
  constexpr int width = 29;
  constexpr int gap = 3;
  for (uint8_t i = 0; i < 4; ++i) {
    const int x = x0 + i * (width + gap);
    if (i < filled) {
      screen.fillRect(x, 0, width, 7, dimColor());
    } else {
      screen.drawRect(x, 0, width, 7, dimColor());
    }
  }
}

void drawHeader() {
  // Rows 7-8 form the requested 2-pixel gap below the session bar.
  screen.fillRect(0, 7, 128, 27, backgroundColor());
  screen.setTextDatum(TC_DATUM);
  screen.setTextColor(dimColor(), backgroundColor());
  const char* title =
      phase == Phase::Work ? "Focus" : (longBreak ? "Time Off" : "Break");
  screen.drawString(title, 64, 9, 4);
}

void drawStatusIcon() {
  // Keep the state indicator directly above the right-aligned seconds.
  screen.fillRect(98, 43, 20, 15, backgroundColor());
  const uint16_t color = dimColor();
  if (running) {
    screen.fillTriangle(103, 45, 103, 56, 113, 50, color);
  } else if (stopped) {
    screen.fillRect(103, 45, 11, 11, color);
  } else {
    screen.fillRect(103, 45, 3, 11, color);
    screen.fillRect(111, 45, 3, 11, color);
  }
}

void drawTime() {
  screen.fillRect(0, 34, 128, 67, backgroundColor());
  const uint32_t minutes = remainingSeconds / 60;
  const uint32_t seconds = remainingSeconds % 60;
  constexpr int minuteW = 25;
  constexpr int minuteH = 51;
  constexpr int secondW = 16;
  constexpr int secondH = 34;
  constexpr int y = 45;
  constexpr int secondY = y + 17;

  drawSegmentDigit(5, y, minuteW, minuteH, minutes / 10, brightColor());
  drawSegmentDigit(34, y, minuteW, minuteH, minutes % 10, brightColor());
  // Center the colon in the gap between minute x=59 and second x=87.
  constexpr int colonX = (59 + 87) / 2;
  screen.fillCircle(colonX, secondY + 9, 2, dimColor());
  screen.fillCircle(colonX, secondY + 25, 2, dimColor());
  drawSegmentDigit(87, secondY, secondW, secondH, seconds / 10, dimColor());
  drawSegmentDigit(108, secondY, secondW, secondH, seconds % 10, dimColor());
  drawStatusIcon();
}

void drawBottomBar(bool showTimesUp) {
  screen.fillRect(0, 105, 128, 23, backgroundColor());
  if (showTimesUp) {
    screen.fillRoundRect(3, 108, 122, 17, 3, dimColor());
    screen.setTextDatum(MC_DATUM);
    screen.setTextColor(backgroundColor(), dimColor());
    screen.drawString("Time's Up", 64, 116, 2);
    return;
  }

  const uint8_t lit =
      totalSeconds == 0
          ? 0
          : static_cast<uint8_t>((remainingSeconds * 10U + totalSeconds - 1U) /
                                 totalSeconds);
  constexpr int width = 10;
  constexpr int gap = 2;
  constexpr int x0 = 5;
  for (uint8_t i = 0; i < 10; ++i) {
    const int x = x0 + i * (width + gap);
    screen.drawRoundRect(x, BOTTOM_BAR_Y, width, BOTTOM_BAR_HEIGHT, 2,
                         dimColor());
    if (i < lit) {
      screen.fillRoundRect(x + 2, BOTTOM_BAR_Y + 2, width - 4,
                           BOTTOM_BAR_HEIGHT - 4, 1, dimColor());
    }
  }
}

void drawScreen(uint32_t now, bool force = false) {
  if (qrVisible || backlightSleeping) return;
  const bool showTimesUp =
      timesUpUntil != 0 && static_cast<int32_t>(timesUpUntil - now) > 0;
  if (!force && remainingSeconds == lastDrawnSeconds &&
      showTimesUp == lastTimesUpVisible) {
    return;
  }
  screen.fillSprite(backgroundColor());
  drawSessionBar();
  drawHeader();
  drawTime();
  drawBottomBar(showTimesUp);
  // Present the complete frame in one transfer so intermediate clears are
  // never visible on the physical LCD.
  screen.pushSprite(0, 0);
  lastDrawnSeconds = remainingSeconds;
  lastTimesUpVisible = showTimesUp;
}

void prepareWork(bool resetSessions) {
  phase = Phase::Work;
  running = false;
  countdownActive = false;
  stopped = true;
  longBreak = false;
  waitingAfterBreak = false;
  if (resetSessions) completedWorkSessions = 0;
  totalSeconds = configuredWorkSeconds;
  remainingSeconds = configuredWorkSeconds;
  timesUpUntil = 0;
  lastDrawnSeconds = UINT32_MAX;
}

void resetCountdown() { prepareWork(true); }

void beginRest(uint32_t now) {
  ++completedWorkSessions;
  longBreak = completedWorkSessions >= 4;
  phase = Phase::Rest;
  running = true;
  countdownActive = true;
  totalSeconds = longBreak ? longBreakSeconds : shortBreakSeconds;
  remainingSeconds = totalSeconds;
  nextTickAt = now + 1000;
  timesUpUntil = now + TIMES_UP_MS;
  lastDrawnSeconds = UINT32_MAX;
}

void finishRest(uint32_t now) {
  const bool completedLongBreak = longBreak;
  if (autoStartAfterBreak) {
    prepareWork(completedLongBreak);
    running = true;
    countdownActive = true;
    stopped = false;
    nextTickAt = now + 1000;
    return;
  }

  // Remain on the completed break at 00:00 and invert the whole frame until
  // the left button explicitly starts the next work session.
  running = false;
  countdownActive = true;
  stopped = true;
  waitingAfterBreak = true;
  remainingSeconds = 0;
  timesUpUntil = 0;
  lastDrawnSeconds = UINT32_MAX;
}

void changeWorkMinutes(int direction) {
  if (phase != Phase::Work || countdownActive) return;
  int minutes = static_cast<int>(configuredWorkSeconds / 60);
  minutes += direction * adjustmentMinutes;
  if (minutes > workLimitMinutes) minutes = 5;
  if (minutes < 5) minutes = 60;
  if (minutes > workLimitMinutes) minutes = workLimitMinutes;
  configuredWorkSeconds = static_cast<uint32_t>(minutes) * 60U;
  totalSeconds = configuredWorkSeconds;
  remainingSeconds = configuredWorkSeconds;
  // A manual adjustment becomes the new startup work time.
  preferences.putUShort("workMinutes", minutes);
  lastDrawnSeconds = UINT32_MAX;
}

void drawHotspotQr() {
  constexpr int MAX_VERSION = 5;
  uint8_t temp[qrcodegen_BUFFER_LEN_FOR_VERSION(MAX_VERSION)];
  uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(MAX_VERSION)];
  const char* wifiQr = "WIFI:T:nopass;S:MiniPomodoro;;";
  if (!qrcodegen_encodeText(wifiQr, temp, qr, qrcodegen_Ecc_LOW, 1,
                            MAX_VERSION, qrcodegen_Mask_AUTO, true)) {
    return;
  }

  const int size = qrcodegen_getSize(qr);
  const int scale = size <= 33 ? 3 : 2;
  const int qrPixels = size * scale;
  const int x0 = (128 - qrPixels) / 2;
  const int y0 = 13;  // Includes a scanner-friendly white quiet zone.
  tft.fillScreen(TFT_WHITE);
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      if (qrcodegen_getModule(qr, x, y)) {
        tft.fillRect(x0 + x * scale, y0 + y * scale, scale, scale, TFT_BLACK);
      }
    }
  }
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.drawString(AP_SSID, 64, y0 + qrPixels + 3, 1);
  tft.drawString("192.168.4.1", 64, y0 + qrPixels + 13, 1);
  qrVisible = true;
}

void handleButtons(uint32_t now) {
  startButton.update(now);
  setButton.update(now);
  const bool anyPressed = startButton.isPressed() || setButton.isPressed();

  if (backlightSleeping && anyPressed) {
    wakeBacklight(now);
    chordActive = true;  // The wake-up press does not perform another action.
  }

  if (chordActive) {
    startButton.takeShortPress();
    startButton.takeLongPress();
    setButton.takeShortPress();
    setButton.takeLongPress();
    if (!anyPressed) chordActive = false;
    return;
  }

  if (qrVisible && anyPressed) {
    qrVisible = false;
    tft.fillScreen(BG);
    lastDrawnSeconds = UINT32_MAX;
    chordActive = true;
    return;
  }

  if (hotspotActive && startButton.isPressed() && setButton.isPressed()) {
    drawHotspotQr();
    chordActive = true;
    return;
  }

  const bool startShort = startButton.takeShortPress();
  const bool setShort = setButton.takeShortPress();
  startButton.takeLongPress();  // IO0 deliberately has no long-press action.

  if (startShort) {
    lastActivityAt = now;
    if (waitingAfterBreak) {
      const bool completedLongBreak = longBreak;
      prepareWork(completedLongBreak);
      running = true;
      countdownActive = true;
      stopped = false;
      nextTickAt = now + 1000;
      lastDrawnSeconds = UINT32_MAX;
      return;
    }
    running = !running;
    if (running) {
      countdownActive = true;
      stopped = false;
      nextTickAt = now + 1000;
      timesUpUntil = 0;
    }
    lastDrawnSeconds = UINT32_MAX;
  }

  if (setButton.takeLongPress()) {
    lastActivityAt = now;
    if (countdownActive || phase == Phase::Rest) {
      resetCountdown();
    } else {
      changeWorkMinutes(-1);
    }
  } else if (setShort) {
    lastActivityAt = now;
    changeWorkMinutes(1);
  }
}

void updateTimer(uint32_t now) {
  if (!running) return;
  while (remainingSeconds > 0 &&
         static_cast<int32_t>(now - nextTickAt) >= 0) {
    --remainingSeconds;
    nextTickAt += 1000;
  }
  if (remainingSeconds == 0) {
    if (phase == Phase::Work) {
      beginRest(now);
    } else {
      finishRest(now);
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  startButton.begin();
  setButton.begin();
  loadSettings();

  tft.init();
  tft.setRotation(0);
  screen.setColorDepth(16);
  screen.createSprite(128, 128);
  screen.fillSprite(BG);
  // Attach PWM after TFT initialization, which otherwise forces BL fully on.
  ledcSetup(BACKLIGHT_PWM_CHANNEL, BACKLIGHT_PWM_FREQUENCY,
            BACKLIGHT_PWM_BITS);
  ledcAttachPin(BACKLIGHT_PIN, BACKLIGHT_PWM_CHANNEL);
  setBacklight(brightnessPercent);
  tft.fillScreen(BG);
  const uint32_t now = millis();
  lastActivityAt = now;
  startHotspot();
  drawScreen(now, true);
}

void loop() {
  const uint32_t now = millis();
  if (hotspotActive) {
    webServer.handleClient();
    if (now - hotspotStartedAt >= HOTSPOT_LIFETIME_MS) stopHotspot();
  }
  handleButtons(now);
  updateTimer(now);
  if (running) {
    lastActivityAt = now;
  } else if (!backlightSleeping && now - lastActivityAt >= BACKLIGHT_IDLE_MS) {
    ledcWrite(BACKLIGHT_PWM_CHANNEL, 255);
    backlightSleeping = true;
  }
  drawScreen(now);
  delay(5);
}
