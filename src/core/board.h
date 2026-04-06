#ifndef BOARD_H
#define BOARD_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* =====================================================================
 *  Constants
 * ===================================================================== */

/* Board dimensions */
#define BOARD_ROWS       4
#define BOARD_COLS       8
#define BOARD_SIZE       (BOARD_ROWS * BOARD_COLS)  /* 32 cells */

/* Piece counts (standard 暗棋 set for each side):
 *   將/帥 x1, 士/仕 x2, 象/相 x2, 車/俥 x2,
 *   馬/傌 x2, 炮/砲 x2, 兵/卒 x5
 *   Total per side: 16, grand total: 32 */
#define PIECES_PER_SIDE  16
#define MAX_MOVES        256   /* generous upper bound for legal moves */
#define MAX_NO_CAPTURE   50    /* draw after 50 turns without capture */

/* =====================================================================
 *  Utility macros
 * ===================================================================== */

/* Convert 1-based (row, col) to 0-based index and back */
#define RC_TO_IDX(r, c)  (((r)-1) * BOARD_COLS + ((c)-1))
#define IDX_TO_ROW(i)    (((i) / BOARD_COLS) + 1)
#define IDX_TO_COL(i)    (((i) % BOARD_COLS) + 1)

/* Check bounds (1-based) */
#define IN_BOUNDS(r, c)  ((r) >= 1 && (r) <= BOARD_ROWS && (c) >= 1 && (c) <= BOARD_COLS)

/* Utility: get cell pointer (0-based internally) */
#define CELL(gs, r, c)   (&(gs)->cells[(r)-1][(c)-1])

/* =====================================================================
 *  Types
 * ===================================================================== */

/* ----- Side / Color ----- */
typedef enum {
    SIDE_NONE  = 0,
    SIDE_RED   = 1,   /* 玩家 */
    SIDE_BLACK = 2    /* 電腦 */
} Side;

/* ----- Piece Rank (rank value used for capture rules) ----- */
typedef enum {
    RANK_NONE    = 0,
    RANK_SOLDIER = 1,   /* 兵 / 卒  (lowest, but can capture 將/帥) */
    RANK_CANNON  = 2,   /* 炮 / 砲  (jump-capture only) */
    RANK_HORSE   = 3,   /* 馬 / 傌  */
    RANK_CHARIOT = 4,   /* 車 / 俥  */
    RANK_ELEPHANT= 5,   /* 象 / 相  */
    RANK_ADVISOR = 6,   /* 士 / 仕  */
    RANK_GENERAL = 7    /* 將 / 帥  (highest, but loses to 兵/卒) */
} PieceRank;

/* ----- Piece State ----- */
typedef enum {
    STATE_EMPTY    = 0,  /* cell is empty */
    STATE_FACEDOWN = 1,  /* covered / not yet flipped */
    STATE_FACEUP   = 2   /* revealed */
} PieceState;

/* ----- Single cell on the board ----- */
typedef struct {
    Side       side;     /* SIDE_NONE if empty */
    PieceRank  rank;     /* RANK_NONE if empty */
    PieceState state;    /* empty / face-down / face-up */
} Cell;

/* ----- Move types ----- */
typedef enum {
    MOVE_FLIP    = 0,   /* flip a face-down piece */
    MOVE_WALK    = 1,   /* move to adjacent empty cell */
    MOVE_CAPTURE = 2    /* capture an enemy piece */
} MoveType;

/* ----- A single move ----- */
typedef struct {
    MoveType type;
    int from_r, from_c;  /* source row, col (1-based) */
    int to_r, to_c;      /* dest row, col   (1-based, same as from for FLIP) */
} Move;

/* ----- Game state ----- */
typedef struct {
    Cell  cells[BOARD_ROWS][BOARD_COLS];   /* [row 0..3][col 0..7] */
    Side  current_turn;                     /* whose turn */
    int   red_alive;                        /* number of red pieces still on board */
    int   black_alive;                      /* number of black pieces still on board */
    int   game_over;                        /* 1 if game ended */
    Side  winner;                           /* who won (SIDE_NONE = draw) */
    int   no_capture_turns;                 /* turns without capture (for draw rule) */
    Move  last_move;                        /* 上一步走法 */
    int   has_last_move;                    /* 是否已有上一步記錄 (0=無, 1=有) */
} GameState;

/* =====================================================================
 *  Function
 * ===================================================================== */

/*
 * 函數名稱：board_init
 * 功能：初始化新遊戲，將 32 顆棋子隨機放置於棋盤上（皆為蓋牌狀態）
 * 輸入：gs - 指向 GameState 結構的指標，將被完全初始化
 * 輸出：無（void）
 * 最後更新：2026-04-06 15:00 (UTC+8)
 */
void board_init(GameState *gs);

/*
 * 函數名稱：board_can_capture
 * 功能：判斷攻擊方棋子是否可以吃掉防守方棋子
 *       規則：大吃小或同級互吃；兵/卒可吃將/帥；將/帥不可吃兵/卒
 * 輸入：attacker - 攻擊方棋子指標, defender - 防守方棋子指標
 * 輸出：int - 可以吃回傳 1，不可以吃回傳 0
 * 最後更新：2026-04-02 23:00 (UTC+8)
 */
int  board_can_capture(const Cell *attacker, const Cell *defender);

/*
 * 函數名稱：board_generate_moves
 * 功能：產生當前回合所有合法走法（翻棋、移動、吃子）
 * 輸入：gs - 指向當前遊戲狀態的常數指標, moves - 用於儲存合法走法的陣列
 * 輸出：int - 合法走法的數量
 * 最後更新：2026-04-02 23:00 (UTC+8)
 */
int  board_generate_moves(const GameState *gs, Move *moves);

/*
 * 函數名稱：board_apply_move
 * 功能：將一步走法套用至遊戲狀態，更新棋盤、回合及勝負判定
 * 輸入：gs - 指向遊戲狀態的指標, mv - 指向欲執行走法的常數指標
 * 輸出：int - 成功回傳 1，非法走法回傳 0
 * 最後更新：2026-04-02 23:00 (UTC+8)
 */
int  board_apply_move(GameState *gs, const Move *mv);

/*
 * 函數名稱：board_check_game_over
 * 功能：檢查遊戲結束條件：一方棋子全滅、連續無吃子達上限（和棋）、無合法走法
 * 輸入：gs - 指向遊戲狀態的指標
 * 輸出：無（void），直接修改 gs->game_over 與 gs->winner
 * 最後更新：2026-04-02 23:00 (UTC+8)
 */
void board_check_game_over(GameState *gs);

#endif /* BOARD_H */
