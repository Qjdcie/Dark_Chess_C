/*
 * cli_battle.c — Headless online battle mode for Dark Chess (暗棋)
 *
 * Used when the binary is launched with both -ngui and -online_battle flags.
 * Connects to the battle server, plays as AI, and logs moves to a file —
 * all without opening any GUI window. Requires no Raylib dependency.
 */

#include "cli_battle.h"
#include "board.h"
#include "strategy.h"
#include "net_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>  /* Sleep */
#include <direct.h>   /* _mkdir */
#else
#include <unistd.h>   /* usleep */
#endif

/* =====================================================================
 *  Constants
 * ===================================================================== */

#define ROOM_ID_MAX  15
#define POLL_US      50000   /* 50 ms polling interval */
#define AI_DELAY_S   1       /* seconds before AI commits a move */

/* =====================================================================
 *  Battle Logger (mirrors raylib_gui.c blog_* functions)
 * ===================================================================== */

#define BLOG_CAP_INIT 8192

typedef struct {
    char *buf;
    int   len;
    int   cap;
    int   active;
    int   saved;
    char  room_id[ROOM_ID_MAX + 1];
    char  my_role[4];
    char  my_color[10];
    int   move_num;
} BattleLog;

static BattleLog g_blog = { 0 };

static void blog_reset(void)
{
    free(g_blog.buf);
    memset(&g_blog, 0, sizeof(g_blog));
}

static void blog_append(const char *fmt, ...)
{
    if (!g_blog.active) return;
    if (!g_blog.buf) {
        g_blog.buf = (char *)malloc(BLOG_CAP_INIT);
        if (!g_blog.buf) return;
        g_blog.cap = BLOG_CAP_INIT;
        g_blog.len = 0;
        g_blog.buf[0] = '\0';
    }

    va_list ap;
    va_start(ap, fmt);
    int avail = g_blog.cap - g_blog.len;
    int needed = vsnprintf(g_blog.buf + g_blog.len, avail, fmt, ap);
    va_end(ap);

    if (needed >= avail) {
        int new_cap = g_blog.cap;
        while (new_cap - g_blog.len <= needed) new_cap *= 2;
        char *tmp = (char *)realloc(g_blog.buf, new_cap);
        if (!tmp) return;
        g_blog.buf = tmp;
        g_blog.cap = new_cap;

        va_list ap2;
        va_start(ap2, fmt);
        vsnprintf(g_blog.buf + g_blog.len, g_blog.cap - g_blog.len, fmt, ap2);
        va_end(ap2);
    }
    g_blog.len += needed;
}

static const char *piece_name_for_log(Side side, PieceRank rank)
{
    if (side == SIDE_RED) {
        switch (rank) {
        case RANK_GENERAL:  return "R_King";
        case RANK_ADVISOR:  return "R_Guard";
        case RANK_ELEPHANT: return "R_Elephant";
        case RANK_CHARIOT:  return "R_Car";
        case RANK_HORSE:    return "R_Horse";
        case RANK_CANNON:   return "R_Cannon";
        case RANK_SOLDIER:  return "R_Soldier";
        default: break;
        }
    } else if (side == SIDE_BLACK) {
        switch (rank) {
        case RANK_GENERAL:  return "B_King";
        case RANK_ADVISOR:  return "B_Guard";
        case RANK_ELEPHANT: return "B_Elephant";
        case RANK_CHARIOT:  return "B_Car";
        case RANK_HORSE:    return "B_Horse";
        case RANK_CANNON:   return "B_Cannon";
        case RANK_SOLDIER:  return "B_Soldier";
        default: break;
        }
    }
    return "?";
}

static void blog_start(const char *room_id, const char *role)
{
    blog_reset();
    g_blog.active = 1;
    g_blog.saved = 0;
    g_blog.move_num = 0;
    snprintf(g_blog.room_id, sizeof(g_blog.room_id), "%s", room_id);
    snprintf(g_blog.my_role, sizeof(g_blog.my_role), "%s", role);
    g_blog.my_color[0] = '\0';

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);

    blog_append("[%s] room=%s role=%s\n", ts, room_id, role);
}

static void blog_move_received(const GameState *gs, const NetUpdateInfo *info,
                                int old_total, const Cell prev[][BOARD_COLS])
{
    if (!g_blog.active) return;

    if (g_blog.my_color[0] == '\0') {
        const char *c = (strcmp(g_blog.my_role, "A") == 0)
            ? info->color_a : info->color_b;
        if (strcmp(c, "None") != 0) {
            strncpy(g_blog.my_color, c, 9);
            g_blog.my_color[9] = '\0';
            blog_append("color=%s\n", g_blog.my_color);
        }
    }

    if (info->total_moves == old_total) return;
    g_blog.move_num++;

    int from_r = -1, from_c = -1, to_r = -1, to_c = -1;
    for (int r = 0; r < BOARD_ROWS; r++)
        for (int c = 0; c < BOARD_COLS; c++) {
            if (prev[r][c].state != STATE_EMPTY &&
                gs->cells[r][c].state == STATE_EMPTY) {
                from_r = r; from_c = c;
            }
            if (memcmp(&prev[r][c], &gs->cells[r][c], sizeof(Cell)) != 0 &&
                gs->cells[r][c].state != STATE_EMPTY) {
                to_r = r; to_c = c;
            }
        }

    const char *who = (strcmp(info->current_turn_role, g_blog.my_role) != 0)
                      ? "ME  " : "OPP ";

    if (from_r < 0 && to_r >= 0) {
        const Cell *cl = &gs->cells[to_r][to_c];
        blog_append("#%-3d %s FLIP (%d,%d)->%s  R%dB%d\n",
                     g_blog.move_num, who, to_r, to_c,
                     piece_name_for_log(cl->side, cl->rank),
                     gs->red_alive, gs->black_alive);
    } else if (from_r >= 0 && to_r >= 0) {
        const Cell *dest = &gs->cells[to_r][to_c];
        int captured = (prev[to_r][to_c].state == STATE_FACEUP &&
                        prev[to_r][to_c].side != SIDE_NONE);
        if (captured) {
            blog_append("#%-3d %s CAPTURE (%d,%d)->(%d,%d) %s x %s  R%dB%d\n",
                         g_blog.move_num, who, from_r, from_c, to_r, to_c,
                         piece_name_for_log(dest->side, dest->rank),
                         piece_name_for_log(prev[to_r][to_c].side,
                                             prev[to_r][to_c].rank),
                         gs->red_alive, gs->black_alive);
        } else {
            blog_append("#%-3d %s MOVE (%d,%d)->(%d,%d) %s  R%dB%d\n",
                         g_blog.move_num, who, from_r, from_c, to_r, to_c,
                         piece_name_for_log(dest->side, dest->rank),
                         gs->red_alive, gs->black_alive);
        }
    }
}

static void blog_game_over(const GameState *gs)
{
    if (!g_blog.active || g_blog.saved) return;

    const char *result = "DRAW";
    if (g_blog.my_color[0] != '\0') {
        int i_am_red = (strcmp(g_blog.my_color, "Red") == 0);
        if (gs->winner == SIDE_NONE)
            result = "DRAW";
        else if ((gs->winner == SIDE_RED && i_am_red) ||
                 (gs->winner == SIDE_BLACK && !i_am_red))
            result = "WIN";
        else
            result = "LOSE";
    }
    blog_append("RESULT=%s moves=%d R%dB%d\n",
                result, g_blog.move_num, gs->red_alive, gs->black_alive);

#ifdef _WIN32
    _mkdir("logs");
#else
    mkdir("logs", 0755);
#endif

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char filename[256];
    snprintf(filename, sizeof(filename),
             "logs/battle_%04d%02d%02d_%02d%02d%02d_%s.log",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec,
             g_blog.room_id);

    FILE *fp = fopen(filename, "w");
    if (fp) {
        fwrite(g_blog.buf, 1, g_blog.len, fp);
        fclose(fp);
        printf("[BattleLog] Saved to %s\n", filename);
    } else {
        printf("[BattleLog] Failed to write %s\n", filename);
    }

    g_blog.saved = 1;
}

/* =====================================================================
 *  Main CLI battle loop
 * ===================================================================== */

int cli_battle_main(const char *room_id)
{
    /* Connect */
    if (net_connect(NET_SERVER_IP, NET_SERVER_PORT) != 0) {
        fprintf(stderr, "[CLI] Failed to connect to %s:%d\n",
                NET_SERVER_IP, NET_SERVER_PORT);
        return 1;
    }

    /* Join room */
    char role_raw[16] = "";
    if (net_join_room(room_id, role_raw, sizeof(role_raw)) != 0) {
        fprintf(stderr, "[CLI] Failed to join room %s\n", room_id);
        net_close();
        return 1;
    }

    /* Map "first"/"second" → "A"/"B" (matches UPDATE current_turn_role values) */
    char my_role_ab[4] = "";
    if (strcmp(role_raw, "first") == 0)
        strcpy(my_role_ab, "A");
    else
        strcpy(my_role_ab, "B");

    printf("[CLI] Joined room %s as role %s (%s)\n", room_id, my_role_ab, role_raw);

    /* Game state */
    GameState gs;
    board_init(&gs);
    strategy_reset();

    NetUpdateInfo net_info;
    memset(&net_info, 0, sizeof(net_info));

    Cell prev_cells[BOARD_ROWS][BOARD_COLS];
    memset(prev_cells, 0, sizeof(prev_cells));

    int sides_determined  = 0;
    Side player_side      = SIDE_NONE;
    int waiting_for_update = 0;
    time_t move_ready_at  = 0;   /* 0 = no pending move timer */
    int move_timer_active = 0;

    blog_start(room_id, my_role_ab);

    /* Main poll loop */
    while (1) {
        /* Receive updates */
        if (!gs.game_over) {
            char recv_buf[8192];
            int recv_result;
            while ((recv_result = net_try_receive(recv_buf, sizeof(recv_buf))) > 0) {
                memcpy(prev_cells, gs.cells, sizeof(prev_cells));
                int old_total = net_info.total_moves;

                if (net_parse_update(recv_buf, &gs, &net_info)) {
                    waiting_for_update = 0;

                    /* Determine our side from color_table */
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

                    /* Detect move for no_capture_turns / last_move */
                    if (net_info.total_moves != old_total && old_total >= 0) {
                        int fr = 0, fc = 0, tr = 0, tc = 0;
                        for (int r = 0; r < BOARD_ROWS; r++) {
                            for (int c = 0; c < BOARD_COLS; c++) {
                                if (prev_cells[r][c].state != STATE_EMPTY
                                    && gs.cells[r][c].state == STATE_EMPTY) {
                                    fr = r + 1; fc = c + 1;
                                }
                                if (memcmp(&prev_cells[r][c], &gs.cells[r][c],
                                           sizeof(Cell)) != 0
                                    && gs.cells[r][c].state != STATE_EMPTY) {
                                    tr = r + 1; tc = c + 1;
                                }
                            }
                        }
                        if (fr == 0 && tr > 0) { fr = tr; fc = tc; }

                        if (tr > 0) {
                            int is_cap = (fr != tr || fc != tc) &&
                                prev_cells[tr-1][tc-1].state == STATE_FACEUP &&
                                prev_cells[tr-1][tc-1].side != SIDE_NONE;
                            gs.no_capture_turns = is_cap ? 0 : gs.no_capture_turns + 1;
                            if (fr == tr && fc == tc) {
                                gs.last_move.type   = MOVE_FLIP;
                                gs.last_move.from_r = tr; gs.last_move.from_c = tc;
                                gs.last_move.to_r   = tr; gs.last_move.to_c   = tc;
                            } else {
                                gs.last_move.type   = is_cap ? MOVE_CAPTURE : MOVE_WALK;
                                gs.last_move.from_r = fr; gs.last_move.from_c = fc;
                                gs.last_move.to_r   = tr; gs.last_move.to_c   = tc;
                            }
                            gs.has_last_move = 1;
                        }

                        /* Arm move timer: send move after AI_DELAY_S seconds */
                        move_ready_at = time(NULL) + AI_DELAY_S;
                        move_timer_active = 1;
                    }

                    blog_move_received(&gs, &net_info, old_total, prev_cells);

                    /* Check for game over */
                    if (strcmp(net_info.state, "playing") != 0 &&
                        strcmp(net_info.state, "waiting") != 0 &&
                        net_info.state[0] != '\0') {
                        gs.game_over = 1;
                        if (strcmp(net_info.winner_role, "A") == 0) {
                            gs.winner = (strcmp(net_info.color_a, "Red") == 0)
                                        ? SIDE_RED : SIDE_BLACK;
                        } else if (strcmp(net_info.winner_role, "B") == 0) {
                            gs.winner = (strcmp(net_info.color_b, "Red") == 0)
                                        ? SIDE_RED : SIDE_BLACK;
                        } else {
                            if (gs.red_alive > 0 && gs.black_alive == 0)
                                gs.winner = SIDE_RED;
                            else if (gs.black_alive > 0 && gs.red_alive == 0)
                                gs.winner = SIDE_BLACK;
                            else
                                gs.winner = SIDE_NONE;
                        }
                        blog_game_over(&gs);
                    }
                }
            }

            if (recv_result < 0) {
                fprintf(stderr, "[CLI] Connection lost\n");
                gs.game_over = 1;
                gs.winner = SIDE_NONE;
                blog_game_over(&gs);
            }
        }

        if (gs.game_over) break;

        /* Check connection */
        if (!net_is_connected()) {
            fprintf(stderr, "[CLI] Disconnected\n");
            gs.game_over = 1;
            gs.winner = SIDE_NONE;
            blog_game_over(&gs);
            break;
        }

        /* AI move: fire when timer expires and it's our turn */
        int is_my_turn =
            strcmp(net_info.state, "playing") == 0 &&
            strcmp(net_info.current_turn_role, my_role_ab) == 0 &&
            !waiting_for_update;

        if (is_my_turn && move_timer_active && time(NULL) >= move_ready_at) {
            move_timer_active = 0;

            gs.current_turn = sides_determined ? player_side : SIDE_RED;

            Move legal[MAX_MOVES];
            int n_legal = board_generate_moves(&gs, legal);
            if (n_legal > 0) {
                Move chosen;
                if (!strategy_select_move(&gs, &chosen))
                    chosen = legal[rand() % n_legal];

                char action[64];
                if (chosen.type == MOVE_FLIP) {
                    snprintf(action, sizeof(action), "%d %d\n",
                             chosen.from_r - 1, chosen.from_c - 1);
                } else {
                    snprintf(action, sizeof(action), "%d %d %d %d\n",
                             chosen.from_r - 1, chosen.from_c - 1,
                             chosen.to_r - 1, chosen.to_c - 1);
                }
                net_send_action(action);
                waiting_for_update = 1;
            }
        }

        /* First update: arm timer immediately so we can act on turn 1 */
        if (is_my_turn && !move_timer_active && !waiting_for_update) {
            move_ready_at = time(NULL) + AI_DELAY_S;
            move_timer_active = 1;
        }

#ifdef _WIN32
        Sleep(POLL_US / 1000);
#else
        usleep(POLL_US);
#endif
    }

    const char *winner_str =
        (gs.winner == SIDE_RED)   ? "Red" :
        (gs.winner == SIDE_BLACK) ? "Black" : "Draw";
    printf("[CLI] Game over. Winner: %s\n", winner_str);

    net_close();
    blog_reset();
    return 0;
}
