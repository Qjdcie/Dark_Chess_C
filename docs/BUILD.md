# Dark Chess (暗棋) — 編譯指南

## 目錄

- [Linux 編譯](#linux-編譯)
- [Windows 編譯](#windows-編譯)
  - [方法一：Linux 交叉編譯（推薦）](#方法一linux-交叉編譯推薦)
  - [方法二：MSYS2 + MinGW（Windows 本機）](#方法二msys2--mingwwindows-本機)
  - [方法三：Visual Studio (MSVC)](#方法三visual-studio-msvc)

---

## Linux 編譯

### 安裝依賴

**Arch Linux：**

```bash
sudo pacman -S gcc raylib pkg-config
```

**Ubuntu / Debian：**

```bash
sudo apt install gcc libraylib-dev pkg-config
```

> 如果套件庫沒有 raylib，可從原始碼編譯：
> https://github.com/raysan5/raylib/wiki/Working-on-GNU-Linux

### 編譯

```bash
cd teamwork_v1
make
```

執行檔產出在 `build/dark_chess_gui`。

### 執行

```bash
./build/dark_chess_gui
```

---

## Windows 編譯

### 方法一：Linux 交叉編譯（推薦）

在 Linux 上直接編譯出 Windows `.exe`，不需要 Windows 環境。

#### 1. 安裝 MinGW-w64 交叉編譯器

**Arch Linux：**

```bash
sudo pacman -S mingw-w64-gcc
```

**Ubuntu / Debian：**

```bash
sudo apt install gcc-mingw-w64-x86-64
```

#### 2. 下載 raylib MinGW 預編譯包

從 [raylib releases](https://github.com/raysan5/raylib/releases) 下載：

```
raylib-*_win64_mingw-w64.zip
```

解壓到任意路徑，例如 `~/raylib-mingw64`，確認結構如下：

```
raylib-mingw64/
├── include/
│   └── raylib.h
└── lib/
    └── libraylib.a
```

#### 3. 編譯

```bash
cd teamwork_v1
make win RAYLIB_MINGW_DIR=~/raylib-mingw64
```

產出 `build/dark_chess_gui.exe`，靜態連結，不需要額外 DLL。

---

### 方法二：MSYS2 + MinGW（Windows 本機）

#### 1. 安裝 MSYS2

從 https://www.msys2.org 下載並安裝。

#### 2. 安裝編譯工具

打開 **MSYS2 MinGW64** 終端（不是普通的 MSYS2 終端），執行：

```bash
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-raylib mingw-w64-x86_64-pkg-config make
```

#### 3. 編譯

在同一個 MinGW64 終端中：

```bash
cd /path/to/teamwork_v1
make
```

產出 `build/dark_chess_gui.exe`。

---

### 方法三：Visual Studio (MSVC)

#### 1. 安裝 Visual Studio

安裝 Visual Studio（Community 即可），勾選「使用 C++ 的桌面開發」。

#### 2. 下載 raylib

從 [raylib releases](https://github.com/raysan5/raylib/releases) 下載 MSVC 版本：

```
raylib-*_win64_msvc16.zip
```

解壓到例如 `C:\raylib`。

#### 3. 編譯

打開 **Developer Command Prompt for VS**，執行：

```cmd
cd teamwork_v1

cl /O2 /Isrc\core /Isrc\gui /IC:\raylib\include ^
   src\gui_main.c src\core\board.c src\gui\raylib_gui.c ^
   /Fe:dark_chess_gui.exe ^
   /link /LIBPATH:C:\raylib\lib raylib.lib ^
   opengl32.lib gdi32.lib winmm.lib user32.lib shell32.lib
```

> 將 `C:\raylib` 替換為你實際的 raylib 解壓路徑。
