#include "strategy.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
static long long get_time_ms(void) {
    return (long long)GetTickCount();
}
#else
#include <sys/time.h>
static long long get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}
#endif

/* =====================================================================
 *  §1 — Constants & Configuration
 * ===================================================================== */

#define NUM_PIECE_TYPES    14   /* 2 sides x 7 ranks */
#define TYPE_IDX(s, r)     (((s) - 1) * 7 + ((r) - 1))

#define DIR4 4
static const int DR4[4] = { -1, 1, 0, 0 };
static const int DC4[4] = { 0, 0, -1, 1 };

static const int PIECE_COUNTS[7] = { 5, 2, 2, 2, 2, 2, 1 };

#define MAX_THREATENED     8
#define MAX_ATTACKERS      4
#define MAX_OPPORTUNITIES  8
#define MAX_CANNON_TARGETS   8
#define MAX_CANNON_PLATFORMS 16

#define PHASE_OPENING  0
#define PHASE_MIDGAME  1
#define PHASE_ENDGAME  2

/* Chase history ring buffer */
#define CHASE_HISTORY_SIZE 32

/* PVS Search */
#define TT_SIZE            (1 << 20)
#define TT_MASK            (TT_SIZE - 1)
#define SEARCH_MAX_BRANCHING 24
#define N_CANDIDATES       12
#define AI_SEARCH_TIME_MS  5000
#define QSEARCH_MAX_DEPTH  6
#define QSEARCH_MAX_CAPTURES 12
#define MAX_PLY            32

#define INF_SCORE          1e9f

/* TT flags */
#define TT_EXACT  0
#define TT_LOWER  1
#define TT_UPPER  2

/* Difficulty levels */
#define DIFF_HARD 0

/* Move history for repetition detection */
#define MOVE_HISTORY_SIZE 20

/* =====================================================================
 *  §2 — Data Structures
 * ===================================================================== */

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
    float prob_rank[BOARD_SIZE][8]; /* prob of rank regardless of side */
    int enemy_general_hidden;
    int my_general_hidden;
    float prob_flip_enemy_general;
    float prob_flip_my_general;
} CardCounter;

typedef struct {
    int r, c;
    PieceRank rank;
    float value;
    int attacker_count;
    struct { int r, c; PieceRank rank; int is_cannon; } attackers[MAX_ATTACKERS];
} ThreatInfo;

typedef struct {
    int r, c;
    PieceRank rank;
    float value;
    int my_r, my_c;
    PieceRank my_rank;
    int is_cannon_capture;
} OpportunityInfo;

typedef struct {
    int r, c;
    Side side;
    PieceRank rank;
} CannonTarget;

/* Chase history entry */
typedef struct {
    int from_r, from_c, to_r, to_c;
} ChaseEntry;

/* Move history for repetition */
typedef struct {
    Move move;
    Side side;
} MoveRecord;

/* Situation assessment */
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
    int enemy_reach[BOARD_SIZE];
    int game_phase;
    int turns_until_draw;
    int enemy_legal_move_count;
    int soldier_near_enemy_gen_1;
    int soldier_near_enemy_gen_2;
    int draw_mode;   /* 1 = 我方無法吃任何敵子且棋面全明，改以打平為主 */
} SituationAssessment;

/* Transposition table entry */
typedef struct {
    unsigned long long key;
    float score;
    int depth;
    int flag;
    Move best_move;
} TTEntry;

/* Zobrist hash tables */
static unsigned long long g_zobrist[BOARD_SIZE][3][3][8]; /* [cell][state][side][rank] */
static unsigned long long g_zobrist_black_turn;
static int g_zobrist_initialized = 0;

/* =====================================================================
 *  §3 — Global State
 * ===================================================================== */

static float g_piece_val[8];
static CardCounter g_cc;
static SituationAssessment g_sa;
static Side g_my_side;

/* Shadow board for capture detection */
static GameState g_shadow;
static int g_shadow_valid = 0;
static int g_cap_hist[NUM_PIECE_TYPES];

/* Chase history ring buffer */
static ChaseEntry g_chase_history[CHASE_HISTORY_SIZE];
static int g_chase_count = 0;

/* Move history for repetition */
static MoveRecord g_move_history[MOVE_HISTORY_SIZE];
static int g_move_history_count = 0;

/* Transposition table */
static TTEntry *g_tt = NULL;

/* Killer moves [ply][slot] */
static Move g_killers[MAX_PLY][2];

/* History heuristic [from][to] */
static int g_history[BOARD_SIZE][BOARD_SIZE];

/* Time management */
static long long g_search_start_time;

/* =====================================================================
 *  §4 — Utility Functions
 * ===================================================================== */

static int manhattan(int r1, int c1, int r2, int c2)
{ return abs(r1 - r2) + abs(c1 - c2); }

static float minf(float a, float b) { return a < b ? a : b; }
static float maxf(float a, float b) { return a > b ? a : b; }

static int cell_idx(int r, int c) { return (r - 1) * BOARD_COLS + (c - 1); }

static int total_pieces_on_board(const GameState *gs)
{ return gs->red_alive + gs->black_alive; }

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

static void record_move(const Move *mv, Side side)
{
    int idx = g_move_history_count % MOVE_HISTORY_SIZE;
    g_move_history[idx].move = *mv;
    g_move_history[idx].side = side;
    g_move_history_count++;
}

static int moves_equal(const Move *a, const Move *b)
{
    return a->type == b->type &&
           a->from_r == b->from_r && a->from_c == b->from_c &&
           a->to_r == b->to_r && a->to_c == b->to_c;
}

/* Chase detection: record and check */
static void chase_record(const Move *mv)
{
    if (mv->type == MOVE_FLIP) return;
    int idx = g_chase_count % CHASE_HISTORY_SIZE;
    g_chase_history[idx].from_r = mv->from_r;
    g_chase_history[idx].from_c = mv->from_c;
    g_chase_history[idx].to_r = mv->to_r;
    g_chase_history[idx].to_c = mv->to_c;
    g_chase_count++;
}

static int chase_check(const Move *mv)
{
    if (mv->type == MOVE_FLIP) return 0;
    int count = 0;
    int limit = g_chase_count < CHASE_HISTORY_SIZE ? g_chase_count : CHASE_HISTORY_SIZE;
    for (int i = 0; i < limit; i++) {
        int idx = (g_chase_count - 1 - i) % CHASE_HISTORY_SIZE;
        if (idx < 0) idx += CHASE_HISTORY_SIZE;
        const ChaseEntry *e = &g_chase_history[idx];
        /* Check forward and reverse match */
        if ((e->from_r == mv->from_r && e->from_c == mv->from_c &&
             e->to_r == mv->to_r && e->to_c == mv->to_c) ||
            (e->from_r == mv->to_r && e->from_c == mv->to_c &&
             e->to_r == mv->from_r && e->to_c == mv->from_c)) {
            count++;
        }
    }
    return count;
}

/* =====================================================================
 *  §5 — Dynamic Piece Value Table
 * ===================================================================== */

static void refresh_piece_values(const GameState *gs)
{
    int total = total_pieces_on_board(gs);

    g_piece_val[RANK_NONE]    = 0.0f;
    g_piece_val[RANK_SOLDIER] = 1.0f;
    g_piece_val[RANK_HORSE]   = 3.5f;
    g_piece_val[RANK_ELEPHANT]= 2.0f;
    g_piece_val[RANK_ADVISOR] = 2.5f;

    /* Cannon dynamic value */
    if      (total > 20) g_piece_val[RANK_CANNON] = 6.0f;
    else if (total > 16) g_piece_val[RANK_CANNON] = 5.0f;
    else if (total > 12) g_piece_val[RANK_CANNON] = 4.0f;
    else if (total >  8) g_piece_val[RANK_CANNON] = 3.0f;
    else                 g_piece_val[RANK_CANNON] = 2.0f;

    /* Chariot dynamic value */
    if      (total > 20) g_piece_val[RANK_CHARIOT] = 4.0f;
    else if (total > 16) g_piece_val[RANK_CHARIOT] = 4.5f;
    else if (total > 12) g_piece_val[RANK_CHARIOT] = 5.0f;
    else if (total >  8) g_piece_val[RANK_CHARIOT] = 5.5f;
    else                 g_piece_val[RANK_CHARIOT] = 6.5f;

    /* General dynamic value based on soldier threat */
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
        if (threat <= 0.1f)      g_piece_val[RANK_GENERAL] = 8.0f;
        else if (threat <= 2.0f) g_piece_val[RANK_GENERAL] = 20.0f;
        else                     g_piece_val[RANK_GENERAL] = 40.0f;
    } else {
        if (threat <= 0.1f)      g_piece_val[RANK_GENERAL] = 15.0f;
        else if (threat <= 2.0f) g_piece_val[RANK_GENERAL] = 40.0f;
        else if (threat <= 4.0f) g_piece_val[RANK_GENERAL] = 60.0f;
        else                     g_piece_val[RANK_GENERAL] = 100.0f;
    }
}

/* =====================================================================
 *  §6 — Card Counting System
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

    for (int s = 1; s <= 2; s++)
        for (int rk = 1; rk <= 7; rk++)
            g_cc.total[TYPE_IDX(s, rk)] = PIECE_COUNTS[rk - 1];

    for (int r = 0; r < BOARD_ROWS; r++)
        for (int c = 0; c < BOARD_COLS; c++) {
            const Cell *cl = &gs->cells[r][c];
            if (cl->state == STATE_FACEDOWN) g_cc.total_facedown++;
            else if (cl->state == STATE_FACEUP && cl->side != SIDE_NONE)
                g_cc.revealed[TYPE_IDX(cl->side, cl->rank)]++;
        }

    for (int i = 0; i < NUM_PIECE_TYPES; i++) {
        g_cc.captured[i] = g_cap_hist[i];
        g_cc.hidden[i] = g_cc.total[i] - g_cc.revealed[i] - g_cc.captured[i];
        if (g_cc.hidden[i] < 0) g_cc.hidden[i] = 0;
    }

    /* Normalize: ensure sum(hidden) == total_facedown */
    int sum_hidden = 0;
    for (int i = 0; i < NUM_PIECE_TYPES; i++) sum_hidden += g_cc.hidden[i];
    if (sum_hidden > 0 && sum_hidden != g_cc.total_facedown) {
        float scale = (float)g_cc.total_facedown / (float)sum_hidden;
        for (int i = 0; i < NUM_PIECE_TYPES; i++)
            g_cc.hidden[i] = (int)(g_cc.hidden[i] * scale + 0.5f);
    }

    /* Compute probabilities */
    if (g_cc.total_facedown > 0) {
        float inv = 1.0f / (float)g_cc.total_facedown;
        for (int r = 0; r < BOARD_ROWS; r++)
            for (int c = 0; c < BOARD_COLS; c++) {
                if (gs->cells[r][c].state != STATE_FACEDOWN) continue;
                int idx = r * BOARD_COLS + c;
                float pf = 0, pe = 0, ev = 0;
                memset(g_cc.prob_rank[idx], 0, sizeof(g_cc.prob_rank[idx]));
                for (int t = 0; t < NUM_PIECE_TYPES; t++) {
                    float p = g_cc.hidden[t] * inv;
                    g_cc.prob[idx][t] = p;
                    int side = (t < 7) ? SIDE_RED : SIDE_BLACK;
                    int rank = (t % 7) + 1;
                    if (side == (int)my_side) pf += p; else pe += p;
                    ev += p * g_piece_val[rank];
                    g_cc.prob_rank[idx][rank] += p;
                }
                g_cc.prob_friendly[idx] = pf;
                g_cc.prob_enemy[idx] = pe;
                g_cc.expected_value[idx] = ev;
            }
    }

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
 *  §7 — Threat Detection
 * ===================================================================== */

static int is_threatened_at(const GameState *gs, int r, int c, Side side, PieceRank rank)
{
    Cell temp = { side, rank, STATE_FACEUP };
    for (int d = 0; d < DIR4; d++) {
        int nr = r + DR4[d], nc = c + DC4[d];
        if (!IN_BOUNDS(nr, nc)) continue;
        const Cell *adj = CELL(gs, nr, nc);
        if (adj->state == STATE_FACEUP && adj->side != SIDE_NONE && adj->side != side)
            /* Cannon cannot capture adjacent pieces — needs a platform to jump */
            if (adj->rank != RANK_CANNON && board_can_capture(adj, &temp)) return 1;
    }
    /* Cannon line threat - horizontal */
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
    /* Cannon line threat - vertical */
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

static void compute_enemy_reach(const GameState *gs, Side my_side)
{
    Side en = opponent(my_side);
    memset(g_sa.enemy_reach, 0, sizeof(g_sa.enemy_reach));

    for (int r = 1; r <= BOARD_ROWS; r++)
        for (int c = 1; c <= BOARD_COLS; c++) {
            const Cell *cl = CELL(gs, r, c);
            if (cl->state != STATE_FACEUP || cl->side != en) continue;

            if (cl->rank != RANK_CANNON) {
                for (int d = 0; d < DIR4; d++) {
                    int nr = r + DR4[d], nc = c + DC4[d];
                    if (IN_BOUNDS(nr, nc))
                        g_sa.enemy_reach[cell_idx(nr, nc)] = 1;
                }
            } else {
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
 *  §8 — Cannon Analysis
 * ===================================================================== */

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

/* =====================================================================
 *  §10 — Situation Assessment
 * ===================================================================== */

/* 檢查我方是否有能力吃到任何敵子（階級相容性檢查，非當前位置檢查）。
 * 只要存在「我方某棋子的階級可以吃掉敵方某棋子」，就回傳 1。
 * 這樣才能正確判斷「根本不可能吃子（如只剩兵對士）」vs「暫時沒有相鄰吃子機會」。
 * 炮：只要場上有炮，理論上永遠可以通過移動建立跳吃，直接回傳 1。 */
static int can_capture_any_enemy(const GameState *gs, Side my_side)
{
    Side en = opponent(my_side);

    for (int r = 1; r <= BOARD_ROWS; r++) {
        for (int c = 1; c <= BOARD_COLS; c++) {
            const Cell *src = CELL(gs, r, c);
            if (src->state != STATE_FACEUP || src->side != my_side) continue;

            /* 炮可以透過移動建立跳吃，只要敵方有棋子就算可以吃 */
            if (src->rank == RANK_CANNON) return 1;

            /* 一般棋子：檢查階級相容性（不看位置，只看能否吃） */
            for (int r2 = 1; r2 <= BOARD_ROWS; r2++) {
                for (int c2 = 1; c2 <= BOARD_COLS; c2++) {
                    const Cell *tgt = CELL(gs, r2, c2);
                    if (tgt->state != STATE_FACEUP || tgt->side != en) continue;
                    if (board_can_capture(src, tgt)) return 1;
                }
            }
        }
    }
    return 0;
}

static void assess_situation(const GameState *gs, Side my_side)
{
    Side en = opponent(my_side);
    memset(&g_sa, 0, sizeof(g_sa));

    /* Pass 1 — Locate pieces & material count */
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

    /* Pass 2 — Threatened friendly pieces */
    for (int r = 1; r <= BOARD_ROWS && g_sa.threat_count < MAX_THREATENED; r++)
        for (int c = 1; c <= BOARD_COLS && g_sa.threat_count < MAX_THREATENED; c++) {
            const Cell *cl = CELL(gs, r, c);
            if (cl->state != STATE_FACEUP || cl->side != my_side) continue;

            ThreatInfo ti;
            memset(&ti, 0, sizeof(ti));
            ti.r = r; ti.c = c; ti.rank = cl->rank; ti.value = g_piece_val[cl->rank];
            Cell temp = { my_side, cl->rank, STATE_FACEUP };

            /* Adjacent attackers */
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
            /* Cannon line attackers */
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

    /* Pass 3 — Capturable enemy pieces */
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
            /* Also check cannon captures */
            if (g_sa.opportunity_count < MAX_OPPORTUNITIES) {
                for (int cr = 1; cr <= BOARD_ROWS; cr++)
                    for (int cc2 = 1; cc2 <= BOARD_COLS; cc2++) {
                        const Cell *cannon = CELL(gs, cr, cc2);
                        if (cannon->state != STATE_FACEUP || cannon->side != my_side ||
                            cannon->rank != RANK_CANNON) continue;
                        /* Check if this cannon can jump-capture cl at (r,c) */
                        if (cr != r && cc2 != c) continue; /* must be same row or col */
                        int between = 0;
                        if (cr == r) {
                            int minc = (cc2 < c) ? cc2 : c;
                            int maxc = (cc2 > c) ? cc2 : c;
                            for (int tc = minc + 1; tc < maxc; tc++)
                                if (CELL(gs, r, tc)->state != STATE_EMPTY) between++;
                        } else {
                            int minr = (cr < r) ? cr : r;
                            int maxr = (cr > r) ? cr : r;
                            for (int tr = minr + 1; tr < maxr; tr++)
                                if (CELL(gs, tr, c)->state != STATE_EMPTY) between++;
                        }
                        if (between == 1) {
                            if (cl->rank == RANK_GENERAL) {
                                g_sa.can_kill_enemy_general = 1;
                                g_sa.kill_general_move = (Move){ MOVE_CAPTURE, cr, cc2, r, c };
                            }
                            if (g_sa.opportunity_count < MAX_OPPORTUNITIES) {
                                OpportunityInfo oi = { r, c, cl->rank, g_piece_val[cl->rank],
                                                       cr, cc2, RANK_CANNON, 1 };
                                g_sa.opportunities[g_sa.opportunity_count++] = oi;
                            }
                        }
                    }
            }
        }

    /* Urgency calculations */
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
                        Cell temp2 = { my_side, RANK_GENERAL, STATE_FACEUP };
                        if (board_can_capture(cl, &temp2)) {
                            if (dist == 1) urg += 6.0f;
                            else if (dist == 2) urg += 2.0f;
                        }
                    }
                    if (cl->rank == RANK_CANNON) {
                        if (r == g_sa.my_gen_r || c == g_sa.my_gen_c)
                            urg += 2.0f;
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
                    if (cl->rank == RANK_CANNON) {
                        if (r == g_sa.enemy_gen_r || c == g_sa.enemy_gen_c)
                            urg += 2.0f;
                    }
                }
        }
        g_sa.kill_opportunity_urgency = minf(urg, 10.0f);
    }

    /* Stagnation pressure — ramp up aggressively to avoid draws */
    if (gs->no_capture_turns > 4) {
        int fd2 = count_facedown(gs);
        int tp = total_pieces_on_board(gs);
        float base = 0.15f;
        if (fd2 == 0 && tp <= 10) base = 0.35f;
        if (fd2 == 0 && tp <= 6)  base = 0.55f;
        if (g_sa.my_material > g_sa.enemy_material * 1.3f) base *= 2.0f;
        float turns = (float)(gs->no_capture_turns - 4);
        /* Quadratic ramp: slow at first, very aggressive near draw */
        g_sa.stagnation_pressure = turns * base * (1.0f + turns * 0.06f);
    }

    g_sa.my_gen_escape_routes = g_sa.my_gen_alive
        ? count_escape_routes(gs, g_sa.my_gen_r, g_sa.my_gen_c) : 0;
    g_sa.enemy_gen_escape_routes = g_sa.enemy_gen_alive
        ? count_escape_routes(gs, g_sa.enemy_gen_r, g_sa.enemy_gen_c) : 0;

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

    {
        int fd2 = count_facedown(gs);
        int tp = total_pieces_on_board(gs);
        if (fd2 > 16) g_sa.game_phase = PHASE_OPENING;
        else if (fd2 == 0 && tp <= 8) g_sa.game_phase = PHASE_ENDGAME;
        else g_sa.game_phase = PHASE_MIDGAME;
    }

    g_sa.turns_until_draw = MAX_NO_CAPTURE - gs->no_capture_turns;

    {
        GameState sim = *gs;
        sim.current_turn = en;
        Move tmp_moves[MAX_MOVES];
        g_sa.enemy_legal_move_count = board_generate_moves(&sim, tmp_moves);
    }

    compute_enemy_reach(gs, my_side);

    {
        int fd3 = count_facedown(gs);
        float mat_ratio = (g_sa.enemy_material > 0.01f)
            ? (g_sa.my_material / g_sa.enemy_material)
            : 999.0f;

        g_sa.draw_mode = (fd3 == 0 &&
                          g_sa.enemy_piece_count > 0 &&
                          !can_capture_any_enemy(gs, my_side) &&
                          gs->no_capture_turns >= 20 &&
                          mat_ratio <= 1.1f) ? 1 : 0;
    }
}

/* =====================================================================
 *  §11 — Heuristic Move Scoring
 * ===================================================================== */

/* Score capture safety (trade analysis at destination) */
static float score_capture_safety(const GameState *gs, const Move *mv, Side my_side)
{
    const Cell *attacker = CELL(gs, mv->from_r, mv->from_c);
    const Cell *victim   = CELL(gs, mv->to_r, mv->to_c);
    float my_val = g_piece_val[attacker->rank];
    float victim_val = g_piece_val[victim->rank];

    /* For cannon captures, the attacker stays at from position conceptually,
       but actually moves to to position. Still check safety at destination. */
    GameState sim = *gs;
    board_apply_move(&sim, mv);

    int att_count = 0, def_count = 0;
    float lowest_attacker_val = 999.0f;

    for (int d = 0; d < DIR4; d++) {
        int nr = mv->to_r + DR4[d], nc = mv->to_c + DC4[d];
        if (!IN_BOUNDS(nr, nc)) continue;
        const Cell *adj = CELL(&sim, nr, nc);
        if (adj->state != STATE_FACEUP || adj->side == SIDE_NONE) continue;
        if (adj->side != my_side) {
            /* Enemy adjacent that can capture us */
            Cell temp = { my_side, attacker->rank, STATE_FACEUP };
            if (board_can_capture(adj, &temp)) {
                att_count++;
                float av = g_piece_val[adj->rank];
                if (av < lowest_attacker_val) lowest_attacker_val = av;
            }
        } else {
            /* Friendly defender (not the moving piece itself) */
            if (nr != mv->from_r || nc != mv->from_c)
                def_count++;
        }
    }

    /* Also check cannon line threats after capture */
    if (is_threatened_at(&sim, mv->to_r, mv->to_c, my_side, attacker->rank))
        if (att_count == 0) att_count = 1;

    float score = 0;
    if (att_count > 0 && def_count == 0) {
        score -= my_val * 0.8f;
    } else if (att_count > def_count) {
        float net = victim_val - my_val;
        if (net < -0.5f) score -= 2.5f;
        else score -= 0.5f;
    } else if (def_count >= att_count && att_count > 0) {
        score += 0.3f;
    }

    /* 2-step threat: enemy at distance 2 that could move adjacent then retake.
       Applies a softer penalty since the threat takes two moves to materialise.
       Key case: attacker is low-value (e.g. Soldier) and enemy Chariot nearby. */
    if (att_count == 0) {
        float two_step_pen = 0.0f;
        for (int d = 0; d < DIR4; d++) {
            int adj_r = mv->to_r + DR4[d], adj_c = mv->to_c + DC4[d];
            if (!IN_BOUNDS(adj_r, adj_c)) continue;
            /* Enemy can only move to adj_r/adj_c if it is empty after capture */
            if (CELL(&sim, adj_r, adj_c)->state != STATE_EMPTY) continue;
            for (int d2 = 0; d2 < DIR4; d2++) {
                int nr2 = adj_r + DR4[d2], nc2 = adj_c + DC4[d2];
                if (!IN_BOUNDS(nr2, nc2)) continue;
                if (nr2 == mv->to_r && nc2 == mv->to_c) continue; /* skip dest */
                const Cell *far = CELL(&sim, nr2, nc2);
                if (far->state != STATE_FACEUP || far->side == my_side ||
                    far->side == SIDE_NONE) continue;
                Cell temp2 = { my_side, attacker->rank, STATE_FACEUP };
                if (board_can_capture(far, &temp2)) {
                    float pen = my_val * 0.35f;
                    if (pen > two_step_pen) two_step_pen = pen;
                }
            }
        }
        score -= two_step_pen;
    }

    return score;
}

/* Score capture move */
static float score_capture(const GameState *gs, const Move *mv, Side my_side)
{
    const Cell *attacker = CELL(gs, mv->from_r, mv->from_c);
    const Cell *victim   = CELL(gs, mv->to_r, mv->to_c);
    float my_val = g_piece_val[attacker->rank];
    float victim_val = g_piece_val[victim->rank];

    float score = 6.0f + victim_val * 1.2f;

    if (victim->rank == RANK_GENERAL)
        score += 80.0f;

    if (my_val < victim_val)
        score += 3.0f;

    if (gs->no_capture_turns > 5)
        score += (gs->no_capture_turns - 5) * 3.0f;

    if (gs->no_capture_turns > 15)
        score += (gs->no_capture_turns - 15) * 5.0f;

    if (gs->no_capture_turns > 30)
        score += (gs->no_capture_turns - 30) * 8.0f;

    score += score_capture_safety(gs, mv, my_side);

    float hidden_danger = 0;
    for (int d = 0; d < DIR4; d++) {
        int nr = mv->to_r + DR4[d], nc = mv->to_c + DC4[d];
        if (!IN_BOUNDS(nr, nc)) continue;
        if (CELL(gs, nr, nc)->state != STATE_FACEDOWN) continue;
        int idx = cell_idx(nr, nc);
        for (int t = 0; t < NUM_PIECE_TYPES; t++) {
            int ts = (t < 7) ? SIDE_RED : SIDE_BLACK;
            int tr = (t % 7) + 1;
            if (ts == (int)my_side) continue;
            Cell att2 = { (Side)ts, (PieceRank)tr, STATE_FACEUP };
            Cell def2 = { my_side, attacker->rank, STATE_FACEUP };
            if (board_can_capture(&att2, &def2))
                hidden_danger += g_cc.prob[idx][t] * my_val * 0.5f;
        }
    }
    score -= hidden_danger;

    return score;
}

/* Score flip move */
static float score_flip(const GameState *gs, const Move *mv, Side my_side)
{
    int r = mv->from_r, c = mv->from_c;
    int idx = cell_idx(r, c);
    int fd = g_cc.total_facedown;

    float score = 1.2f + 0.06f * fd;

    /* Adjacent bonus */
    int adj_friendly = 0, adj_enemy = 0;
    for (int d = 0; d < DIR4; d++) {
        int nr = r + DR4[d], nc = c + DC4[d];
        if (!IN_BOUNDS(nr, nc)) continue;
        const Cell *adj = CELL(gs, nr, nc);
        if (adj->state != STATE_FACEUP) continue;
        if (adj->side == my_side) adj_friendly++;
        else if (adj->side != SIDE_NONE) adj_enemy++;
    }
    score += adj_friendly * 0.15f;
    score += adj_enemy * 0.10f;

    /* Underdog bonus */
    if (g_sa.my_piece_count < g_sa.enemy_piece_count)
        score += 0.5f;

    /* Stagnation bonus — flips are the primary way to break stagnation */
    score += g_sa.stagnation_pressure * 2.5f;

    /* Card counting analysis */
    float p_friendly = g_cc.prob_friendly[idx];
    float p_enemy = g_cc.prob_enemy[idx];
    score += (p_friendly - p_enemy) * 0.8f;

    float flip_risk = cc_flip_risk(gs, r, c, my_side);
    float flip_opp = cc_flip_opportunity(gs, r, c, my_side);
    score -= flip_risk * 3.5f;
    score += flip_opp * 1.5f;

    float ev = g_cc.expected_value[idx];
    score += ev * p_friendly * 0.15f;
    score -= ev * p_enemy * 0.10f;

    /* Key piece tracking */
    if (g_cc.enemy_general_hidden) {
        /* Check if my soldier is adjacent */
        for (int d = 0; d < DIR4; d++) {
            int nr = r + DR4[d], nc = c + DC4[d];
            if (!IN_BOUNDS(nr, nc)) continue;
            const Cell *adj = CELL(gs, nr, nc);
            if (adj->state == STATE_FACEUP && adj->side == my_side &&
                adj->rank == RANK_SOLDIER) {
                int en_gen_t = TYPE_IDX(opponent(my_side), RANK_GENERAL);
                score += g_cc.prob[idx][en_gen_t] * 12.0f;
                break;
            }
        }
    }
    if (g_cc.my_general_hidden) {
        for (int d = 0; d < DIR4; d++) {
            int nr = r + DR4[d], nc = c + DC4[d];
            if (!IN_BOUNDS(nr, nc)) continue;
            const Cell *adj = CELL(gs, nr, nc);
            if (adj->state == STATE_FACEUP && adj->side != my_side &&
                adj->side != SIDE_NONE && adj->rank == RANK_SOLDIER) {
                int my_gen_t = TYPE_IDX(my_side, RANK_GENERAL);
                score -= g_cc.prob[idx][my_gen_t] * 10.0f;
                break;
            }
        }
    }

    /* Endgame high-value tracking (facedown <= 8) */
    if (fd <= 8) {
        for (int d = 0; d < DIR4; d++) {
            int nr = r + DR4[d], nc = c + DC4[d];
            if (!IN_BOUNDS(nr, nc)) continue;
            const Cell *adj = CELL(gs, nr, nc);
            if (adj->state != STATE_FACEUP || adj->side != my_side) continue;
            for (int t = 0; t < NUM_PIECE_TYPES; t++) {
                int ts = (t < 7) ? SIDE_RED : SIDE_BLACK;
                int tr = (t % 7) + 1;
                if (ts == (int)my_side) continue;
                Cell tmp_a = { my_side, adj->rank, STATE_FACEUP };
                Cell tmp_d = { (Side)ts, (PieceRank)tr, STATE_FACEUP };
                if (board_can_capture(&tmp_a, &tmp_d))
                    score += g_cc.prob[idx][t] * g_piece_val[tr] * 0.4f;
            }
        }
    }

    return score;
}

/* Score cannon platform effect - moving away might break our cannon lines */
static float score_cannon_platform(const GameState *gs, const Move *mv, Side my_side)
{
    float score = 0;
    int fr = mv->from_r, fc = mv->from_c;

    /* Check 4 directions: am I a cannon platform for a friendly cannon? */
    for (int axis = 0; axis < 2; axis++) {
        for (int dir = -1; dir <= 1; dir += 2) {
            int cannon_r = 0, cannon_c = 0;
            int found_cannon = 0;

            /* Search in one direction for our cannon */
            for (int step = 1; ; step++) {
                int nr = (axis == 0) ? fr : fr + dir * step;
                int nc = (axis == 0) ? fc + dir * step : fc;
                if (!IN_BOUNDS(nr, nc)) break;
                const Cell *t = CELL(gs, nr, nc);
                if (t->state == STATE_EMPTY) continue;
                if (t->state == STATE_FACEUP && t->side == my_side &&
                    t->rank == RANK_CANNON) {
                    cannon_r = nr; cannon_c = nc;
                    found_cannon = 1;
                }
                break;
            }
            if (!found_cannon) continue;

            /* Search in opposite direction for enemy target */
            for (int step = 1; ; step++) {
                int nr = (axis == 0) ? fr : fr + (-dir) * step;
                int nc = (axis == 0) ? fc + (-dir) * step : fc;
                if (!IN_BOUNDS(nr, nc)) break;
                const Cell *t = CELL(gs, nr, nc);
                if (t->state == STATE_EMPTY) continue;
                if (t->state == STATE_FACEUP && t->side != my_side &&
                    t->side != SIDE_NONE) {
                    /* I'm the platform between our cannon and enemy target */
                    float target_val = g_piece_val[t->rank];
                    score -= target_val * 0.35f;
                    if (t->rank == RANK_GENERAL)
                        score -= 5.0f;
                }
                break;
            }
            (void)cannon_r; (void)cannon_c;
        }
    }

    return score;
}

/* Score cannon line setup opportunities */
static float score_cannon_setup(const GameState *gs, const Move *mv, Side my_side)
{
    float score = 0;
    int tr = mv->to_r, tc = mv->to_c;
    int fr = mv->from_r, fc = mv->from_c;
    const Cell *mover = CELL(gs, fr, fc);

    /* Case A: Moving to become a cannon platform */
    for (int axis = 0; axis < 2; axis++) {
        for (int dir = -1; dir <= 1; dir += 2) {
            /* Look for our cannon in one direction from destination */
            int cannon_found = 0;
            for (int step = 1; ; step++) {
                int nr = (axis == 0) ? tr : tr + dir * step;
                int nc = (axis == 0) ? tc + dir * step : tc;
                if (!IN_BOUNDS(nr, nc)) break;
                const Cell *t = CELL(gs, nr, nc);
                if (t->state == STATE_EMPTY) continue;
                if (t->state == STATE_FACEUP && t->side == my_side &&
                    t->rank == RANK_CANNON)
                    cannon_found = 1;
                break;
            }
            if (!cannon_found) continue;

            /* Look for enemy target in opposite direction */
            int pieces_between = 0;
            for (int step = 1; ; step++) {
                int nr = (axis == 0) ? tr : tr + (-dir) * step;
                int nc = (axis == 0) ? tc + (-dir) * step : tc;
                if (!IN_BOUNDS(nr, nc)) break;
                const Cell *t = CELL(gs, nr, nc);
                if (t->state == STATE_EMPTY) continue;
                pieces_between++;
                if (pieces_between == 1 && t->state == STATE_FACEUP &&
                    t->side != my_side && t->side != SIDE_NONE) {
                    /* We'd be the platform: cannon -> us -> target */
                    float tv = g_piece_val[t->rank];
                    score += tv * 0.5f;
                    if (t->rank == RANK_GENERAL) score += 8.0f;
                }
                break;
            }
        }
    }

    /* Case B: Cannon moving to firing position */
    if (mover->rank == RANK_CANNON) {
        for (int axis = 0; axis < 2; axis++) {
            for (int dir = -1; dir <= 1; dir += 2) {
                int jumped = 0;
                for (int step = 1; ; step++) {
                    int nr = (axis == 0) ? tr : tr + dir * step;
                    int nc = (axis == 0) ? tc + dir * step : tc;
                    if (!IN_BOUNDS(nr, nc)) break;
                    const Cell *t = CELL(gs, nr, nc);
                    if (t->state == STATE_EMPTY) continue;
                    if (!jumped) { jumped = 1; continue; }
                    if (t->state == STATE_FACEUP && t->side != my_side &&
                        t->side != SIDE_NONE) {
                        float tv = g_piece_val[t->rank];
                        score += tv * 0.45f;
                        if (t->rank == RANK_GENERAL) score += 8.0f;
                    }
                    break;
                }
            }
        }
    }

    /* Case C: Moving away clears a cannon line */
    /* After we move from (fr,fc), check if a friendly cannon now has
       a clear shot through where we were */
    for (int axis = 0; axis < 2; axis++) {
        for (int dir = -1; dir <= 1; dir += 2) {
            int cannon_found = 0;
            for (int step = 1; ; step++) {
                int nr = (axis == 0) ? fr : fr + dir * step;
                int nc = (axis == 0) ? fc + dir * step : fc;
                if (!IN_BOUNDS(nr, nc)) break;
                if (nr == tr && nc == tc) continue; /* destination */
                const Cell *t = CELL(gs, nr, nc);
                if (t->state == STATE_EMPTY) continue;
                if (t->state == STATE_FACEUP && t->side == my_side &&
                    t->rank == RANK_CANNON)
                    cannon_found = 1;
                break;
            }
            if (!cannon_found) continue;

            /* Check opposite direction for a single piece then enemy target */
            int pieces = 0;
            for (int step = 1; ; step++) {
                int nr = (axis == 0) ? fr : fr + (-dir) * step;
                int nc = (axis == 0) ? fc + (-dir) * step : fc;
                if (!IN_BOUNDS(nr, nc)) break;
                if (nr == tr && nc == tc) continue;
                const Cell *t = CELL(gs, nr, nc);
                if (t->state == STATE_EMPTY) continue;
                pieces++;
                if (pieces == 1) continue; /* this would be platform */
                if (t->state == STATE_FACEUP && t->side != my_side &&
                    t->side != SIDE_NONE) {
                    float tv = g_piece_val[t->rank];
                    score += tv * 0.35f;
                    if (t->rank == RANK_GENERAL) score += 8.0f;
                }
                break;
            }
        }
    }

    /* Case D: Moving TO destination makes us a hostile cannon platform
     * (enemy cannon → 0 pieces gap → us → 0 pieces gap → friendly piece).
     * This penalizes moves like stepping into the line between an enemy
     * Cannon and your own King/high-value piece. */
    for (int axis = 0; axis < 2; axis++) {
        for (int dir = -1; dir <= 1; dir += 2) {
            /* Look for enemy Cannon in one direction from destination,
             * with no pieces between it and destination */
            int enemy_cannon_r = 0, enemy_cannon_c = 0, cannon_found = 0;
            for (int step = 1; ; step++) {
                int nr = (axis == 0) ? tr : tr + dir * step;
                int nc = (axis == 0) ? tc + dir * step : tc;
                if (!IN_BOUNDS(nr, nc)) break;
                if (nr == fr && nc == fc) continue; /* our source square becomes empty */
                const Cell *t = CELL(gs, nr, nc);
                if (t->state == STATE_EMPTY) continue;
                if (t->state == STATE_FACEUP && t->side != my_side &&
                    t->side != SIDE_NONE && t->rank == RANK_CANNON) {
                    enemy_cannon_r = nr; enemy_cannon_c = nc;
                    cannon_found = 1;
                }
                break; /* stop at first non-empty */
            }
            if (!cannon_found) continue;
            (void)enemy_cannon_r; (void)enemy_cannon_c;

            /* Look for friendly piece in opposite direction from destination,
             * with no pieces between it and destination */
            for (int step = 1; ; step++) {
                int nr = (axis == 0) ? tr : tr + (-dir) * step;
                int nc = (axis == 0) ? tc + (-dir) * step : tc;
                if (!IN_BOUNDS(nr, nc)) break;
                if (nr == fr && nc == fc) continue; /* our source becomes empty */
                const Cell *t = CELL(gs, nr, nc);
                if (t->state == STATE_EMPTY) continue;
                if (t->state == STATE_FACEUP && t->side == my_side &&
                    t->side != SIDE_NONE) {
                    float tv = g_piece_val[t->rank];
                    score -= tv * 1.5f;
                    if (t->rank == RANK_GENERAL) score -= 20.0f;
                }
                break; /* stop at first non-empty */
            }
        }
    }

    return score;
}

/* Score soldier-general interactions */
static float score_soldier_general(const GameState *gs, const Move *mv, Side my_side)
{
    float score = 0;
    const Cell *mover = CELL(gs, mv->from_r, mv->from_c);
    Side en = opponent(my_side);

    /* Soldier approaching enemy general */
    if (mover->rank == RANK_SOLDIER && g_sa.enemy_gen_alive) {
        int dist_before = manhattan(mv->from_r, mv->from_c,
                                     g_sa.enemy_gen_r, g_sa.enemy_gen_c);
        int dist_after = manhattan(mv->to_r, mv->to_c,
                                    g_sa.enemy_gen_r, g_sa.enemy_gen_c);
        if (dist_after < dist_before) {
            if (dist_after <= 3) score += 5.0f;
            else score += 2.0f;
            if (dist_after == 1) score += 8.0f;
        }
    }

    /* General smart retreat from enemy soldiers */
    if (mover->rank == RANK_GENERAL) {
        float retreat_score = 0;
        int near_enemy_soldier = 0;

        for (int r = 1; r <= BOARD_ROWS; r++)
            for (int c = 1; c <= BOARD_COLS; c++) {
                const Cell *cl = CELL(gs, r, c);
                if (cl->state != STATE_FACEUP || cl->side != en ||
                    cl->rank != RANK_SOLDIER) continue;
                int dist_from = manhattan(mv->from_r, mv->from_c, r, c);
                if (dist_from <= 3) near_enemy_soldier = 1;

                int dist_to = manhattan(mv->to_r, mv->to_c, r, c);
                if (dist_to > dist_from) {
                    /* Retreating away */
                    retreat_score = 4.0f;

                    /* Lure toward our ally that can kill the soldier */
                    for (int d = 0; d < DIR4; d++) {
                        int ar = mv->to_r + DR4[d], ac = mv->to_c + DC4[d];
                        if (!IN_BOUNDS(ar, ac)) continue;
                        const Cell *ally = CELL(gs, ar, ac);
                        if (ally->state != STATE_FACEUP || ally->side != my_side) continue;
                        Cell sol = { en, RANK_SOLDIER, STATE_FACEUP };
                        if (board_can_capture(ally, &sol)) {
                            int ally_dist = manhattan(ar, ac, r, c);
                            if (ally_dist <= 2)
                                retreat_score += 6.0f;
                            else
                                retreat_score += 3.0f;
                            break;
                        }
                    }
                }
            }

        if (near_enemy_soldier && retreat_score > 0) {
            /* Corner penalty */
            int escape = count_escape_routes(gs, mv->to_r, mv->to_c);
            /* Subtract one because we came from somewhere */
            if (escape == 0) retreat_score -= 5.0f;
            else if (escape == 1) retreat_score -= 2.0f;

            /* Near friendly bonus */
            int friends = count_friendly_adj(gs, mv->to_r, mv->to_c, my_side);
            retreat_score += friends * 1.5f;

            score += retreat_score;
        }
    }

    /* Friendly intercept: protect our general from enemy soldiers */
    if (g_sa.my_gen_alive && g_sa.my_general_threatened && mover->rank != RANK_GENERAL) {
        for (int i = 0; i < g_sa.threat_count; i++) {
            if (g_sa.threatened[i].rank != RANK_GENERAL) continue;
            for (int a = 0; a < g_sa.threatened[i].attacker_count; a++) {
                int ar = g_sa.threatened[i].attackers[a].r;
                int ac = g_sa.threatened[i].attackers[a].c;
                if (g_sa.threatened[i].attackers[a].rank != RANK_SOLDIER &&
                    !g_sa.threatened[i].attackers[a].is_cannon) continue;

                int dist_from = manhattan(mv->from_r, mv->from_c, ar, ac);
                int dist_to = manhattan(mv->to_r, mv->to_c, ar, ac);

                if (dist_to < dist_from) {
                    score += 12.0f;
                    if (dist_to == 1) score += 35.0f;
                    else if (dist_to == 2) score += 15.0f;
                }
            }
        }
    }

    return score;
}

/* Score general safety when general is threatened */
static float score_general_safety(const GameState *gs, const Move *mv, Side my_side)
{
    if (!g_sa.my_general_threatened) return 0;

    const Cell *mover = CELL(gs, mv->from_r, mv->from_c);

    /* Case 1: General escapes */
    if (mover->rank == RANK_GENERAL && mv->type == MOVE_WALK) {
        GameState sim = *gs;
        board_apply_move(&sim, mv);
        int safe = !is_threatened_at(&sim, mv->to_r, mv->to_c, my_side, RANK_GENERAL);
        if (safe) {
            float escape_quality = 50.0f;
            int routes = count_escape_routes(&sim, mv->to_r, mv->to_c);
            if (routes == 0) escape_quality -= 10.0f;
            else if (routes == 1) escape_quality -= 4.0f;
            escape_quality += count_friendly_adj(&sim, mv->to_r, mv->to_c, my_side) * 3.0f;
            return escape_quality;
        }
        return 0;
    }

    /* Case 2: Capture the attacker */
    if (mv->type == MOVE_CAPTURE) {
        for (int i = 0; i < g_sa.threat_count; i++) {
            if (g_sa.threatened[i].rank != RANK_GENERAL) continue;
            for (int a = 0; a < g_sa.threatened[i].attacker_count; a++) {
                if (mv->to_r == g_sa.threatened[i].attackers[a].r &&
                    mv->to_c == g_sa.threatened[i].attackers[a].c)
                    return 40.0f;
            }
        }
    }

    /* Case 3: Block cannon line */
    if (mv->type == MOVE_WALK) {
        for (int i = 0; i < g_sa.threat_count; i++) {
            if (g_sa.threatened[i].rank != RANK_GENERAL) continue;
            for (int a = 0; a < g_sa.threatened[i].attacker_count; a++) {
                if (!g_sa.threatened[i].attackers[a].is_cannon) continue;
                /* Check if moving to mv->to blocks the cannon line */
                GameState sim2 = *gs;
                board_apply_move(&sim2, mv);
                if (!is_threatened_at(&sim2, g_sa.my_gen_r, g_sa.my_gen_c,
                                       my_side, RANK_GENERAL))
                    return 30.0f;
            }
        }
    }

    return 0;
}

/* Score piece priority: save threatened pieces, capture their attackers */
static float score_piece_priority(const GameState *gs, const Move *mv, Side my_side)
{
    float bonus = 0;
    const Cell *mover = CELL(gs, mv->from_r, mv->from_c);
    float mover_val = g_piece_val[mover->rank];

    /* Move threatened piece to safety */
    if (mv->type == MOVE_WALK) {
        for (int i = 0; i < g_sa.threat_count; i++) {
            if (g_sa.threatened[i].r == mv->from_r &&
                g_sa.threatened[i].c == mv->from_c) {
                /* Check if destination is safe */
                if (!is_threatened_at(gs, mv->to_r, mv->to_c, my_side, mover->rank))
                    bonus += mover_val * 1.5f;
                break;
            }
        }
    }

    /* Capture a piece that threatens our piece */
    if (mv->type == MOVE_CAPTURE) {
        for (int i = 0; i < g_sa.threat_count; i++) {
            for (int a = 0; a < g_sa.threatened[i].attacker_count; a++) {
                if (mv->to_r == g_sa.threatened[i].attackers[a].r &&
                    mv->to_c == g_sa.threatened[i].attackers[a].c) {
                    bonus += g_sa.threatened[i].value * 1.2f;
                    break;
                }
            }
        }
    }

    /* Penalty for moving away from threatened general */
    if (g_sa.my_gen_alive && g_sa.general_danger_urgency > 3.0f &&
        mv->type == MOVE_WALK && mover->rank != RANK_GENERAL) {
        int dist_from = manhattan(mv->from_r, mv->from_c, g_sa.my_gen_r, g_sa.my_gen_c);
        int dist_to = manhattan(mv->to_r, mv->to_c, g_sa.my_gen_r, g_sa.my_gen_c);
        if (dist_to > dist_from)
            bonus -= g_sa.general_danger_urgency * 0.3f;
    }

    return bonus;
}

/* Score walk move */
static float score_walk(const GameState *gs, const Move *mv, Side my_side)
{
    float score = 0;
    const Cell *mover = CELL(gs, mv->from_r, mv->from_c);
    float piece_val = g_piece_val[mover->rank];
    Side en = opponent(my_side);

    int is_curr_threatened = is_threatened_at(gs, mv->from_r, mv->from_c,
                                               my_side, mover->rank);
    int is_dest_safe = !is_threatened_at(gs, mv->to_r, mv->to_c,
                                          my_side, mover->rank);

    int dominant_eg = (g_sa.enemy_piece_count <= 2 &&
                       g_sa.my_material >= g_sa.enemy_material * 2.5f);
    int should_flee = is_curr_threatened && is_dest_safe &&
                      !(dominant_eg && mover->rank == RANK_SOLDIER);

    if (should_flee) {
        float flee_score = 2.5f + piece_val * 0.35f;

        int dest_escape = count_escape_routes(gs, mv->to_r, mv->to_c);
        if (dest_escape == 0) flee_score -= piece_val * 0.4f;
        else if (dest_escape == 1) flee_score -= piece_val * 0.15f;

        flee_score += count_friendly_adj(gs, mv->to_r, mv->to_c, my_side) * 0.5f;

        for (int d = 0; d < DIR4; d++) {
            int ar = mv->from_r + DR4[d], ac = mv->from_c + DC4[d];
            if (!IN_BOUNDS(ar, ac)) continue;
            const Cell *att = CELL(gs, ar, ac);
            if (att->state != STATE_FACEUP || att->side != en) continue;
            Cell temp_me = { my_side, mover->rank, STATE_FACEUP };
            if (!board_can_capture(att, &temp_me)) continue;
            float att_val = g_piece_val[att->rank];

            for (int d2 = 0; d2 < DIR4; d2++) {
                int allyr = mv->to_r + DR4[d2], allyc = mv->to_c + DC4[d2];
                if (!IN_BOUNDS(allyr, allyc)) continue;
                const Cell *ally = CELL(gs, allyr, allyc);
                if (ally->state != STATE_FACEUP || ally->side != my_side) continue;
                if (board_can_capture(ally, att)) {
                    int ally_dist_att = manhattan(allyr, allyc, ar, ac);
                    if (ally_dist_att <= 2)
                        flee_score += att_val * 0.5f;
                    else
                        flee_score += att_val * 0.15f;
                    break;
                }
            }
        }

        flee_score *= 0.7f;
        score += flee_score;
    }

    if (!is_dest_safe && !is_curr_threatened) {
        for (int d = 0; d < DIR4; d++) {
            int er = mv->to_r + DR4[d], ec = mv->to_c + DC4[d];
            if (!IN_BOUNDS(er, ec)) continue;
            const Cell *enemy_cell = CELL(gs, er, ec);
            if (enemy_cell->state != STATE_FACEUP || enemy_cell->side != en) continue;
            Cell temp_me2 = { my_side, mover->rank, STATE_FACEUP };
            if (!board_can_capture(enemy_cell, &temp_me2)) continue;
            float att_val = g_piece_val[enemy_cell->rank];

            for (int d2 = 0; d2 < DIR4; d2++) {
                int allyr = mv->to_r + DR4[d2], allyc = mv->to_c + DC4[d2];
                if (allyr == mv->from_r && allyc == mv->from_c) continue;
                if (!IN_BOUNDS(allyr, allyc)) continue;
                const Cell *ally = CELL(gs, allyr, allyc);
                if (ally->state != STATE_FACEUP || ally->side != my_side) continue;
                if (board_can_capture(ally, enemy_cell)) {
                    float sac_gain = att_val - piece_val;
                    if (sac_gain > 0.5f)
                        score += sac_gain * 0.6f;
                    break;
                }
            }
        }
    }

    if (is_dest_safe) {
        for (int d = 0; d < DIR4; d++) {
            int nr = mv->to_r + DR4[d], nc = mv->to_c + DC4[d];
            if (!IN_BOUNDS(nr, nc)) continue;
            const Cell *adj = CELL(gs, nr, nc);
            if (adj->state != STATE_FACEUP || adj->side != en) continue;
            if (board_can_capture(mover, adj)) {
                float enemy_val = g_piece_val[adj->rank];
                float w = g_sa.draw_mode ? 0.6f : 1.0f;
                score += (1.5f + enemy_val * 0.25f) * w;
            }
        }
    }

    float hidden_danger_dest = cc_hidden_threat(gs, mv->to_r, mv->to_c,
                                                 my_side, mover->rank);
    float hidden_danger_from = cc_hidden_threat(gs, mv->from_r, mv->from_c,
                                                 my_side, mover->rank);
    score -= hidden_danger_dest * piece_val * 0.35f;
    score += (hidden_danger_from - hidden_danger_dest) * piece_val * 0.25f;

    float hidden_opp = 0;
    for (int d = 0; d < DIR4; d++) {
        int nr = mv->to_r + DR4[d], nc = mv->to_c + DC4[d];
        if (!IN_BOUNDS(nr, nc)) continue;
        if (CELL(gs, nr, nc)->state != STATE_FACEDOWN) continue;
        int fidx = cell_idx(nr, nc);
        for (int t = 0; t < NUM_PIECE_TYPES; t++) {
            int ts = (t < 7) ? SIDE_RED : SIDE_BLACK;
            int tr = (t % 7) + 1;
            if (ts == (int)my_side) continue;
            Cell tmp_a = { my_side, mover->rank, STATE_FACEUP };
            Cell tmp_d = { (Side)ts, (PieceRank)tr, STATE_FACEUP };
            if (board_can_capture(&tmp_a, &tmp_d))
                hidden_opp += g_cc.prob[fidx][t] * g_piece_val[tr];
        }
    }
    score += hidden_opp * 0.12f;

    score += score_cannon_platform(gs, mv, my_side);
    score += score_cannon_setup(gs, mv, my_side);

    int center_from = ((mv->from_r >= 2 && mv->from_r <= 3) ? 1 : 0) +
                      ((mv->from_c >= 3 && mv->from_c <= 6) ? 1 : 0);
    int center_to = ((mv->to_r >= 2 && mv->to_r <= 3) ? 1 : 0) +
                    ((mv->to_c >= 3 && mv->to_c <= 6) ? 1 : 0);
    if (center_to > center_from) score += 0.15f;
    else if (center_to < center_from) score -= 0.08f;

    if (!is_dest_safe) {
        if (mover->rank == RANK_GENERAL)
            score -= piece_val * 3.0f;
        else if (count_friendly_adj(gs, mv->to_r, mv->to_c, my_side) == 0)
            score -= piece_val * 1.2f;
        else
            score -= piece_val * 0.5f;
    }

    if (!is_curr_threatened) {
        int creates_threat = 0;
        int threat_count = 0;
        for (int d = 0; d < DIR4; d++) {
            int nr = mv->to_r + DR4[d], nc = mv->to_c + DC4[d];
            if (!IN_BOUNDS(nr, nc)) continue;
            const Cell *adj = CELL(gs, nr, nc);
            if (adj->state == STATE_FACEUP && adj->side == en &&
                board_can_capture(mover, adj)) {
                creates_threat = 1;
                threat_count++;
            }
        }
        if (creates_threat && !g_sa.draw_mode)
            score += 0.6f + threat_count * 0.5f;
        else if (!creates_threat && !g_sa.draw_mode)
            score -= 0.5f + g_sa.stagnation_pressure * 0.8f;
    }

    int fd = count_facedown(gs);
    int tp = total_pieces_on_board(gs);
    int en_pc = g_sa.enemy_piece_count;
    int my_pc = g_sa.my_piece_count;
    /* Piece-count-based trigger only when all tiles revealed, so mid-game
     * flipping isn't abandoned in favour of chasing visible enemies */
    int is_endgame = (fd == 0 && tp <= 8) ||
                     (g_sa.my_material >= g_sa.enemy_material * 2.0f) ||
                     (fd == 0 && en_pc > 0 && my_pc >= en_pc * 3);

    if (is_endgame && en_pc > 0) {
        float endgame_mult = 1.0f;
        if (tp <= 4)      endgame_mult = 2.5f;
        else if (tp <= 6) endgame_mult = 2.0f;
        else if (tp <= 8) endgame_mult = 1.5f;

        /* Extra boost when all pieces revealed and we're clearly dominant */
        if (fd == 0) {
            if      (en_pc == 1)               endgame_mult = maxf(endgame_mult, 10.0f);
            else if (en_pc <= 2 && my_pc >= 6) endgame_mult = maxf(endgame_mult, 7.0f);
            else if (en_pc <= 2 && my_pc >= 4) endgame_mult = maxf(endgame_mult, 5.0f);
            else if (en_pc <= 3 && my_pc >= 6) endgame_mult = maxf(endgame_mult, 4.0f);
            else if (en_pc <= 3 && my_pc >= 4) endgame_mult = maxf(endgame_mult, 3.0f);
            else if (g_sa.my_material >= g_sa.enemy_material * 2.0f)
                                                endgame_mult = maxf(endgame_mult, 2.5f);
        }
        /* Stagnation boost: the longer we go without capture, the more aggressive */
        if (gs->no_capture_turns > 10)
            endgame_mult *= 1.0f + (gs->no_capture_turns - 10) * 0.05f;

        /* Simulate move once; reuse for all escape-count comparisons */
        GameState sim = *gs;
        board_apply_move(&sim, mv);

        /* Find closest capturable enemy target */
        int best_dist = 999;
        int best_er = 0, best_ec = 0;
        for (int r = 1; r <= BOARD_ROWS; r++)
            for (int c = 1; c <= BOARD_COLS; c++) {
                const Cell *cl = CELL(gs, r, c);
                if (cl->state != STATE_FACEUP || cl->side != en) continue;
                if (board_can_capture(mover, cl)) {
                    int dist = manhattan(mv->from_r, mv->from_c, r, c);
                    if (dist < best_dist) {
                        best_dist = dist;
                        best_er = r; best_ec = c;
                    }
                }
            }

        if (best_er > 0) {
            /* This piece CAN capture an enemy — chase it down */
            int dist_before = manhattan(mv->from_r, mv->from_c, best_er, best_ec);
            int dist_after  = manhattan(mv->to_r,   mv->to_c,   best_er, best_ec);
            float pursuit_score = 0;

            if (dist_after < dist_before)
                pursuit_score += (dist_before - dist_after) * 1.5f;
            /* Bonus for being one step away: ready to capture next turn */
            if (dist_after <= 1) pursuit_score += 2.0f;

            int escape_before = count_escape_routes(gs,   best_er, best_ec);
            int escape_after  = count_escape_routes(&sim, best_er, best_ec);
            if (escape_after < escape_before)
                pursuit_score += (escape_before - escape_after) * 1.5f;

            if (mv->to_r == best_er || mv->to_c == best_ec)
                pursuit_score += 1.0f;

            int freedom = escape_before;
            if (freedom <= 1) pursuit_score += 3.0f;
            else if (freedom <= 2) pursuit_score += 1.5f;

            score += pursuit_score * endgame_mult;
        } else if (fd == 0) {
            /* This piece CANNOT capture any visible enemy.
             * Reward squeezing enemy escape routes AND closing distance
             * to help form a net around enemies. The strong repetition
             * penalty handles oscillation prevention. */
            float conv_score = 0;
            int conv_targets = 0;
            for (int r = 1; r <= BOARD_ROWS; r++) {
                for (int c = 1; c <= BOARD_COLS; c++) {
                    const Cell *cl = CELL(gs, r, c);
                    if (cl->state != STATE_FACEUP || cl->side != en) continue;
                    conv_targets++;

                    int esc_b = count_escape_routes(gs,   r, c);
                    int esc_a = count_escape_routes(&sim, r, c);
                    if (esc_a < esc_b)
                        conv_score += (esc_b - esc_a) * 3.0f;

                    /* Also reward closing distance to help form wall */
                    int db = manhattan(mv->from_r, mv->from_c, r, c);
                    int da = manhattan(mv->to_r,   mv->to_c,   r, c);
                    if (da < db && da <= 3)
                        conv_score += (db - da) * 1.5f;
                }
            }
            float conv_mult = minf(endgame_mult, 4.0f);
            if (conv_targets > 0)
                score += (conv_score / conv_targets) * conv_mult;
        }
    }

    return score;
}

/* Main heuristic scoring function */
static float ai_heuristic_score(const GameState *gs, const Move *mv, Side my_side)
{
    float score = 0;

    switch (mv->type) {
    case MOVE_CAPTURE:
        score = score_capture(gs, mv, my_side);
        break;
    case MOVE_FLIP:
        score = score_flip(gs, mv, my_side);
        break;
    case MOVE_WALK:
        score = score_walk(gs, mv, my_side);
        break;
    }

    score += score_general_safety(gs, mv, my_side);
    score += score_piece_priority(gs, mv, my_side);
    score += score_soldier_general(gs, mv, my_side);

    int fd2 = count_facedown(gs);
    int is_endgame2 = (fd2 == 0) ||
                      (g_sa.my_material >= g_sa.enemy_material * 1.5f);
    if (mv->type == MOVE_CAPTURE && is_endgame2) {
        if (g_sa.enemy_piece_count == 1) score += 80.0f;
        else if (g_sa.enemy_piece_count == 2) score += 40.0f;
        else if (g_sa.enemy_piece_count == 3) score += 20.0f;
        else if (g_sa.enemy_piece_count <= 5) score += 10.0f;
    }
    /* Any capture when stagnating is valuable */
    if (mv->type == MOVE_CAPTURE && gs->no_capture_turns > 15)
        score += (gs->no_capture_turns - 15) * 2.0f;

    if (g_sa.draw_mode) {
        if (mv->type != MOVE_CAPTURE) {
            int min_dist_from = 999, min_dist_to = 999;
            for (int dr = 1; dr <= BOARD_ROWS; dr++) {
                for (int dc = 1; dc <= BOARD_COLS; dc++) {
                    const Cell *cl = CELL(gs, dr, dc);
                    if (cl->state != STATE_FACEUP ||
                        cl->side != opponent(my_side)) continue;
                    int df = manhattan(mv->from_r, mv->from_c, dr, dc);
                    int dt = manhattan(mv->to_r,   mv->to_c,   dr, dc);
                    if (df < min_dist_from) min_dist_from = df;
                    if (dt < min_dist_to)   min_dist_to   = dt;
                }
            }
            if (min_dist_from < 999) {
                int delta = min_dist_to - min_dist_from;
                if (delta > 0)
                    score += delta * 0.25f;
                else if (delta < 0)
                    score += delta * 0.3f;
            }
        }
    }

    int fd = count_facedown(gs);
    int tp = total_pieces_on_board(gs);
    int is_deep_endgame = (fd == 0 && tp <= 10) ||
                          (fd == 0 &&
                           g_sa.my_piece_count >= g_sa.enemy_piece_count * 3 &&
                           g_sa.enemy_piece_count <= 4);
    int rep = chase_check(mv);

    if (rep > 0) {
        if (g_sa.draw_mode) {
            if      (rep >= 4) score -= 8.0f;
            else if (rep >= 2) score -= 4.0f;
            else               score -= 2.0f;
        } else {
            float pen;
            if (is_deep_endgame) {
                if      (rep >= 4) pen = 80.0f;
                else if (rep >= 3) pen = 55.0f;
                else if (rep >= 2) pen = 35.0f;
                else               pen = 20.0f;
            } else {
                if      (rep >= 4) pen = 45.0f;
                else if (rep >= 3) pen = 30.0f;
                else if (rep >= 2) pen = 18.0f;
                else               pen = 8.0f;
            }
            /* Dominant + stagnating: push harder to vary moves */
            if (g_sa.stagnation_pressure > 1.0f)
                pen *= 1.5f + g_sa.stagnation_pressure * 0.3f;
            if (g_sa.my_material >= g_sa.enemy_material * 1.3f)
                pen *= 1.8f;
            score -= pen;
        }
    }

    /* Draw countdown emergency: when no-capture counter is rising,
     * urgently squeeze enemy escape routes and close distance.
     * Applies both when board is fully revealed AND when facedown tiles exist. */
    if (!g_sa.draw_mode &&
        g_sa.turns_until_draw <= 42 &&
        g_sa.my_material >= g_sa.enemy_material * 0.9f &&
        mv->type != MOVE_CAPTURE) {
        float urgency = (43.0f - g_sa.turns_until_draw) * 0.8f;
        /* Extra urgency when clearly winning */
        if (g_sa.my_material > g_sa.enemy_material * 1.5f)
            urgency *= 1.5f;
        /* Extreme urgency in last 15 turns */
        if (g_sa.turns_until_draw <= 15)
            urgency *= 2.0f;
        GameState sim2 = *gs;
        board_apply_move(&sim2, mv);
        Side en2 = opponent(my_side);
        for (int r = 1; r <= BOARD_ROWS; r++) {
            for (int c = 1; c <= BOARD_COLS; c++) {
                const Cell *cl = CELL(gs, r, c);
                if (cl->state != STATE_FACEUP || cl->side != en2) continue;
                int esc_b = count_escape_routes(gs,    r, c);
                int esc_a = count_escape_routes(&sim2, r, c);
                if (esc_a < esc_b)
                    score += (esc_b - esc_a) * urgency;
                int db = manhattan(mv->from_r, mv->from_c, r, c);
                int da = manhattan(mv->to_r,   mv->to_c,   r, c);
                if (da < db)
                    score += (db - da) * urgency * 0.6f;
            }
        }
        /* Bonus for flipping when stagnating with facedown tiles */
        if (mv->type == MOVE_FLIP && fd > 0)
            score += urgency * 1.5f;
    }

    return score;
}

/* =====================================================================
 *  §12 — Static Board Evaluation (for search)
 * ===================================================================== */

static float ai_heuristic_eval(const GameState *gs)
{
    float score = 0;
    int facedown = 0;

    /* Pass 1: Material */
    for (int r = 1; r <= BOARD_ROWS; r++)
        for (int c = 1; c <= BOARD_COLS; c++) {
            const Cell *cl = CELL(gs, r, c);
            if (cl->state == STATE_FACEDOWN) { facedown++; continue; }
            if (cl->state != STATE_FACEUP || cl->side == SIDE_NONE) continue;
            float val = g_piece_val[cl->rank];
            float sign = (cl->side == SIDE_RED) ? 1.0f : -1.0f;
            score += val * sign;
        }

    score += (gs->red_alive - gs->black_alive) * 1.5f;

    /* Pass 2: Hidden material estimate (card counting weighted) */
    if (facedown > 0) {
        float certainty = 1.0f - facedown / 32.0f;
        float hidden_weight = 0.15f + 0.15f * certainty;

        for (int i = 0; i < NUM_PIECE_TYPES; i++) {
            int side = (i < 7) ? SIDE_RED : SIDE_BLACK;
            int rank = (i % 7) + 1;
            float sign = (side == SIDE_RED) ? 1.0f : -1.0f;
            score += g_cc.hidden[i] * g_piece_val[rank] * hidden_weight * sign;
        }

        /* Hidden danger for visible pieces */
        for (int r = 1; r <= BOARD_ROWS; r++)
            for (int c = 1; c <= BOARD_COLS; c++) {
                const Cell *cl = CELL(gs, r, c);
                if (cl->state != STATE_FACEUP || cl->side == SIDE_NONE) continue;
                float sign = (cl->side == SIDE_RED) ? 1.0f : -1.0f;
                float danger = 0;
                for (int d = 0; d < DIR4; d++) {
                    int nr = r + DR4[d], nc = c + DC4[d];
                    if (!IN_BOUNDS(nr, nc)) continue;
                    if (CELL(gs, nr, nc)->state != STATE_FACEDOWN) continue;
                    int idx = cell_idx(nr, nc);
                    for (int t = 0; t < NUM_PIECE_TYPES; t++) {
                        int ts = (t < 7) ? SIDE_RED : SIDE_BLACK;
                        int tr = (t % 7) + 1;
                        if (ts == (int)cl->side) continue;
                        Cell att = { (Side)ts, (PieceRank)tr, STATE_FACEUP };
                        if (board_can_capture(&att, cl))
                            danger += g_cc.prob[idx][t] * g_piece_val[cl->rank] * 0.08f;
                    }
                }
                score -= danger * sign;
            }
    }

    /* Pass 3: Tactical threats */
    for (int r = 1; r <= BOARD_ROWS; r++)
        for (int c = 1; c <= BOARD_COLS; c++) {
            const Cell *cl = CELL(gs, r, c);
            if (cl->state != STATE_FACEUP || cl->side == SIDE_NONE) continue;
            float sign = (cl->side == SIDE_RED) ? 1.0f : -1.0f;

            /* Adjacent threats/opportunities */
            for (int d = 0; d < DIR4; d++) {
                int nr = r + DR4[d], nc = c + DC4[d];
                if (!IN_BOUNDS(nr, nc)) continue;
                const Cell *adj = CELL(gs, nr, nc);
                if (adj->state != STATE_FACEUP || adj->side == SIDE_NONE ||
                    adj->side == cl->side) continue;
                if (board_can_capture(cl, adj))
                    score += (0.4f + g_piece_val[adj->rank] * 0.12f) * sign;
                if (board_can_capture(adj, cl))
                    score -= (0.4f + g_piece_val[cl->rank] * 0.12f) * sign;
            }

            /* Cannon line threats */
            if (cl->rank == RANK_CANNON) {
                CannonTarget targets[MAX_CANNON_TARGETS];
                int nt = find_cannon_targets(gs, r, c, cl->side, targets, MAX_CANNON_TARGETS);
                for (int i = 0; i < nt; i++)
                    score += (0.3f + g_piece_val[targets[i].rank] * 0.08f) * sign;
            }
        }

    /* Pass 4: Mobility */
    for (int r = 1; r <= BOARD_ROWS; r++)
        for (int c = 1; c <= BOARD_COLS; c++) {
            const Cell *cl = CELL(gs, r, c);
            if (cl->state != STATE_FACEUP || cl->side == SIDE_NONE) continue;
            float sign = (cl->side == SIDE_RED) ? 1.0f : -1.0f;
            int safe_moves = 0;
            for (int d = 0; d < DIR4; d++) {
                int nr = r + DR4[d], nc = c + DC4[d];
                if (!IN_BOUNDS(nr, nc)) continue;
                if (CELL(gs, nr, nc)->state == STATE_EMPTY &&
                    !is_threatened_at(gs, nr, nc, cl->side, cl->rank))
                    safe_moves++;
            }
            score += safe_moves * 0.15f * sign;
        }

    /* Pass 5: Coordination (isolation penalty for high-value pieces) */
    for (int r = 1; r <= BOARD_ROWS; r++)
        for (int c = 1; c <= BOARD_COLS; c++) {
            const Cell *cl = CELL(gs, r, c);
            if (cl->state != STATE_FACEUP || cl->side == SIDE_NONE) continue;
            if (g_piece_val[cl->rank] < 3.0f) continue;
            float sign = (cl->side == SIDE_RED) ? 1.0f : -1.0f;
            int has_friend = 0;
            for (int d = 0; d < DIR4; d++) {
                int nr = r + DR4[d], nc = c + DC4[d];
                if (!IN_BOUNDS(nr, nc)) continue;
                const Cell *adj = CELL(gs, nr, nc);
                if (adj->state == STATE_FACEUP && adj->side == cl->side) {
                    has_friend = 1;
                    break;
                }
            }
            if (!has_friend)
                score -= 0.5f * sign;
        }

    /* Terminal states */
    if (gs->game_over) {
        if (gs->winner == SIDE_RED) score += 100.0f;
        else if (gs->winner == SIDE_BLACK) score -= 100.0f;
    }

    return score;
}

/* Simplified board evaluation (faster, no card counting) */
static float board_evaluate(const GameState *gs)
{
    float score = 0;

    for (int r = 1; r <= BOARD_ROWS; r++)
        for (int c = 1; c <= BOARD_COLS; c++) {
            const Cell *cl = CELL(gs, r, c);
            if (cl->state != STATE_FACEUP || cl->side == SIDE_NONE) continue;
            float sign = (cl->side == SIDE_RED) ? 1.0f : -1.0f;
            score += g_piece_val[cl->rank] * sign;

            for (int d = 0; d < DIR4; d++) {
                int nr = r + DR4[d], nc = c + DC4[d];
                if (!IN_BOUNDS(nr, nc)) continue;
                const Cell *adj = CELL(gs, nr, nc);
                if (adj->state != STATE_FACEUP || adj->side == cl->side ||
                    adj->side == SIDE_NONE) continue;
                if (board_can_capture(cl, adj))
                    score += (0.5f + g_piece_val[adj->rank] * 0.15f) * sign;
                if (board_can_capture(adj, cl))
                    score -= (0.5f + g_piece_val[cl->rank] * 0.15f) * sign;
            }
        }

    score += (gs->red_alive - gs->black_alive) * 1.0f;

    /* Draw proximity: reduce winning advantage as no-capture counter rises.
     * Incentivises the search to prefer captures when winning. */
    {
        int nc = gs->no_capture_turns;
        if (nc >= 15) {
            float draw_pen = (nc - 14) * 0.6f;
            /* Quadratic ramp near draw limit */
            if (nc >= 35) draw_pen += (nc - 34) * 1.5f;
            if (score > 1.0f) score -= draw_pen;
            else if (score < -1.0f) score += draw_pen;
        }
    }

    if (gs->game_over) {
        if (gs->winner == SIDE_RED) score += 100.0f;
        else if (gs->winner == SIDE_BLACK) score -= 100.0f;
    }

    return score;
}

/* =====================================================================
 *  §13 — Monte Carlo Flip Simulation
 * ===================================================================== */

static void fisher_yates_shuffle(int *arr, int n)
{
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = arr[i]; arr[i] = arr[j]; arr[j] = tmp;
    }
}

static float mc_evaluate_flip(const GameState *gs, int flip_r, int flip_c, Side my_side)
{
    /* Collect facedown positions */
    int fd_positions[BOARD_SIZE];
    int fd_count = 0;
    for (int r = 0; r < BOARD_ROWS; r++)
        for (int c = 0; c < BOARD_COLS; c++)
            if (gs->cells[r][c].state == STATE_FACEDOWN)
                fd_positions[fd_count++] = r * BOARD_COLS + c;

    if (fd_count == 0) return 0;

    /* Build hidden piece pool */
    typedef struct { Side side; PieceRank rank; } PieceInfo;
    PieceInfo pool[BOARD_SIZE];
    int pool_size = 0;
    for (int i = 0; i < NUM_PIECE_TYPES; i++) {
        int side = (i < 7) ? SIDE_RED : SIDE_BLACK;
        int rank = (i % 7) + 1;
        for (int j = 0; j < g_cc.hidden[i]; j++) {
            if (pool_size < BOARD_SIZE) {
                pool[pool_size].side = (Side)side;
                pool[pool_size].rank = (PieceRank)rank;
                pool_size++;
            }
        }
    }

    if (pool_size == 0 || pool_size < fd_count) return 0;

    /* Dynamic sample count */
    int n_samples = 32;
    if (pool_size <= 4) n_samples = 128;
    else if (pool_size <= 8) n_samples = 64;
    else if (pool_size <= 16) n_samples = 48;

    /* Index array for shuffling */
    int indices[BOARD_SIZE];
    for (int i = 0; i < pool_size; i++) indices[i] = i;

    float total_score = 0;
    int flip_fd_idx = -1;
    for (int i = 0; i < fd_count; i++) {
        int r = fd_positions[i] / BOARD_COLS;
        int c = fd_positions[i] % BOARD_COLS;
        if (r + 1 == flip_r && c + 1 == flip_c) { flip_fd_idx = i; break; }
    }
    if (flip_fd_idx < 0) return 0;

    for (int s = 0; s < n_samples; s++) {
        fisher_yates_shuffle(indices, pool_size);

        GameState sim = *gs;
        /* Assign shuffled pieces to facedown positions */
        for (int i = 0; i < fd_count && i < pool_size; i++) {
            int r = fd_positions[i] / BOARD_COLS;
            int c = fd_positions[i] % BOARD_COLS;
            sim.cells[r][c].side = pool[indices[i]].side;
            sim.cells[r][c].rank = pool[indices[i]].rank;
        }

        /* Flip the target cell */
        sim.cells[flip_r - 1][flip_c - 1].state = STATE_FACEUP;
        sim.current_turn = opponent(gs->current_turn);

        float eval = ai_heuristic_eval(&sim);
        float adjusted = (my_side == SIDE_RED) ? eval : -eval;
        total_score += adjusted;
    }

    return total_score / n_samples;
}

/* =====================================================================
 *  §14 — Zobrist Hashing & Transposition Table
 * ===================================================================== */

static unsigned long long xorshift64(unsigned long long *state)
{
    unsigned long long x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static void zobrist_init(void)
{
    if (g_zobrist_initialized) return;
    unsigned long long state = 0x12345678ABCDEF01ULL;
    for (int i = 0; i < BOARD_SIZE; i++)
        for (int s = 0; s < 3; s++)
            for (int side = 0; side < 3; side++)
                for (int rank = 0; rank < 8; rank++)
                    g_zobrist[i][s][side][rank] = xorshift64(&state);
    g_zobrist_black_turn = xorshift64(&state);
    g_zobrist_initialized = 1;
}

static unsigned long long compute_hash(const GameState *gs)
{
    unsigned long long h = 0;
    for (int r = 0; r < BOARD_ROWS; r++)
        for (int c = 0; c < BOARD_COLS; c++) {
            int idx = r * BOARD_COLS + c;
            const Cell *cl = &gs->cells[r][c];
            h ^= g_zobrist[idx][cl->state][cl->side][cl->rank];
        }
    if (gs->current_turn == SIDE_BLACK)
        h ^= g_zobrist_black_turn;
    return h;
}

static void tt_clear(void)
{
    if (!g_tt) {
        g_tt = (TTEntry *)calloc(TT_SIZE, sizeof(TTEntry));
    }
    if (g_tt)
        memset(g_tt, 0, TT_SIZE * sizeof(TTEntry));
}

static void tt_store(unsigned long long key, int depth, float score, int flag, const Move *best)
{
    if (!g_tt) return;
    int idx = (int)(key & TT_MASK);
    TTEntry *e = &g_tt[idx];
    /* Replace if deeper or new */
    if (e->key == 0 || depth >= e->depth) {
        e->key = key;
        e->score = score;
        e->depth = depth;
        e->flag = flag;
        if (best) e->best_move = *best;
    }
}

static int tt_probe(unsigned long long key, int depth, float alpha, float beta, float *out_score)
{
    if (!g_tt) return 0;
    int idx = (int)(key & TT_MASK);
    TTEntry *e = &g_tt[idx];
    if (e->key != key || e->depth < depth) return 0;

    if (e->flag == TT_EXACT) { *out_score = e->score; return 1; }
    if (e->flag == TT_LOWER && e->score >= beta) { *out_score = e->score; return 1; }
    if (e->flag == TT_UPPER && e->score <= alpha) { *out_score = e->score; return 1; }
    return 0;
}

static int tt_probe_move(unsigned long long key, Move *out)
{
    if (!g_tt) return 0;
    int idx = (int)(key & TT_MASK);
    TTEntry *e = &g_tt[idx];
    if (e->key != key) return 0;
    *out = e->best_move;
    return 1;
}

/* =====================================================================
 *  §15 — Killer Moves & History Heuristic
 * ===================================================================== */

static void killer_clear(void)
{
    memset(g_killers, 0, sizeof(g_killers));
}

static void killer_store(const Move *mv, int ply)
{
    if (ply >= MAX_PLY) return;
    if (!moves_equal(mv, &g_killers[ply][0])) {
        g_killers[ply][1] = g_killers[ply][0];
        g_killers[ply][0] = *mv;
    }
}

static void history_clear(void)
{
    memset(g_history, 0, sizeof(g_history));
}

static void history_record(const Move *mv, int depth)
{
    int from = cell_idx(mv->from_r, mv->from_c);
    int to = cell_idx(mv->to_r, mv->to_c);
    g_history[from][to] += depth * depth;
}

static void history_age(void)
{
    for (int i = 0; i < BOARD_SIZE; i++)
        for (int j = 0; j < BOARD_SIZE; j++)
            g_history[i][j] >>= 1;
}

/* =====================================================================
 *  §16 — Move Ordering for Search
 * ===================================================================== */

typedef struct {
    Move move;
    float priority;
} ScoredMove;

static int scored_move_cmp(const void *a, const void *b)
{
    float pa = ((const ScoredMove *)a)->priority;
    float pb = ((const ScoredMove *)b)->priority;
    if (pb > pa) return 1;
    if (pb < pa) return -1;
    return 0;
}

static int order_moves_for_search(const GameState *gs, Move *moves, int n,
                                   ScoredMove *out, int ply, Side my_side)
{
    Move tt_move;
    unsigned long long hash = compute_hash(gs);
    int has_tt = tt_probe_move(hash, &tt_move);

    for (int i = 0; i < n; i++) {
        out[i].move = moves[i];
        float pri = 0;

        if (has_tt && moves_equal(&moves[i], &tt_move)) {
            pri = 1e8f;
        } else if (moves[i].type == MOVE_CAPTURE) {
            const Cell *victim = CELL(gs, moves[i].to_r, moves[i].to_c);
            const Cell *attacker = CELL(gs, moves[i].from_r, moves[i].from_c);
            pri = g_piece_val[victim->rank] * 10.0f - g_piece_val[attacker->rank];
            if (victim->rank == RANK_GENERAL) pri += 200.0f;
        } else if (ply < MAX_PLY && moves_equal(&moves[i], &g_killers[ply][0])) {
            pri = 50.0f;
        } else if (ply < MAX_PLY && moves_equal(&moves[i], &g_killers[ply][1])) {
            pri = 49.0f;
        } else if (moves[i].type == MOVE_WALK) {
            pri = ai_heuristic_score(gs, &moves[i], my_side);
            int from = cell_idx(moves[i].from_r, moves[i].from_c);
            int to = cell_idx(moves[i].to_r, moves[i].to_c);
            pri += g_history[from][to] * 0.01f;
        } else if (moves[i].type == MOVE_FLIP) {
            pri = -1e6f;
        }

        out[i].priority = pri;
    }

    qsort(out, n, sizeof(ScoredMove), scored_move_cmp);

    int limit = n < SEARCH_MAX_BRANCHING ? n : SEARCH_MAX_BRANCHING;
    return limit;
}

/* =====================================================================
 *  §17 — Quiescence Search
 * ===================================================================== */

static float quiescence(const GameState *gs, int qdepth, float alpha, float beta,
                         int maximizing, Side my_side)
{
    float standpat = ai_heuristic_eval(gs);
    if (my_side == SIDE_BLACK) standpat = -standpat;

    if (qdepth <= 0) return standpat;
    if (gs->game_over) return standpat;

    if (maximizing) {
        if (standpat >= beta) return standpat;
        if (standpat > alpha) alpha = standpat;
    } else {
        if (standpat <= alpha) return standpat;
        if (standpat < beta) beta = standpat;
    }

    /* Generate captures + evasions */
    Move all_moves[MAX_MOVES];
    int n_all = board_generate_moves(gs, all_moves);

    ScoredMove scored[MAX_MOVES];
    int n_q = 0;

    for (int i = 0; i < n_all && n_q < MAX_MOVES; i++) {
        if (all_moves[i].type == MOVE_CAPTURE) {
            const Cell *victim = CELL(gs, all_moves[i].to_r, all_moves[i].to_c);
            const Cell *attacker = CELL(gs, all_moves[i].from_r, all_moves[i].from_c);
            scored[n_q].move = all_moves[i];
            scored[n_q].priority = g_piece_val[victim->rank] * 10.0f -
                                    g_piece_val[attacker->rank];
            if (victim->rank == RANK_GENERAL) scored[n_q].priority += 200.0f;
            n_q++;
        }
    }

    /* Add evasions for high-value threatened pieces */
    Side cur = gs->current_turn;
    for (int i = 0; i < n_all && n_q < MAX_MOVES; i++) {
        if (all_moves[i].type != MOVE_WALK) continue;
        const Cell *mover = CELL(gs, all_moves[i].from_r, all_moves[i].from_c);
        if (mover->side != cur) continue;
        if (mover->rank < RANK_CANNON && mover->rank != RANK_ADVISOR) continue;
        if (is_threatened_at(gs, all_moves[i].from_r, all_moves[i].from_c,
                              mover->side, mover->rank) &&
            !is_threatened_at(gs, all_moves[i].to_r, all_moves[i].to_c,
                               mover->side, mover->rank)) {
            scored[n_q].move = all_moves[i];
            scored[n_q].priority = g_piece_val[mover->rank] * 0.5f;
            n_q++;
        }
    }

    qsort(scored, n_q, sizeof(ScoredMove), scored_move_cmp);
    int limit = n_q < QSEARCH_MAX_CAPTURES ? n_q : QSEARCH_MAX_CAPTURES;

    if (maximizing) {
        float best = standpat;
        for (int i = 0; i < limit; i++) {
            /* Delta pruning */
            if (scored[i].move.type == MOVE_CAPTURE) {
                const Cell *victim = CELL(gs, scored[i].move.to_r, scored[i].move.to_c);
                if (g_piece_val[victim->rank] + 3.0f < alpha &&
                    victim->rank != RANK_GENERAL)
                    continue;
            }

            GameState child = *gs;
            if (!board_apply_move(&child, &scored[i].move)) continue;

            float eval = quiescence(&child, qdepth - 1, alpha, beta, 0, my_side);
            if (eval > best) best = eval;
            if (best > alpha) alpha = best;
            if (alpha >= beta) break;
        }
        return best;
    } else {
        float best = standpat;
        for (int i = 0; i < limit; i++) {
            if (scored[i].move.type == MOVE_CAPTURE) {
                const Cell *victim = CELL(gs, scored[i].move.to_r, scored[i].move.to_c);
                if (g_piece_val[victim->rank] + 3.0f < -beta &&
                    victim->rank != RANK_GENERAL)
                    continue;
            }

            GameState child = *gs;
            if (!board_apply_move(&child, &scored[i].move)) continue;

            float eval = quiescence(&child, qdepth - 1, alpha, beta, 1, my_side);
            if (eval < best) best = eval;
            if (best < beta) beta = best;
            if (alpha >= beta) break;
        }
        return best;
    }
}

/* =====================================================================
 *  §18 — PVS Search Engine
 * ===================================================================== */

static float minimax_optimism(const GameState *gs)
{
    int fd = count_facedown(gs);
    return (fd / 32.0f) * 0.20f;
}

static float pvs(const GameState *gs, int depth, float alpha, float beta,
                  int maximizing, Side my_side, int ply)
{
    /* Time check */
    if ((ply & 0xF) == 0) {
        long long elapsed = get_time_ms() - g_search_start_time;
        if (elapsed > AI_SEARCH_TIME_MS) return ai_heuristic_eval(gs);
    }

    /* Terminal */
    if (gs->game_over) {
        float val = 0;
        if (gs->winner == SIDE_RED) val = 100.0f;
        else if (gs->winner == SIDE_BLACK) val = -100.0f;
        return (my_side == SIDE_RED) ? val : -val;
    }

    /* Leaf: quiescence */
    if (depth <= 0)
        return quiescence(gs, QSEARCH_MAX_DEPTH, alpha, beta, maximizing, my_side);

    /* TT probe */
    unsigned long long hash = compute_hash(gs);
    float tt_score;
    if (tt_probe(hash, depth, alpha, beta, &tt_score))
        return tt_score;

    /* Generate & order moves */
    Move raw_moves[MAX_MOVES];
    int n_raw = board_generate_moves(gs, raw_moves);
    if (n_raw == 0) {
        /* No moves = lose */
        return maximizing ? -90.0f : 90.0f;
    }

    ScoredMove ordered[MAX_MOVES];
    int n_ordered = order_moves_for_search(gs, raw_moves, n_raw, ordered, ply, my_side);

    Move best_move = ordered[0].move;

    if (maximizing) {
        float max_eval = -INF_SCORE;
        for (int i = 0; i < n_ordered; i++) {
            float eval;
            if (ordered[i].move.type == MOVE_FLIP) {
                /* Monte Carlo for flips in search */
                eval = mc_evaluate_flip(gs, ordered[i].move.from_r,
                                         ordered[i].move.from_c, my_side) + 0.3f;
            } else {
                GameState child = *gs;
                if (!board_apply_move(&child, &ordered[i].move)) continue;

                if (i == 0) {
                    eval = pvs(&child, depth - 1, alpha, beta, 0, my_side, ply + 1);
                } else {
                    /* Null window */
                    eval = pvs(&child, depth - 1, alpha, alpha + 0.01f, 0, my_side, ply + 1);
                    if (eval > alpha && eval < beta) {
                        eval = pvs(&child, depth - 1, alpha, beta, 0, my_side, ply + 1);
                    }
                }
            }

            if (eval > max_eval) {
                max_eval = eval;
                best_move = ordered[i].move;
            }
            if (max_eval > alpha) alpha = max_eval;
            if (alpha >= beta) {
                /* Beta cutoff */
                if (ordered[i].move.type != MOVE_CAPTURE) {
                    history_record(&ordered[i].move, depth);
                    killer_store(&ordered[i].move, ply);
                }
                break;
            }
        }

        int flag = (max_eval <= alpha) ? TT_UPPER : (max_eval >= beta) ? TT_LOWER : TT_EXACT;
        tt_store(hash, depth, max_eval, flag, &best_move);
        return max_eval;
    } else {
        float min_eval = INF_SCORE;
        for (int i = 0; i < n_ordered; i++) {
            float eval;
            if (ordered[i].move.type == MOVE_FLIP) {
                eval = mc_evaluate_flip(gs, ordered[i].move.from_r,
                                         ordered[i].move.from_c,
                                         opponent(my_side)) - 0.3f;
            } else {
                GameState child = *gs;
                if (!board_apply_move(&child, &ordered[i].move)) continue;

                if (i == 0) {
                    eval = pvs(&child, depth - 1, alpha, beta, 1, my_side, ply + 1);
                } else {
                    eval = pvs(&child, depth - 1, beta - 0.01f, beta, 1, my_side, ply + 1);
                    if (eval > alpha && eval < beta) {
                        eval = pvs(&child, depth - 1, alpha, beta, 1, my_side, ply + 1);
                    }
                }
            }

            if (eval < min_eval) {
                min_eval = eval;
                best_move = ordered[i].move;
            }
            if (min_eval < beta) beta = min_eval;
            if (alpha >= beta) {
                if (ordered[i].move.type != MOVE_CAPTURE) {
                    history_record(&ordered[i].move, depth);
                    killer_store(&ordered[i].move, ply);
                }
                break;
            }
        }

        /* Incomplete information optimism correction */
        float optimism = minimax_optimism(gs);
        if (optimism > 0.001f) {
            float standpat = ai_heuristic_eval(gs);
            float opp_standpat = (my_side == SIDE_RED) ? standpat : -standpat;
            min_eval = min_eval * (1.0f - optimism) + opp_standpat * optimism;
        }

        int flag = (min_eval <= alpha) ? TT_UPPER : (min_eval >= beta) ? TT_LOWER : TT_EXACT;
        tt_store(hash, depth, min_eval, flag, &best_move);
        return min_eval;
    }
}

/* =====================================================================
 *  §19 — Iterative Deepening with Aspiration Windows
 * ===================================================================== */

static int get_dynamic_depth(const GameState *gs)
{
    int tp = total_pieces_on_board(gs);
    if (tp > 14) return 4;
    if (tp >= 9) return 5;
    if (tp >= 7) return 6;
    if (tp >= 5) return 7;
    if (tp >= 4) return 8;
    return 10;
}

static float iterative_deepening(const GameState *gs, ScoredMove *candidates,
                                  int n_cand, Move *best_out, Side my_side)
{
    int max_depth = get_dynamic_depth(gs);
    float best_score = -INF_SCORE;
    *best_out = candidates[0].move;

    g_search_start_time = get_time_ms();

    for (int depth = 1; depth <= max_depth; depth++) {
        float asp_alpha, asp_beta;
        if (depth >= 2) {
            asp_alpha = best_score - 0.5f;
            asp_beta = best_score + 0.5f;
        } else {
            asp_alpha = -INF_SCORE;
            asp_beta = INF_SCORE;
        }

        for (int attempt = 0; attempt < 3; attempt++) {
            float iter_best = -INF_SCORE;
            Move iter_best_move = candidates[0].move;

            for (int i = 0; i < n_cand; i++) {
                float score;
                if (candidates[i].move.type == MOVE_FLIP) {
                    score = candidates[i].priority; /* Use heuristic score for flips */
                } else {
                    GameState child = *gs;
                    if (!board_apply_move(&child, &candidates[i].move)) continue;
                    score = pvs(&child, depth - 1, asp_alpha, asp_beta, 0, my_side, 0);
                }

                if (score > iter_best) {
                    iter_best = score;
                    iter_best_move = candidates[i].move;
                }
            }

            if (iter_best <= asp_alpha) {
                asp_alpha = -INF_SCORE;
            } else if (iter_best >= asp_beta) {
                asp_beta = INF_SCORE;
            } else {
                best_score = iter_best;
                *best_out = iter_best_move;
                break;
            }
        }

        history_age();

        /* Time check */
        long long elapsed = get_time_ms() - g_search_start_time;
        if (elapsed > AI_SEARCH_TIME_MS) break;
    }

    return best_score;
}

/* =====================================================================
 *  §20 — Move Selection Main Flow
 * ===================================================================== */

int strategy_select_move(const GameState *gs, Move *out_move)
{
    g_my_side = gs->current_turn;
    Side my = g_my_side;
    Side enemy = opponent(my);

    /* Record opponent's last move */
    if (gs->has_last_move)
        record_move(&gs->last_move, enemy);

    /* Initialize analysis tools */
    detect_captures(gs);
    refresh_piece_values(gs);
    card_counter_update(gs, my);
    assess_situation(gs, my);
    zobrist_init();

    /* Generate legal moves */
    Move legal[MAX_MOVES];
    int n = board_generate_moves(gs, legal);
    if (n == 0) return 0;
    if (n == 1) {
        *out_move = legal[0];
        chase_record(out_move);
        record_move(out_move, my);
        return 1;
    }

    /* === P0: My general is threatened === */
    if (g_sa.my_general_threatened) {
        /* Priority 1: Ally directly captures attacker */
        float best_emergency = -INF_SCORE;
        int best_idx = -1;

        for (int i = 0; i < n; i++) {
            if (legal[i].type != MOVE_CAPTURE) continue;
            const Cell *mover_cell = CELL(gs, legal[i].from_r, legal[i].from_c);
            if (mover_cell->rank == RANK_GENERAL) continue; /* prefer non-general capture */
            for (int t = 0; t < g_sa.threat_count; t++) {
                if (g_sa.threatened[t].rank != RANK_GENERAL) continue;
                for (int a = 0; a < g_sa.threatened[t].attacker_count; a++) {
                    if (legal[i].to_r == g_sa.threatened[t].attackers[a].r &&
                        legal[i].to_c == g_sa.threatened[t].attackers[a].c) {
                        float s = 60.0f;
                        if (s > best_emergency) {
                            best_emergency = s;
                            best_idx = i;
                        }
                    }
                }
            }
        }

        if (best_idx >= 0) {
            *out_move = legal[best_idx];
            chase_record(out_move);
            record_move(out_move, my);
            return 1;
        }

        /* Priority 2: Ally approaches attacker + General escape + Block cannon */
        best_emergency = -INF_SCORE;
        best_idx = -1;

        for (int i = 0; i < n; i++) {
            float s = 0;

            /* General escape */
            s += score_general_safety(gs, &legal[i], my);

            /* Ally approach attacker */
            if (legal[i].type == MOVE_WALK) {
                const Cell *m = CELL(gs, legal[i].from_r, legal[i].from_c);
                if (m->rank != RANK_GENERAL) {
                    for (int t = 0; t < g_sa.threat_count; t++) {
                        if (g_sa.threatened[t].rank != RANK_GENERAL) continue;
                        for (int a = 0; a < g_sa.threatened[t].attacker_count; a++) {
                            int ar = g_sa.threatened[t].attackers[a].r;
                            int ac = g_sa.threatened[t].attackers[a].c;
                            int dist_to = manhattan(legal[i].to_r, legal[i].to_c, ar, ac);
                            int dist_from = manhattan(legal[i].from_r, legal[i].from_c, ar, ac);
                            if (dist_to < dist_from) {
                                float approach = 45.0f;
                                if (dist_to == 1) approach += 10.0f;
                                if (approach > s) s = approach;
                            }
                        }
                    }
                }
            }

            if (s > best_emergency) {
                best_emergency = s;
                best_idx = i;
            }
        }

        if (best_idx >= 0 && best_emergency >= 30.0f) {
            *out_move = legal[best_idx];
            chase_record(out_move);
            record_move(out_move, my);
            return 1;
        }
        /* Fall through to normal scoring */
    }

    /* === P1: Can kill enemy general === */
    if (g_sa.can_kill_enemy_general) {
        *out_move = g_sa.kill_general_move;
        chase_record(out_move);
        record_move(out_move, my);
        return 1;
    }

    /* === Epsilon exploration (3% for Hard) === */
    float exploration_rate = 0.03f;
    float r_val = (float)rand() / (float)RAND_MAX;
    if (r_val < exploration_rate) {
        float r2 = (float)rand() / (float)RAND_MAX;
        if (r2 < 0.8f) {
            /* Prefer captures */
            Move caps[MAX_MOVES];
            int nc = 0;
            for (int i = 0; i < n; i++)
                if (legal[i].type == MOVE_CAPTURE) caps[nc++] = legal[i];
            if (nc > 0) {
                *out_move = caps[rand() % nc];
                chase_record(out_move);
                record_move(out_move, my);
                return 1;
            }
        }
        *out_move = legal[rand() % n];
        chase_record(out_move);
        record_move(out_move, my);
        return 1;
    }

    /* === Heuristic scoring for all moves === */
    float scores[MAX_MOVES];
    float noise_weight = 0.05f;

    for (int i = 0; i < n; i++) {
        float raw_score = ai_heuristic_score(gs, &legal[i], my);
        float noise = ((float)rand() / (float)RAND_MAX - 0.5f) * 4.0f;
        scores[i] = raw_score * (1.0f - noise_weight) + noise * noise_weight;
    }

    /* === Forced action near draw === */
    if (gs->no_capture_turns >= 35) {
        /* Find best capture */
        float best_cap = -INF_SCORE;
        int best_cap_idx = -1;
        for (int i = 0; i < n; i++) {
            if (legal[i].type == MOVE_CAPTURE && scores[i] > best_cap) {
                best_cap = scores[i];
                best_cap_idx = i;
            }
        }
        if (best_cap_idx >= 0) {
            *out_move = legal[best_cap_idx];
            chase_record(out_move);
            record_move(out_move, my);
            return 1;
        }
        /* Force flip if available */
        float best_flip = -INF_SCORE;
        int best_flip_idx = -1;
        for (int i = 0; i < n; i++) {
            if (legal[i].type == MOVE_FLIP && scores[i] > best_flip) {
                best_flip = scores[i];
                best_flip_idx = i;
            }
        }
        if (best_flip_idx >= 0) {
            *out_move = legal[best_flip_idx];
            chase_record(out_move);
            record_move(out_move, my);
            return 1;
        }
    }

    /* === Find heuristic best === */
    float heuristic_best_score = -INF_SCORE;
    int heuristic_best_idx = 0;
    for (int i = 0; i < n; i++) {
        if (scores[i] > heuristic_best_score) {
            heuristic_best_score = scores[i];
            heuristic_best_idx = i;
        }
    }

    /* === Monte Carlo blending for flip moves === */
    for (int i = 0; i < n; i++) {
        if (legal[i].type == MOVE_FLIP) {
            float mc_score = mc_evaluate_flip(gs, legal[i].from_r, legal[i].from_c, my);
            float heuristic_score = scores[i];
            scores[i] = 0.7f * heuristic_score + 0.3f * mc_score;
        }
    }

    /* Recalculate best after MC blending */
    heuristic_best_score = -INF_SCORE;
    heuristic_best_idx = 0;
    for (int i = 0; i < n; i++) {
        if (scores[i] > heuristic_best_score) {
            heuristic_best_score = scores[i];
            heuristic_best_idx = i;
        }
    }

    /* === PVS Search (Hard mode) === */
    /* Select top N candidates */
    ScoredMove candidates[MAX_MOVES];
    for (int i = 0; i < n; i++) {
        candidates[i].move = legal[i];
        candidates[i].priority = scores[i];
    }
    qsort(candidates, n, sizeof(ScoredMove), scored_move_cmp);

    int n_cand = n < N_CANDIDATES ? n : N_CANDIDATES;

    tt_clear();
    history_clear();
    killer_clear();

    Move search_best;
    float search_score = iterative_deepening(gs, candidates, n_cand, &search_best, my);

    /* === Search result verification === */
    Move final_move = legal[heuristic_best_idx];

    if (!moves_equal(&search_best, &legal[heuristic_best_idx])) {
        /* Find search_best's heuristic score */
        float search_heuristic = -INF_SCORE;
        for (int i = 0; i < n; i++) {
            if (moves_equal(&legal[i], &search_best)) {
                search_heuristic = scores[i];
                break;
            }
        }

        float diff = search_score - heuristic_best_score;
        if (diff > 0.5f) {
            final_move = search_best; /* Search found better move — trust it */
        } else {
            /* Check if heuristic best is bad in search */
            float heur_in_search = -INF_SCORE;
            for (int i = 0; i < n_cand; i++) {
                if (moves_equal(&candidates[i].move, &legal[heuristic_best_idx])) {
                    GameState child = *gs;
                    if (board_apply_move(&child, &candidates[i].move)) {
                        heur_in_search = board_evaluate(&child);
                        if (my == SIDE_BLACK) heur_in_search = -heur_in_search;
                    }
                    break;
                }
            }
            if (heur_in_search < -10.0f)
                final_move = search_best; /* Heuristic best is catastrophic in search */
        }
        (void)search_heuristic;
    }

    *out_move = final_move;
    chase_record(out_move);
    record_move(out_move, my);
    return 1;
}

/* =====================================================================
 *  §21 — Reset
 * ===================================================================== */

int strategy_is_draw_mode(void)
{
    return g_sa.draw_mode;
}

void strategy_reset(void)
{
    memset(g_cap_hist, 0, sizeof(g_cap_hist));
    g_shadow_valid = 0;
    memset(g_move_history, 0, sizeof(g_move_history));
    g_move_history_count = 0;
    memset(g_chase_history, 0, sizeof(g_chase_history));
    g_chase_count = 0;

    if (g_tt) {
        free(g_tt);
        g_tt = NULL;
    }

    history_clear();
    killer_clear();
}
