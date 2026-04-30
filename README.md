# Dark Chess (暗棋)

以 C 語言實作的暗棋遊戲，支援本機對 AI 與線上雙人對戰。

## 功能

- GUI 模式：基於 [Raylib](https://www.raylib.com/) 的圖形介面
- AI 對手：三層啟發式引擎 + PVS 搜尋
- 線上對戰：TCP 連線，可選 GUI 或純命令列（無頭）模式
- 跨平台：Linux / macOS / Windows（MSYS2 或交叉編譯）

## 快速開始

### 安裝依賴並編譯

**Arch Linux**
```bash
sudo pacman -S gcc raylib pkg-config
make
```

**Ubuntu / Debian**
```bash
sudo apt install gcc libraylib-dev pkg-config
make
```

產出：`build/dark_chess_gui`

詳細編譯說明（Windows、MSYS2、MSVC）請參閱 [docs/BUILD.md](docs/BUILD.md)。

### 執行

```bash
# 本機對 AI
./build/dark_chess_gui

# 線上對戰（GUI）
./build/dark_chess_gui -online_battle <room_id>

# 線上對戰（無 GUI，純命令列）
./build/dark_chess_gui -ngui -online_battle <room_id>
```

`-ngui` 必須與 `-online_battle` 同時使用，參數順序不限。

### 對戰 Log

無 GUI 模式每局結束後自動寫入：

```
logs/battle_YYYYMMDD_HHMMSS_<room_id>.log
```

```
[2026-04-30 12:00:00] room=room42 role=A
color=Red
#1   ME   FLIP (2,3)->R_Soldier  R16B16
#2   OPP  FLIP (0,5)->B_Cannon   R16B16
#3   ME   CAPTURE (2,3)->(0,5) R_Soldier x B_Cannon  R16B15
...
RESULT=WIN moves=47 R8B0
```

`logs/` 目錄若不存在會自動建立。

## 遊戲規則

- 4×8 棋盤，32 枚棋子（紅黑各 16），全部蓋面朝下開局
- 階級高低：將(7) > 士(6) > 象(5) > 車(4) > 馬(3) > 炮(2) > 兵(1)
- 特殊規則：兵可吃將；將不能吃兵；炮需隔一枚棋子才能吃子
- 50 手無吃子或翻棋判和

## 架構

```
src/
├── gui_main.c          — 入口，解析 -online_battle / -ngui 旗標
├── core/
│   ├── board.h/c       — 棋盤狀態、走法生成、勝負判定
│   ├── strategy.h/c    — AI 引擎（卡牌計數、PVS 搜尋、Zobrist hash）
│   └── net_client.c    — TCP 連線、UPDATE 協議解析
└── gui/
    ├── raylib_gui.h/c  — Raylib 渲染與輸入
    ├── cli_battle.h/c  — 無 GUI 對戰主迴圈
    └── embedded_font.h — 內嵌字型資料
```

## 文件

- [docs/BUILD.md](docs/BUILD.md) — 完整的編譯說明
