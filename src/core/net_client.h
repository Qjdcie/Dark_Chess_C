#ifndef NET_CLIENT_H
#define NET_CLIENT_H

#include "board.h"

/* Default server settings */
#define NET_SERVER_IP   "140.124.184.220"
#define NET_SERVER_PORT 8888

/* Parsed info from an UPDATE message */
typedef struct {
    char current_turn_role[4];  /* "A" or "B" */
    char state[16];             /* "waiting", "playing", etc. */
    char color_a[10];           /* "Red", "Black", or "None" */
    char color_b[10];
    char winner_role[4];        /* "A", "B", or "" (from server) */
    int  total_moves;
    int  player_count;
} NetUpdateInfo;

/*
 * Connect to the battle server.
 * Returns 0 on success, -1 on failure.
 */
int net_connect(const char *ip, int port);

/*
 * Join a room by sending "JOIN <room_id>\n".
 * Blocks until server responds.
 * Fills role_out with "first" or "second".
 * Returns 0 on success, -1 on failure.
 */
int net_join_room(const char *room_id, char *role_out, int role_len);

/*
 * Send an action string to the server (e.g. "0 3\n" or "1 2 1 3\n").
 */
void net_send_action(const char *action);

/*
 * Non-blocking receive. Returns the number of bytes read into buffer,
 * 0 if nothing available, -1 on error/disconnect.
 * Internally buffers partial messages and returns complete UPDATE messages.
 */
int net_try_receive(char *buffer, int len);

/*
 * Parse an UPDATE JSON string and fill GameState + NetUpdateInfo.
 * The input should contain "UPDATE {json...}".
 * Returns 1 on success, 0 on parse failure.
 */
int net_parse_update(const char *raw, GameState *gs, NetUpdateInfo *info);

/* Close the connection and clean up. */
void net_close(void);

/* Returns 1 if currently connected. */
int net_is_connected(void);

#endif /* NET_CLIENT_H */
