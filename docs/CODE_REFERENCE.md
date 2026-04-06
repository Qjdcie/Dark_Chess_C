# 暗棋 AI — 代碼完整參考手冊

> **版本：** 2026-04-06
> **涵蓋原始碼：** `src/core/board.h`、`src/core/board.c`、`src/core/strategy.h`、`src/core/strategy.c`

---

## 目錄

1. [專案結構總覽](#1-專案結構總覽)
2. [游戲規則摘要](#2-遊戲規則摘要)
3. [核心型別與常數（board.h）](#3-核心型別與常數)
4. [棋盤模組（board.c）](#4-棋盤模組)
5. [座標系統（必讀）](#5-座標系統必讀)
6. [策略模組架構（strategy.c）](#6-策略模組架構)
7. [全域狀態變數](#7-全域狀態變數)
8. [資料結構詳解](#8-資料結構詳解)
9. [工具函數](#9-工具函數)
10. [動態棋子價值系統](#10-動態棋子價值系統)
11. [算牌系統](#11-算牌系統)
12. [威脅偵測函數](#12-威脅偵測函數)
13. [砲線分析函數](#13-砲線分析函數)
14. [吃子分析函數](#14-吃子分析函數)
15. [情勢評估系統](#15-情勢評估系統)
16. [AI 主流程](#16-ai-主流程)
17. [常用代碼模式](#17-常用代碼模式)
18. [新增策略功能指南](#18-新增策略功能指南)
19. [編譯與執行](#19-編譯與執行)

---

## 1. 專案結構總覽

```
Dark_Chess_nS_GUI/
│
├── src/
│   ├── core/
│   │   ├── board.h        ← 所有共用型別定義、巨集、棋盤 API 宣告
│   │   ├── board.c        ← 棋盤規則實作（初始化、走法產生、執行、勝負判定）
│   │   ├── strategy.h     ← 策略模組對外 API（只有 2 個函數）
│   │   └── strategy.c     ← AI 策略實作（算牌、威脅、情勢評估、決策主流程）
│   │
│   ├── gui/
│   │   ├── raylib_gui.h   ← GUI 模組對外介面
│   │   ├── raylib_gui.c   ← Raylib 繪圖與滑鼠事件處理
│   │   └── embedded_font.h← 內嵌字型資料（自動產生，勿手動編輯）
│   │
│   └── gui_main.c         ← 程式入口，遊戲主迴圈
│
├── docs/
│   ├── BUILD.md               ← 編譯環境設置說明
│   ├── CODE_REFERENCE.md      ← 本文件（代碼完整參考手冊）
│   └── AI_STRATEGY_IMPLEMENTATION.md  ← 進階策略設計文件（未來擴展用）
│
├── Makefile               ← 建置系統
└── build/                 ← 編譯輸出目錄
```

### 模組依賴關係

```
gui_main.c
    ├── board.h / board.c    (棋盤規則)
    ├── strategy.h / strategy.c  (AI 策略，內部引用 board.h)
    └── gui/raylib_gui.h / .c    (畫面顯示，內部引用 board.h)
```

**strategy.c 只能讀取 GameState，不能修改它。** 所有走法透過
`board_apply_move()` 由外部執行，strategy 只負責「選哪步」。

---

## 2. 遊戲規則摘要

| 項目 | 說明 |
|------|------|
| 棋盤 | 4 列 × 8 行，共 32 格 |
| 棋子 | 每方 16 顆，共 32 顆，開局全部蓋牌（`STATE_FACEDOWN`） |
| 階級（由大到小） | 將/帥 > 士/仕 > 象/相 > 車/俥 > 馬/傌 > 炮/砲 > 兵/卒 |
| 吃子規則 | 大吃小；**兵可以吃將（特例）；將不能吃兵** |
| 砲的吃法 | 跳過一顆棋子（砲架，任意方）後才能吃第二顆；普通移動不用跳 |
| 走法類型 | 翻棋（FLIP）、移動到空格（WALK）、吃子（CAPTURE） |
| 和棋條件 | 連續 50 回合無吃子（`no_capture_turns >= MAX_NO_CAPTURE`） |
| 勝負 | 一方所有棋子被吃光；或對方無合法走法 |

---

## 3. 核心型別與常數

> 定義於 `src/core/board.h`，整個專案都可以引用。

### 3.1 列舉型別

```c
/* 陣營 */
typedef enum {
    SIDE_NONE  = 0,   /* 空格 */
    SIDE_RED   = 1,   /* 紅方（玩家） */
    SIDE_BLACK = 2    /* 黑方（電腦 AI） */
} Side;

/* 棋子階級 */
typedef enum {
    RANK_NONE    = 0,
    RANK_SOLDIER = 1,   /* 兵 / 卒 */
    RANK_CANNON  = 2,   /* 炮 / 砲 */
    RANK_HORSE   = 3,   /* 馬 / 傌 */
    RANK_CHARIOT = 4,   /* 車 / 俥 */
    RANK_ELEPHANT= 5,   /* 象 / 相 */
    RANK_ADVISOR = 6,   /* 士 / 仕 */
    RANK_GENERAL = 7    /* 將 / 帥 */
} PieceRank;

/* 格子狀態 */
typedef enum {
    STATE_EMPTY    = 0,  /* 空格 */
    STATE_FACEDOWN = 1,  /* 蓋牌（未翻） */
    STATE_FACEUP   = 2   /* 翻開 */
} PieceState;

/* 走法類型 */
typedef enum {
    MOVE_FLIP    = 0,   /* 翻一顆蓋牌棋子 */
    MOVE_WALK    = 1,   /* 移動到相鄰空格 */
    MOVE_CAPTURE = 2    /* 吃相鄰敵方棋子 */
} MoveType;
```

### 3.2 結構

```c
/* 單一格子 */
typedef struct {
    Side       side;   /* SIDE_NONE 表示空 */
    PieceRank  rank;   /* RANK_NONE 表示空 */
    PieceState state;  /* 空 / 蓋牌 / 翻開 */
} Cell;

/* 單一走法 */
typedef struct {
    MoveType type;
    int from_r, from_c;  /* 起始位置，1-based */
    int to_r, to_c;      /* 目標位置，1-based（FLIP 時與 from 相同） */
} Move;

/* 遊戲狀態（整盤棋的完整資訊） */
typedef struct {
    Cell cells[BOARD_ROWS][BOARD_COLS]; /* cells[0..3][0..7]，0-based */
    Side current_turn;                  /* 現在輪到哪方 */
    int  red_alive;                     /* 紅方存活棋子數 */
    int  black_alive;                   /* 黑方存活棋子數 */
    int  game_over;                     /* 1 = 遊戲結束 */
    Side winner;                        /* 勝方（SIDE_NONE = 和棋） */
    int  no_capture_turns;              /* 連續未吃子回合數（和棋計時器） */
} GameState;
```

### 3.3 重要常數

```c
#define BOARD_ROWS    4
#define BOARD_COLS    8
#define BOARD_SIZE    (BOARD_ROWS * BOARD_COLS)   /* 總格子數 = 32 */
#define PIECES_PER_SIDE 16    /* 每方棋子數 */
#define MAX_MOVES     256     /* board_generate_moves 的緩衝區最小大小 */
#define MAX_NO_CAPTURE 50     /* 超過這個回合數判和棋 */
```

### 3.4 巨集

```c
/* 取得格子指標（輸入 1-based，內部轉 0-based） */
CELL(gs, r, c)         → const Cell *

/* 邊界檢查（1-based） */
IN_BOUNDS(r, c)        → 1 = 合法，0 = 超出

/* 1-based 座標 → 0-based 線性索引（0–31） */
RC_TO_IDX(r, c)        → int

/* 0-based 線性索引 → 1-based 行 / 列 */
IDX_TO_ROW(i)          → int
IDX_TO_COL(i)          → int
```

---

## 4. 棋盤模組

> 實作於 `src/core/board.c`，宣告於 `src/core/board.h`。

### `board_init(gs)`

```c
void board_init(GameState *gs);
```

初始化新遊戲：將 32 顆棋子（紅 16 + 黑 16）**隨機打散**放在棋盤上，
全部設為 `STATE_FACEDOWN`。清除所有計數器與勝負旗標。

**何時呼叫：** 遊戲開始或按 R 重新開始時。`strategy_reset()` 也應同步呼叫。

---

### `board_generate_moves(gs, moves)`

```c
int board_generate_moves(const GameState *gs, Move *moves);
```

產生 `gs->current_turn` 這方**所有合法走法**，填入 `moves[]`，
回傳走法數量。

- `moves` 陣列長度至少要 `MAX_MOVES`（256）
- 不修改 `gs`
- 回傳 0 表示無合法走法（應判輸）

```c
Move legal[MAX_MOVES];
int n = board_generate_moves(gs, legal);
// legal[0..n-1] 是所有合法走法
```

---

### `board_apply_move(gs, mv)`

```c
int board_apply_move(GameState *gs, const Move *mv);
```

執行走法，修改 `gs`：更新棋盤、切換回合、更新存活計數、
呼叫 `board_check_game_over()`。

- 回傳 1 = 成功，0 = 走法非法
- **會直接修改 gs**，模擬請先複製：`GameState sim = *gs;`

---

### `board_can_capture(attacker, defender)`

```c
int board_can_capture(const Cell *attacker, const Cell *defender);
```

判斷 `attacker` 能否吃掉 `defender`（依暗棋規則）。
回傳 1 = 可以，0 = 不能。

**規則：**
- 一般：攻方 rank ≥ 守方 rank
- 特例：兵（rank=1）可以吃將（rank=7）
- 特例：將（rank=7）不能吃兵（rank=1）
- 砲的跳吃不走這個函數（砲的邏輯在 `board_generate_moves` 中處理）

```c
// 範例：臨時建立假設棋子測試
Cell attacker = { SIDE_RED, RANK_SOLDIER, STATE_FACEUP };
Cell defender = { SIDE_BLACK, RANK_GENERAL, STATE_FACEUP };
if (board_can_capture(&attacker, &defender)) { /* 兵吃將 → 1 */ }
```

---

### `board_check_game_over(gs)`

```c
void board_check_game_over(GameState *gs);
```

檢查遊戲是否結束，直接修改 `gs->game_over` 與 `gs->winner`。
`board_apply_move()` 內部會自動呼叫，通常不需要手動呼叫。

---

## 5. 座標系統（必讀）

這是整份代碼中**最容易犯錯**的地方，請務必熟悉。

### 兩套座標

| 用途 | 座標系 | 列範圍 | 行範圍 |
|------|--------|--------|--------|
| `gs->cells[r][c]` 直接存取 | **0-based** | 0–3 | 0–7 |
| `CELL(gs, r, c)` 巨集 | **1-based** | 1–4 | 1–8 |
| `IN_BOUNDS(r, c)` | **1-based** | 1–4 | 1–8 |
| `Move` 結構的 `from_r/to_r` | **1-based** | 1–4 | 1–8 |
| `cell_idx(r, c)`（strategy.c） | 輸入 1-based | — | — |
| `g_cc.prob[idx]` 的 `idx` | 0-based 線性 | 0–31 | — |

### 正確的遍歷寫法

```c
/* 用 CELL 遍歷 → 用 1-based */
for (int r = 1; r <= BOARD_ROWS; r++)
    for (int c = 1; c <= BOARD_COLS; c++) {
        const Cell *cl = CELL(gs, r, c);
        /* 使用 cl */
    }

/* 直接存取 cells[][] → 用 0-based */
for (int r = 0; r < BOARD_ROWS; r++)
    for (int c = 0; c < BOARD_COLS; c++) {
        const Cell *cl = &gs->cells[r][c];
        /* 使用 cl */
    }
```

### 線性索引轉換

```c
/* strategy.c 裡面的 cell_idx()（1-based 輸入）*/
int idx = cell_idx(r, c);   // = (r-1)*8 + (c-1)

/* board.h 的巨集（也是 1-based 輸入）*/
int idx = RC_TO_IDX(r, c);  // 效果相同
```

### 錯誤寫法

```c
/* 錯：for 用 0-based，但 CELL 要 1-based */
for (int r = 0; r < BOARD_ROWS; r++)
    const Cell *cl = CELL(gs, r, c);   // r=0 會取到 r=-1 的格子！

/* 正確 */
for (int r = 0; r < BOARD_ROWS; r++)
    const Cell *cl = &gs->cells[r][c];  // 0-based 直接存取
```

---

## 6. 策略模組架構

> `src/core/strategy.c` 按章節（§）組織，由上到下依序為：

```
strategy.c
│
├── §1  常數與設定
│       NUM_PIECE_TYPES=14, TYPE_IDX(s,r)
│       DIR4=4, DR4[]={-1,1,0,0}, DC4[]={0,0,-1,1}  （上、下、左、右）
│       PIECE_COUNTS[] — 各階級初始數量（兵×5, 其餘×2或×1）
│       PHASE_OPENING / PHASE_MIDGAME / PHASE_ENDGAME
│
├── §2  資料結構
│       CardCounter            — 算牌器（機率分布）
│       ThreatInfo             — 單顆受威脅棋子的詳細資訊
│       OpportunityInfo        — 單個吃子機會的詳細資訊
│       CannonTarget           — 砲線目標
│       CannonPlatformInfo     — 砲架資訊（砲、砲架、目標三方位置）
│       CaptureAnalysis        — 吃子分析結果（淨值、可否反吃）
│       MoveRecord             — 走法歷史紀錄（走法 + 執行方）
│       SituationAssessment    — 全盤情勢評估結果（含新增工具欄位）
│
├── §3  全域狀態
│       g_piece_val[]    — 動態棋子價值表
│       g_cc             — 算牌器實例
│       g_sa             — 情勢評估實例
│       g_my_side        — 當前 AI 陣營
│       g_shadow         — 影子棋盤（偵測吃子用）
│       g_move_history[] — 走法歷史環形緩衝區（最近 20 步）
│       g_move_history_count — 已記錄的走法總數
│
├── §4  工具函數
│       manhattan(), maxf(), minf(), cell_idx()
│       total_pieces_on_board(), count_facedown()
│       opponent(), count_escape_routes()
│       is_type_fully_revealed()   — 某種棋子是否已全數現身
│       is_full_information()      — 完全資訊旗標
│       record_move()              — 記錄走法到歷史緩衝區
│       get_history()              — 取得 N 步前的走法記錄
│       moves_equal()              — 比較兩步走法是否相同
│       detect_repetition()        — 偵測來回重複走法
│       is_move_a_retreat()        — 對手是否退回原位
│       is_opponent_chasing()      — 對手是否正在追擊
│
├── §5  動態棋子價值表
│       refresh_piece_values()  — 每回合更新 g_piece_val[]
│
├── §6  算牌系統
│       detect_captures()       — 比對影子棋盤偵測哪些棋子被吃
│       card_counter_update()   — 重算所有機率，填充 g_cc
│       cc_flip_risk()          — 翻某格被吃的機率
│       cc_flip_opportunity()   — 翻某格可吃敵方的機率
│       cc_hidden_threat()      — 某明子旁暗格藏有威脅的機率
│
├── §7  威脅偵測
│       is_threatened_at()      — 某格是否被威脅（鄰接 + 砲線）
│       count_attackers()       — 鄰接攻擊者數量
│       count_friendly_adj()    — 友方鄰居數量
│       compute_enemy_reach()   — 預計算敵方控制範圍地圖
│
├── §8  砲線分析
│       cannon_platform_count()        — 某門砲的可用砲架數
│       find_cannon_targets()          — 某門砲可跳吃的目標列表
│       find_cannon_victims()          — 被敵方砲線瞄準的我方棋子
│       find_enemy_cannon_platforms()  — 敵方砲的砲架（移走可解除砲線）
│
├── §9  吃子分析
│       evaluate_capture()      — 吃子淨值 + 反吃模擬
│       is_unprotected()        — 目標是否無保護
│
├── §10 情勢評估
│       assess_situation()      — 三遍掃描 + 填充所有工具欄位
│
└── §11 公開 API               ← 修改策略的主要地方
        strategy_select_move()
        strategy_reset()
```

### 每回合的執行順序

`strategy_select_move()` 被呼叫時，依序執行：

```
1. detect_captures(gs)        — 更新被吃記錄（影子棋盤比對）
2. refresh_piece_values(gs)   — 更新 g_piece_val[]
3. card_counter_update(gs, my)— 更新 g_cc（機率分布）
4. assess_situation(gs, my)   — 填充 g_sa（情勢分析）
5. board_generate_moves(gs, legal) — 取得所有合法走法
6. 決策邏輯（優先級 1→2→3→fallback）
```

步驟 1–4 完成後，`g_piece_val[]`、`g_cc`、`g_sa`
全部是最新的，決策邏輯可以直接使用。

---

## 7. 全域狀態變數

這些變數定義在 `strategy.c` 頂部，`static`（模組私有），
每回合在 `strategy_select_move()` 開頭自動更新。

### `g_piece_val[rank]` — 棋子價值表

```c
static float g_piece_val[8];  // index = PieceRank (0–7)
```

由 `refresh_piece_values()` 每回合更新。砲和將帥是動態值，其餘固定。

| rank | 棋子 | 預設值 | 動態？ |
|------|------|--------|--------|
| 0 | 空 | 0.0 | 否 |
| 1 | 兵/卒 | 1.0 | 否 |
| 2 | 砲 | 2.0–6.0 | **是**（棋子越多砲越強） |
| 3 | 馬 | 3.5 | 否 |
| 4 | 車 | 5.0 | 否 |
| 5 | 象 | 2.0 | 否 |
| 6 | 士 | 2.5 | 否 |
| 7 | 將/帥 | 8.0–100.0 | **是**（兵卒威脅越大值越高） |

使用方式：
```c
float val = g_piece_val[victim->rank];   // 取得某棋子的價值
```

---

### `g_cc` — 算牌器（CardCounter）

```c
static CardCounter g_cc;
```

由 `card_counter_update()` 每回合填充，記錄每種棋子的已知資訊和機率分布。
詳見 [§11 算牌系統](#11-算牌系統)。

---

### `g_sa` — 情勢評估（SituationAssessment）

```c
static SituationAssessment g_sa;
```

由 `assess_situation()` 每回合填充，包含將帥位置、子力統計、
威脅清單、殺將機會等。詳見 [§13 情勢評估系統](#13-情勢評估系統)。

---

### `g_my_side` — AI 陣營

```c
static Side g_my_side;
```

每回合從 `gs->current_turn` 更新。等同於 `gs->current_turn`，
但在工具函數中可以不傳參數直接取用。

---

## 8. 資料結構詳解

### `CardCounter` — 算牌器

```c
typedef struct {
    int total[NUM_PIECE_TYPES];           // 每種類型的初始總數（固定）
    int revealed[NUM_PIECE_TYPES];        // 已翻開在場上的數量
    int captured[NUM_PIECE_TYPES];        // 已被吃的數量（從 g_cap_hist 來）
    int hidden[NUM_PIECE_TYPES];          // 仍藏在暗子中的推算數量
    int total_facedown;                   // 場上暗子總數

    float prob[BOARD_SIZE][NUM_PIECE_TYPES]; // prob[cell_idx][type] = 該暗格含該類型的機率
    float prob_friendly[BOARD_SIZE];         // 某暗格是我方的機率
    float prob_enemy[BOARD_SIZE];            // 某暗格是敵方的機率
    float expected_value[BOARD_SIZE];        // 某暗格棋子的期望價值

    int   enemy_general_hidden;       // 敵將是否仍在暗子中
    int   my_general_hidden;          // 我將是否仍在暗子中
    float prob_flip_enemy_general;    // 隨機翻一格翻到敵將的機率
    float prob_flip_my_general;       // 隨機翻一格翻到我將的機率
} CardCounter;
```

**TYPE_IDX 巨集：** 將 (side, rank) 轉成 0–13 的索引：
```
TYPE_IDX(side, rank) = (side - 1) * 7 + (rank - 1)

紅方：兵=0, 砲=1, 馬=2, 車=3, 象=4, 士=5, 將=6
黑方：兵=7, 砲=8, 馬=9, 車=10, 象=11, 士=12, 帥=13
```

---

### `ThreatInfo` — 受威脅棋子資訊

```c
typedef struct {
    int r, c;                   // 被威脅棋子的位置（1-based）
    PieceRank rank;             // 被威脅棋子的階級
    float value;                // 被威脅棋子的價值

    int attacker_count;         // 有幾個攻擊者（最多 MAX_ATTACKERS=4）
    struct {
        int r, c;               // 攻擊者位置（1-based）
        PieceRank rank;         // 攻擊者階級
        int is_cannon;          // 1 = 砲線攻擊，0 = 鄰接攻擊
    } attackers[MAX_ATTACKERS];
} ThreatInfo;
```

在 `g_sa.threatened[]` 陣列中，最多記錄 `MAX_THREATENED=8` 顆。

---

### `OpportunityInfo` — 吃子機會資訊

```c
typedef struct {
    int r, c;              // 敵方目標的位置（1-based）
    PieceRank rank;        // 敵方目標的階級
    float value;           // 敵方目標的價值

    int my_r, my_c;        // 我方攻擊者的位置（1-based）
    PieceRank my_rank;     // 我方攻擊者的階級
    int is_cannon_capture; // 1 = 砲跳吃，0 = 普通吃
} OpportunityInfo;
```

在 `g_sa.opportunities[]` 陣列中，最多記錄 `MAX_OPPORTUNITIES=8` 個。

---

### `CannonTarget` — 砲線目標

```c
typedef struct {
    int r, c;         // 目標位置（1-based）
    Side side;        // 目標陣營
    PieceRank rank;   // 目標階級
} CannonTarget;
#define MAX_CANNON_TARGETS 8
```

由 `find_cannon_targets()` 和 `find_cannon_victims()` 填充。

---

### `CannonPlatformInfo` — 砲架資訊

```c
typedef struct {
    int platform_r, platform_c;   // 砲架的位置（1-based）
    int target_r, target_c;       // 被砲擊的目標位置
    PieceRank target_rank;        // 被砲擊的目標階級
    int cannon_r, cannon_c;       // 砲的位置
} CannonPlatformInfo;
#define MAX_CANNON_PLATFORMS 16
```

由 `find_enemy_cannon_platforms()` 填充。記錄「哪顆砲 → 透過哪顆砲架 → 打我方哪顆棋子」的三方關係。

---

### `CaptureAnalysis` — 吃子分析結果

```c
typedef struct {
    int can_recapture;    // 對方能否立即反吃 (1/0)
    float net_value;      // 淨值 = target_value - (被反吃時)attacker_value
    float target_value;   // 目標棋子的價值
    float attacker_value; // 我方攻擊者的價值
} CaptureAnalysis;
```

由 `evaluate_capture()` 填充。透過一步模擬判斷吃子是否划算。

---

### `MoveRecord` — 走法歷史紀錄

```c
#define MOVE_HISTORY_SIZE 20

typedef struct {
    Move move;    // 走法內容
    Side side;    // 執行方（SIDE_RED 或 SIDE_BLACK）
} MoveRecord;
```

環形緩衝區 `g_move_history[MOVE_HISTORY_SIZE]` 儲存最近 20 步走法（雙方交替）。
由 `record_move()` 寫入，`get_history()` 讀取。

> **注意：** `strategy_reset()` 會清空整個緩衝區。

---

### `GameState` 新增欄位 — 上一步走法

```c
Move  last_move;      // 上一步走法（由 board_apply_move() 記錄）
int   has_last_move;  // 是否已有上一步記錄 (0=無, 1=有)
```

定義於 `board.h` 的 `GameState` 結構中。`board_apply_move()` 每次執行走法時自動更新。
AI 在 `strategy_select_move()` 開始時讀取 `gs->last_move` 以記錄對手走法到歷史緩衝區。

---

### 局面階段常數

```c
#define PHASE_OPENING  0   // 開局：暗子 > 16
#define PHASE_MIDGAME  1   // 中局
#define PHASE_ENDGAME  2   // 殘局：暗子 == 0 且總棋子 ≤ 8
```

存於 `g_sa.game_phase`，每回合由 `assess_situation()` 自動設定。

---

### `SituationAssessment` — 情勢評估

詳見 [§13 情勢評估系統](#13-情勢評估系統)。

---

## 9. 工具函數

> 定義於 `strategy.c §4`，`static`（只在 strategy.c 內可用）。

### `manhattan(r1, c1, r2, c2)` — 曼哈頓距離

```c
static int manhattan(int r1, int c1, int r2, int c2);
// 輸入：1-based 座標
// 回傳：|r1-r2| + |c1-c2|
```

```c
// 我方將帥與敵方棋子的距離
int dist = manhattan(g_sa.my_gen_r, g_sa.my_gen_c, enemy_r, enemy_c);
```

---

### `maxf(a, b)` / `minf(a, b)` — 浮點數 max/min

```c
static float maxf(float a, float b);
static float minf(float a, float b);
```

C 標準沒有浮點 max/min，自行實作。用於分數夾值：
```c
float score = minf(raw_score, 10.0f);  // 上限 10.0
```

---

### `cell_idx(r, c)` — 1-based 座標轉線性索引

```c
static int cell_idx(int r, int c);
// 輸入：1-based (r, c)
// 回傳：0-based 線性索引 (0–31)
// 公式：(r-1) * 8 + (c-1)
```

主要用於查詢 `g_cc.prob[]`、`g_cc.prob_friendly[]` 等陣列：
```c
int idx = cell_idx(r, c);
float p_enemy = g_cc.prob_enemy[idx];
```

---

### `total_pieces_on_board(gs)` — 場上棋子總數

```c
static int total_pieces_on_board(const GameState *gs);
// 回傳：gs->red_alive + gs->black_alive（不含暗子中已被吃的）
```

---

### `count_facedown(gs)` — 暗子總數

```c
static int count_facedown(const GameState *gs);
// 掃描整盤，回傳 STATE_FACEDOWN 的格子數
```

```c
int fd = count_facedown(gs);
if (fd > 20)  { /* 開局，暗子多 */ }
if (fd == 0)  { /* 所有棋子都翻開，完全資訊 */ }
```

---

### `opponent(side)` — 對方陣營

```c
static Side opponent(Side s);
// RED → BLACK, BLACK → RED
```

```c
Side enemy = opponent(gs->current_turn);
```

---

### `count_escape_routes(gs, r, c)` — 安全逃路數

```c
static int count_escape_routes(const GameState *gs, int r, int c);
// 輸入：1-based (r, c)
// 回傳：(r,c) 四方向中 STATE_EMPTY 的格子數（0–4）
```

```c
int esc = count_escape_routes(gs, to_r, to_c);
if (esc == 0) { /* 死胡同，逃過去反而被困 */ }
```

---

### `is_type_fully_revealed(side, rank)` — 某種棋子是否全數現身

```c
static int is_type_fully_revealed(Side side, PieceRank rank);
// 回傳：1=該類型棋子全部翻開或被吃，暗子中已無
// 注意：需在 card_counter_update() 後呼叫
```

```c
// 敵方砲是否全部現身？
if (is_type_fully_revealed(opponent(my), RANK_CANNON)) {
    // 砲線威脅已完全確定，不需要再估機率
}
```

---

### `is_full_information()` — 完全資訊旗標

```c
static int is_full_information(void);
// 回傳：1=所有暗子已翻開（完全資訊局面），0=仍有暗子
// 注意：需在 card_counter_update() 後呼叫
```

```c
if (is_full_information()) {
    // 不需要算牌，所有資訊已知
}
```

---

### `record_move(mv, side)` — 記錄走法到歷史緩衝區

```c
static void record_move(const Move *mv, Side side);
// 輸入：mv - 走法指標, side - 執行方
// 效果：寫入 g_move_history 環形緩衝區，g_move_history_count++
```

在 `strategy_select_move()` 中自動呼叫：
- 函數開始時：從 `gs->last_move` 記錄對手的走法
- 函數結束前：記錄 AI 選擇的走法

---

### `get_history(steps_ago)` — 取得 N 步前的走法記錄

```c
static const MoveRecord* get_history(int steps_ago);
// 輸入：steps_ago - 幾步前（0=最近一步，1=一步前，2=兩步前…）
// 回傳：MoveRecord 指標，若超出範圍回傳 NULL
```

```c
const MoveRecord *last = get_history(0);
if (last && last->side == SIDE_RED) {
    // 最近一步是紅方走的
}
```

---

### `moves_equal(a, b)` — 比較兩步走法是否相同

```c
static int moves_equal(const Move *a, const Move *b);
// 回傳：1=type、from、to 完全相同，0=不同
```

---

### `detect_repetition()` — 偵測來回重複走法

```c
static int detect_repetition(void);
// 回傳：0=無重複，1=出現一次來回（A→B→A），2=兩次以上（A→B→A→B→A）
```

偵測邏輯：檢查同一方在 2 步前和 4 步前是否走了相同的走法（A→B→A→B 循環）。
用途：避免 AI 陷入無意義的來回循環。

---

### `is_move_a_retreat(gs)` — 對手是否退回原位

```c
static int is_move_a_retreat(const GameState *gs);
// 回傳：1=對手上一步將棋子移回兩步前的位置，0=否
// 需要：gs->has_last_move 為真，且歷史至少 2 筆
```

---

### `is_opponent_chasing(gs, target_r, target_c)` — 對手是否正在追擊

```c
static int is_opponent_chasing(const GameState *gs, int target_r, int target_c);
// 輸入：target_r, target_c - 我方被追擊棋子的 1-based 座標
// 回傳：1=對手上一步移到 target 鄰接位置且可吃該棋子，0=否
```

```c
// 我方將帥是否被追擊？
if (is_opponent_chasing(gs, g_sa.my_gen_r, g_sa.my_gen_c)) {
    // 優先考慮保護或逃跑
}
```

---

## 10. 動態棋子價值系統

> 定義於 `strategy.c §5`：`refresh_piece_values(gs)`

### 砲的動態價值

砲需要砲架（中間有一顆棋子）才能發揮最大威力。
棋子越少，砲架越難找，砲的實際效用越低。

| 場上棋子總數 | 砲的價值 |
|-------------|---------|
| > 20 | 6.0 |
| 17–20 | 5.0 |
| 13–16 | 4.0 |
| 9–12 | 3.0 |
| ≤ 8 | 2.0 |

### 將帥的動態價值

將帥是決定勝負的關鍵。兵/卒對將帥有特殊威脅（兵能吃將），
所以敵方兵卒越多越危險，將帥的「保護優先級」越高。

計算邏輯：
1. 統計**敵方**可見的兵卒數（`visible_soldiers`）
2. 加上暗子中推算的兵卒數（`hidden_soldiers`）
3. 根據威脅程度 `threat = visible + hidden` 設定價值

| 狀態 | threat | 將帥價值 |
|------|--------|---------|
| 雙方都有將 | ≤ 0.1 | 15.0 |
| 雙方都有將 | ≤ 2.0 | 40.0 |
| 雙方都有將 | ≤ 4.0 | 60.0 |
| 雙方都有將 | > 4.0 | 100.0 |
| 只剩我方有將（敵將已死） | ≤ 0.1 | 8.0 |
| 只剩我方有將（敵將已死） | ≤ 2.0 | 20.0 |
| 只剩我方有將（敵將已死） | > 2.0 | 40.0 |
| 雙方都沒有將 | — | 10.0 |

---

## 11. 算牌系統

> 定義於 `strategy.c §6`

暗棋是**不完全資訊**遊戲，蓋牌的棋子無法直接看到。
算牌系統透過記錄「哪些棋子已經出現過」來推算「暗格中
各種棋子的機率」。

### `detect_captures(gs)` — 偵測吃子事件

```c
static void detect_captures(const GameState *gs);
```

**原理：** 用影子棋盤（`g_shadow`）記錄上一回合的狀態，
與當前狀態比對，找出哪些棋子消失了（被吃了）。
被吃的記錄存入 `g_cap_hist[]`。

**第一次呼叫時：** 僅儲存當前狀態為影子，不做比對。

---

### `card_counter_update(gs, my_side)` — 更新機率分布

```c
static void card_counter_update(const GameState *gs, Side my_side);
```

執行順序：
1. 初始化 `total[]`（每方：兵×5, 砲/馬/車/象/士×2, 將×1）
2. 掃描棋盤，統計 `revealed[]`（已翻開）和 `total_facedown`
3. `hidden[i] = total[i] - revealed[i] - captured[i]`（加上已知被吃的）
4. 修正：讓 `Σhidden == total_facedown`
5. 對每個暗格計算機率：`prob[idx][t] = hidden[t] / total_facedown`
6. 加總得出 `prob_friendly[]`、`prob_enemy[]`、`expected_value[]`
7. 更新將帥隱藏狀態

### 使用算牌器的方式

```c
// 查詢：敵方還有幾顆馬藏在暗子中？
int hidden_horses = g_cc.hidden[TYPE_IDX(opponent(my), RANK_HORSE)];

// 查詢：某暗格是敵方的機率
int idx = cell_idx(r, c);  // r, c 是 1-based
float p_enemy = g_cc.prob_enemy[idx];

// 查詢：某暗格含有敵方砲的機率
float p_enemy_cannon = g_cc.prob[idx][TYPE_IDX(opponent(my), RANK_CANNON)];

// 查詢：敵將是否還在暗子中
if (g_cc.enemy_general_hidden) {
    float p = g_cc.prob_flip_enemy_general;  // 隨機翻一格翻到敵將的機率
}

// 查詢：場上是否所有棋子都翻開了
if (g_cc.total_facedown == 0) { /* 完全資訊 */ }
```

---

### `cc_flip_risk(gs, r, c, my_side)` — 翻棋被吃機率

```c
static float cc_flip_risk(const GameState *gs, int r, int c, Side my_side);
// 回傳：[0.0, 1.0]，翻出後立即被相鄰敵方吃掉的機率
```

**原理：** 遍歷 (r,c) 的每個相鄰敵方明子，
對每種「我方可能翻出的棋子類型」，如果敵方能吃它，
就累加該類型在此格的機率。

```c
// 翻棋時篩掉高風險格子
for (int i = 0; i < nf; i++) {
    float risk = cc_flip_risk(gs, flips[i].from_r, flips[i].from_c, my);
    if (risk < 0.4f) { *out_move = flips[i]; return 1; }
}
```

---

### `cc_flip_opportunity(gs, r, c, my_side)` — 翻棋可吃機率

```c
static float cc_flip_opportunity(const GameState *gs, int r, int c, Side my_side);
// 回傳：[0.0, 1.0]，翻出後能吃相鄰敵方明子的機率
```

```c
// 優先翻吃子機率高的格子
float best_opp = -1;
int best_fi = 0;
for (int i = 0; i < nf; i++) {
    float opp = cc_flip_opportunity(gs, flips[i].from_r, flips[i].from_c, my);
    if (opp > best_opp) { best_opp = opp; best_fi = i; }
}
*out_move = flips[best_fi];
```

---

### `cc_hidden_threat(gs, r, c, side, rank)` — 暗子隱藏威脅

```c
static float cc_hidden_threat(const GameState *gs, int r, int c,
                               Side side, PieceRank rank);
// r, c：被評估的明子位置（1-based）
// side, rank：該明子的陣營與階級
// 回傳：[0.0, 1.0]，周圍暗子中藏有能吃它的敵方的機率
```

```c
// 移動前評估目的地的隱藏危險
const Cell *src = CELL(gs, mv->from_r, mv->from_c);
float hidden_danger = cc_hidden_threat(gs, mv->to_r, mv->to_c,
                                       src->side, src->rank);
if (hidden_danger > 0.3f) {
    // 30% 機率目的地旁有暗子能吃我
}
```

---

## 12. 威脅偵測函數

> 定義於 `strategy.c §7`

### `is_threatened_at(gs, r, c, side, rank)` — 是否被威脅

```c
static int is_threatened_at(const GameState *gs, int r, int c,
                             Side side, PieceRank rank);
// r, c：1-based
// 回傳：1 = 被威脅，0 = 安全
```

**檢查兩種威脅：**
1. **鄰接威脅：** 四方向相鄰是否有敵方明子，且 `board_can_capture(敵, 我)` 為真
2. **砲線威脅：** 橫向或縱向，跳過一顆棋子（砲架）後是否有敵方砲

```c
// 逃跑判斷：從危險走到安全
const Cell *src = CELL(gs, mv->from_r, mv->from_c);
int from_danger = is_threatened_at(gs, mv->from_r, mv->from_c,
                                   src->side, src->rank);
int to_danger   = is_threatened_at(gs, mv->to_r, mv->to_c,
                                   src->side, src->rank);

if (from_danger && !to_danger) {
    // 成功的逃跑走法
}
if (!from_danger && to_danger) {
    // 這步會把棋子送進危險，避免
}
```

---

### `count_attackers(gs, r, c, side, rank)` — 鄰接攻擊者數量

```c
static int count_attackers(const GameState *gs, int r, int c,
                            Side side, PieceRank rank);
// 只計算四方向鄰接的敵方明子（不含砲線）
// 回傳：0–4
```

```c
int att = count_attackers(gs, r, c, my, src->rank);
if (att >= 2) { /* 被包圍，危險 */ }
```

---

### `count_friendly_adj(gs, r, c, side)` — 友方鄰居數量

```c
static int count_friendly_adj(const GameState *gs, int r, int c, Side side);
// 計算四方向鄰接的我方明子數
// 回傳：0–4
```

```c
// 翻棋時選有友軍保護的位置
int friends = count_friendly_adj(gs, flip_r, flip_c, my);
if (friends >= 1) { /* 有友軍在旁，翻出後比較安全 */ }
```

---

### `compute_enemy_reach(gs, my_side)` — 敵方控制範圍地圖

```c
static void compute_enemy_reach(const GameState *gs, Side my_side);
// 填充 g_sa.enemy_reach[BOARD_SIZE]（0=不在範圍, 1=在敵方攻擊範圍內）
// 每回合由 assess_situation() 自動呼叫
```

**原理：** 遍歷所有敵方明子：
- 非砲棋子：標記四方向鄰接格
- 砲：掃描砲線，標記跳過砲架後的目標格

> **注意：** 這是概略估計（不考慮階級限制）。「在敵方攻擊範圍內」不代表你的棋子一定會被吃，
> 具體是否被威脅仍需 `is_threatened_at()` 依據階級判斷。

```c
// 移動時避開敵方控制區
int idx = cell_idx(to_r, to_c);
if (g_sa.enemy_reach[idx]) {
    // 目標格在敵方攻擊範圍內，謹慎
}
```

---

## 13. 砲線分析函數

> 定義於 `strategy.c §8`

### `cannon_platform_count(gs, r, c)` — 砲架數量

```c
static int cannon_platform_count(const GameState *gs, int r, int c);
// r, c：砲的位置（1-based）
// 回傳：該砲的橫縱線上有多少顆棋子可作為砲架
```

```c
// 判斷某門砲的威力（砲架越多，跳吃機會越多）
int platforms = cannon_platform_count(gs, cannon_r, cannon_c);
if (platforms == 0) {
    // 這門砲沒有砲架，暫時無法發揮跳吃功能
}
```

---

### `find_cannon_targets(gs, r, c, cannon_side, out, max)` — 砲可跳吃的目標

```c
static int find_cannon_targets(const GameState *gs, int r, int c,
                                Side cannon_side, CannonTarget *out, int max);
// r, c：砲的位置（1-based）
// cannon_side：砲的陣營
// out：輸出陣列（CannonTarget），max：最大輸出數量
// 回傳：找到的目標數量
```

```c
// 找出我方某門砲能打的所有目標
CannonTarget targets[MAX_CANNON_TARGETS];
int n = find_cannon_targets(gs, my_cannon_r, my_cannon_c, my, targets, MAX_CANNON_TARGETS);
for (int i = 0; i < n; i++) {
    // targets[i].r, .c, .side, .rank — 目標資訊
}
```

---

### `find_cannon_victims(gs, victim_side, out, max)` — 被砲線瞄準的棋子

```c
static int find_cannon_victims(const GameState *gs, Side victim_side,
                                CannonTarget *out, int max);
// victim_side：受害方陣營
// 回傳：被敵方砲線瞄準的我方棋子數量
```

```c
// 找出所有正被敵方砲線瞄準的我方棋子
CannonTarget victims[MAX_CANNON_TARGETS];
int n = find_cannon_victims(gs, my, victims, MAX_CANNON_TARGETS);
for (int i = 0; i < n; i++) {
    // victims[i] 正被砲線瞄準
}
```

---

### `find_enemy_cannon_platforms(gs, my_side, out, max)` — 敵方砲架識別

```c
static int find_enemy_cannon_platforms(const GameState *gs, Side my_side,
                                       CannonPlatformInfo *out, int max);
// 回傳：砲架資訊數量
```

找出「哪顆敵方砲，透過哪顆砲架，正在瞄準我方哪顆棋子」。
移走砲架（或吃掉砲架）可以解除該砲線威脅。

```c
CannonPlatformInfo platforms[MAX_CANNON_PLATFORMS];
int n = find_enemy_cannon_platforms(gs, my, platforms, MAX_CANNON_PLATFORMS);
for (int i = 0; i < n; i++) {
    // platforms[i].cannon_r/c   — 敵方砲的位置
    // platforms[i].platform_r/c — 砲架的位置
    // platforms[i].target_r/c   — 被瞄準的我方棋子位置
    // platforms[i].target_rank  — 被瞄準棋子的階級
}
```

---

## 14. 吃子分析函數

> 定義於 `strategy.c §9`

### `evaluate_capture(gs, mv, out)` — 吃子淨值分析

```c
static void evaluate_capture(const GameState *gs, const Move *mv, CaptureAnalysis *out);
// mv：必須是 MOVE_CAPTURE 類型的走法
// 透過 out 回傳分析結果
```

**原理：** 用 `board_apply_move()` 模擬吃子後的局面，
再用 `is_threatened_at()` 檢查攻擊者在新位置是否會被對方反吃。

```c
// 吃子前先評估是否划算
CaptureAnalysis ca;
evaluate_capture(gs, &legal[i], &ca);
if (ca.can_recapture && ca.net_value < 0) {
    // 虧本交換（例如用車吃兵但被反吃），不划算
}
if (!ca.can_recapture) {
    // 對方無法反吃，穩賺
}
```

---

### `is_unprotected(gs, r, c)` — 目標是否無保護

```c
static int is_unprotected(const GameState *gs, int r, int c);
// r, c：目標棋子位置（1-based）
// 回傳：1=無保護（無同方鄰居且無砲線保護），0=有保護
```

```c
// 找出所有無保護的敵方棋子（最佳攻擊目標）
for (int r = 1; r <= BOARD_ROWS; r++)
    for (int c = 1; c <= BOARD_COLS; c++) {
        const Cell *cl = CELL(gs, r, c);
        if (cl->state != STATE_FACEUP || cl->side == my) continue;
        if (is_unprotected(gs, r, c)) {
            // 這顆敵方棋子沒有保護，吃了不會被反吃
        }
    }
```

---

## 15. 情勢評估系統

> 定義於 `strategy.c §10`：`assess_situation(gs, my_side)`
>
> 包含原有的三遍掃描 + 新增的工具欄位計算。

每回合自動執行三遍掃描，將結果存入 `g_sa`。

### 三遍掃描內容

| 遍次 | 工作 | 填充的 g_sa 欄位 |
|------|------|-----------------|
| Pass 1 | 定位棋子、統計子力 | `my/enemy_gen_r/c`, `my/enemy_gen_alive`, `my/enemy_material`, `my/enemy_piece_count` |
| Pass 2 | 找受威脅的我方棋子 | `threatened[]`, `threat_count`, `my_general_threatened` |
| Pass 3 | 找可吃的敵方棋子 | `opportunities[]`, `opportunity_count`, `can_kill_enemy_general`, `kill_general_move` |
| 計算緊迫度 | — | `general_danger_urgency`, `kill_opportunity_urgency`, `stagnation_pressure` |

### 完整欄位說明

#### 將帥狀態

```c
g_sa.my_gen_alive                   // 我方將帥是否存活 (1/0)
g_sa.enemy_gen_alive                // 敵方將帥是否存活 (1/0)
g_sa.my_gen_r,    g_sa.my_gen_c     // 我方將帥位置（1-based；不存活則為 0）
g_sa.enemy_gen_r, g_sa.enemy_gen_c  // 敵方將帥位置
```

#### 子力統計

```c
g_sa.my_material        // 我方所有翻開棋子的價值總和 (float)
g_sa.enemy_material     // 敵方同上
g_sa.my_piece_count     // 我方翻開棋子數量 (int)
g_sa.enemy_piece_count  // 敵方同上
```

#### 殺將機會

```c
g_sa.can_kill_enemy_general   // 這回合能否直接吃掉敵將 (1/0)
g_sa.kill_general_move        // 殺將的具體走法 (Move)
```

```c
// 直接使用，最簡單的殺將判斷
if (g_sa.can_kill_enemy_general) {
    *out_move = g_sa.kill_general_move;
    return 1;
}
```

#### 將帥安全狀態

```c
g_sa.my_general_threatened    // 我方將帥是否正在被直接攻擊 (1/0)
```

#### 受威脅棋子清單（最多 8 顆）

```c
g_sa.threat_count               // 受威脅的我方棋子數

// 第 i 顆被威脅棋子：
g_sa.threatened[i].r, .c        // 位置（1-based）
g_sa.threatened[i].rank         // 階級
g_sa.threatened[i].value        // 價值
g_sa.threatened[i].attacker_count        // 攻擊者數量
g_sa.threatened[i].attackers[j].r, .c   // 第 j 個攻擊者位置
g_sa.threatened[i].attackers[j].rank    // 攻擊者階級
g_sa.threatened[i].attackers[j].is_cannon // 1=砲線攻擊
```

```c
// 找被威脅的最高價值棋子
int worst_idx = -1;
float worst_val = 0;
for (int i = 0; i < g_sa.threat_count; i++) {
    if (g_sa.threatened[i].value > worst_val) {
        worst_val = g_sa.threatened[i].value;
        worst_idx = i;
    }
}
```

```c
// 找能吃掉威脅者的走法（反將軍）
for (int i = 0; i < g_sa.threat_count; i++) {
    for (int j = 0; j < g_sa.threatened[i].attacker_count; j++) {
        int ar = g_sa.threatened[i].attackers[j].r;
        int ac = g_sa.threatened[i].attackers[j].c;
        for (int k = 0; k < n; k++) {
            if (legal[k].type == MOVE_CAPTURE &&
                legal[k].to_r == ar && legal[k].to_c == ac) {
                *out_move = legal[k];
                return 1;  // 吃掉攻擊者
            }
        }
    }
}
```

#### 可吃機會清單（最多 8 個）

```c
g_sa.opportunity_count              // 可吃的機會數

// 第 i 個機會：
g_sa.opportunities[i].r, .c        // 敵方目標位置（1-based）
g_sa.opportunities[i].rank         // 敵方目標階級
g_sa.opportunities[i].value        // 敵方目標價值
g_sa.opportunities[i].my_r, .my_c  // 我方攻擊者位置
g_sa.opportunities[i].my_rank      // 我方攻擊者階級
g_sa.opportunities[i].is_cannon_capture  // 目前恆為 0（見下方說明）
```

> **注意：** Pass 3 只偵測**鄰接普通吃子**，砲的跳吃不在偵測範圍內，
> 因此 `is_cannon_capture` 目前永遠是 `0`。
> 未來若要加入砲的吃子機會偵測，需在 `assess_situation()` 的 Pass 3 中另行實作。

#### 緊迫度指標

```c
g_sa.general_danger_urgency    // 我方將帥危險程度（0.0–10.0）
                               //  10.0 = 將帥正在被攻擊
                               //   8.0 = 敵兵在旁邊（1格）
                               //   0.0 = 安全

g_sa.kill_opportunity_urgency  // 殺將機會程度（0.0–10.0）
                               //  10.0 = 可以直接殺
                               //   8.0 = 我兵在敵將旁邊

g_sa.stagnation_pressure       // 停滯壓力（≥ 0.0）
                               // 每回合未吃子就累積，越高表示越接近和棋
```

```c
if (g_sa.general_danger_urgency >= 8.0f) { /* 將帥告急，優先防守 */ }
if (g_sa.kill_opportunity_urgency >= 8.0f) { /* 可以殺將，優先進攻 */ }
if (g_sa.stagnation_pressure > 3.0f) { /* 停滯太久，應主動出擊 */ }
```

#### 將帥逃路數

```c
g_sa.my_gen_escape_routes      // 我方將帥周圍空格數（0–4），不存活為 0
g_sa.enemy_gen_escape_routes   // 敵方將帥周圍空格數
```

```c
// 敵將被困（逃路少），進攻機會大
if (g_sa.enemy_gen_alive && g_sa.enemy_gen_escape_routes == 0) {
    // 敵將完全被困，只要兵靠近就必殺
}
```

#### 我方兵卒距敵將距離

```c
g_sa.soldier_near_enemy_gen_1   // 距敵將 dist==1 的我方兵卒數
g_sa.soldier_near_enemy_gen_2   // 距敵將 dist<=2 的我方兵卒數
```

```c
if (g_sa.soldier_near_enemy_gen_1 > 0) {
    // 有兵已在敵將旁邊，下一步可殺
}
```

#### 敵方控制範圍

```c
g_sa.enemy_reach[cell_idx(r, c)]   // 1=該格在敵方攻擊範圍內
```

> 概略估計，不考慮階級限制。精確判斷用 `is_threatened_at()`。

```c
// 移動時避開敵方控制區
if (!g_sa.enemy_reach[cell_idx(to_r, to_c)]) {
    // 目標格不在敵方攻擊範圍內
}
```

#### 局面階段

```c
g_sa.game_phase   // PHASE_OPENING(0) / PHASE_MIDGAME(1) / PHASE_ENDGAME(2)
```

| 常數 | 條件 |
|------|------|
| `PHASE_OPENING` | 暗子 > 16 |
| `PHASE_MIDGAME` | 其餘情況 |
| `PHASE_ENDGAME` | 暗子 == 0 且 總棋子 ≤ 8 |

```c
if (g_sa.game_phase == PHASE_ENDGAME) {
    // 殘局：完全資訊，應切換到精確搜尋策略
}
```

#### 和棋倒數

```c
g_sa.turns_until_draw   // MAX_NO_CAPTURE - no_capture_turns
```

```c
if (g_sa.turns_until_draw <= 5) {
    // 快和棋了，必須主動吃子或翻棋
}
```

#### 敵方合法走法數量

```c
g_sa.enemy_legal_move_count   // 敵方目前可走的合法走法總數
```

```c
if (g_sa.enemy_legal_move_count <= 3) {
    // 敵方陷入被動，選擇極少
}
```

---

## 16. AI 主流程

> 定義於 `strategy.c §11`：`strategy_select_move(gs, out_move)`

### 當前策略（基礎版）

```
優先級 1 → 能吃就吃（選最高價值目標）
優先級 2 → 被威脅就逃跑（走到安全格）
優先級 3 → 隨機翻棋
Fallback → 隨機走
```

每個優先級是一個獨立的 `if` 區塊，符合條件就 `return 1`，
不符合就**往下走到下一個優先級**。

> **固定優先級的問題**
>
> 現在這份優先級清單是**靜態且無條件的**——不管當前局面如何，
> 邏輯永遠按同一個順序判斷。這在真實對弈中必然產生明顯的缺陷。
>
> **可行的做法** 是根據 `g_sa` 的緊迫度指標（`general_danger_urgency`、
> `kill_opportunity_urgency`）動態決定優先級順序，而不是把順序寫死。

### 代碼結構

```c
int strategy_select_move(const GameState *gs, Move *out_move)
{
    g_my_side = gs->current_turn;
    Side my = g_my_side;
    Side enemy = (my == SIDE_RED) ? SIDE_BLACK : SIDE_RED;

    // ── 記錄對手上一步走法 ────────────────────────────────
    if (gs->has_last_move)
        record_move(&gs->last_move, enemy);

    // ── 初始化分析工具（每回合必須執行）──────────────────
    detect_captures(gs);
    refresh_piece_values(gs);
    card_counter_update(gs, my);
    assess_situation(gs, my);

    Move legal[MAX_MOVES];
    int n = board_generate_moves(gs, legal);
    if (n == 0) return 0;       // 無合法走法
    if (n == 1) { *out_move = legal[0]; record_move(out_move, my); return 1; }

    // ── 優先級 1：能吃就吃 ───────────────────────────────
    float best_cap_val = -1.0f;
    int best_cap_idx = -1;
    for (int i = 0; i < n; i++) {
        if (legal[i].type != MOVE_CAPTURE) continue;
        const Cell *victim = CELL(gs, legal[i].to_r, legal[i].to_c);
        float val = g_piece_val[victim->rank];
        if (val > best_cap_val) { best_cap_val = val; best_cap_idx = i; }
    }
    if (best_cap_idx >= 0) {
        *out_move = legal[best_cap_idx]; record_move(out_move, my); return 1;
    }

    // ── 優先級 2：被威脅就逃跑 ──────────────────────────
    for (int i = 0; i < n; i++) {
        if (legal[i].type != MOVE_WALK) continue;
        const Cell *src = CELL(gs, legal[i].from_r, legal[i].from_c);
        if (is_threatened_at(gs, legal[i].from_r, legal[i].from_c,
                             src->side, src->rank) &&
            !is_threatened_at(gs, legal[i].to_r, legal[i].to_c,
                              src->side, src->rank)) {
            *out_move = legal[i]; record_move(out_move, my); return 1;
        }
    }

    // ── 優先級 3：隨機翻棋 ──────────────────────────────
    Move flips[MAX_MOVES];
    int nf = 0;
    for (int i = 0; i < n; i++)
        if (legal[i].type == MOVE_FLIP) flips[nf++] = legal[i];
    if (nf > 0) { *out_move = flips[rand() % nf]; record_move(out_move, my); return 1; }

    // ── Fallback：隨機走 ─────────────────────────────────
    *out_move = legal[rand() % n];
    record_move(out_move, my);
    return 1;
}
```

### `strategy_reset()` — 重置狀態

```c
void strategy_reset(void);
```

**新遊戲開始時必須呼叫**，否則上一局的算牌記錄會污染新局。

清除內容：
- `g_cap_hist[]`（被吃記錄）
- `g_shadow_valid`（影子棋盤重置）
- `g_move_history[]`（走法歷史緩衝區）
- `g_move_history_count`（歸零）

---

## 17. 常用代碼模式

### 遍歷所有格子

```c
/* 用 CELL（1-based） */
for (int r = 1; r <= BOARD_ROWS; r++)
    for (int c = 1; c <= BOARD_COLS; c++) {
        const Cell *cl = CELL(gs, r, c);
        if (cl->state == STATE_FACEUP && cl->side == my)      { /* 我方明子 */ }
        if (cl->state == STATE_FACEUP && cl->side == enemy)   { /* 敵方明子 */ }
        if (cl->state == STATE_FACEDOWN)                       { /* 暗子 */ }
        if (cl->state == STATE_EMPTY)                          { /* 空格 */ }
    }
```

### 遍歷四方向鄰居

方向陣列定義（`strategy.c §1`）：
```
DR4[4] = { -1,  1,  0,  0 }   // 列偏移：上、下、左、右
DC4[4] = {  0,  0, -1,  1 }   // 行偏移：上、下、左、右
```

```c
/* r, c 是 1-based */
for (int d = 0; d < DIR4; d++) {
    int nr = r + DR4[d], nc = c + DC4[d];
    if (!IN_BOUNDS(nr, nc)) continue;
    const Cell *adj = CELL(gs, nr, nc);
    /* adj 是 (r,c) 的上/下/左/右鄰居 */
}
```

### 篩選特定類型的走法

```c
for (int i = 0; i < n; i++) {
    if (legal[i].type == MOVE_CAPTURE) { /* 吃子走法 */ }
    if (legal[i].type == MOVE_WALK)    { /* 移動走法 */ }
    if (legal[i].type == MOVE_FLIP)    { /* 翻棋走法 */ }
}
```

### 砲線掃描

砲的吃法：**跳過一顆棋子（砲架）**後，才能吃第二顆。

```c
/* 從 (r, c) 出發，掃描橫向與縱向的砲線 */
for (int axis = 0; axis < 2; axis++) {          // 0=橫, 1=縱
    for (int dir = -1; dir <= 1; dir += 2) {     // 雙向（正/負）
        int jumped = 0;
        for (int step = 1; ; step++) {
            int nr = (axis == 0) ? r         : r + dir * step;
            int nc = (axis == 0) ? c + dir * step : c;
            if (!IN_BOUNDS(nr, nc)) break;
            const Cell *t = CELL(gs, nr, nc);
            if (t->state == STATE_EMPTY) continue;
            if (!jumped) { jumped = 1; continue; }  // 第一顆 = 砲架，跳過
            /* 第二顆 = 砲線目標：t->side, t->rank */
            break;
        }
    }
}
```

### 模擬走法（不影響原始狀態）

```c
GameState sim = *gs;           // 複製一份
board_apply_move(&sim, &legal[i]);
// sim 是走了那步之後的狀態，gs 不受影響
// 注意：sim.current_turn 已切換到對方回合
```

### 從 legal[] 中找目標格的走法

```c
/* 找所有吃 (tr, tc) 的走法 */
for (int i = 0; i < n; i++) {
    if (legal[i].type == MOVE_CAPTURE &&
        legal[i].to_r == tr && legal[i].to_c == tc) {
        /* 找到了 */
    }
}
```

---

## 18. 新增策略功能指南

### 基本原則

在 `strategy_select_move()` 中，每個優先級是一個獨立的 `if` 區塊。
**越前面越優先**，一旦 `return 1`，後面全部跳過。

### 新增優先級的位置

```c
int strategy_select_move(...) {
    /* ... 初始化 ... */

    /* 現有優先級 1：能吃就吃 */
    ...
    if (best_cap_idx >= 0) { ...; return 1; }

    /* ← 在這裡插入比「逃跑」更高優先的新邏輯 */

    /* 現有優先級 2：被威脅就逃跑 */
    ...
    if (...) { ...; return 1; }

    /* ← 在這裡插入比「翻棋」更高優先的新邏輯 */

    /* 現有優先級 3：隨機翻棋 */
    ...
}
```

### 範例 A：加入「直接殺將就殺」

```c
    /* 在優先級 1（能吃就吃）之前插入 */
    if (g_sa.can_kill_enemy_general) {
        *out_move = g_sa.kill_general_move;
        return 1;
    }
```

> `assess_situation()` 已經計算好 `can_kill_enemy_general` 和
> `kill_general_move`，直接用即可。

### 範例 B：逃跑時選逃路最多的方向

```c
    /* 替換現有的「優先級 2：逃跑」 */
    int best_flee_idx = -1, best_flee_esc = -1;
    for (int i = 0; i < n; i++) {
        if (legal[i].type != MOVE_WALK) continue;
        const Cell *src = CELL(gs, legal[i].from_r, legal[i].from_c);
        if (!is_threatened_at(gs, legal[i].from_r, legal[i].from_c,
                              src->side, src->rank)) continue;
        if (is_threatened_at(gs, legal[i].to_r, legal[i].to_c,
                             src->side, src->rank)) continue;
        int esc = count_escape_routes(gs, legal[i].to_r, legal[i].to_c);
        if (esc > best_flee_esc) { best_flee_esc = esc; best_flee_idx = i; }
    }
    if (best_flee_idx >= 0) { *out_move = legal[best_flee_idx]; return 1; }
```

### 範例 C：翻棋偏好有友軍的位置

```c
    /* 替換現有的「優先級 3：隨機翻棋」 */
    Move flips[MAX_MOVES]; int nf = 0;
    for (int i = 0; i < n; i++)
        if (legal[i].type == MOVE_FLIP) flips[nf++] = legal[i];
    if (nf > 0) {
        int best_adj = -1, best_fi = 0;
        for (int i = 0; i < nf; i++) {
            int adj = count_friendly_adj(gs, flips[i].from_r, flips[i].from_c, my);
            if (adj > best_adj) { best_adj = adj; best_fi = i; }
        }
        *out_move = flips[best_fi];
        return 1;
    }
```

### 範例 D：翻棋時避免高風險格子

```c
    if (nf > 0) {
        for (int i = 0; i < nf; i++) {
            float risk = cc_flip_risk(gs, flips[i].from_r, flips[i].from_c, my);
            if (risk < 0.5f) { *out_move = flips[i]; return 1; }
        }
        /* 所有格子都高風險，隨機挑一個 */
        *out_move = flips[rand() % nf]; return 1;
    }
```

### 邏輯複雜時，抽成獨立函數

新函數**必須放在 `strategy_select_move()` 上面**（C 的先宣告後使用規則）。
建議放在 `§8` 和 `§9` 之間。

```c
/* 放在 assess_situation() 之後、strategy_select_move() 之前 */

static int find_best_escape(const GameState *gs, Move *legal, int n,
                             Side my, Move *out)
{
    /* 你的逃跑邏輯 */
    /* 找到最佳逃法時 */
    if (found) { *out = best; return 1; }
    return 0;
}

/* 然後在主流程呼叫 */
    if (find_best_escape(gs, legal, n, my, out_move)) return 1;
```

### 新增全域狀態時記得清零

```c
/* 如果你在 §3 加了新的 static 變數 */
static int my_new_counter = 0;

/* 必須在 strategy_reset() 中清零 */
void strategy_reset(void)
{
    memset(g_cap_hist, 0, sizeof(g_cap_hist));
    g_shadow_valid = 0;
    my_new_counter = 0;   // ← 記得加這行
}
```

---

## 19. 編譯與執行

詳細的環境設置見 `docs/BUILD.md`。

### 快速指令

```bash
# 編譯
make clean && make

# 執行
./build/dark_chess_gui
```

### 關於編譯警告

`-Wunused-function` 警告（關於 `cc_flip_risk`、`cc_flip_opportunity`、
`cc_hidden_threat`、`count_attackers`、`count_friendly_adj`、
`count_escape_routes`、`maxf` 等）是**正常的**。

這些函數是刻意保留的工具，供未來策略擴展使用，目前的基礎策略
尚未使用它們。警告不影響編譯與執行。

### 進階策略文件

`docs/AI_STRATEGY_IMPLEMENTATION.md` 記錄了完整的進階策略設計：
PVS 搜尋、蒙地卡羅翻棋模擬、Zobrist 雜湊、置換表、
殺手啟發、歷史啟發等。這份文件供日後擴展時參考。
