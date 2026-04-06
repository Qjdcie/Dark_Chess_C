/*
 * raylib_gui.c — Raylib Graphical Board for Chinese Dark Chess (暗棋)
 *
 * Simplified version: Player (RED) vs Random Computer (BLACK)
 * Computer randomly selects a legal move each turn.
 *
 * Features:
 *   - 4x8 board with wooden-style colours
 *   - Click to flip / select+click to move/capture
 *   - Computer auto-plays as BLACK (random moves)
 *   - CJK piece characters via embedded monospace font
 */

#include "raylib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "strategy.h"
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

/* =====================================================================
 *  Types
 * ===================================================================== */

typedef enum {
    SCREEN_CHOOSE  = 0,   /* 選擇先手畫面 */
    SCREEN_PLAYING = 1    /* 遊戲進行中 */
} ScreenState;

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
 * 功能：繪製選擇先手畫面，顯示「玩家先手」與「電腦先手」兩個按鈕
 * 輸入：mouse_x/mouse_y - 滑鼠座標
 * 輸出：無（void），直接繪製至 Raylib 畫面
 * 最後更新：2026-04-02 23:00 (UTC+8)
 */
static void draw_choose_screen(int mouse_x, int mouse_y)
{
    ClearBackground(COL_BG);

    /* 標題 */
    int title_y = WIN_H / 2 - BTN_H - BTN_GAP - 40;
    render_text_centered("Dark Chess", WIN_W / 2, title_y - 30,
                         FONT_SIZE_LARGE, COL_TITLE_TEXT);

    /* 兩個按鈕的位置 */
    int btn_x = WIN_W / 2 - BTN_W / 2;
    int btn1_y = WIN_H / 2 - BTN_H - BTN_GAP / 2;
    int btn2_y = WIN_H / 2 + BTN_GAP / 2;

    Rectangle btn1 = { (float)btn_x, (float)btn1_y, (float)BTN_W, (float)BTN_H };
    Rectangle btn2 = { (float)btn_x, (float)btn2_y, (float)BTN_W, (float)BTN_H };

    int hover1 = CheckCollisionPointRec((Vector2){ (float)mouse_x, (float)mouse_y }, btn1);
    int hover2 = CheckCollisionPointRec((Vector2){ (float)mouse_x, (float)mouse_y }, btn2);

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
}

/* =====================================================================
 *  公開 API
 * ===================================================================== */

/*
 * 函數名稱：gui_main
 * 功能：遊戲主迴圈，含選擇先手畫面、首翻決定顏色、玩家輸入處理、AI 回合、畫面繪製
 * 輸入：無（void）
 * 輸出：int - 正常結束回傳 0，字型載入失敗回傳 1
 * 最後更新：2026-04-06 15:00 (UTC+8)
 */
int gui_main(void)
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

    while (!WindowShouldClose()) {
        if (IsKeyPressed(KEY_Q)) break;

        Vector2 mouse = GetMousePosition();
        int mx = (int)mouse.x, my = (int)mouse.y;

        /* ============================================================
         *  畫面：選擇誰先手
         * ============================================================ */
        if (screen == SCREEN_CHOOSE) {
            int btn_x = WIN_W / 2 - BTN_W / 2;
            int btn1_y = WIN_H / 2 - BTN_H - BTN_GAP / 2;
            int btn2_y = WIN_H / 2 + BTN_GAP / 2;

            Rectangle btn1 = { (float)btn_x, (float)btn1_y,
                                (float)BTN_W, (float)BTN_H };
            Rectangle btn2 = { (float)btn_x, (float)btn2_y,
                                (float)BTN_W, (float)BTN_H };

            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                int chose = 0;
                if (CheckCollisionPointRec((Vector2){ (float)mx, (float)my }, btn1)) {
                    player_goes_first = 1;
                    chose = 1;
                } else if (CheckCollisionPointRec((Vector2){ (float)mx, (float)my }, btn2)) {
                    player_goes_first = 0;
                    chose = 1;
                }
                if (chose) {
                    strategy_reset();
                    board_init(&gs);
                    sides_determined = 0;
                    player_side = SIDE_NONE;
                    sel_r = sel_c = 0;
                    last_from_r = last_from_c = 0;
                    last_to_r = last_to_c = 0;
                    ai_delay = player_goes_first ? 0 : 20;
                    screen = SCREEN_PLAYING;
                }
            }

            BeginDrawing();
            draw_choose_screen(mx, my);
            EndDrawing();
            continue;
        }

        /* ============================================================
         *  畫面：遊戲進行中
         * ============================================================ */

        /* R 鍵重新開始 → 回到選擇先手畫面 */
        if (IsKeyPressed(KEY_R)) {
            screen = SCREEN_CHOOSE;
            continue;
        }

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

    unload_font();
    CloseWindow();
    return 0;
}
