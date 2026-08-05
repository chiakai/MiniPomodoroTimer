# Mini Pomodoro Timer for LilyGo T-QT Pro

[中文版 README](README.md)

## Screenshots

| Work countdown | Countdown and status display |
|---|---|
| ![Mini Pomodoro Timer work countdown](image/Countdown1.jpg) | ![Mini Pomodoro Timer running countdown](image/Countdown2.jpg) |

Mini Pomodoro Timer is a Pomodoro timer firmware designed for the
[LilyGo T-QT Pro](https://github.com/Xinyuan-LilyGO/T-QT), using its ESP32-S3
and onboard 128×128 GC9A01 color LCD.

The project provides configurable work sessions, short and long breaks,
seven-segment countdown digits, cycle and time progress indicators, persistent
Web settings, automatic backlight sleep, and a temporary setup hotspot.

## Features

- Default 25-minute work session.
- Configurable work duration and maximum work duration.
- Configurable Right-button adjustment step, defaulting to 5 minutes.
- Five-minute short break after each of the first three work sessions.
- Twenty-minute long break after every fourth work session.
- Optional automatic start after a break.
- Work time adjusted with the Right button is remembered across restarts.
- Seven-segment minute and second display.
- Ten-segment remaining-time indicator at the bottom.
- Four-segment session indicator at the top.
- Play, pause, and stop status icons.
- `Time's Up` notification for five seconds.
- Ten-step PWM backlight control from 10% to 100%.
- Backlight turns off after ten minutes without an active countdown.
- Settings are stored in ESP32 NVS and survive reboot.
- A three-minute setup hotspot starts on every boot.
- An LCD QR code makes it easy to join the setup hotspot.
- A 128×128 off-screen sprite presents each frame atomically to prevent
  visible flicker during second changes.

## Hardware

| Item | Configuration |
|---|---|
| Board | LilyGo T-QT Pro N4R2 |
| MCU | ESP32-S3 |
| Flash / PSRAM | 4 MB / 2 MB |
| LCD | GC9A01, 128×128 |
| Start/pause button | Left button (GPIO 0) |
| Set/reset button | Right button (GPIO 47) |
| LCD reset | IO1 |
| Backlight | IO10, active-low PWM |

GPIO 1 is connected to LCD Reset and cannot be used as a button. Button
combinations therefore use the Left and Right buttons.

## Controls

### Left button

- Short press: start the countdown.
- Short press while running: pause.
- Short press while paused: resume.
- Hold for two seconds: rotate the display by 180 degrees and swap the Left and
  Right button roles.
- After rotation, the original Right button is the new Left button; hold it for
  two seconds to return to the normal orientation.

### Right button

Before a work countdown has started:

- Short press: increase work duration by the configured adjustment step.
- Hold for two seconds: decrease work duration by the configured step.
- The adjusted work time is saved automatically and restored at the next boot.
- The default step is five minutes and can be changed in the Web interface.
- The duration wraps between five minutes and the configured maximum.

During an active work/break cycle:

- Hold for two seconds to reset the complete cycle.
- The configured work duration is preserved.

### Left + Right buttons

While the boot hotspot is active, press both buttons together to show:

- A QR code for joining the `MiniPomodoro` open hotspot.
- The Web address `192.168.4.1`.

Press either button to leave the QR screen.

## Pomodoro Cycle

1. The device boots in a stopped state with the configured work duration.
2. Press the Left button to start work.
3. A five-minute short break starts automatically after work ends.
4. After a short break, the `Break 00:00` screen remains with a dim-red
   background and black foreground.
5. Press the Left button to switch to work and start counting down immediately.
6. After the fourth work session, a twenty-minute long break starts.
7. The long break remains on a dim-white background with a black
   `Time Off 00:00` foreground; pressing
   the Left button resets the cycle counter and immediately starts session one.

The Web option “Auto-start work after a break” bypasses the color-swapped `00:00`
waiting screen.

## Display

### Session bar

- Seven pixels high, with a two-pixel gap before the title.
- Divided into four equal outlined segments.
- Session n displays the first n segments filled.
- All segments are empty during a long break.

### Titles

| Phase | Title |
|---|---|
| Work | `Focus` |
| Short break | `Break` |
| Long break | `Time Off` |

### Colors

| Phase | Minutes | Other elements |
|---|---|---|
| Work | Bright green | Dim green |
| Short break | Bright red | Dim red |
| Long break | Bright white | Dim white/gray |

The supported old-panel batch swaps red and blue channels. The firmware
compensates for this in its color constants.

## Default Settings

| Setting | Default |
|---|---:|
| Backlight brightness | 90% |
| Work duration | 25 minutes |
| Maximum work duration | 60 minutes |
| Right-button adjustment step | 5 minutes |
| Short break | 5 minutes |
| Long break | 20 minutes |
| Auto-start after break | Off |
| Backlight idle timeout | 10 minutes |
| Hotspot lifetime | 3 minutes |

The Web `Reset` button restores these values.

## Web Interface

The following open hotspot starts on every boot:

| Item | Value |
|---|---|
| SSID | `MiniPomodoro` |
| Password | None |
| Settings page | `http://192.168.4.1` |
| Lifetime | Three minutes after boot |

The settings page title is `Pomodoro Timer`. It can configure:

- Backlight brightness in 10% steps.
- Right-button adjustment step in minutes.
- Maximum work duration.
- Work duration.
- Short-break duration.
- Long-break duration.
- Automatic work start after a break.

Press `Save` to store and apply settings. Press `Reset` to restore defaults.
Wi-Fi is fully disabled when the three-minute window expires and does not
restart until the next boot.

## Backlight Sleep

- The default brightness is 90%.
- The backlight turns off after ten minutes while stopped or paused.
- It remains on during work and break countdowns.
- Either button wakes the display.
- The wake-up press is consumed and does not also start, adjust, or reset.

## Flicker-free Rendering

Earlier versions cleared the time, title, and progress regions directly on the
LCD before redrawing them every second. The brief black interval between the
clear and redraw operations could appear as a visible flicker, particularly
while Wi-Fi activity delayed individual drawing operations.

The current firmware uses a 128×128, 16-bit `TFT_eSprite` off-screen buffer:

1. Each frame is cleared and fully rendered in memory.
2. Digits, titles, icons, and progress bars are completed before presentation.
3. `pushSprite()` transfers the complete frame to the LCD in one operation, so
   intermediate drawing states are never visible.

The sprite consumes approximately 32 KB of RAM, which is well within the
T-QT Pro's available memory. The hotspot QR code is a separate screen and is
still rendered directly to the LCD.

## LCD Configuration

| Item | Value |
|---|---|
| Driver | GC9A01 |
| Resolution | 128×128 |
| SPI controller / frequency | HSPI / 40 MHz |
| MOSI / SCLK | IO2 / IO3 |
| CS / DC / Reset | IO5 / IO6 / IO1 |
| Panel profile | LilyGo `old_panel` |
| CGRAM offset | `(0,0)` |

The zero CGRAM offset removes the one-pixel top artifact and two-pixel left
artifact found on this panel batch.

## Build

The project uses PlatformIO and the Arduino framework. Clone LilyGo's official
T-QT repository before the first build:

```powershell
git clone --depth 1 https://github.com/Xinyuan-LilyGO/T-QT.git vendor/T-QT
pio run
```

The pre-build script selects the old-panel driver, applies the CGRAM
calibration, and extracts the lightweight QR encoder bundled with LVGL.

## Upload

```powershell
pio run --target upload
```

To specify a port:

```powershell
pio run --target upload --upload-port COM6
```

## Project Layout

```text
MiniPomodoroTimer/
├── platformio.ini
├── README.md
├── README_EN.md
├── scripts/
│   └── use_old_panel.py
├── lib/
│   └── qrcodegen/      # Generated at build time
├── src/
│   └── main.cpp
└── vendor/
    └── T-QT/           # Official LilyGo driver; not tracked
```

## Credits

This project uses LilyGo's official T-QT hardware definitions and modified
TFT_eSPI display driver.

Requirements analysis, firmware design, debugging, display calibration,
documentation, and firmware uploads for this project were completed with
assistance from OpenAI Codex.
