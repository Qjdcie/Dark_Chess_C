/*
 * raylib_gui.c — Raylib Graphical Board for Chinese Dark Chess (暗棋)
 *
 * Modes:
 *   - Player First / Computer First: local game vs AI
 *   - Online Battle: connect to battle server, play against remote opponent
 *
 * Features:
 *   - 4x8 board with wooden-style colours
 *   - Click to flip / select+click to move/capture
 *   - Computer auto-plays via strategy module (offline mode)
 *   - Online mode connects to battle server for board state
 *   - CJK piece characters via embedded monospace font
 */

#include "raylib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "strategy.h"
#include "net_client.h"
#include "raylib_gui.h"
#include "embedded_font.h"

/* =====================================================================
 *  Constants
 * ===================================================================== */

/* Layout */
#define CELL_SIZE      90
#define BOARD_PAD      40
#define STATUS_HEIGHT  50

#define WIN_W  (BOARD_PAD * 2 + BOARD_COLS * CELL_SIZE)
#define WIN_H  (BOARD_PAD * 2 + BOARD_ROWS * CELL_SIZE + STATUS_HEIGHT)

#define PIECE_R  (CELL_SIZE / 2 - 6)

/* Font sizes */
#define FONT_SIZE_LARGE  36
#define FONT_SIZE_SMALL  18

/* Choose-screen button dimensions */
#define BTN_W    280
#define BTN_H     60
#define BTN_GAP   30

/* Room input */
#define ROOM_ID_MAX  15
#define INPUT_BOX_W  240
#define INPUT_BOX_H  40

/* =====================================================================
 *  Types
 * ===================================================================== */

typedef enum {
    SCREEN_CHOOSE        = 0,   /* 選擇模式畫面 */
    SCREEN_PLAYING       = 1,   /* 遊戲進行中 */
    SCREEN_ONLINE_CHOOSE = 2,   /* 線上對戰：選擇電腦/玩家 */
    SCREEN_ONLINE_ROOM   = 3    /* 輸入房號畫面 */
} ScreenState;

/* Connection state for online room screen */
typedef enum {
    CONN_IDLE       = 0,
    CONN_CONNECTING = 1,
    CONN_FAILED     = 2
} ConnState;

/* =====================================================================
 *  Colours
 * ===================================================================== */

/* Board */
static const Color COL_BG              = {  60,  40,  20, 255 };
static const Color COL_BOARD           = { 220, 190, 130, 255 };
static const Color COL_LINE            = {  80,  60,  30, 255 };
static const Color COL_HIGHLIGHT       = { 255, 255, 100, 120 };
static const Color COL_LASTMOVE        = { 180, 220, 180,  80 };
static const Color COL_LASTMOVE_FROM   = { 180, 220, 180,  50 };

/* Pieces */
static const Color COL_FACEDOWN        = { 139,  90,  43, 255 };
static const Color COL_FACEDOWN_BORDER = { 100,  65,  25, 255 };
static const Color COL_PIECE_BG        = { 245, 235, 210, 255 };
static const Color COL_RED             = { 200,  30,  30, 255 };
static const Color COL_BLACK_TEXT      = {  30,  30,  30, 255 };
static const Color COL_PIECE_BORDER    = {  60,  40,  20, 255 };
static const Color COL_FACEDOWN_TEXT   = { 200, 170, 120, 255 };

/* Status bar */
static const Color COL_STATUS_BG       = {  40,  30,  15, 255 };
static const Color COL_STATUS_TEXT     = { 220, 200, 160, 255 };

/* Choose-screen buttons */
static const Color COL_BTN_NORMAL  = { 180, 140,  90, 255 };
static const Color COL_BTN_HOVER   = { 220, 180, 120, 255 };
static const Color COL_BTN_BORDER  = {  80,  60,  30, 255 };
static const Color COL_BTN_TEXT    = {  40,  30,  15, 255 };
static const Color COL_TITLE_TEXT  = { 240, 220, 180, 255 };

/* Input field */
static const Color COL_INPUT_BG    = { 245, 235, 210, 255 };
static const Color COL_INPUT_BORDER= {  80,  60,  30, 255 };
static const Color COL_INPUT_TEXT  = {  40,  30,  15, 255 };
static const Color COL_ERROR_TEXT  = { 220,  60,  60, 255 };

/* =====================================================================
 *  Font
 * ===================================================================== */

static Font g_font = { 0 };
static int  g_font_ready = 0;


/*
 * 函數名稱：build_codepoints
 * 功能：收集遊戲所需的所有 Unicode 碼位（ASCII 可印字元 + 中文棋子名稱 + 狀態列文字）
 * 輸入：out - 儲存碼位的整數陣列, max - 陣列最大容量
 * 輸出：int - 實際收集到的碼位數量
 * 最後更新：2026-04-02 23:00 (UTC+8)
 */
static int build_codepoints(int *out, int max)
{
    int count = 0;

    /* ASCII printable range */
    for (int i = 0x20; i <= 0x7E && count < max; i++)
        out[count++] = i;

    /* All CJK characters used in piece names + status bar text.
     * Piece names: 帥仕相俥傌炮兵將士象車馬砲卒
     * Status text: 遊戲結束紅方勝黑和棋按重新開始的回合無吃子
     * Fullwidth:   ？—｜ */
    static const char *cjk_chars[] = {
        /* Red pieces */
        "帥", "仕", "相", "俥", "傌", "炮", "兵",
        /* Black pieces */
        "將", "士", "象", "車", "馬", "砲", "卒",
        /* Status bar */
        "遊", "戲", "結", "束", "紅", "方", "勝",
        "黑", "和", "棋", "按", "重", "新", "開",
        "始", "的", "回", "合", "無", "吃", "子",
        /* Fullwidth punctuation */
        "？", "—", "｜",
        NULL
    };

    for (int i = 0; cjk_chars[i] && count < max; i++) {
        /* Decode UTF-8 codepoint */
        const unsigned char *s = (const unsigned char *)cjk_chars[i];
        int cp = 0;
        if (s[0] < 0x80) {
            cp = s[0];
        } else if ((s[0] & 0xE0) == 0xC0) {
            cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        } else if ((s[0] & 0xF0) == 0xE0) {
            cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        } else if ((s[0] & 0xF8) == 0xF0) {
            cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12)
               | ((s[2] & 0x3F) << 6)  |  (s[3] & 0x3F);
        }
        if (cp > 0) out[count++] = cp;
    }

    return count;
}

/*
 * 函數名稱：load_font
 * 功能：從內嵌字型資料載入字型，失敗則報錯
 * 輸入：無（void），使用全域變數 g_font, g_font_ready
 * 輸出：int - 成功回傳 1，失敗回傳 0
 * 最後更新：2026-04-06 (UTC+8)
 */
static int load_font(void)
{
    int codepoints[256];
    int cp_count = build_codepoints(codepoints, 256);

    g_font = LoadFontFromMemory(".ttf",
                embedded_font_data, (int)embedded_font_data_len,
                FONT_SIZE_LARGE, codepoints, cp_count);
    if (g_font.glyphCount > 0) {
        SetTextureFilter(g_font.texture, TEXTURE_FILTER_BILINEAR);
        g_font_ready = 1;
        return 1;
    }

    fprintf(stderr, "[FATAL] 無法載入內嵌字型，程式無法繼續執行。\n");
    return 0;
}

/*
 * 函數名稱：unload_font
 * 功能：釋放已載入的字型資源
 * 輸入：無（void），使用全域變數 g_font, g_font_ready
 * 輸出：無（void），釋放後設定 g_font_ready = 0
 * 最後更新：2026-04-02 23:00 (UTC+8)
 */
static void unload_font(void)
{
    if (g_font_ready) {
        UnloadFont(g_font);
        g_font_ready = 0;
    }
}

/*
 * 函數名稱：render_glyph_centered
 * 功能：在指定座標繪製單一 UTF-8 字元，精確置中對齊
 * 輸入：utf8_char - UTF-8 字元字串, cx/cy - 置中座標, fontSize - 字型大小, color - 顏色
 * 輸出：無（void），直接繪製至畫面
 * 最後更新：2026-04-02 23:00 (UTC+8)
 */
static void render_glyph_centered(const char *utf8_char, int cx, int cy,
                                   int fontSize, Color color)
{
    if (!g_font_ready || !utf8_char || !utf8_char[0]) return;

    int bytes = 0;
    int cp = GetCodepointNext(utf8_char, &bytes);
    int idx = GetGlyphIndex(g_font, cp);

    float scale = (float)fontSize / (float)g_font.baseSize;

    /* Source rectangle in the font atlas */
    Rectangle src = g_font.recs[idx];

    /* Destination: centre the bitmap at (cx, cy) */
    float gw = src.width  * scale;
    float gh = src.height * scale;
    Rectangle dst = { cx - gw / 2.0f, cy - gh / 2.0f, gw, gh };

    DrawTexturePro(g_font.texture, src, dst, (Vector2){0, 0}, 0.0f, color);
}

/*
 * 函數名稱：render_text_centered
 * 功能：在指定座標繪製多字元 UTF-8 文字，使用自訂字型置中對齊
 * 輸入：text - UTF-8 文字字串, cx/cy - 置中座標, fontSize - 字型大小, color - 顏色
 * 輸出：無（void），直接繪製至畫面
 * 最後更新：2026-04-02 23:00 (UTC+8)
 */
static void render_text_centered(const char *text, int cx, int cy,
                                  int fontSize, Color color)
{
    if (!g_font_ready || !text || !text[0]) return;
    Vector2 size = MeasureTextEx(g_font, text, (float)fontSize, 1);
    Vector2 pos = { cx - size.x / 2.0f, cy - size.y / 2.0f };
    DrawTextEx(g_font, text, pos, (float)fontSize, 1, color);
}

/* =====================================================================
 *  Helpers
 * ===================================================================== */

/*
 * 函數名稱：cell_centre
 * 功能：將棋盤格座標轉換為螢幕像素中心點座標
 * 輸入：row/col - 棋盤格座標(1-based), cx/cy - 輸出的螢幕像素座標指標
 * 輸出：無（void），透過 cx/cy 指標回傳結果
 * 最後更新：2026-04-02 23:00 (UTC+8)
 */
static void cell_centre(int row, int col, int *cx, int *cy)
{
    *cx = BOARD_PAD + (col - 1) * CELL_SIZE + CELL_SIZE / 2;
    *cy = BOARD_PAD + (row - 1) * CELL_SIZE + CELL_SIZE / 2;
}

/*
 * 函數名稱：pixel_to_cell
 * 功能：將螢幕像素座標轉換為棋盤格座標（1-based）
 * 輸入：px/py - 螢幕像素座標, row/col - 輸出的棋盤格座標指標
 * 輸出：int - 在棋盤範圍內回傳 1，超出範圍回傳 0
 * 最後更新：2026-04-02 23:00 (UTC+8)
 */
static int pixel_to_cell(int px, int py, int *row, int *col)
{
    int c = (px - BOARD_PAD) / CELL_SIZE + 1;
    int r = (py - BOARD_PAD) / CELL_SIZE + 1;
    if (r >= 1 && r <= BOARD_ROWS && c >= 1 && c <= BOARD_COLS) {
        *row = r;
        *col = c;
        return 1;
    }
    return 0;
}

/* =====================================================================
 *  Piece text
 * ===================================================================== */

/*
 * 函數名稱：piece_cjk
 * 功能：根據陣營與階級回傳對應的中文棋子名稱字串
 * 輸入：side - 棋子陣營（紅/黑）, rank - 棋子階級
 * 輸出：const char* - 對應的中文字元字串（如 "帥"、"將"）
 * 最後更新：2026-04-02 23:00 (UTC+8)
 */
static const char *piece_cjk(Side side, PieceRank rank)
{
    if (side == SIDE_RED) {
        switch (rank) {
        case RANK_GENERAL:  return "帥";
        case RANK_ADVISOR:  return "仕";
        case RANK_ELEPHANT: return "相";
        case RANK_CHARIOT:  return "俥";
        case RANK_HORSE:    return "傌";
        case RANK_CANNON:   return "炮";
        case RANK_SOLDIER:  return "兵";
        default: return "?";
        }
    } else {
        switch (rank) {
        case RANK_GENERAL:  return "將";
        case RANK_ADVISOR:  return "士";
        case RANK_ELEPHANT: return "象";
        case RANK_CHARIOT:  return "車";
        case RANK_HORSE:    return "馬";
        case RANK_CANNON:   return "砲";
        case RANK_SOLDIER:  return "卒";
        default: return "?";
        }
    }
}

/* =====================================================================
 *  Draw
 * ===================================================================== */

/*
 * 函數名稱：draw_board
 * 功能：繪製完整棋盤畫面，包含格線、棋子、選取高亮、上一步標記及狀態列
 * 輸入：gs - 遊戲狀態指標, sel_r/sel_c - 選取中的棋子座標(0 表示無選取),
 *       last_from_r/last_from_c - 上一步起點, last_to_r/last_to_c - 上一步終點
 * 輸出：無（void），直接繪製至 Raylib 畫面
 * 最後更新：2026-04-02 23:00 (UTC+8)
 */
static void draw_board(const GameState *gs,
                       int sel_r, int sel_c,
                       int last_from_r, int last_from_c,
                       int last_to_r, int last_to_c)
{
    ClearBackground(COL_BG);

    /* Board surface */
    DrawRectangle(BOARD_PAD - 5, BOARD_PAD - 5,
                  BOARD_COLS * CELL_SIZE + 10,
                  BOARD_ROWS * CELL_SIZE + 10,
                  COL_BOARD);

    /* Grid lines */
    for (int r = 0; r <= BOARD_ROWS; r++) {
        int y = BOARD_PAD + r * CELL_SIZE;
        DrawLine(BOARD_PAD, y,
                 BOARD_PAD + BOARD_COLS * CELL_SIZE, y, COL_LINE);
    }
    for (int c = 0; c <= BOARD_COLS; c++) {
        int x = BOARD_PAD + c * CELL_SIZE;
        DrawLine(x, BOARD_PAD,
                 x, BOARD_PAD + BOARD_ROWS * CELL_SIZE, COL_LINE);
    }

    /* Highlight last move cells */
    if (last_to_r >= 1) {
        DrawRectangle(BOARD_PAD + (last_to_c - 1) * CELL_SIZE + 1,
                      BOARD_PAD + (last_to_r - 1) * CELL_SIZE + 1,
                      CELL_SIZE - 1, CELL_SIZE - 1,
                      COL_LASTMOVE);
    }
    if (last_from_r >= 1 && (last_from_r != last_to_r || last_from_c != last_to_c)) {
        DrawRectangle(BOARD_PAD + (last_from_c - 1) * CELL_SIZE + 1,
                      BOARD_PAD + (last_from_r - 1) * CELL_SIZE + 1,
                      CELL_SIZE - 1, CELL_SIZE - 1,
                      COL_LASTMOVE_FROM);
    }

    /* Highlight selected cell */
    if (sel_r >= 1 && sel_c >= 1) {
        DrawRectangle(BOARD_PAD + (sel_c - 1) * CELL_SIZE + 1,
                      BOARD_PAD + (sel_r - 1) * CELL_SIZE + 1,
                      CELL_SIZE - 1, CELL_SIZE - 1,
                      COL_HIGHLIGHT);
    }

    /* Draw pieces */
    for (int r = 1; r <= BOARD_ROWS; r++) {
        for (int c = 1; c <= BOARD_COLS; c++) {
            const Cell *cell = CELL(gs, r, c);
            if (cell->state == STATE_EMPTY) continue;

            int cx, cy;
            cell_centre(r, c, &cx, &cy);

            if (cell->state == STATE_FACEDOWN) {
                /* Face-down piece: brown circle with pattern */
                DrawCircle(cx, cy, (float)(PIECE_R + 2), COL_FACEDOWN_BORDER);
                DrawCircle(cx, cy, (float)PIECE_R, COL_FACEDOWN);
                /* Inner decorative ring */
                DrawRing((Vector2){ (float)cx, (float)cy },
                         (float)(PIECE_R - 9), (float)(PIECE_R - 8),
                         0, 360, 36, COL_FACEDOWN_BORDER);
            } else {
                /* Face-up piece */
                DrawCircle(cx, cy, (float)(PIECE_R + 2), COL_PIECE_BORDER);
                DrawCircle(cx, cy, (float)PIECE_R, COL_PIECE_BG);

                Color ring_col = (cell->side == SIDE_RED) ? COL_RED : COL_BLACK_TEXT;
                DrawRing((Vector2){ (float)cx, (float)cy },
                         (float)(PIECE_R - 5), (float)(PIECE_R - 4),
                         0, 360, 36, ring_col);

                const char *txt = piece_cjk(cell->side, cell->rank);
                render_glyph_centered(txt, cx, cy, FONT_SIZE_LARGE, ring_col);
            }
        }
    }

    /* Status bar */
    DrawRectangle(0, WIN_H - STATUS_HEIGHT, WIN_W, STATUS_HEIGHT, COL_STATUS_BG);

    char status[128];
    if (gs->game_over) {
        if (gs->winner == SIDE_RED)
            snprintf(status, sizeof(status), "Game Over - RED wins! (R to restart)");
        else if (gs->winner == SIDE_BLACK)
            snprintf(status, sizeof(status), "Game Over - BLACK wins! (R to restart)");
        else
            snprintf(status, sizeof(status), "Game Over - Draw! (R to restart)");
    } else {
        const char *turn = (gs->current_turn == SIDE_RED) ? "RED" : "BLACK";
        snprintf(status, sizeof(status), "%s turn | R:%d B:%d | No capture: %d/%d",
                 turn, gs->red_alive, gs->black_alive,
                 gs->no_capture_turns, MAX_NO_CAPTURE);
    }

    render_text_centered(status,
                         WIN_W / 2, WIN_H - STATUS_HEIGHT / 2,
                         FONT_SIZE_SMALL, COL_STATUS_TEXT);
}

/*
 * 函數名稱：draw_choose_screen
 * 功能：繪製選擇模式畫面，顯示三個按鈕
 * 輸入：mouse_x/mouse_y - 滑鼠座標
 * 輸出：無（void），直接繪製至 Raylib 畫面
 */
static void draw_choose_screen(int mouse_x, int mouse_y)
{
    ClearBackground(COL_BG);

    /* 標題 */
    int total_btn_height = 3 * BTN_H + 2 * BTN_GAP;
    int start_y = WIN_H / 2 - total_btn_height / 2;
    int title_y = start_y - 50;
    render_text_centered("Dark Chess", WIN_W / 2, title_y,
                         FONT_SIZE_LARGE, COL_TITLE_TEXT);

    /* 三個按鈕的位置 */
    int btn_x  = WIN_W / 2 - BTN_W / 2;
    int btn1_y = start_y;
    int btn2_y = start_y + BTN_H + BTN_GAP;
    int btn3_y = start_y + 2 * (BTN_H + BTN_GAP);

    Rectangle btn1 = { (float)btn_x, (float)btn1_y, (float)BTN_W, (float)BTN_H };
    Rectangle btn2 = { (float)btn_x, (float)btn2_y, (float)BTN_W, (float)BTN_H };
    Rectangle btn3 = { (float)btn_x, (float)btn3_y, (float)BTN_W, (float)BTN_H };

    int hover1 = CheckCollisionPointRec((Vector2){ (float)mouse_x, (float)mouse_y }, btn1);
    int hover2 = CheckCollisionPointRec((Vector2){ (float)mouse_x, (float)mouse_y }, btn2);
    int hover3 = CheckCollisionPointRec((Vector2){ (float)mouse_x, (float)mouse_y }, btn3);

    /* 按鈕 1：玩家先手 */
    DrawRectangleRec(btn1, hover1 ? COL_BTN_HOVER : COL_BTN_NORMAL);
    DrawRectangleLinesEx(btn1, 2, COL_BTN_BORDER);
    render_text_centered("Player First",
                         btn_x + BTN_W / 2, btn1_y + BTN_H / 2,
                         FONT_SIZE_SMALL + 4, COL_BTN_TEXT);

    /* 按鈕 2：電腦先手 */
    DrawRectangleRec(btn2, hover2 ? COL_BTN_HOVER : COL_BTN_NORMAL);
    DrawRectangleLinesEx(btn2, 2, COL_BTN_BORDER);
    render_text_centered("Computer First",
                         btn_x + BTN_W / 2, btn2_y + BTN_H / 2,
                         FONT_SIZE_SMALL + 4, COL_BTN_TEXT);

    /* 按鈕 3：線上對戰 */
    DrawRectangleRec(btn3, hover3 ? COL_BTN_HOVER : COL_BTN_NORMAL);
    DrawRectangleLinesEx(btn3, 2, COL_BTN_BORDER);
    render_text_centered("Online Battle",
                         btn_x + BTN_W / 2, btn3_y + BTN_H / 2,
                         FONT_SIZE_SMALL + 4, COL_BTN_TEXT);
}

/*
 * 函數名稱：draw_online_choose_screen
 * 功能：繪製線上對戰模式選擇畫面（電腦遊玩 / 玩家遊玩）
 * 輸入：mouse_x/mouse_y - 滑鼠座標
 * 輸出：無（void），直接繪製至 Raylib 畫面
 */
static void draw_online_choose_screen(int mouse_x, int mouse_y)
{
    ClearBackground(COL_BG);

    /* Title */
    int total_btn_height = 2 * BTN_H + BTN_GAP;
    int start_y = WIN_H / 2 - total_btn_height / 2;
    int title_y = start_y - 50;
    render_text_centered("Online Battle", WIN_W / 2, title_y,
                         FONT_SIZE_LARGE, COL_TITLE_TEXT);

    /* Two buttons */
    int btn_x  = WIN_W / 2 - BTN_W / 2;
    int btn1_y = start_y;
    int btn2_y = start_y + BTN_H + BTN_GAP;

    Rectangle btn1 = { (float)btn_x, (float)btn1_y, (float)BTN_W, (float)BTN_H };
    Rectangle btn2 = { (float)btn_x, (float)btn2_y, (float)BTN_W, (float)BTN_H };

    int hover1 = CheckCollisionPointRec((Vector2){ (float)mouse_x, (float)mouse_y }, btn1);
    int hover2 = CheckCollisionPointRec((Vector2){ (float)mouse_x, (float)mouse_y }, btn2);

    /* Button 1: Computer Play */
    DrawRectangleRec(btn1, hover1 ? COL_BTN_HOVER : COL_BTN_NORMAL);
    DrawRectangleLinesEx(btn1, 2, COL_BTN_BORDER);
    render_text_centered("Computer Play",
                         btn_x + BTN_W / 2, btn1_y + BTN_H / 2,
                         FONT_SIZE_SMALL + 4, COL_BTN_TEXT);

    /* Button 2: Player Play */
    DrawRectangleRec(btn2, hover2 ? COL_BTN_HOVER : COL_BTN_NORMAL);
    DrawRectangleLinesEx(btn2, 2, COL_BTN_BORDER);
    render_text_centered("Player Play",
                         btn_x + BTN_W / 2, btn2_y + BTN_H / 2,
                         FONT_SIZE_SMALL + 4, COL_BTN_TEXT);

    /* Back hint */
    render_text_centered("Press R to go back",
                         WIN_W / 2, btn2_y + BTN_H + 30,
                         FONT_SIZE_SMALL, COL_STATUS_TEXT);
}

/*
 * 函數名稱：draw_online_room_screen
 * 功能：繪製線上對戰房號輸入畫面
 * 輸入：mouse_x/mouse_y - 滑鼠座標, room_input - 房號字串,
 *       conn_state - 連線狀態, error_msg - 錯誤訊息
 * 輸出：無（void），直接繪製至 Raylib 畫面
 */
static void draw_online_room_screen(int mouse_x, int mouse_y,
                                    const char *room_input,
                                    ConnState conn_state,
                                    const char *error_msg)
{
    ClearBackground(COL_BG);

    /* Title */
    int center_y = WIN_H / 2 - 60;
    render_text_centered("Online Battle", WIN_W / 2, center_y - 60,
                         FONT_SIZE_LARGE, COL_TITLE_TEXT);

    /* Room ID label */
    render_text_centered("Room ID:", WIN_W / 2, center_y,
                         FONT_SIZE_SMALL + 2, COL_TITLE_TEXT);

    /* Input box */
    int input_x = WIN_W / 2 - INPUT_BOX_W / 2;
    int input_y = center_y + 20;
    Rectangle input_rect = { (float)input_x, (float)input_y,
                              (float)INPUT_BOX_W, (float)INPUT_BOX_H };
    DrawRectangleRec(input_rect, COL_INPUT_BG);
    DrawRectangleLinesEx(input_rect, 2, COL_INPUT_BORDER);

    /* Input text + blinking cursor */
    char display_text[ROOM_ID_MAX + 2];
    snprintf(display_text, sizeof(display_text), "%s", room_input);
    if ((int)(GetTime() * 2) % 2 == 0) {
        int len = (int)strlen(display_text);
        if (len < ROOM_ID_MAX) {
            display_text[len] = '_';
            display_text[len + 1] = '\0';
        }
    }
    render_text_centered(display_text,
                         WIN_W / 2, input_y + INPUT_BOX_H / 2,
                         FONT_SIZE_SMALL + 2, COL_INPUT_TEXT);

    /* Buttons: Back and Confirm */
    int btn_w = 120;
    int btn_h = 45;
    int btn_gap = 40;
    int btn_y = input_y + INPUT_BOX_H + 30;
    int back_x  = WIN_W / 2 - btn_gap / 2 - btn_w;
    int conf_x  = WIN_W / 2 + btn_gap / 2;

    Rectangle btn_back = { (float)back_x, (float)btn_y,
                            (float)btn_w, (float)btn_h };
    Rectangle btn_conf = { (float)conf_x, (float)btn_y,
                            (float)btn_w, (float)btn_h };

    int hover_back = CheckCollisionPointRec(
        (Vector2){ (float)mouse_x, (float)mouse_y }, btn_back);
    int hover_conf = CheckCollisionPointRec(
        (Vector2){ (float)mouse_x, (float)mouse_y }, btn_conf);

    DrawRectangleRec(btn_back, hover_back ? COL_BTN_HOVER : COL_BTN_NORMAL);
    DrawRectangleLinesEx(btn_back, 2, COL_BTN_BORDER);
    render_text_centered("Back",
                         back_x + btn_w / 2, btn_y + btn_h / 2,
                         FONT_SIZE_SMALL + 2, COL_BTN_TEXT);

    DrawRectangleRec(btn_conf, hover_conf ? COL_BTN_HOVER : COL_BTN_NORMAL);
    DrawRectangleLinesEx(btn_conf, 2, COL_BTN_BORDER);
    render_text_centered("Confirm",
                         conf_x + btn_w / 2, btn_y + btn_h / 2,
                         FONT_SIZE_SMALL + 2, COL_BTN_TEXT);

    /* Status / error message */
    if (conn_state == CONN_CONNECTING) {
        render_text_centered("Connecting...",
                             WIN_W / 2, btn_y + btn_h + 30,
                             FONT_SIZE_SMALL, COL_TITLE_TEXT);
    } else if (conn_state == CONN_FAILED && error_msg[0]) {
        render_text_centered(error_msg,
                             WIN_W / 2, btn_y + btn_h + 30,
                             FONT_SIZE_SMALL, COL_ERROR_TEXT);
    }
}

/* =====================================================================
 *  公開 API
 * ===================================================================== */

/*
 * 函數名稱：gui_main
 * 功能：遊戲主迴圈，含選擇先手畫面、首翻決定顏色、玩家輸入處理、AI 回合、畫面繪製
 * 輸入：auto_online - 非零表示自動啟動線上對戰（電腦模式）
 *       auto_room_id - 自動加入的房號（NULL 表示不自動加入）
 * 輸出：int - 正常結束回傳 0，字型載入失敗回傳 1
 * 最後更新：2026-04-26 (UTC+8)
 */
int gui_main(int auto_online, const char *auto_room_id)
{
    InitWindow(WIN_W, WIN_H, "Dark Chess");
    SetTargetFPS(60);

    if (!load_font()) {
        CloseWindow();
        return 1;
    }

    /* Game state */
    GameState gs;
    board_init(&gs);

    ScreenState screen = SCREEN_CHOOSE;
    int player_goes_first = 1;       /* 1 = 玩家先翻, 0 = 電腦先翻 */
    int sides_determined  = 0;       /* 首翻後才決定雙方顏色 */
    Side player_side = SIDE_NONE;    /* 由首翻決定 */
    int sel_r = 0, sel_c = 0;       /* selected piece (0 = none) */
    int last_from_r = 0, last_from_c = 0;
    int last_to_r = 0, last_to_c = 0;
    int ai_delay = 0;               /* frames to wait before AI moves */

    /* --- Online mode state --- */
    int online_mode = 0;
    int online_ai_mode = 0;         /* 1 = 電腦自動遊玩, 0 = 玩家操控 */
    char room_input[ROOM_ID_MAX + 1] = "";
    ConnState conn_state = CONN_IDLE;
    int conn_frame = 0;             /* frame counter for connecting */
    char conn_error[64] = "";
    char my_role_ab[4] = "";        /* "A" or "B" */
    int  waiting_for_update = 0;    /* 1 = sent action, waiting for server response */
    Cell prev_cells[BOARD_ROWS][BOARD_COLS]; /* previous board for diff */
    NetUpdateInfo net_info;
    memset(&net_info, 0, sizeof(net_info));
    memset(prev_cells, 0, sizeof(prev_cells));

    /* CLI auto-start: -online_battle <room_id> → 電腦模式直接連線 */
    if (auto_online && auto_room_id) {
        online_ai_mode = 1;
        strncpy(room_input, auto_room_id, ROOM_ID_MAX);
        room_input[ROOM_ID_MAX] = '\0';
        conn_state = CONN_CONNECTING;
        conn_frame = 0;
        screen = SCREEN_ONLINE_ROOM;
    }

    while (!WindowShouldClose()) {
        if (IsKeyPressed(KEY_Q)) break;

        Vector2 mouse = GetMousePosition();
        int mx = (int)mouse.x, my = (int)mouse.y;

        /* ============================================================
         *  畫面：選擇模式
         * ============================================================ */
        if (screen == SCREEN_CHOOSE) {
            int total_btn_height = 3 * BTN_H + 2 * BTN_GAP;
            int start_y = WIN_H / 2 - total_btn_height / 2;
            int btn_x  = WIN_W / 2 - BTN_W / 2;
            int btn1_y = start_y;
            int btn2_y = start_y + BTN_H + BTN_GAP;
            int btn3_y = start_y + 2 * (BTN_H + BTN_GAP);

            Rectangle btn1 = { (float)btn_x, (float)btn1_y,
                                (float)BTN_W, (float)BTN_H };
            Rectangle btn2 = { (float)btn_x, (float)btn2_y,
                                (float)BTN_W, (float)BTN_H };
            Rectangle btn3 = { (float)btn_x, (float)btn3_y,
                                (float)BTN_W, (float)BTN_H };

            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                Vector2 mp = { (float)mx, (float)my };
                if (CheckCollisionPointRec(mp, btn1) ||
                    CheckCollisionPointRec(mp, btn2)) {
                    /* Offline mode: Player First or Computer First */
                    player_goes_first = CheckCollisionPointRec(mp, btn1) ? 1 : 0;
                    online_mode = 0;
                    strategy_reset();
                    board_init(&gs);
                    sides_determined = 0;
                    player_side = SIDE_NONE;
                    sel_r = sel_c = 0;
                    last_from_r = last_from_c = 0;
                    last_to_r = last_to_c = 0;
                    ai_delay = player_goes_first ? 0 : 20;
                    screen = SCREEN_PLAYING;
                } else if (CheckCollisionPointRec(mp, btn3)) {
                    /* Online Battle → choose Computer/Player */
                    screen = SCREEN_ONLINE_CHOOSE;
                }
            }

            BeginDrawing();
            draw_choose_screen(mx, my);
            EndDrawing();
            continue;
        }

        /* ============================================================
         *  畫面：線上對戰模式選擇（電腦 / 玩家）
         * ============================================================ */
        if (screen == SCREEN_ONLINE_CHOOSE) {
            if (IsKeyPressed(KEY_R)) {
                screen = SCREEN_CHOOSE;
            } else if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                int total_btn_height = 2 * BTN_H + BTN_GAP;
                int start_y = WIN_H / 2 - total_btn_height / 2;
                int btn_x  = WIN_W / 2 - BTN_W / 2;
                int btn1_y = start_y;
                int btn2_y = start_y + BTN_H + BTN_GAP;

                Rectangle btn1 = { (float)btn_x, (float)btn1_y,
                                    (float)BTN_W, (float)BTN_H };
                Rectangle btn2 = { (float)btn_x, (float)btn2_y,
                                    (float)BTN_W, (float)BTN_H };
                Vector2 mp = { (float)mx, (float)my };

                if (CheckCollisionPointRec(mp, btn1)) {
                    online_ai_mode = 1;  /* Computer Play */
                    room_input[0] = '\0';
                    conn_state = CONN_IDLE;
                    conn_error[0] = '\0';
                    screen = SCREEN_ONLINE_ROOM;
                } else if (CheckCollisionPointRec(mp, btn2)) {
                    online_ai_mode = 0;  /* Player Play */
                    room_input[0] = '\0';
                    conn_state = CONN_IDLE;
                    conn_error[0] = '\0';
                    screen = SCREEN_ONLINE_ROOM;
                }
            }

            BeginDrawing();
            draw_online_choose_screen(mx, my);
            EndDrawing();
            continue;
        }

        /* ============================================================
         *  畫面：輸入房號（Online Battle）
         * ============================================================ */
        if (screen == SCREEN_ONLINE_ROOM) {
            /* Handle text input for room ID */
            if (conn_state == CONN_IDLE || conn_state == CONN_FAILED) {
                /* Keyboard input for room ID */
                int key = GetCharPressed();
                while (key > 0) {
                    if (key >= 32 && key < 127) {
                        int len = (int)strlen(room_input);
                        if (len < ROOM_ID_MAX) {
                            room_input[len] = (char)key;
                            room_input[len + 1] = '\0';
                        }
                    }
                    key = GetCharPressed();
                }
                if (IsKeyPressed(KEY_BACKSPACE)) {
                    int len = (int)strlen(room_input);
                    if (len > 0) room_input[len - 1] = '\0';
                }

                /* Enter key = confirm */
                if (IsKeyPressed(KEY_ENTER) && room_input[0]) {
                    conn_state = CONN_CONNECTING;
                    conn_frame = 0;
                }

                /* Button clicks */
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    int center_y = WIN_H / 2 - 60;
                    int input_y = center_y + 20;
                    int btn_w = 120, btn_h = 45, btn_gap = 40;
                    int btn_y = input_y + INPUT_BOX_H + 30;
                    int back_x = WIN_W / 2 - btn_gap / 2 - btn_w;
                    int conf_x = WIN_W / 2 + btn_gap / 2;

                    Rectangle btn_back = { (float)back_x, (float)btn_y,
                                            (float)btn_w, (float)btn_h };
                    Rectangle btn_conf = { (float)conf_x, (float)btn_y,
                                            (float)btn_w, (float)btn_h };
                    Vector2 mp = { (float)mx, (float)my };

                    if (CheckCollisionPointRec(mp, btn_back)) {
                        screen = SCREEN_CHOOSE;
                    } else if (CheckCollisionPointRec(mp, btn_conf) &&
                               room_input[0]) {
                        conn_state = CONN_CONNECTING;
                        conn_frame = 0;
                    }
                }
            }

            /* Handle connection attempt (delayed by 1 frame to show status) */
            if (conn_state == CONN_CONNECTING) {
                if (conn_frame == 0) {
                    conn_frame = 1; /* first frame: just draw "Connecting..." */
                } else {
                    /* Actually connect */
                    if (net_connect(NET_SERVER_IP, NET_SERVER_PORT) != 0) {
                        conn_state = CONN_FAILED;
                        snprintf(conn_error, sizeof(conn_error),
                                 "Connection failed!");
                    } else {
                        char role[16] = "";
                        if (net_join_room(room_input, role,
                                          sizeof(role)) != 0) {
                            conn_state = CONN_FAILED;
                            snprintf(conn_error, sizeof(conn_error),
                                     "Failed to join room %s", room_input);
                            net_close();
                        } else {
                            /* Success! Determine role */
                            if (strcmp(role, "first") == 0)
                                strcpy(my_role_ab, "A");
                            else
                                strcpy(my_role_ab, "B");

                            /* Initialize online game state */
                            online_mode = 1;
                            memset(&gs, 0, sizeof(gs));
                            memset(&net_info, 0, sizeof(net_info));
                            sides_determined = 0;
                            player_side = SIDE_NONE;
                            sel_r = sel_c = 0;
                            last_from_r = last_from_c = 0;
                            last_to_r = last_to_c = 0;
                            waiting_for_update = 0;
                            memset(prev_cells, 0, sizeof(prev_cells));
                            screen = SCREEN_PLAYING;
                        }
                    }
                }
            }

            BeginDrawing();
            draw_online_room_screen(mx, my, room_input,
                                    conn_state, conn_error);
            EndDrawing();
            continue;
        }

        /* ============================================================
         *  畫面：遊戲進行中
         * ============================================================ */

        /* R 鍵重新開始 → 回到選擇先手畫面 */
        if (IsKeyPressed(KEY_R)) {
            if (online_mode) {
                net_close();
                online_mode = 0;
            }
            screen = SCREEN_CHOOSE;
            continue;
        }

        /* ==========================================================
         *  Online mode game loop
         * ========================================================== */
        if (online_mode) {
            /* Check connection */
            if (!net_is_connected()) {
                gs.game_over = 1;
                gs.winner = SIDE_NONE;
            }

            /* Receive updates from server */
            if (!gs.game_over) {
                char recv_buf[8192];
                int recv_result;
                while ((recv_result = net_try_receive(
                            recv_buf, sizeof(recv_buf))) > 0) {
                    /* Save previous board for move detection */
                    memcpy(prev_cells, gs.cells, sizeof(prev_cells));
                    int old_total = net_info.total_moves;

                    if (net_parse_update(recv_buf, &gs, &net_info)) {
                        waiting_for_update = 0;
                        if (online_ai_mode) ai_delay = 0; /* 2s delay */

                        /* Determine player color from color_table */
                        const char *my_color =
                            (strcmp(my_role_ab, "A") == 0)
                            ? net_info.color_a : net_info.color_b;

                        if (strcmp(my_color, "Red") == 0) {
                            player_side = SIDE_RED;
                            sides_determined = 1;
                        } else if (strcmp(my_color, "Black") == 0) {
                            player_side = SIDE_BLACK;
                            sides_determined = 1;
                        } else {
                            player_side = SIDE_NONE;
                            sides_determined = 0;
                        }

                        /* Detect move for highlighting */
                        if (net_info.total_moves != old_total &&
                            old_total >= 0) {
                            int fr = 0, fc = 0, tr = 0, tc = 0;
                            for (int r = 0; r < BOARD_ROWS; r++) {
                                for (int c = 0; c < BOARD_COLS; c++) {
                                    if (prev_cells[r][c].state != STATE_EMPTY
                                        && gs.cells[r][c].state == STATE_EMPTY) {
                                        fr = r + 1; fc = c + 1;
                                    }
                                    if (memcmp(&prev_cells[r][c],
                                               &gs.cells[r][c],
                                               sizeof(Cell)) != 0
                                        && gs.cells[r][c].state != STATE_EMPTY) {
                                        tr = r + 1; tc = c + 1;
                                    }
                                }
                            }
                            if (fr == 0 && tr > 0) {
                                fr = tr; fc = tc;
                            }
                            last_from_r = fr; last_from_c = fc;
                            last_to_r = tr;   last_to_c = tc;
                        }

                        /* Game over is determined by the server.
                         * We only detect it via state field or
                         * connection loss — NOT by local piece counts,
                         * since Covered pieces are invisible to us. */
                        if (strcmp(net_info.state, "playing") != 0 &&
                            strcmp(net_info.state, "waiting") != 0 &&
                            net_info.state[0] != '\0') {
                            gs.game_over = 1;
                            /* Determine winner from server's winner field */
                            if (strcmp(net_info.winner_role, "A") == 0) {
                                if (strcmp(net_info.color_a, "Red") == 0)
                                    gs.winner = SIDE_RED;
                                else
                                    gs.winner = SIDE_BLACK;
                            } else if (strcmp(net_info.winner_role, "B") == 0) {
                                if (strcmp(net_info.color_b, "Red") == 0)
                                    gs.winner = SIDE_RED;
                                else
                                    gs.winner = SIDE_BLACK;
                            } else {
                                gs.winner = SIDE_NONE;  /* draw */
                            }
                        }
                    }
                }
                if (recv_result < 0) {
                    /* Connection lost */
                    gs.game_over = 1;
                    gs.winner = SIDE_NONE;
                }
            }

            /* Determine if it's our turn */
            int is_my_turn = 0;
            if (!gs.game_over &&
                strcmp(net_info.state, "playing") == 0 &&
                strcmp(net_info.current_turn_role, my_role_ab) == 0 &&
                !waiting_for_update) {
                is_my_turn = 1;
            }

            /* --- Online AI auto-play --- */
            if (online_ai_mode && is_my_turn) {
                if (ai_delay > 0) {
                    ai_delay--;
                } else {
                    /* Set current_turn for move generation */
                    gs.current_turn = (player_side != SIDE_NONE)
                                      ? player_side : SIDE_RED;

                    Move legal[MAX_MOVES];
                    int n_legal = board_generate_moves(&gs, legal);
                    if (n_legal > 0) {
                        Move chosen;
                        /* Try strategy module, fall back to random */
                        if (sides_determined) {
                            strategy_reset();
                        }
                        if (!strategy_select_move(&gs, &chosen)) {
                            chosen = legal[rand() % n_legal];
                        }

                        char action[64];
                        if (chosen.type == MOVE_FLIP) {
                            snprintf(action, sizeof(action), "%d %d\n",
                                     chosen.from_r - 1, chosen.from_c - 1);
                        } else {
                            snprintf(action, sizeof(action),
                                     "%d %d %d %d\n",
                                     chosen.from_r - 1, chosen.from_c - 1,
                                     chosen.to_r - 1, chosen.to_c - 1);
                        }
                        net_send_action(action);
                        waiting_for_update = 1;
                        ai_delay = 120; /* ~2 seconds at 60fps */
                    }
                }
            }

            /* --- Online Player input --- */
            if (!online_ai_mode && is_my_turn &&
                IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                int click_r, click_c;
                if (pixel_to_cell(mx, my, &click_r, &click_c)) {
                    const Cell *clicked = CELL(&gs, click_r, click_c);

                    if (sel_r == 0) {
                        /* Nothing selected */
                        if (clicked->state == STATE_FACEDOWN) {
                            /* Flip */
                            char action[32];
                            snprintf(action, sizeof(action), "%d %d\n",
                                     click_r - 1, click_c - 1);
                            net_send_action(action);
                            waiting_for_update = 1;
                            sel_r = sel_c = 0;
                        } else if (clicked->state == STATE_FACEUP &&
                                   sides_determined &&
                                   clicked->side == player_side) {
                            /* Select own piece */
                            sel_r = click_r;
                            sel_c = click_c;
                        }
                    } else {
                        /* Piece is selected */
                        if (click_r == sel_r && click_c == sel_c) {
                            /* Deselect */
                            sel_r = sel_c = 0;
                        } else if (clicked->state == STATE_FACEDOWN) {
                            /* Flip instead (deselect current) */
                            char action[32];
                            snprintf(action, sizeof(action), "%d %d\n",
                                     click_r - 1, click_c - 1);
                            net_send_action(action);
                            waiting_for_update = 1;
                            sel_r = sel_c = 0;
                        } else if (clicked->state == STATE_EMPTY ||
                                   (clicked->state == STATE_FACEUP &&
                                    clicked->side != player_side)) {
                            /* Move or capture: send from→to */
                            char action[32];
                            snprintf(action, sizeof(action),
                                     "%d %d %d %d\n",
                                     sel_r - 1, sel_c - 1,
                                     click_r - 1, click_c - 1);
                            net_send_action(action);
                            waiting_for_update = 1;
                            sel_r = sel_c = 0;
                        } else if (clicked->state == STATE_FACEUP &&
                                   clicked->side == player_side) {
                            /* Re-select different own piece */
                            sel_r = click_r;
                            sel_c = click_c;
                        } else {
                            sel_r = sel_c = 0;
                        }
                    }
                }
            }

            /* Right-click to deselect */
            if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                sel_r = sel_c = 0;
            }

            /* --- Draw online mode --- */
            BeginDrawing();
            draw_board(&gs, sel_r, sel_c,
                       last_from_r, last_from_c, last_to_r, last_to_c);

            /* Override status bar for online mode */
            DrawRectangle(0, WIN_H - STATUS_HEIGHT, WIN_W,
                          STATUS_HEIGHT, COL_STATUS_BG);

            char status[128];
            if (!net_is_connected() || gs.game_over) {
                if (gs.winner == SIDE_RED)
                    snprintf(status, sizeof(status),
                             "Game Over - RED wins! (R to return)");
                else if (gs.winner == SIDE_BLACK)
                    snprintf(status, sizeof(status),
                             "Game Over - BLACK wins! (R to return)");
                else if (!net_is_connected())
                    snprintf(status, sizeof(status),
                             "Disconnected (R to return)");
                else
                    snprintf(status, sizeof(status),
                             "Game Over - Draw! (R to return)");
            } else if (strcmp(net_info.state, "waiting") == 0) {
                snprintf(status, sizeof(status),
                         "Waiting for opponent... (Room: %s, Role: %s)",
                         room_input, my_role_ab);
            } else if (waiting_for_update) {
                snprintf(status, sizeof(status),
                         "Waiting for server...");
            } else {
                const char *mode_str = online_ai_mode
                    ? "[Computer]" : "[Player]";
                const char *turn_str;
                if (online_ai_mode)
                    turn_str = is_my_turn ? "AI thinking..." : "Opponent's turn";
                else
                    turn_str = is_my_turn ? "Your turn" : "Opponent's turn";
                const char *color_str = "";
                if (player_side == SIDE_RED) color_str = " RED";
                else if (player_side == SIDE_BLACK) color_str = " BLACK";
                snprintf(status, sizeof(status),
                         "%s %s%s | R:%d B:%d",
                         mode_str, turn_str, color_str,
                         gs.red_alive, gs.black_alive);
            }

            render_text_centered(status,
                                 WIN_W / 2, WIN_H - STATUS_HEIGHT / 2,
                                 FONT_SIZE_SMALL, COL_STATUS_TEXT);
            EndDrawing();
            continue;
        }

        /* ==========================================================
         *  Offline mode (original logic below)
         * ========================================================== */

        /* ==========================================================
         *  階段一：尚未決定顏色（首翻）
         * ========================================================== */
        if (!sides_determined) {
            if (player_goes_first) {
                /* 玩家翻第一顆棋 */
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    int click_r, click_c;
                    if (pixel_to_cell(mx, my, &click_r, &click_c)) {
                        const Cell *clicked = CELL(&gs, click_r, click_c);
                        if (clicked->state == STATE_FACEDOWN) {
                            /* 先讀取隱藏顏色 */
                            Side flipped_color = clicked->side;

                            Move mv = { MOVE_FLIP, click_r, click_c, click_r, click_c };
                            board_apply_move(&gs, &mv);

                            /* 翻到什麼顏色，玩家就是那個顏色 */
                            player_side = flipped_color;
                            Side second_mover = (flipped_color == SIDE_RED)
                                                ? SIDE_BLACK : SIDE_RED;
                            gs.current_turn = second_mover;
                            sides_determined = 1;

                            last_from_r = click_r; last_from_c = click_c;
                            last_to_r = click_r;   last_to_c = click_c;
                            ai_delay = 30;
                        }
                    }
                }
            } else {
                /* 電腦翻第一顆棋 */
                if (ai_delay > 0) {
                    ai_delay--;
                } else {
                    Move ai_mv;
                    if (strategy_select_move(&gs, &ai_mv)) {
                        Side flipped_color =
                            CELL(&gs, ai_mv.from_r, ai_mv.from_c)->side;

                        board_apply_move(&gs, &ai_mv);

                        /* 電腦翻到什麼顏色，電腦就是那個顏色，玩家是另一方 */
                        player_side = (flipped_color == SIDE_RED)
                                      ? SIDE_BLACK : SIDE_RED;
                        Side second_mover = (flipped_color == SIDE_RED)
                                            ? SIDE_BLACK : SIDE_RED;
                        gs.current_turn = second_mover;
                        sides_determined = 1;

                        last_from_r = ai_mv.from_r; last_from_c = ai_mv.from_c;
                        last_to_r = ai_mv.to_r;     last_to_c = ai_mv.to_c;
                    }
                }
            }
        }

        /* ==========================================================
         *  階段二：顏色已確定，正常遊戲
         * ========================================================== */
        if (sides_determined) {
            /* --- 玩家輸入 --- */
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                if (!gs.game_over && gs.current_turn == player_side) {
                    int click_r, click_c;
                    if (pixel_to_cell(mx, my, &click_r, &click_c)) {
                        const Cell *clicked = CELL(&gs, click_r, click_c);

                        if (sel_r == 0) {
                            if (clicked->state == STATE_FACEDOWN) {
                                Move mv = { MOVE_FLIP, click_r, click_c,
                                            click_r, click_c };
                                board_apply_move(&gs, &mv);
                                last_from_r = click_r; last_from_c = click_c;
                                last_to_r = click_r;   last_to_c = click_c;
                                sel_r = sel_c = 0;
                                ai_delay = 30;
                            } else if (clicked->state == STATE_FACEUP &&
                                       clicked->side == player_side) {
                                sel_r = click_r;
                                sel_c = click_c;
                            }
                        } else {
                            if (click_r == sel_r && click_c == sel_c) {
                                sel_r = sel_c = 0;
                            } else if (clicked->state == STATE_FACEDOWN) {
                                Move mv = { MOVE_FLIP, click_r, click_c,
                                            click_r, click_c };
                                board_apply_move(&gs, &mv);
                                last_from_r = click_r; last_from_c = click_c;
                                last_to_r = click_r;   last_to_c = click_c;
                                sel_r = sel_c = 0;
                                ai_delay = 30;
                            } else {
                                MoveType mt = MOVE_WALK;
                                if (clicked->state == STATE_FACEUP &&
                                    clicked->side != SIDE_NONE &&
                                    clicked->side != player_side) {
                                    mt = MOVE_CAPTURE;
                                }

                                Move mv = { mt, sel_r, sel_c,
                                            click_r, click_c };

                                Move legal[MAX_MOVES];
                                int n_legal = board_generate_moves(&gs, legal);
                                int found = 0;
                                for (int i = 0; i < n_legal; i++) {
                                    if (legal[i].from_r == mv.from_r &&
                                        legal[i].from_c == mv.from_c &&
                                        legal[i].to_r == mv.to_r &&
                                        legal[i].to_c == mv.to_c) {
                                        mv.type = legal[i].type;
                                        found = 1;
                                        break;
                                    }
                                }

                                if (found) {
                                    board_apply_move(&gs, &mv);
                                    last_from_r = mv.from_r;
                                    last_from_c = mv.from_c;
                                    last_to_r = mv.to_r;
                                    last_to_c = mv.to_c;
                                    sel_r = sel_c = 0;
                                    ai_delay = 30;
                                } else {
                                    if (clicked->state == STATE_FACEUP &&
                                        clicked->side == player_side) {
                                        sel_r = click_r;
                                        sel_c = click_c;
                                    } else {
                                        sel_r = sel_c = 0;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            /* Right-click to deselect */
            if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                sel_r = sel_c = 0;
            }

            /* --- Computer turn --- */
            if (!gs.game_over && gs.current_turn != player_side) {
                if (ai_delay > 0) {
                    ai_delay--;
                } else {
                    Move ai_mv;
                    if (strategy_select_move(&gs, &ai_mv)) {
                        board_apply_move(&gs, &ai_mv);
                        last_from_r = ai_mv.from_r;
                        last_from_c = ai_mv.from_c;
                        last_to_r = ai_mv.to_r;
                        last_to_c = ai_mv.to_c;
                    } else {
                        gs.game_over = 1;
                        gs.winner = player_side;
                    }
                }
            }
        }

        /* --- Draw --- */
        BeginDrawing();
        draw_board(&gs, sel_r, sel_c,
                   last_from_r, last_from_c, last_to_r, last_to_c);

        /* 首翻階段：覆蓋狀態列顯示提示 */
        if (!sides_determined) {
            DrawRectangle(0, WIN_H - STATUS_HEIGHT, WIN_W,
                          STATUS_HEIGHT, COL_STATUS_BG);
            const char *hint = player_goes_first
                ? "Click any piece to flip (your color = flipped color)"
                : "Computer is flipping...";
            render_text_centered(hint, WIN_W / 2,
                                 WIN_H - STATUS_HEIGHT / 2,
                                 FONT_SIZE_SMALL, COL_STATUS_TEXT);
        }

        EndDrawing();
    }

    /* Cleanup */
    if (online_mode && net_is_connected()) {
        net_close();
    }
    unload_font();
    CloseWindow();
    return 0;
}
