#include "strategy.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* =====================================================================
 *  §1 — 常數與設定
 * ===================================================================== */

#define NUM_PIECE_TYPES    14   /* 2 sides × 7 ranks */
#define TYPE_IDX(s, r)     (((s) - 1) * 7 + ((r) - 1))

#define DIR4 4
static const int DR4[4] = { -1, 1, 0, 0 };
static const int DC4[4] = { 0, 0, -1, 1 };

/* 每種棋子初始數量 (index = rank-1) */
static const int PIECE_COUNTS[7] = { 5, 2, 2, 2, 2, 2, 1 };

/* 威脅 / 機會陣列上限 */
#define MAX_THREATENED     8
#define MAX_ATTACKERS      4
#define MAX_OPPORTUNITIES  8

/* 砲線陣列上限 */
#define MAX_CANNON_TARGETS   8
#define MAX_CANNON_PLATFORMS 16

/* 局面階段 */
#define PHASE_OPENING  0
#define PHASE_MIDGAME  1
#define PHASE_ENDGAME  2

/* 走法歷史環形緩衝區大小 */
#define MOVE_HISTORY_SIZE 20

/* =====================================================================
 *  §2 — 資料結構
 * ===================================================================== */

/* --- 算牌器 --- */
typedef struct {
    int total[NUM_PIECE_TYPES];
    int revealed[NUM_PIECE_TYPES];
    int captured[NUM_PIECE_TYPES];
    int hidden[NUM_PIECE_TYPES];
    int total_facedown;
    float prob[BOARD_SIZE][NUM_PIECE_TYPES];
    float prob_friendly[BOARD_SIZE];
    float prob_enemy[BOARD_SIZE];
    float expected_value[BOARD_SIZE];
    int enemy_general_hidden;
    int my_general_hidden;
    float prob_flip_enemy_general;
    float prob_flip_my_general;
} CardCounter;

/* --- 威脅資訊 --- */
typedef struct {
    int r, c;
    PieceRank rank;
    float value;
    int attacker_count;
    struct { int r, c; PieceRank rank; int is_cannon; } attackers[MAX_ATTACKERS];
} ThreatInfo;

/* --- 機會資訊 --- */
typedef struct {
    int r, c;
    PieceRank rank;
    float value;
    int my_r, my_c;
    PieceRank my_rank;
    int is_cannon_capture;
} OpportunityInfo;

/* --- 砲線目標 --- */
typedef struct {
    int r, c;
    Side side;
    PieceRank rank;
} CannonTarget;

/* --- 砲架資訊 --- */
typedef struct {
    int platform_r, platform_c;
    int target_r, target_c;
    PieceRank target_rank;
    int cannon_r, cannon_c;
} CannonPlatformInfo;

/* --- 吃子分析 --- */
typedef struct {
    int can_recapture;
    float net_value;
    float target_value;
    float attacker_value;
} CaptureAnalysis;

/* --- 走法歷史記錄 --- */
typedef struct {
    Move move;
    Side side;             /* 走這步的一方 */
} MoveRecord;

/* --- 情勢評估 --- */
typedef struct {
    int my_gen_r, my_gen_c, enemy_gen_r, enemy_gen_c;
    int my_gen_alive, enemy_gen_alive;
    float my_material, enemy_material;
    int my_piece_count, enemy_piece_count;
    ThreatInfo threatened[MAX_THREATENED];
    int threat_count;
    int my_general_threatened;
    OpportunityInfo opportunities[MAX_OPPORTUNITIES];
    int opportunity_count;
    int can_kill_enemy_general;
    Move kill_general_move;
    float general_danger_urgency;
    float kill_opportunity_urgency;
    float stagnation_pressure;
    int my_gen_escape_routes;
    int enemy_gen_escape_routes;
    int enemy_reach[BOARD_SIZE];       /* 敵方控制範圍：1=該格在敵方攻擊範圍內 */
    int game_phase;                    /* PHASE_OPENING / MIDGAME / ENDGAME */
    int turns_until_draw;
    int enemy_legal_move_count;
    int soldier_near_enemy_gen_1;      /* 我方兵距敵將 dist==1 */
    int soldier_near_enemy_gen_2;      /* 我方兵距敵將 dist<=2 */
} SituationAssessment;

/* =====================================================================
 *  §3 — 全域狀態
 * ===================================================================== */

static float g_piece_val[8];             /* 動態棋子價值 [rank] */
static CardCounter g_cc;                 /* 算牌器 */
static SituationAssessment g_sa;         /* 情勢評估 */
static Side g_my_side;                   /* AI 陣營 */

/* 影子棋盤（偵測吃子用） */
static GameState g_shadow;
static int g_shadow_valid = 0;
static int g_cap_hist[NUM_PIECE_TYPES];

/* 走法歷史（環形緩衝區） */
static MoveRecord g_move_history[MOVE_HISTORY_SIZE];
static int g_move_history_count = 0;   /* 已記錄的總步數（可 > SIZE） */

/* =====================================================================
 *  §4 — 工具函數
 * ===================================================================== */

static int manhattan(int r1, int c1, int r2, int c2)
{ return abs(r1 - r2) + abs(c1 - c2); }

static float maxf(float a, float b) { return a > b ? a : b; }
static float minf(float a, float b) { return a < b ? a : b; }

static int cell_idx(int r, int c) { return (r - 1) * BOARD_COLS + (c - 1); }

static int total_pieces_on_board(const GameState *gs)
{
    return gs->red_alive + gs->black_alive;
}

static int count_facedown(const GameState *gs)
{
    int n = 0;
    for (int r = 0; r < BOARD_ROWS; r++)
        for (int c = 0; c < BOARD_COLS; c++)
            if (gs->cells[r][c].state == STATE_FACEDOWN) n++;
    return n;
}

static Side opponent(Side s) { return s == SIDE_RED ? SIDE_BLACK : SIDE_RED; }

static int count_escape_routes(const GameState *gs, int r, int c)
{
    int n = 0;
    for (int d = 0; d < DIR4; d++) {
        int nr = r + DR4[d], nc = c + DC4[d];
        if (IN_BOUNDS(nr, nc) && CELL(gs, nr, nc)->state == STATE_EMPTY)
            n++;
    }
    return n;
}

/*
 * 函數名稱：is_type_fully_revealed
 * 功能：判斷某類型棋子是否已全數現身（翻開或被吃），暗子中已無該類型
 * 輸入：side - 陣營, rank - 階級
 * 輸出：int - 1=已全數現身, 0=暗子中仍有
 * 最後更新：2026-04-05 (UTC+8)
 * 注意：需在 card_counter_update() 後呼叫
 */
static int is_type_fully_revealed(Side side, PieceRank rank)
{
    return g_cc.hidden[TYPE_IDX(side, rank)] == 0;
}

/*
 * 函數名稱：is_full_information
 * 功能：判斷棋盤是否進入完全資訊狀態（所有暗子皆已翻開）
 * 輸入：無
 * 輸出：int - 1=完全資訊, 0=仍有暗子
 * 最後更新：2026-04-05 (UTC+8)
 * 注意：需在 card_counter_update() 後呼叫
 */
static int is_full_information(void)
{
    return g_cc.total_facedown == 0;
}

/*
 * 函數名稱：record_move
 * 功能：將一步走法記錄到歷史環形緩衝區中
 * 輸入：mv - 走法, side - 走這步的一方
 * 輸出：無
 * 最後更新：2026-04-05 (UTC+8)
 */
static void record_move(const Move *mv, Side side)
{
    int idx = g_move_history_count % MOVE_HISTORY_SIZE;
    g_move_history[idx].move = *mv;
    g_move_history[idx].side = side;
    g_move_history_count++;
}

/*
 * 函數名稱：get_history
 * 功能：取得第 steps_ago 步前的走法記錄（0=最近一步）
 * 輸入：steps_ago - 往回幾步（0=最近）
 * 輸出：指向 MoveRecord 的指標，無效時回傳 NULL
 * 最後更新：2026-04-05 (UTC+8)
 */
static const MoveRecord *get_history(int steps_ago)
{
    if (steps_ago < 0 || steps_ago >= g_move_history_count ||
        steps_ago >= MOVE_HISTORY_SIZE)
        return NULL;
    int idx = (g_move_history_count - 1 - steps_ago) % MOVE_HISTORY_SIZE;
    return &g_move_history[idx];
}

/*
 * 函數名稱：moves_equal
 * 功能：比較兩步走法是否相同（類型 + 起點 + 終點）
 * 輸入：a, b - 走法指標
 * 輸出：int - 1=相同, 0=不同
 * 最後更新：2026-04-05 (UTC+8)
 */
static int moves_equal(const Move *a, const Move *b)
{
    return a->type == b->type &&
           a->from_r == b->from_r && a->from_c == b->from_c &&
           a->to_r == b->to_r && a->to_c == b->to_c;
}

/*
 * 函數名稱：detect_repetition
 * 功能：偵測是否出現走法循環（A→B→A→B 來回徘徊）
 * 輸入：無（從全域歷史中讀取）
 * 輸出：int - 重複次數（0=無重複，1=出現一次來回，2=兩次以上）
 * 最後更新：2026-04-05 (UTC+8)
 * 說明：偵測「同一方」最近的走法是否與先前相同（步數間隔=2，因為雙方交替走）
 */
static int detect_repetition(void)
{
    /* 至少需要 4 步歷史（我-敵-我-敵）才能偵測一次完整循環 */
    if (g_move_history_count < 4) return 0;

    /* 最近我方走的步：history[0]
     * 上上次我方走的步：history[2]（跳過中間敵方的一步）
     * 上上上次我方走的步：history[4] */
    const MoveRecord *h0 = get_history(0);  /* 最近 */
    const MoveRecord *h2 = get_history(2);  /* 兩步前（同一方） */
    const MoveRecord *h4 = get_history(4);  /* 四步前（同一方） */

    if (!h0 || !h2) return 0;

    int rep = 0;
    if (moves_equal(&h0->move, &h2->move)) {
        rep = 1;
        if (h4 && moves_equal(&h0->move, &h4->move))
            rep = 2;
    }
    return rep;
}

/*
 * 函數名稱：is_move_a_retreat
 * 功能：判斷對方上一步是否是「撤退」（走法目標是我方某棋子的來源格）
 * 輸入：gs - 遊戲狀態
 * 輸出：int - 1=對方正在撤退/回頭, 0=否
 * 最後更新：2026-04-05 (UTC+8)
 */
static int is_move_a_retreat(const GameState *gs)
{
    if (!gs->has_last_move) return 0;
    if (g_move_history_count < 2) return 0;

    const MoveRecord *enemy_last = get_history(0);
    const MoveRecord *enemy_prev = get_history(2);

    if (!enemy_last || !enemy_prev) return 0;
    if (enemy_last->move.type == MOVE_FLIP) return 0;

    /* 對方把棋子走回了之前的位置 */
    return (enemy_last->move.to_r == enemy_prev->move.from_r &&
            enemy_last->move.to_c == enemy_prev->move.from_c &&
            enemy_last->move.from_r == enemy_prev->move.to_r &&
            enemy_last->move.from_c == enemy_prev->move.to_c);
}

/*
 * 函數名稱：is_opponent_chasing
 * 功能：偵測對方是否正在追殺我方某顆棋子（連續逼近同一格）
 * 輸入：gs - 遊戲狀態, target_r, target_c - 我方棋子位置（1-based）
 * 輸出：int - 1=對方正在追殺, 0=否
 * 最後更新：2026-04-05 (UTC+8)
 */
static int is_opponent_chasing(const GameState *gs, int target_r, int target_c)
{
    if (!gs->has_last_move) return 0;

    const Move *last = &gs->last_move;
    if (last->type != MOVE_WALK && last->type != MOVE_CAPTURE) return 0;

    /* 對方上一步的目標格是否正好在我方棋子旁邊 */
    int dist = abs(last->to_r - target_r) + abs(last->to_c - target_c);
    if (dist != 1) return 0;

    /* 檢查對方棋子是否真的能吃我方棋子 */
    const Cell *enemy = CELL(gs, last->to_r, last->to_c);
    const Cell *mine  = CELL(gs, target_r, target_c);
    if (enemy->state != STATE_FACEUP || mine->state != STATE_FACEUP) return 0;
    if (enemy->side == mine->side) return 0;

    return board_can_capture(enemy, mine);
}

/* =====================================================================
 *  §5 — 動態棋子價值表
 * ===================================================================== */

static void refresh_piece_values(const GameState *gs)
{
    int total = total_pieces_on_board(gs);

    g_piece_val[RANK_NONE]    = 0.0f;
    g_piece_val[RANK_SOLDIER] = 1.0f;
    g_piece_val[RANK_HORSE]   = 3.5f;
    g_piece_val[RANK_CHARIOT] = 5.0f;
    g_piece_val[RANK_ELEPHANT]= 2.0f;
    g_piece_val[RANK_ADVISOR] = 2.5f;

    /* 砲的動態值（砲架隨棋子減少而稀缺） */
    if      (total > 20) g_piece_val[RANK_CANNON] = 6.0f;
    else if (total > 16) g_piece_val[RANK_CANNON] = 5.0f;
    else if (total > 12) g_piece_val[RANK_CANNON] = 4.0f;
    else if (total >  8) g_piece_val[RANK_CANNON] = 3.0f;
    else                 g_piece_val[RANK_CANNON] = 2.0f;

    /* 將帥的動態值 — 基於兵卒威脅 */
    Side my = g_my_side, en = opponent(my);
    int visible_soldiers = 0, my_gen_alive = 0, en_gen_alive = 0;
    int fd = count_facedown(gs);

    for (int r = 0; r < BOARD_ROWS; r++)
        for (int c = 0; c < BOARD_COLS; c++) {
            const Cell *cl = &gs->cells[r][c];
            if (cl->state != STATE_FACEUP) continue;
            if (cl->side == en && cl->rank == RANK_SOLDIER) visible_soldiers++;
            if (cl->rank == RANK_GENERAL) {
                if (cl->side == my) my_gen_alive = 1;
                if (cl->side == en) en_gen_alive = 1;
            }
        }

    float hidden_soldiers = (5.0f - visible_soldiers) * (fd / 32.0f);
    float threat = visible_soldiers + hidden_soldiers;

    if (!my_gen_alive && !en_gen_alive) {
        g_piece_val[RANK_GENERAL] = 10.0f;
    } else if (!en_gen_alive) {
        if (threat <= 0.1f)     g_piece_val[RANK_GENERAL] = 8.0f;
        else if (threat <= 2.0f) g_piece_val[RANK_GENERAL] = 20.0f;
        else                     g_piece_val[RANK_GENERAL] = 40.0f;
    } else {
        if (threat <= 0.1f)     g_piece_val[RANK_GENERAL] = 15.0f;
        else if (threat <= 2.0f) g_piece_val[RANK_GENERAL] = 40.0f;
        else if (threat <= 4.0f) g_piece_val[RANK_GENERAL] = 60.0f;
        else                     g_piece_val[RANK_GENERAL] = 100.0f;
    }
}

/* =====================================================================
 *  §6 — 算牌系統
 * ===================================================================== */

static void detect_captures(const GameState *gs)
{
    if (!g_shadow_valid) { g_shadow = *gs; g_shadow_valid = 1; return; }

    int old_cnt[NUM_PIECE_TYPES] = {0};
    int new_cnt[NUM_PIECE_TYPES] = {0};

    for (int r = 0; r < BOARD_ROWS; r++)
        for (int c = 0; c < BOARD_COLS; c++) {
            const Cell *o = &g_shadow.cells[r][c];
            const Cell *n = &gs->cells[r][c];
            if (o->state == STATE_FACEUP && o->side != SIDE_NONE)
                old_cnt[TYPE_IDX(o->side, o->rank)]++;
            if (n->state == STATE_FACEUP && n->side != SIDE_NONE)
                new_cnt[TYPE_IDX(n->side, n->rank)]++;
        }

    for (int i = 0; i < NUM_PIECE_TYPES; i++) {
        int diff = old_cnt[i] - new_cnt[i];
        if (diff > 0) g_cap_hist[i] += diff;
    }

    g_shadow = *gs;
}

static void card_counter_update(const GameState *gs, Side my_side)
{
    memset(&g_cc, 0, sizeof(g_cc));

    /* 初始化 total */
    for (int s = 1; s <= 2; s++)
        for (int rk = 1; rk <= 7; rk++)
            g_cc.total[TYPE_IDX(s, rk)] = PIECE_COUNTS[rk - 1];

    /* 掃描棋盤 */
    for (int r = 0; r < BOARD_ROWS; r++)
        for (int c = 0; c < BOARD_COLS; c++) {
            const Cell *cl = &gs->cells[r][c];
            if (cl->state == STATE_FACEDOWN) g_cc.total_facedown++;
            else if (cl->state == STATE_FACEUP && cl->side != SIDE_NONE)
                g_cc.revealed[TYPE_IDX(cl->side, cl->rank)]++;
        }

    /* 計算 hidden */
    for (int i = 0; i < NUM_PIECE_TYPES; i++) {
        g_cc.captured[i] = g_cap_hist[i];
        g_cc.hidden[i] = g_cc.total[i] - g_cc.revealed[i] - g_cc.captured[i];
        if (g_cc.hidden[i] < 0) g_cc.hidden[i] = 0;
    }

    /* 修正：確保 Σhidden == total_facedown */
    int sum_hidden = 0;
    for (int i = 0; i < NUM_PIECE_TYPES; i++) sum_hidden += g_cc.hidden[i];
    if (sum_hidden > 0 && sum_hidden != g_cc.total_facedown) {
        float scale = (float)g_cc.total_facedown / (float)sum_hidden;
        for (int i = 0; i < NUM_PIECE_TYPES; i++)
            g_cc.hidden[i] = (int)(g_cc.hidden[i] * scale + 0.5f);
    }

    /* 計算機率 */
    if (g_cc.total_facedown > 0) {
        float inv = 1.0f / (float)g_cc.total_facedown;
        for (int r = 0; r < BOARD_ROWS; r++)
            for (int c = 0; c < BOARD_COLS; c++) {
                if (gs->cells[r][c].state != STATE_FACEDOWN) continue;
                int idx = r * BOARD_COLS + c;
                float pf = 0, pe = 0, ev = 0;
                for (int t = 0; t < NUM_PIECE_TYPES; t++) {
                    float p = g_cc.hidden[t] * inv;
                    g_cc.prob[idx][t] = p;
                    int side = (t < 7) ? SIDE_RED : SIDE_BLACK;
                    int rank = (t % 7) + 1;
                    if (side == (int)my_side) pf += p; else pe += p;
                    ev += p * g_piece_val[rank];
                }
                g_cc.prob_friendly[idx] = pf;
                g_cc.prob_enemy[idx] = pe;
                g_cc.expected_value[idx] = ev;
            }
    }

    /* 將帥隱藏狀態 */
    int my_gen_t = TYPE_IDX(my_side, RANK_GENERAL);
    int en_gen_t = TYPE_IDX(opponent(my_side), RANK_GENERAL);
    g_cc.my_general_hidden = g_cc.hidden[my_gen_t] > 0;
    g_cc.enemy_general_hidden = g_cc.hidden[en_gen_t] > 0;
    g_cc.prob_flip_my_general = (g_cc.total_facedown > 0)
        ? (float)g_cc.hidden[my_gen_t] / g_cc.total_facedown : 0;
    g_cc.prob_flip_enemy_general = (g_cc.total_facedown > 0)
        ? (float)g_cc.hidden[en_gen_t] / g_cc.total_facedown : 0;
}

static float cc_flip_risk(const GameState *gs, int r, int c, Side my_side)
{
    int idx = cell_idx(r, c);
    float risk = 0;
    for (int d = 0; d < DIR4; d++) {
        int nr = r + DR4[d], nc = c + DC4[d];
        if (!IN_BOUNDS(nr, nc)) continue;
        const Cell *adj = CELL(gs, nr, nc);
        if (adj->state != STATE_FACEUP || adj->side == SIDE_NONE) continue;
        if (adj->side == my_side) continue;
        for (int t = 0; t < NUM_PIECE_TYPES; t++) {
            int ts = (t < 7) ? SIDE_RED : SIDE_BLACK;
            int tr = (t % 7) + 1;
            if (ts != (int)my_side) continue;
            Cell tmp = { (Side)ts, (PieceRank)tr, STATE_FACEUP };
            if (board_can_capture(adj, &tmp))
                risk += g_cc.prob[idx][t];
        }
    }
    return minf(risk, 1.0f);
}

static float cc_flip_opportunity(const GameState *gs, int r, int c, Side my_side)
{
    int idx = cell_idx(r, c);
    float opp = 0;
    for (int d = 0; d < DIR4; d++) {
        int nr = r + DR4[d], nc = c + DC4[d];
        if (!IN_BOUNDS(nr, nc)) continue;
        const Cell *adj = CELL(gs, nr, nc);
        if (adj->state != STATE_FACEUP || adj->side == my_side || adj->side == SIDE_NONE)
            continue;
        for (int t = 0; t < NUM_PIECE_TYPES; t++) {
            int ts = (t < 7) ? SIDE_RED : SIDE_BLACK;
            int tr = (t % 7) + 1;
            if (ts != (int)my_side) continue;
            Cell tmp = { (Side)ts, (PieceRank)tr, STATE_FACEUP };
            if (board_can_capture(&tmp, adj))
                opp += g_cc.prob[idx][t];
        }
    }
    return minf(opp, 1.0f);
}

static float cc_hidden_threat(const GameState *gs, int r, int c, Side side, PieceRank rank)
{
    float threat = 0;
    Cell temp = { side, rank, STATE_FACEUP };
    for (int d = 0; d < DIR4; d++) {
        int nr = r + DR4[d], nc = c + DC4[d];
        if (!IN_BOUNDS(nr, nc)) continue;
        if (CELL(gs, nr, nc)->state != STATE_FACEDOWN) continue;
        int idx = cell_idx(nr, nc);
        for (int t = 0; t < NUM_PIECE_TYPES; t++) {
            int ts = (t < 7) ? SIDE_RED : SIDE_BLACK;
            int tr = (t % 7) + 1;
            if (ts == (int)side) continue;
            Cell att = { (Side)ts, (PieceRank)tr, STATE_FACEUP };
            if (board_can_capture(&att, &temp))
                threat += g_cc.prob[idx][t];
        }
    }
    return minf(threat, 1.0f);
}

/* =====================================================================
 *  §7 — 威脅偵測
 * ===================================================================== */

static int is_threatened_at(const GameState *gs, int r, int c, Side side, PieceRank rank)
{
    Cell temp = { side, rank, STATE_FACEUP };
    /* 鄰接威脅 */
    for (int d = 0; d < DIR4; d++) {
        int nr = r + DR4[d], nc = c + DC4[d];
        if (!IN_BOUNDS(nr, nc)) continue;
        const Cell *adj = CELL(gs, nr, nc);
        if (adj->state == STATE_FACEUP && adj->side != SIDE_NONE && adj->side != side)
            if (board_can_capture(adj, &temp)) return 1;
    }
    /* 砲線威脅 — 橫向 */
    for (int dc = -1; dc <= 1; dc += 2) {
        int jumped = 0;
        for (int nc = c + dc; nc >= 1 && nc <= BOARD_COLS; nc += dc) {
            const Cell *t = CELL(gs, r, nc);
            if (t->state == STATE_EMPTY) continue;
            if (!jumped) { jumped = 1; continue; }
            if (t->state == STATE_FACEUP && t->side != SIDE_NONE &&
                t->side != side && t->rank == RANK_CANNON) return 1;
            break;
        }
    }
    /* 砲線威脅 — 縱向 */
    for (int dr = -1; dr <= 1; dr += 2) {
        int jumped = 0;
        for (int nr = r + dr; nr >= 1 && nr <= BOARD_ROWS; nr += dr) {
            const Cell *t = CELL(gs, nr, c);
            if (t->state == STATE_EMPTY) continue;
            if (!jumped) { jumped = 1; continue; }
            if (t->state == STATE_FACEUP && t->side != SIDE_NONE &&
                t->side != side && t->rank == RANK_CANNON) return 1;
            break;
        }
    }
    return 0;
}

static int count_attackers(const GameState *gs, int r, int c, Side side, PieceRank rank)
{
    Cell temp = { side, rank, STATE_FACEUP };
    int cnt = 0;
    for (int d = 0; d < DIR4; d++) {
        int nr = r + DR4[d], nc = c + DC4[d];
        if (!IN_BOUNDS(nr, nc)) continue;
        const Cell *adj = CELL(gs, nr, nc);
        if (adj->state == STATE_FACEUP && adj->side != SIDE_NONE &&
            adj->side != side && board_can_capture(adj, &temp))
            cnt++;
    }
    return cnt;
}

static int count_friendly_adj(const GameState *gs, int r, int c, Side side)
{
    int cnt = 0;
    for (int d = 0; d < DIR4; d++) {
        int nr = r + DR4[d], nc = c + DC4[d];
        if (!IN_BOUNDS(nr, nc)) continue;
        const Cell *adj = CELL(gs, nr, nc);
        if (adj->state == STATE_FACEUP && adj->side == side) cnt++;
    }
    return cnt;
}

/*
 * 函數名稱：compute_enemy_reach
 * 功能：預計算敵方控制範圍地圖，標記所有在敵方攻擊範圍內的格子
 * 輸入：gs - 遊戲狀態, my_side - 我方陣營
 * 輸出：填充 g_sa.enemy_reach[]（0=不在範圍, 1=在敵方攻擊範圍）
 * 最後更新：2026-04-05 (UTC+8)
 * 注意：這是概略估計（不考慮階級限制），精確判斷仍需 is_threatened_at()
 */
static void compute_enemy_reach(const GameState *gs, Side my_side)
{
    Side en = opponent(my_side);
    memset(g_sa.enemy_reach, 0, sizeof(g_sa.enemy_reach));

    for (int r = 1; r <= BOARD_ROWS; r++)
        for (int c = 1; c <= BOARD_COLS; c++) {
            const Cell *cl = CELL(gs, r, c);
            if (cl->state != STATE_FACEUP || cl->side != en) continue;

            if (cl->rank != RANK_CANNON) {
                /* 非砲棋子：標記四方向鄰接格 */
                for (int d = 0; d < DIR4; d++) {
                    int nr = r + DR4[d], nc = c + DC4[d];
                    if (IN_BOUNDS(nr, nc))
                        g_sa.enemy_reach[cell_idx(nr, nc)] = 1;
                }
            } else {
                /* 砲：標記砲線目標格 */
                for (int axis = 0; axis < 2; axis++) {
                    for (int dir = -1; dir <= 1; dir += 2) {
                        int jumped = 0;
                        for (int step = 1; ; step++) {
                            int nr = (axis == 0) ? r : r + dir * step;
                            int nc = (axis == 0) ? c + dir * step : c;
                            if (!IN_BOUNDS(nr, nc)) break;
                            const Cell *t = CELL(gs, nr, nc);
                            if (t->state == STATE_EMPTY) continue;
                            if (!jumped) { jumped = 1; continue; }
                            g_sa.enemy_reach[cell_idx(nr, nc)] = 1;
                            break;
                        }
                    }
                }
            }
        }
}

/* =====================================================================
 *  §8 — 砲線分析
 * ===================================================================== */

/*
 * 函數名稱：cannon_platform_count
 * 功能：計算某門砲在橫縱線上有多少顆棋子可作為砲架
 * 輸入：gs - 遊戲狀態, r, c - 砲的位置（1-based）
 * 輸出：int - 可用砲架數
 * 最後更新：2026-04-05 (UTC+8)
 */
static int cannon_platform_count(const GameState *gs, int r, int c)
{
    int cnt = 0;
    /* 橫向 */
    for (int nc = 1; nc <= BOARD_COLS; nc++) {
        if (nc == c) continue;
        const Cell *cl = CELL(gs, r, nc);
        if (cl->state != STATE_EMPTY) cnt++;
    }
    /* 縱向 */
    for (int nr = 1; nr <= BOARD_ROWS; nr++) {
        if (nr == r) continue;
        const Cell *cl = CELL(gs, nr, c);
        if (cl->state != STATE_EMPTY) cnt++;
    }
    return cnt;
}

/*
 * 函數名稱：find_cannon_targets
 * 功能：找出某門砲目前可以跳吃的所有目標（砲線掃描）
 * 輸入：gs - 遊戲狀態, r, c - 砲的位置（1-based）,
 *       cannon_side - 砲的陣營, out - 輸出陣列, max - 最大輸出數量
 * 輸出：int - 找到的目標數量
 * 最後更新：2026-04-05 (UTC+8)
 */
static int find_cannon_targets(const GameState *gs, int r, int c,
                                Side cannon_side, CannonTarget *out, int max)
{
    Side en = opponent(cannon_side);
    int cnt = 0;
    for (int axis = 0; axis < 2 && cnt < max; axis++) {
        for (int dir = -1; dir <= 1 && cnt < max; dir += 2) {
            int jumped = 0;
            for (int step = 1; ; step++) {
                int nr = (axis == 0) ? r : r + dir * step;
                int nc = (axis == 0) ? c + dir * step : c;
                if (!IN_BOUNDS(nr, nc)) break;
                const Cell *t = CELL(gs, nr, nc);
                if (t->state == STATE_EMPTY) continue;
                if (!jumped) { jumped = 1; continue; }
                if (t->state == STATE_FACEUP && t->side == en) {
                    out[cnt].r = nr; out[cnt].c = nc;
                    out[cnt].side = t->side; out[cnt].rank = t->rank;
                    cnt++;
                }
                break;
            }
        }
    }
    return cnt;
}

/*
 * 函數名稱：find_cannon_victims
 * 功能：找出所有正在被某方砲線瞄準的指定方棋子
 * 輸入：gs - 遊戲狀態, victim_side - 受害方陣營,
 *       out - 輸出陣列, max - 最大輸出數量
 * 輸出：int - 被砲線瞄準的棋子數量
 * 最後更新：2026-04-05 (UTC+8)
 */
static int find_cannon_victims(const GameState *gs, Side victim_side,
                                CannonTarget *out, int max)
{
    Side en = opponent(victim_side);
    int cnt = 0;
    /* 遍歷敵方所有砲 */
    for (int r = 1; r <= BOARD_ROWS && cnt < max; r++)
        for (int c = 1; c <= BOARD_COLS && cnt < max; c++) {
            const Cell *cl = CELL(gs, r, c);
            if (cl->state != STATE_FACEUP || cl->side != en ||
                cl->rank != RANK_CANNON) continue;
            CannonTarget tmp[MAX_CANNON_TARGETS];
            int n = find_cannon_targets(gs, r, c, en, tmp, MAX_CANNON_TARGETS);
            for (int i = 0; i < n && cnt < max; i++) {
                if (tmp[i].side == victim_side)
                    out[cnt++] = tmp[i];
            }
        }
    return cnt;
}

/*
 * 函數名稱：find_enemy_cannon_platforms
 * 功能：找出哪些棋子正充當敵方砲的砲架（移走可解除砲線威脅）
 * 輸入：gs - 遊戲狀態, my_side - 我方陣營,
 *       out - 輸出陣列, max - 最大輸出數量
 * 輸出：int - 砲架資訊數量
 * 最後更新：2026-04-05 (UTC+8)
 */
static int find_enemy_cannon_platforms(const GameState *gs, Side my_side,
                                       CannonPlatformInfo *out, int max)
{
    Side en = opponent(my_side);
    int cnt = 0;

    for (int r = 1; r <= BOARD_ROWS && cnt < max; r++)
        for (int c = 1; c <= BOARD_COLS && cnt < max; c++) {
            const Cell *cl = CELL(gs, r, c);
            if (cl->state != STATE_FACEUP || cl->side != en ||
                cl->rank != RANK_CANNON) continue;

            /* 掃描這門敵砲的四條線 */
            for (int axis = 0; axis < 2 && cnt < max; axis++) {
                for (int dir = -1; dir <= 1 && cnt < max; dir += 2) {
                    int jumped = 0;
                    int plat_r = 0, plat_c = 0;
                    for (int step = 1; ; step++) {
                        int nr = (axis == 0) ? r : r + dir * step;
                        int nc = (axis == 0) ? c + dir * step : c;
                        if (!IN_BOUNDS(nr, nc)) break;
                        const Cell *t = CELL(gs, nr, nc);
                        if (t->state == STATE_EMPTY) continue;
                        if (!jumped) {
                            jumped = 1;
                            plat_r = nr; plat_c = nc;
                            continue;
                        }
                        /* 第二顆 = 目標，如果是我方棋子，記錄砲架 */
                        if (t->state == STATE_FACEUP && t->side == my_side) {
                            out[cnt].platform_r = plat_r;
                            out[cnt].platform_c = plat_c;
                            out[cnt].target_r = nr;
                            out[cnt].target_c = nc;
                            out[cnt].target_rank = t->rank;
                            out[cnt].cannon_r = r;
                            out[cnt].cannon_c = c;
                            cnt++;
                        }
                        break;
                    }
                }
            }
        }
    return cnt;
}

/* =====================================================================
 *  §9 — 吃子分析
 * ===================================================================== */

/*
 * 函數名稱：evaluate_capture
 * 功能：評估某步吃子走法的淨值，模擬對方是否能立即反吃
 * 輸入：gs - 遊戲狀態, mv - 吃子走法（必須是 MOVE_CAPTURE）, out - 輸出分析結果
 * 輸出：無（透過 out 回傳）
 * 最後更新：2026-04-05 (UTC+8)
 */
static void evaluate_capture(const GameState *gs, const Move *mv, CaptureAnalysis *out)
{
    const Cell *attacker = CELL(gs, mv->from_r, mv->from_c);
    const Cell *victim   = CELL(gs, mv->to_r, mv->to_c);

    out->attacker_value = g_piece_val[attacker->rank];
    out->target_value   = g_piece_val[victim->rank];

    /* 模擬吃子後的棋盤 */
    GameState sim = *gs;
    board_apply_move(&sim, mv);

    /* 檢查對方能否立即反吃我的攻擊者（現在位於 to_r, to_c） */
    out->can_recapture = is_threatened_at(&sim, mv->to_r, mv->to_c,
                                           attacker->side, attacker->rank);

    if (out->can_recapture)
        out->net_value = out->target_value - out->attacker_value;
    else
        out->net_value = out->target_value;
}

/*
 * 函數名稱：is_unprotected
 * 功能：判斷某敵方棋子是否無保護（可被吃且對方無法反吃）
 * 輸入：gs - 遊戲狀態, r, c - 目標位置（1-based）
 * 輸出：int - 1=無保護, 0=有保護或不可吃
 * 最後更新：2026-04-05 (UTC+8)
 */
static int is_unprotected(const GameState *gs, int r, int c)
{
    const Cell *target = CELL(gs, r, c);
    if (target->state != STATE_FACEUP || target->side == SIDE_NONE) return 0;

    Side target_side = target->side;

    /* 檢查四方向有沒有友方可以保護它 */
    for (int d = 0; d < DIR4; d++) {
        int nr = r + DR4[d], nc = c + DC4[d];
        if (!IN_BOUNDS(nr, nc)) continue;
        const Cell *adj = CELL(gs, nr, nc);
        if (adj->state != STATE_FACEUP || adj->side != target_side) continue;
        /* 如果有同方棋子相鄰，且該棋子能吃掉任何可能來攻擊的敵方，
         * 就算有保護。簡化判斷：有同方鄰居就算有保護。 */
        return 0;
    }

    /* 檢查砲線上有沒有同方砲可以保護（跳過一顆棋子反吃） */
    for (int axis = 0; axis < 2; axis++) {
        for (int dir = -1; dir <= 1; dir += 2) {
            int jumped = 0;
            for (int step = 1; ; step++) {
                int nr = (axis == 0) ? r : r + dir * step;
                int nc = (axis == 0) ? c + dir * step : c;
                if (!IN_BOUNDS(nr, nc)) break;
                const Cell *t = CELL(gs, nr, nc);
                if (t->state == STATE_EMPTY) continue;
                if (!jumped) { jumped = 1; continue; }
                if (t->state == STATE_FACEUP && t->side == target_side &&
                    t->rank == RANK_CANNON)
                    return 0;   /* 有砲保護 */
                break;
            }
        }
    }

    return 1;  /* 無保護 */
}

/* =====================================================================
 *  §10 — 情勢評估
 * ===================================================================== */

static void assess_situation(const GameState *gs, Side my_side)
{
    Side en = opponent(my_side);
    memset(&g_sa, 0, sizeof(g_sa));

    /* Pass 1 — 定位棋子 & 子力統計 */
    for (int r = 1; r <= BOARD_ROWS; r++)
        for (int c = 1; c <= BOARD_COLS; c++) {
            const Cell *cl = CELL(gs, r, c);
            if (cl->state != STATE_FACEUP || cl->side == SIDE_NONE) continue;
            float val = g_piece_val[cl->rank];
            if (cl->side == my_side) {
                g_sa.my_material += val;
                g_sa.my_piece_count++;
                if (cl->rank == RANK_GENERAL) {
                    g_sa.my_gen_r = r; g_sa.my_gen_c = c;
                    g_sa.my_gen_alive = 1;
                }
            } else {
                g_sa.enemy_material += val;
                g_sa.enemy_piece_count++;
                if (cl->rank == RANK_GENERAL) {
                    g_sa.enemy_gen_r = r; g_sa.enemy_gen_c = c;
                    g_sa.enemy_gen_alive = 1;
                }
            }
        }

    /* Pass 2 — 我方受威脅棋子 */
    for (int r = 1; r <= BOARD_ROWS && g_sa.threat_count < MAX_THREATENED; r++)
        for (int c = 1; c <= BOARD_COLS && g_sa.threat_count < MAX_THREATENED; c++) {
            const Cell *cl = CELL(gs, r, c);
            if (cl->state != STATE_FACEUP || cl->side != my_side) continue;

            ThreatInfo ti;
            memset(&ti, 0, sizeof(ti));
            ti.r = r; ti.c = c; ti.rank = cl->rank; ti.value = g_piece_val[cl->rank];
            Cell temp = { my_side, cl->rank, STATE_FACEUP };

            /* 鄰接攻擊者 */
            for (int d = 0; d < DIR4 && ti.attacker_count < MAX_ATTACKERS; d++) {
                int nr = r + DR4[d], nc = c + DC4[d];
                if (!IN_BOUNDS(nr, nc)) continue;
                const Cell *adj = CELL(gs, nr, nc);
                if (adj->state == STATE_FACEUP && adj->side == en &&
                    board_can_capture(adj, &temp)) {
                    ti.attackers[ti.attacker_count].r = nr;
                    ti.attackers[ti.attacker_count].c = nc;
                    ti.attackers[ti.attacker_count].rank = adj->rank;
                    ti.attackers[ti.attacker_count].is_cannon = 0;
                    ti.attacker_count++;
                }
            }
            /* 砲線攻擊者 */
            for (int axis = 0; axis < 2 && ti.attacker_count < MAX_ATTACKERS; axis++) {
                for (int dir = -1; dir <= 1; dir += 2) {
                    int jumped = 0;
                    for (int step = 1; ; step++) {
                        int nr = (axis == 0) ? r : r + dir * step;
                        int nc = (axis == 0) ? c + dir * step : c;
                        if (!IN_BOUNDS(nr, nc)) break;
                        const Cell *t = CELL(gs, nr, nc);
                        if (t->state == STATE_EMPTY) continue;
                        if (!jumped) { jumped = 1; continue; }
                        if (t->state == STATE_FACEUP && t->side == en &&
                            t->rank == RANK_CANNON && ti.attacker_count < MAX_ATTACKERS) {
                            ti.attackers[ti.attacker_count].r = nr;
                            ti.attackers[ti.attacker_count].c = nc;
                            ti.attackers[ti.attacker_count].rank = RANK_CANNON;
                            ti.attackers[ti.attacker_count].is_cannon = 1;
                            ti.attacker_count++;
                        }
                        break;
                    }
                }
            }

            if (ti.attacker_count > 0) {
                if (cl->rank == RANK_GENERAL) g_sa.my_general_threatened = 1;
                g_sa.threatened[g_sa.threat_count++] = ti;
            }
        }

    /* Pass 3 — 可吃的敵方棋子 */
    for (int r = 1; r <= BOARD_ROWS && g_sa.opportunity_count < MAX_OPPORTUNITIES; r++)
        for (int c = 1; c <= BOARD_COLS && g_sa.opportunity_count < MAX_OPPORTUNITIES; c++) {
            const Cell *cl = CELL(gs, r, c);
            if (cl->state != STATE_FACEUP || cl->side != en) continue;
            for (int d = 0; d < DIR4; d++) {
                int nr = r + DR4[d], nc = c + DC4[d];
                if (!IN_BOUNDS(nr, nc)) continue;
                const Cell *adj = CELL(gs, nr, nc);
                if (adj->state == STATE_FACEUP && adj->side == my_side &&
                    board_can_capture(adj, cl)) {
                    OpportunityInfo oi = { r, c, cl->rank, g_piece_val[cl->rank],
                                           nr, nc, adj->rank, 0 };
                    if (cl->rank == RANK_GENERAL) {
                        g_sa.can_kill_enemy_general = 1;
                        g_sa.kill_general_move = (Move){ MOVE_CAPTURE, nr, nc, r, c };
                    }
                    g_sa.opportunities[g_sa.opportunity_count++] = oi;
                    break;
                }
            }
        }

    /* 緊迫度 */
    if (g_sa.my_gen_alive) {
        if (g_sa.my_general_threatened) {
            g_sa.general_danger_urgency = 10.0f;
        } else {
            float urg = 0;
            for (int r = 1; r <= BOARD_ROWS; r++)
                for (int c = 1; c <= BOARD_COLS; c++) {
                    const Cell *cl = CELL(gs, r, c);
                    if (cl->state != STATE_FACEUP || cl->side != en) continue;
                    int dist = manhattan(r, c, g_sa.my_gen_r, g_sa.my_gen_c);
                    if (cl->rank == RANK_SOLDIER) {
                        if (dist == 1) urg += 8.0f;
                        else if (dist == 2) urg += 4.0f;
                    } else {
                        Cell temp = { my_side, RANK_GENERAL, STATE_FACEUP };
                        if (board_can_capture(cl, &temp)) {
                            if (dist == 1) urg += 6.0f;
                            else if (dist == 2) urg += 2.0f;
                        }
                    }
                }
            g_sa.general_danger_urgency = minf(urg, 10.0f);
        }
    }

    if (g_sa.enemy_gen_alive) {
        float urg = 0;
        if (g_sa.can_kill_enemy_general) { urg = 10.0f; }
        else {
            for (int r = 1; r <= BOARD_ROWS; r++)
                for (int c = 1; c <= BOARD_COLS; c++) {
                    const Cell *cl = CELL(gs, r, c);
                    if (cl->state != STATE_FACEUP || cl->side != my_side) continue;
                    int dist = manhattan(r, c, g_sa.enemy_gen_r, g_sa.enemy_gen_c);
                    if (cl->rank == RANK_SOLDIER) {
                        if (dist == 1) urg += 8.0f;
                        else if (dist == 2) urg += 3.0f;
                    }
                }
        }
        g_sa.kill_opportunity_urgency = minf(urg, 10.0f);
    }

    /* 停滯壓力 */
    if (gs->no_capture_turns > 8) {
        int fd = count_facedown(gs);
        int tp = total_pieces_on_board(gs);
        float base = 0.08f;
        if (fd == 0 && tp <= 10) base = 0.15f;
        if (fd == 0 && tp <= 6)  base = 0.25f;
        if (g_sa.my_material > g_sa.enemy_material * 1.5f) base *= 1.5f;
        g_sa.stagnation_pressure = (gs->no_capture_turns - 8) * base;
    }

    /* 將帥逃路數 */
    g_sa.my_gen_escape_routes = g_sa.my_gen_alive
        ? count_escape_routes(gs, g_sa.my_gen_r, g_sa.my_gen_c) : 0;
    g_sa.enemy_gen_escape_routes = g_sa.enemy_gen_alive
        ? count_escape_routes(gs, g_sa.enemy_gen_r, g_sa.enemy_gen_c) : 0;

    /* 我方兵卒距敵將距離分布 */
    g_sa.soldier_near_enemy_gen_1 = 0;
    g_sa.soldier_near_enemy_gen_2 = 0;
    if (g_sa.enemy_gen_alive) {
        for (int r = 1; r <= BOARD_ROWS; r++)
            for (int c = 1; c <= BOARD_COLS; c++) {
                const Cell *cl = CELL(gs, r, c);
                if (cl->state != STATE_FACEUP || cl->side != my_side ||
                    cl->rank != RANK_SOLDIER) continue;
                int dist = manhattan(r, c, g_sa.enemy_gen_r, g_sa.enemy_gen_c);
                if (dist == 1) g_sa.soldier_near_enemy_gen_1++;
                if (dist <= 2) g_sa.soldier_near_enemy_gen_2++;
            }
    }

    /* 局面階段 */
    {
        int fd = count_facedown(gs);
        int tp = total_pieces_on_board(gs);
        if (fd > 16)
            g_sa.game_phase = PHASE_OPENING;
        else if (fd == 0 && tp <= 8)
            g_sa.game_phase = PHASE_ENDGAME;
        else
            g_sa.game_phase = PHASE_MIDGAME;
    }

    /* 距和棋剩餘回合 */
    g_sa.turns_until_draw = MAX_NO_CAPTURE - gs->no_capture_turns;

    /* 敵方合法走法數量 */
    {
        GameState sim = *gs;
        sim.current_turn = en;
        Move tmp_moves[MAX_MOVES];
        g_sa.enemy_legal_move_count = board_generate_moves(&sim, tmp_moves);
    }

    /* 敵方控制範圍地圖 */
    compute_enemy_reach(gs, my_side);
}

/* =====================================================================
 *  §11 — 公開 API
 * ===================================================================== */

/*
 * 函數名稱：strategy_select_move
 * 功能：AI 主策略函數 — 綜合算牌、情勢評估、啟發式評分與搜尋，選出最佳走法
 * 輸入：gs - 指向當前遊戲狀態的常數指標, out_move - 儲存所選走法的指標
 * 輸出：int - 成功選擇走法回傳 1，無合法走法回傳 0
 * 最後更新：2026-04-02 23:30 (UTC+8)
 */
int strategy_select_move(const GameState *gs, Move *out_move)
{
    g_my_side = gs->current_turn;
    Side my = g_my_side;
    Side enemy = (my == SIDE_RED) ? SIDE_BLACK : SIDE_RED;

    /* 記錄對手的上一步走法 */
    if (gs->has_last_move) {
        record_move(&gs->last_move, enemy);
    }

    /* 初始化分析工具 */
    detect_captures(gs);
    refresh_piece_values(gs);
    card_counter_update(gs, my);
    assess_situation(gs, my);

    Move legal[MAX_MOVES];
    int n = board_generate_moves(gs, legal);
    if (n == 0) return 0;
    if (n == 1) { *out_move = legal[0]; record_move(out_move, my); return 1; }

    /* === 優先級 1：能吃就吃（選最高價值目標）=== */
    float best_cap_val = -1.0f;
    int best_cap_idx = -1;
    for (int i = 0; i < n; i++) {
        if (legal[i].type != MOVE_CAPTURE) continue;
        const Cell *victim = CELL(gs, legal[i].to_r, legal[i].to_c);
        float val = g_piece_val[victim->rank];
        if (val > best_cap_val) {
            best_cap_val = val;
            best_cap_idx = i;
        }
    }
    if (best_cap_idx >= 0) {
        *out_move = legal[best_cap_idx];
        record_move(out_move, my);
        return 1;
    }

    /* === 優先級 2：被威脅就逃跑 === */
    for (int i = 0; i < n; i++) {
        if (legal[i].type != MOVE_WALK) continue;
        const Cell *src = CELL(gs, legal[i].from_r, legal[i].from_c);
        if (is_threatened_at(gs, legal[i].from_r, legal[i].from_c, src->side, src->rank) &&
            !is_threatened_at(gs, legal[i].to_r, legal[i].to_c, src->side, src->rank)) {
            *out_move = legal[i];
            record_move(out_move, my);
            return 1;
        }
    }

    /* === 優先級 3：隨機翻棋 === */
    Move flips[MAX_MOVES];
    int nf = 0;
    for (int i = 0; i < n; i++)
        if (legal[i].type == MOVE_FLIP) flips[nf++] = legal[i];
    if (nf > 0) {
        *out_move = flips[rand() % nf];
        record_move(out_move, my);
        return 1;
    }

    /* === Fallback：隨機走 === */
    *out_move = legal[rand() % n];
    record_move(out_move, my);
    return 1;
}

/*
 * 函數名稱：strategy_reset
 * 功能：重置策略系統的所有內部狀態（算牌、追逐歷史、置換表等），新遊戲時呼叫
 * 輸入：無
 * 輸出：無
 * 最後更新：2026-04-02 23:30 (UTC+8)
 */
void strategy_reset(void)
{
    memset(g_cap_hist, 0, sizeof(g_cap_hist));
    g_shadow_valid = 0;
    memset(g_move_history, 0, sizeof(g_move_history));
    g_move_history_count = 0;
}
