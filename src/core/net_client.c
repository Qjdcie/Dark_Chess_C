/*
 * net_client.c — Cross-platform TCP client for Dark Chess online battle
 *
 * Connects to the battle server, sends/receives game data,
 * and parses UPDATE JSON into GameState.
 */

#include "net_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* =====================================================================
 *  Platform-specific socket abstractions
 * ===================================================================== */

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET socket_t;
  #define INVALID_SOCK  INVALID_SOCKET
  #define CLOSE_SOCKET  closesocket
  static int g_wsa_inited = 0;
#else
  #include <sys/socket.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/select.h>
  typedef int socket_t;
  #define INVALID_SOCK  (-1)
  #define CLOSE_SOCKET  close
#endif

/* =====================================================================
 *  Static state
 * ===================================================================== */

static socket_t g_socket = INVALID_SOCK;

/* Accumulation buffer for partial recv data */
#define RECV_BUF_SIZE 16384
static char g_recv_accum[RECV_BUF_SIZE];
static int  g_recv_len = 0;

/* =====================================================================
 *  Piece name → Side/Rank mapping
 * ===================================================================== */

typedef struct {
    const char *name;
    Side        side;
    PieceRank   rank;
} PieceMapping;

static const PieceMapping piece_map[] = {
    { "Red_King",       SIDE_RED,   RANK_GENERAL  },
    { "Red_Guard",      SIDE_RED,   RANK_ADVISOR  },
    { "Red_Elephant",   SIDE_RED,   RANK_ELEPHANT },
    { "Red_Car",        SIDE_RED,   RANK_CHARIOT  },
    { "Red_Horse",      SIDE_RED,   RANK_HORSE    },
    { "Red_Cannon",     SIDE_RED,   RANK_CANNON   },
    { "Red_Soldier",    SIDE_RED,   RANK_SOLDIER  },
    { "Black_King",     SIDE_BLACK, RANK_GENERAL  },
    { "Black_Guard",    SIDE_BLACK, RANK_ADVISOR  },
    { "Black_Elephant", SIDE_BLACK, RANK_ELEPHANT },
    { "Black_Car",      SIDE_BLACK, RANK_CHARIOT  },
    { "Black_Horse",    SIDE_BLACK, RANK_HORSE    },
    { "Black_Cannon",   SIDE_BLACK, RANK_CANNON   },
    { "Black_Soldier",  SIDE_BLACK, RANK_SOLDIER  },
    { NULL, 0, 0 }
};

static void lookup_piece(const char *name, Side *side, PieceRank *rank)
{
    for (int i = 0; piece_map[i].name; i++) {
        if (strcmp(name, piece_map[i].name) == 0) {
            *side = piece_map[i].side;
            *rank = piece_map[i].rank;
            return;
        }
    }
    *side = SIDE_NONE;
    *rank = RANK_NONE;
}

/* =====================================================================
 *  JSON parsing helpers (minimal, no external library)
 * ===================================================================== */

/*
 * Extract a JSON string value for a given key.
 * Searches for "key": "value" and copies value into out.
 * Returns 1 on success, 0 if not found.
 */
static int json_get_string(const char *json, const char *key,
                           char *out, int out_len)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);

    /* Skip whitespace and colon */
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n') p++;
    if (*p != '"') return 0;
    p++; /* skip opening quote */

    const char *end = strchr(p, '"');
    if (!end) return 0;

    int len = (int)(end - p);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 1;
}

/*
 * Extract a JSON integer value for a given key.
 * Searches for "key": number.
 * Returns 1 on success, 0 if not found.
 */
static int json_get_int(const char *json, const char *key, int *out)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);

    /* Skip whitespace and colon */
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n') p++;

    *out = atoi(p);
    return 1;
}

/*
 * Extract a color from the color_table for a role.
 * color_table format: {"A": "Red", "B": "Black"} or {"B": "Black", "A": "Red"}
 */
static void json_get_role_color(const char *json, const char *role,
                                char *out, int out_len)
{
    /* Find "color_table" first */
    const char *ct = strstr(json, "\"color_table\"");
    if (!ct) { strncpy(out, "None", out_len); return; }

    /* Search for the role key within color_table section */
    char pattern[16];
    snprintf(pattern, sizeof(pattern), "\"%s\"", role);

    const char *p = strstr(ct, pattern);
    if (!p) { strncpy(out, "None", out_len); return; }

    p += strlen(pattern);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n') p++;

    if (*p != '"') { strncpy(out, "None", out_len); return; }
    p++;

    const char *end = strchr(p, '"');
    if (!end) { strncpy(out, "None", out_len); return; }

    int len = (int)(end - p);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, p, len);
    out[len] = '\0';
}

/*
 * Parse the board 2D array from the UPDATE JSON.
 * Format: "board": [["piece","piece",...], [...], [...], [...]]
 */
static void parse_board(const char *json, GameState *gs)
{
    const char *brd = strstr(json, "\"board\"");
    if (!brd) return;

    /* Find outer '[' */
    const char *p = strchr(brd, '[');
    if (!p) return;
    p++; /* skip outer '[' */

    int red_alive = 0, black_alive = 0;

    for (int row = 0; row < BOARD_ROWS; row++) {
        p = strchr(p, '[');
        if (!p) return;
        p++; /* skip row '[' */

        for (int col = 0; col < BOARD_COLS; col++) {
            p = strchr(p, '"');
            if (!p) return;
            p++; /* skip opening '"' */

            const char *end = strchr(p, '"');
            if (!end) return;

            int len = (int)(end - p);
            char piece_name[32];
            if (len > 31) len = 31;
            memcpy(piece_name, p, len);
            piece_name[len] = '\0';

            Cell *cell = &gs->cells[row][col];

            if (strcmp(piece_name, "Null") == 0) {
                cell->state = STATE_EMPTY;
                cell->side  = SIDE_NONE;
                cell->rank  = RANK_NONE;
            } else if (strcmp(piece_name, "Covered") == 0) {
                cell->state = STATE_FACEDOWN;
                cell->side  = SIDE_NONE;
                cell->rank  = RANK_NONE;
            } else {
                cell->state = STATE_FACEUP;
                lookup_piece(piece_name, &cell->side, &cell->rank);
                if (cell->side == SIDE_RED)   red_alive++;
                if (cell->side == SIDE_BLACK) black_alive++;
            }

            p = end + 1; /* past closing '"' */
        }
    }

    gs->red_alive   = red_alive;
    gs->black_alive = black_alive;
}

/* =====================================================================
 *  Public API
 * ===================================================================== */

int net_is_connected(void)
{
    return g_socket != INVALID_SOCK;
}

int net_connect(const char *ip, int port)
{
#ifdef _WIN32
    if (!g_wsa_inited) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
        g_wsa_inited = 1;
    }
#endif

    g_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (g_socket == INVALID_SOCK) return -1;

    /* Set receive timeout to 5 seconds for the join phase */
    struct timeval tv;
    tv.tv_sec  = 5;
    tv.tv_usec = 0;
    setsockopt(g_socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));

    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_family      = AF_INET;
    server.sin_port        = htons((unsigned short)port);
    server.sin_addr.s_addr = inet_addr(ip);

    if (connect(g_socket, (struct sockaddr *)&server, sizeof(server)) < 0) {
        fprintf(stderr, "[NET] connect failed: %s\n", strerror(errno));
        CLOSE_SOCKET(g_socket);
        g_socket = INVALID_SOCK;
        return -1;
    }

    /* Reset receive buffer */
    g_recv_len = 0;
    g_recv_accum[0] = '\0';

    fprintf(stderr, "[NET] Connected to %s:%d\n", ip, port);
    return 0;
}

int net_join_room(const char *room_id, char *role_out, int role_len)
{
    if (g_socket == INVALID_SOCK) return -1;

    /* Send "JOIN <room_id>\n" */
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "JOIN %s\n", room_id);
    send(g_socket, cmd, (int)strlen(cmd), 0);

    /* Receive response (blocking with timeout) */
    char response[2048];
    int n = recv(g_socket, response, sizeof(response) - 1, 0);
    if (n <= 0) {
        fprintf(stderr, "[NET] No response from server\n");
        return -1;
    }
    response[n] = '\0';
    fprintf(stderr, "[NET] Server: %s\n", response);

    if (!strstr(response, "SUCCESS")) {
        fprintf(stderr, "[NET] Join failed\n");
        return -1;
    }

    /* Extract role: look for "ROLE first" or "ROLE second" */
    const char *role_ptr = strstr(response, "ROLE ");
    if (!role_ptr) return -1;

    role_ptr += 5;
    /* Copy the role word */
    int i = 0;
    while (role_ptr[i] && role_ptr[i] != ' ' && role_ptr[i] != '\n'
           && role_ptr[i] != '\r' && i < role_len - 1) {
        role_out[i] = role_ptr[i];
        i++;
    }
    role_out[i] = '\0';

    fprintf(stderr, "[NET] Assigned role: %s\n", role_out);

    /* Remove the recv timeout for game loop (will use select for non-blocking) */
    struct timeval tv;
    tv.tv_sec  = 0;
    tv.tv_usec = 0;
    setsockopt(g_socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));

    return 0;
}

void net_send_action(const char *action)
{
    if (g_socket == INVALID_SOCK) return;
    send(g_socket, action, (int)strlen(action), 0);
    fprintf(stderr, "[NET] Sent: %s", action);
}

int net_try_receive(char *buffer, int len)
{
    if (g_socket == INVALID_SOCK) return -1;

    /* Use select with zero timeout to check for available data */
    fd_set readfds;
    struct timeval tv = { 0, 0 };

    /* Read all available data into accumulation buffer */
    while (1) {
        FD_ZERO(&readfds);
        FD_SET(g_socket, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 0;

        int sel = select((int)g_socket + 1, &readfds, NULL, NULL, &tv);
        if (sel <= 0) break;

        int space = RECV_BUF_SIZE - g_recv_len - 1;
        if (space <= 0) {
            /* Buffer full, discard old data */
            g_recv_len = 0;
            space = RECV_BUF_SIZE - 1;
        }

        int n = recv(g_socket, g_recv_accum + g_recv_len, space, 0);
        if (n <= 0) {
            /* Connection closed or error */
            fprintf(stderr, "[NET] Connection lost\n");
            CLOSE_SOCKET(g_socket);
            g_socket = INVALID_SOCK;
            return -1;
        }
        g_recv_len += n;
        g_recv_accum[g_recv_len] = '\0';
    }

    /* Find the last complete UPDATE message in the buffer.
     * An UPDATE message starts with "UPDATE " and contains a complete JSON
     * object (matched braces). */
    char *last_update = NULL;
    {
        char *p = g_recv_accum;
        while ((p = strstr(p, "UPDATE ")) != NULL) {
            last_update = p;
            p += 7;
        }
    }

    if (!last_update) return 0;

    /* Find the JSON opening brace */
    char *json_start = strchr(last_update, '{');
    if (!json_start) return 0;

    /* Find matching closing brace */
    int depth = 0;
    char *end = json_start;
    while (*end) {
        if (*end == '{') depth++;
        else if (*end == '}') {
            depth--;
            if (depth == 0) break;
        }
        end++;
    }

    if (depth != 0) return 0; /* Incomplete JSON, wait for more data */

    /* Copy the complete UPDATE message */
    int msg_len = (int)(end + 1 - last_update);
    if (msg_len >= len) msg_len = len - 1;
    memcpy(buffer, last_update, msg_len);
    buffer[msg_len] = '\0';

    /* Print last received JSON for debugging */
    // const char *json_dbg = strchr(buffer, '{');
    // if (json_dbg)
    //     fprintf(stderr, "[NET] Last JSON:\n%s\n", json_dbg);

    /* Remove all processed data up to and including this message */
    int consumed = (int)(end + 1 - g_recv_accum);
    g_recv_len -= consumed;
    if (g_recv_len > 0) {
        memmove(g_recv_accum, end + 1, g_recv_len);
    }
    g_recv_accum[g_recv_len] = '\0';

    return msg_len;
}

int net_parse_update(const char *raw, GameState *gs, NetUpdateInfo *info)
{
    /* Find the JSON part (after "UPDATE ") */
    const char *json = strstr(raw, "{");
    if (!json) return 0;

    /* Parse game state */
    if (!json_get_string(json, "state", info->state, sizeof(info->state)))
        strcpy(info->state, "unknown");

    if (!json_get_string(json, "current_turn_role",
                         info->current_turn_role,
                         sizeof(info->current_turn_role)))
        strcpy(info->current_turn_role, "");

    if (!json_get_int(json, "total_moves", &info->total_moves))
        info->total_moves = 0;

    if (!json_get_int(json, "player_count", &info->player_count))
        info->player_count = 0;

    /* Parse color_table */
    json_get_role_color(json, "A", info->color_a, sizeof(info->color_a));
    json_get_role_color(json, "B", info->color_b, sizeof(info->color_b));

    /* Parse winner role (e.g. "A", "B", or absent) */
    if (!json_get_string(json, "winner", info->winner_role,
                         sizeof(info->winner_role)))
        strcpy(info->winner_role, "");

    /* Parse board */
    parse_board(json, gs);

    /* Don't set game_over from here — let the GUI handle it */
    gs->game_over = 0;
    gs->winner    = SIDE_NONE;

    return 1;
}

void net_close(void)
{
    if (g_socket != INVALID_SOCK) {
        CLOSE_SOCKET(g_socket);
        g_socket = INVALID_SOCK;
    }
    g_recv_len = 0;
    g_recv_accum[0] = '\0';

#ifdef _WIN32
    if (g_wsa_inited) {
        WSACleanup();
        g_wsa_inited = 0;
    }
#endif

    fprintf(stderr, "[NET] Disconnected\n");
}
