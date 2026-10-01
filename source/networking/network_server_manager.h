/*
NETWORK_SERVER_MANAGER.H

header included in hcex build.
*/

#ifndef __NETWORK_SERVER_MANAGER_H
#define __NETWORK_SERVER_MANAGER_H
#pragma once

/* ---------- headers */

#include "cseries.h"

/* ---------- constants */

/* ---------- macros */

/* ---------- structures */

/* ---------- prototypes/NETWORK_SERVER_MANAGER.C */

struct network_game_server;
struct game_variant;

struct network_game_server *network_game_server_create(
	void);
void network_game_server_dispose(
	struct network_game_server *server);
boolean network_game_server_idle(
	struct network_game_server *server);
void network_game_server_open_game(
	struct network_game_server *server);
void network_game_server_switch_to_postgame(
	struct network_game_server *server);
boolean network_game_server_admin_end_match(
	struct network_game_server *server);
boolean network_game_server_graceful_shutdown(
	struct network_game_server *server);
boolean network_game_server_reset_to_pregame(
	struct network_game_server *server);
void network_game_server_pause_countdown(
	struct network_game_server *server,
	boolean pause_countdown);
void network_game_generate_join_game_token(
	byte *join_token);
void network_game_server_kick_machine(
	long machine_index);
/* the host's ban command (console.c, hs.c) */
enum
{
	NETWORK_GAME_SERVER_NAME_TEXT_SIZE = 16,
};
boolean network_game_server_ban_player(
	char const *text);
short network_game_server_matching_player_names(
	char const *text,
	char (*names)[NETWORK_GAME_SERVER_NAME_TEXT_SIZE],
	short maximum_count);
unsigned long network_game_server_machine_address(
	long machine_index);
char const *network_game_server_machine_hardware_id(
	long machine_index);
void network_game_server_update_ticks(
	struct network_game_server *server,
	short tick_count);
boolean network_game_server_admin_start_immediately(
	struct network_game_server *server,
	char const **failure_reason);
void network_game_server_change_map_name(
	struct network_game_server *server,
	char const *map_name);
void network_game_server_change_game_variant(
	struct network_game_server *server,
	struct game_variant *variant);
boolean network_game_server_admin_queue_map(
	struct network_game_server *server,
	char const *map_name,
	struct game_variant const *variant,
	boolean front);
long network_game_server_admin_map_queue_count(
	void);
boolean network_game_server_admin_map_queue_get(
	long index,
	char *map_name,
	long map_name_size,
	struct game_variant *variant);
boolean network_game_server_admin_next_map(
	struct network_game_server *server);
boolean network_game_server_is_pregame(
	struct network_game_server *server);
boolean network_game_server_is_ingame(
	struct network_game_server *server);
boolean network_game_server_is_postgame(
	struct network_game_server *server);
char const *network_game_server_get_map_name(
	struct network_game_server *server);
struct game_variant *network_game_server_get_game_variant(
	struct network_game_server *server);
short network_game_server_get_player_count(
	struct network_game_server *server);
short network_game_server_get_machine_count(
	struct network_game_server *server);

/* ---------- globals */

/* ---------- public code */

#endif // __NETWORK_SERVER_MANAGER_H
