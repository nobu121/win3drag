# 3drag

**[中文说明](README_cn.md)**

Windows utility: **three-finger press-and-drag** on a Precision Touchpad performs a **left-button mouse drag**. Single `3drag.exe`, CLI-only, runs in the background with no window or tray icon.

## Features

- Three-finger drag on Precision Touchpads (PTP)
- Lifting any finger ends the drag
- Runs silently in the background (~30–40 KB exe, no extra DLLs)
- Adjustable speed; optional logon autostart
- Config hot-reload without restart

**Requirements:** Windows 10+ with a Precision Touchpad. Disable Windows built-in three-finger gestures first (see below).

## Install

```powershell
winget install --id nobu121.win3drag
```

WinGet adds `3drag` to PATH; open a **new** terminal, then run `3drag start`.

## Usage

Do **not** double-click the exe. Run from a terminal:

```
3drag start    Start in background
3drag stop     Stop
3drag reload   Reload config
3drag status   Check if running
3drag config   Open config in default editor
3drag help     Help
```

Exit codes: `0` OK; `1` not running (`status` / `reload`); `2` unknown command.

**Edit config:** `3drag config` → save → `3drag reload`

Config path: `%APPDATA%\3drag\3drag.ini` (log: `3drag.log` when `debug = 1`)

```ini
[general]
sensitivity = 120     ; 100 = 1:1; lower = slower, higher = faster
dt_clamp    = 50      ; reduce pointer jumps; 0 = off
debug       = 0       ; 1 = write debug log
autostart   = 1       ; 1 = run at logon
```

## Disable Windows three-finger gestures

Required, or Windows will capture the gesture before 3drag sees it.

1. **Settings** (Win+I) → **Bluetooth & devices** → **Touchpad**
2. **Three-finger gestures** → set **Swipe** and **Tap** to **Nothing**

## Build

MinGW-w64 `g++` on PATH, then:

```bat
build.bat
```

## License

[MIT](LICENSE)
