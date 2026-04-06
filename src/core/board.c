#include "board.h"

/* =====================================================================
 *  Internal helpers
 * ===================================================================== */

/* Standard piece set for one side (16 pieces) */
static const PieceRank PIECE_SET[PIECES_PER_SIDE] = {
    RANK_GENERAL,                                   /* x1 */
    RANK_ADVISOR, RANK_ADVISOR,                     /* x2 */
    RANK_ELEPHANT, RANK_ELEPHANT,                   /* x2 */
    RANK_CHARIOT, RANK_CHARIOT,                     /* x2 */
    RANK_HORSE, RANK_HORSE,                         /* x2 */
    RANK_CANNON, RANK_CANNON,                       /* x2 */
    RANK_SOLDIER, RANK_SOLDIER, RANK_SOLDIER,       /* x5 */
    RANK_SOLDIER, RANK_SOLDIER
};

/*
 * 函數名稱：shuffle_int
 * 功能：使用 Fisher-Yates 演算法隨機打亂整數陣列
 * 輸入：arr - 整數陣列指標, n - 陣列元素數量
 * 輸出：無（void），直接修改傳入的陣列
 * 最後更新：2026-04-02 23:00 (UTC+8)
 */
static void shuffle_int(int *arr, int n)
{
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }
}

/* 4 directions: up, down, left, right (row delta, col delta) */
static const int DIR_DR[4] = { -1,  1,  0,  0 };
static const int DIR_DC[4] = {  0,  0, -1,  1 };

/*
 * 函數名稱：count_between
 * 功能：計算兩個格子之間直線上的棋子數量（用於炮的跳吃判定）
 * 輸入：gs - 遊戲狀態指標, r1/c1 - 起點座標(1-based), r2/c2 - 終點座標(1-based)
 * 輸出：int - 兩點之間的棋子數量；若不在同一直線上則回傳 -1
 * 最後更新：2026-04-02 23:00 (UTC+8)
 */
static int count_between(const GameState *gs, int r1, int c1, int r2, int c2)
{
    if (r1 != r2 && c1 != c2) return -1;  /* not in a line */
    if (r1 == r2 && c1 == c2) return -1;

    int count = 0;
    if (r1 == r2) {
        int minc = (c1 < c2) ? c1 : c2;
        int maxc = (c1 > c2) ? c1 : c2;
        for (int c = minc + 1; c < maxc; c++) {
            if (gs->cells[r1 - 1][c - 1].state != STATE_EMPTY)
                count++;
        }
    } else {
        int minr = (r1 < r2) ? r1 : r2;
        int maxr = (r1 > r2) ? r1 : r2;
        for (int r = minr + 1; r < maxr; r++) {
            if (gs->cells[r - 1][c1 - 1].state != STATE_EMPTY)
                count++;
        }
    }
    return count;
}

/*
 * 函數名稱：board_init
 * 功能：初始化新遊戲，將 32 顆棋子隨機放置於棋盤上（皆為蓋牌狀態）
 * 輸入：gs - 指向 GameState 結構的指標，將被完全初始化
 * 輸出：無（void）
 * 最後更新：2026-04-06 15:00 (UTC+8)
 */
void board_init(GameState *gs)
{
    memset(gs, 0, sizeof(GameState));

    /* Build an array of 32 pieces: 16 red + 16 black */
    typedef struct { Side side; PieceRank rank; } PieceDef;
    PieceDef pieces[BOARD_SIZE];

    for (int i = 0; i < PIECES_PER_SIDE; i++) {
        pieces[i].side = SIDE_RED;
        pieces[i].rank = PIECE_SET[i];
        pieces[PIECES_PER_SIDE + i].side = SIDE_BLACK;
        pieces[PIECES_PER_SIDE + i].rank = PIECE_SET[i];
    }

    /* Create index array and shuffle */
    int indices[BOARD_SIZE];
    for (int i = 0; i < BOARD_SIZE; i++) indices[i] = i;
    shuffle_int(indices, BOARD_SIZE);

    /* Place pieces on board */
    for (int i = 0; i < BOARD_SIZE; i++) {
        int idx = indices[i];
        int r = idx / BOARD_COLS;
        int c = idx % BOARD_COLS;
        gs->cells[r][c].side  = pieces[i].side;
        gs->cells[r][c].rank  = pieces[i].rank;
        gs->cells[r][c].state = STATE_FACEDOWN;
    }

    gs->current_turn     = SIDE_NONE;
    gs->red_alive        = PIECES_PER_SIDE;
    gs->black_alive      = PIECES_PER_SIDE;
    gs->game_over        = 0;
    gs->winner           = SIDE_NONE;
    gs->no_capture_turns = 0;
}

/*
 * 函數名稱：board_can_capture
 * 功能：判斷攻擊方棋子是否可以吃掉防守方棋子
 *       規則：大吃小或同級互吃；兵/卒可吃將/帥；將/帥不可吃兵/卒
 * 輸入：attacker - 攻擊方棋子指標, defender - 防守方棋子指標
 * 輸出：int - 可以吃回傳 1，不可以吃回傳 0
 * 最後更新：2026-04-02 23:00 (UTC+8)
 */
int board_can_capture(const Cell *attacker, const Cell *defender)
{
    /* 進攻方未翻開 || 防守方未翻開 -> 不合法 */
    if (attacker->state != STATE_FACEUP || defender->state != STATE_FACEUP)
        return 0;
    /* 進攻方陣營 == 防守方陣營 -> 不合法 */
    if (attacker->side == defender->side)
        return 0;
    /* 選擇位置沒有棋子 -> 不合法 */
    if (attacker->side == SIDE_NONE || defender->side == SIDE_NONE)
        return 0;

    /* 儲存等級 */
    PieceRank ar = attacker->rank;
    PieceRank dr = defender->rank;

    /* 兵/卒 can capture 將/帥 */
    if (ar == RANK_SOLDIER && dr == RANK_GENERAL)
        return 1;

    /* 將/帥 cannot capture 兵/卒 */
    if (ar == RANK_GENERAL && dr == RANK_SOLDIER)
        return 0;

    /* Normal: higher or equal rank captures */
    if (ar >= dr)
        return 1;

    return 0;
}

/*
 * 函數名稱：board_generate_moves
 * 功能：產生當前回合所有合法走法（翻棋、移動、吃子）
 * 輸入：gs - 指向當前遊戲狀態的常數指標, moves - 用於儲存合法走法的陣列
 * 輸出：int - 合法走法的數量
 * 最後更新：2026-04-02 23:00 (UTC+8)
 */
int board_generate_moves(const GameState *gs, Move *moves)
{
    int n = 0;
    Side side = gs->current_turn;

    for (int r = 1; r <= BOARD_ROWS; r++) {
        for (int c = 1; c <= BOARD_COLS; c++) {
            const Cell *cell = CELL(gs, r, c);

            /* --- FLIP: any face-down piece can be flipped --- */
            if (cell->state == STATE_FACEDOWN) {
                moves[n].type   = MOVE_FLIP;
                moves[n].from_r = r;
                moves[n].from_c = c;
                moves[n].to_r   = r;
                moves[n].to_c   = c;
                n++;
                continue;  /* cannot do other moves with a face-down piece */
            }

            /* --- Only our own face-up pieces can walk/capture --- */
            if (cell->state != STATE_FACEUP || cell->side != side)
                continue;

            /* --- CANNON special: can jump-capture (jump over exactly 1 piece) --- */
            if (cell->rank == RANK_CANNON) {
                /* Cannon can still walk to adjacent empty cells normally */
                for (int d = 0; d < 4; d++) {
                    int nr = r + DIR_DR[d];
                    int nc = c + DIR_DC[d];
                    if (!IN_BOUNDS(nr, nc)) continue;
                    const Cell *target = CELL(gs, nr, nc);
                    if (target->state == STATE_EMPTY) {
                        moves[n].type   = MOVE_WALK;
                        moves[n].from_r = r;
                        moves[n].from_c = c;
                        moves[n].to_r   = nr;
                        moves[n].to_c   = nc;
                        n++;
                    }
                }
                /* Cannon jump-capture: search along 4 directions for a target
                   with exactly 1 piece in between */
                /* Search in row (left and right) */
                for (int dc = -1; dc <= 1; dc += 2) {
                    int jumped = 0;
                    for (int nc = c + dc; nc >= 1 && nc <= BOARD_COLS; nc += dc) {
                        const Cell *t = CELL(gs, r, nc);
                        if (t->state == STATE_EMPTY) continue;
                        if (jumped == 0) {
                            jumped = 1;
                        } else {
                            if (t->state == STATE_FACEUP && t->side != side) {
                                moves[n].type   = MOVE_CAPTURE;
                                moves[n].from_r = r;
                                moves[n].from_c = c;
                                moves[n].to_r   = r;
                                moves[n].to_c   = nc;
                                n++;
                            }
                            break;
                        }
                    }
                }
                /* Search in column (up and down) */
                for (int dr = -1; dr <= 1; dr += 2) {
                    int jumped = 0;
                    for (int nr = r + dr; nr >= 1 && nr <= BOARD_ROWS; nr += dr) {
                        const Cell *t = CELL(gs, nr, c);
                        if (t->state == STATE_EMPTY) continue;
                        if (jumped == 0) {
                            jumped = 1;
                        } else {
                            if (t->state == STATE_FACEUP && t->side != side) {
                                moves[n].type   = MOVE_CAPTURE;
                                moves[n].from_r = r;
                                moves[n].from_c = c;
                                moves[n].to_r   = nr;
                                moves[n].to_c   = c;
                                n++;
                            }
                            break;
                        }
                    }
                }
                continue;  /* cannon done, don't fall through to normal adjacent capture */
            }

            /* --- Non-cannon pieces: adjacent walk and capture --- */
            for (int d = 0; d < 4; d++) {
                int nr = r + DIR_DR[d];
                int nc = c + DIR_DC[d];
                if (!IN_BOUNDS(nr, nc)) continue;

                const Cell *target = CELL(gs, nr, nc);

                if (target->state == STATE_EMPTY) {
                    /* Walk to empty cell */
                    moves[n].type   = MOVE_WALK;
                    moves[n].from_r = r;
                    moves[n].from_c = c;
                    moves[n].to_r   = nr;
                    moves[n].to_c   = nc;
                    n++;
                } else if (target->state == STATE_FACEUP && target->side != side) {
                    /* Try to capture */
                    if (board_can_capture(cell, target)) {
                        moves[n].type   = MOVE_CAPTURE;
                        moves[n].from_r = r;
                        moves[n].from_c = c;
                        moves[n].to_r   = nr;
                        moves[n].to_c   = nc;
                        n++;
                    }
                }
            }
        }
    }

    return n;
}

/*
 * 函數名稱：board_apply_move
 * 功能：將一步走法套用至遊戲狀態，更新棋盤、回合及勝負判定
 * 輸入：gs - 指向遊戲狀態的指標, mv - 指向欲執行走法的常數指標
 * 輸出：int - 成功回傳 1，非法走法回傳 0
 * 最後更新：2026-04-02 23:00 (UTC+8)
 */
int board_apply_move(GameState *gs, const Move *mv)
{
    if (gs->game_over) return 0;

    int fr = mv->from_r, fc = mv->from_c;
    int tr = mv->to_r,   tc = mv->to_c;

    if (!IN_BOUNDS(fr, fc)) return 0;
    if (!IN_BOUNDS(tr, tc)) return 0;

    Cell *src = CELL(gs, fr, fc);
    Cell *dst = CELL(gs, tr, tc);

    int captured = 0;

    switch (mv->type) {
    case MOVE_FLIP:
        if (src->state != STATE_FACEDOWN) return 0;
        src->state = STATE_FACEUP;
        break;

    case MOVE_WALK:
        if (src->state != STATE_FACEUP) return 0;
        if (src->side != gs->current_turn) return 0;
        if (dst->state != STATE_EMPTY) return 0;
        /* Check adjacency (non-cannon pieces must be adjacent) */
        if (abs(fr - tr) + abs(fc - tc) != 1) return 0;
        /* Move piece */
        dst->side  = src->side;
        dst->rank  = src->rank;
        dst->state = STATE_FACEUP;
        src->side  = SIDE_NONE;
        src->rank  = RANK_NONE;
        src->state = STATE_EMPTY;
        break;

    case MOVE_CAPTURE:
        if (src->state != STATE_FACEUP) return 0;
        if (src->side != gs->current_turn) return 0;
        if (dst->state != STATE_FACEUP) return 0;
        if (dst->side == src->side) return 0;

        /* Cannon jump capture: allow non-adjacent */
        if (src->rank == RANK_CANNON) {
            int between = count_between(gs, fr, fc, tr, tc);
            if (between != 1) return 0;
        } else {
            /* Non-cannon must be adjacent */
            if (abs(fr - tr) + abs(fc - tc) != 1) return 0;
            if (!board_can_capture(src, dst)) return 0;
        }

        /* Remove defender */
        if (dst->side == SIDE_RED)   gs->red_alive--;
        if (dst->side == SIDE_BLACK) gs->black_alive--;
        captured = 1;

        /* Move attacker to destination */
        dst->side  = src->side;
        dst->rank  = src->rank;
        dst->state = STATE_FACEUP;
        src->side  = SIDE_NONE;
        src->rank  = RANK_NONE;
        src->state = STATE_EMPTY;
        break;

    default:
        return 0;
    }

    /* Update no-capture counter */
    if (captured) {
        gs->no_capture_turns = 0;
    } else {
        gs->no_capture_turns++;
    }

    /* Record last move */
    gs->last_move = *mv;
    gs->has_last_move = 1;

    /* Switch turn */
    gs->current_turn = (gs->current_turn == SIDE_RED) ? SIDE_BLACK : SIDE_RED;

    /* Check game over */
    board_check_game_over(gs);

    return 1;
}

/*
 * 函數名稱：board_check_game_over
 * 功能：檢查遊戲結束條件：一方棋子全滅、連續無吃子達上限（和棋）、無合法走法
 * 輸入：gs - 指向遊戲狀態的指標
 * 輸出：無（void），直接修改 gs->game_over 與 gs->winner
 * 最後更新：2026-04-02 23:00 (UTC+8)
 */
void board_check_game_over(GameState *gs)
{
    if (gs->red_alive == 0) {
        gs->game_over = 1;
        gs->winner = SIDE_BLACK;
        return;
    }
    if (gs->black_alive == 0) {
        gs->game_over = 1;
        gs->winner = SIDE_RED;
        return;
    }
    if (gs->no_capture_turns >= MAX_NO_CAPTURE) {
        gs->game_over = 1;
        gs->winner = SIDE_NONE;  /* draw */
        return;
    }

    /* Check if current player has any moves */
    Move moves[MAX_MOVES];
    int n = board_generate_moves(gs, moves);
    if (n == 0) {
        gs->game_over = 1;
        /* No moves = lose */
        gs->winner = (gs->current_turn == SIDE_RED) ? SIDE_BLACK : SIDE_RED;
    }
}
