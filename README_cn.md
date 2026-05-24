# 3drag — Windows 三指拖拽工具

**[English](README.md)**

在精确触摸板（PTP）上 **三指按压并移动** 即可 **左键拖拽**。单个 `3drag.exe`，纯命令行，后台运行，无窗口、无托盘。

## 功能

- 三指拖拽（仅 Precision Touchpad）
- 抬起任意一指即结束拖拽
- 静默后台（exe 约 30–40 KB，无额外 DLL）
- 可调移动速度；可选开机自启
- 改配置后 `reload` 即时生效，无需重启

**要求：** Windows 10+ 且为精确触摸板。须先关闭系统自带三指手势（见下文）。

## 安装

```powershell
winget install --id nobu121.win3drag
```

WinGet 会把 `3drag` 加入 PATH；**重新打开终端**后执行 `3drag start`。

## 使用

**不要双击 exe**。在终端中执行：

```
3drag start    后台启动
3drag stop     停止
3drag reload   重新加载配置
3drag status   查询是否在运行
3drag config   用默认编辑器打开配置
3drag help     帮助
```

退出码：成功 0；未运行（`status` / `reload`）1；未知命令 2。

**改配置：** `3drag config` → 保存 → `3drag reload`

配置路径：`%APPDATA%\3drag\3drag.ini`（`debug = 1` 时写 `3drag.log`）

```ini
[general]
sensitivity = 120     ; 100 = 1:1；越小越慢，越大越快
dt_clamp    = 50      ; 减轻指针乱跳；0 关闭
debug       = 0       ; 1 = 写调试日志
autostart   = 1       ; 1 = 开机自启
```

## 关闭 Windows 三指手势

必做，否则系统会先拦截手势，本工具收不到。

1. **设置**（Win+I）→ **蓝牙和其他设备** → **触摸板**
2. **三指手势** → **轻扫**、**点击** 均设为 **无**

## 编译

PATH 中有 MinGW-w64 的 `g++`，然后：

```bat
build.bat
```

## 许可

[MIT](LICENSE)
