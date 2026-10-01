/*
TELNET_CONSOLE.C

symbols in this file:
001201D0 00e0:
	_telnet_console_initialize (0000)
001202B0 0050:
	_telnet_console_dispose (0000)
00120300 00c0:
	_telnet_console_print (0000)
001203C0 01f0:
	_code_001203c0 (0000)
001205B0 0160:
	_telnet_console_process (0000)
00288D04 003e:
	??_C@_0DO@PCBIDEOI@create_transport_endpoint?$CI?$CJ?5fail@ (0000)
00288D44 0032:
	??_C@_0DC@CJAJBFKG@bind_endpoint?$CI?$CJ?5failed?5on?5telnet@ (0000)
00288D78 0034:
	??_C@_0DE@MBCCFOEL@listen_endpoint?$CI?$CJ?5failed?5on?5teln@ (0000)
00288DAC 0021:
	??_C@_0CB@NCDEMBCM@connection?5lost?5to?5telnet?5client@ (0000)
00288DD0 002f:
	??_C@_0CP@LBGDAMPB@?$AN?6overflowed?5client?5buffer?$DL?5rese@ (0000)
00288E00 0028:
	??_C@_0CI@BALEJICP@failed?5to?5write?5to?5telnet?5client@ (0000)
00288E28 000d:
	??_C@_0N@FIIHEHGK@?$AN?6goodbye?$CB?$AN?6?$AA@ (0000)
00288E38 0028:
	??_C@_0CI@BMDHLBAG@connection?5lost?5to?5telnet?5client@ (0000)
00288E60 001f:
	??_C@_0BP@BBNCCABM@error?5processing?5telnet?5client?$AA@ (0000)
00288E80 0048:
	??_C@_0EI@HONFGAEA@sorry?5?9?5the?5maximum?5number?5of?5cl@ (0000)
00288EC8 0021:
	??_C@_0CB@MOGHMNHN@Would?5you?5like?5to?5play?5a?5game?$DP?$AN?6@ (0000)
00456D00 008c:
	_telnet_console_globals (0000)
*/


/* ---------- headers */

#include "cseries.h"
#include "cseries/errors.h"
#include "bungie_net/network/transport.h"
#include "bungie_net/network/transport_address_constants.h"
#include "bungie_net/network/transport_endpoint.h"
#include "hs/hs.h"
#include "networking/telnet_console.h"
#if defined(__linux__) || defined(HALO_NATIVE_DESKTOP)
#include "game/game_engine.h"
#include "game/players.h"
#include "interface/player_ui.h"
#include "main/main.h"
#include "memory/data.h"
#include "networking/network_client_manager.h"
#include "networking/network_game_globals.h"
#include "networking/network_game_manager.h"
#include "networking/network_server_manager.h"
#include "networking/network_server_manager_internal.h"
#include "text/unicode.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#endif

/* ---------- constants */

enum
{
	MAXIMUM_TELNET_CLIENTS = 1,
	TELNET_CLIENT_BUFFER_SIZE = 128,
	/* the Xbox's was 23 (telnet), which a program that is not the
	administrator cannot listen on (Linux, Android): the native builds' is
	debug.telnet_console_port */
	TELNET_CONSOLE_DEFAULT_PORT = 2323,
	_transport_endpoint_type_telnet = 0x12
};

/* ---------- macros */

/* ---------- structures */

struct telnet_client
{
	struct transport_endpoint *endpoint;
	char buffer[TELNET_CLIENT_BUFFER_SIZE];
};

struct telnet_console_globals
{
	struct transport_endpoint *listening_endpoint;
	struct telnet_client clients[MAXIMUM_TELNET_CLIENTS];
	boolean initialized;
};

/* ---------- prototypes */

/* the platform layer's (port/linux/src/port_config.c) */
int config_boolean(const char *name);
long config_integer(const char *name);

static boolean telnet_client_write(
	struct telnet_client *client,
	char const *string,
	long length);
static void telnet_client_disconnect(
	struct telnet_client *client);
static boolean process_telnet_client_buffer(
	char *buffer,
	long size,
	struct telnet_client *client);
#if defined(__linux__) || defined(HALO_NATIVE_DESKTOP)
static boolean telnet_console_admin_command(
	char const *command,
	struct telnet_client *client);
#endif

/* ---------- globals */

static struct telnet_console_globals telnet_console_globals = {0};

/* ---------- public code */

void telnet_console_initialize(
	void)
{
	csmemset(&telnet_console_globals, 0, sizeof(telnet_console_globals));

	/* the native builds' console runs any script it is sent, with no
	password: only when asked for (debug.telnet_console), and only from
	this machine */
	if (!config_boolean("debug.telnet_console"))
		return;

	telnet_console_globals.listening_endpoint = create_transport_endpoint(_transport_endpoint_type_telnet);
	if (telnet_console_globals.listening_endpoint)
	{
		struct transport_address address = {{0}};

		address.address_length = IPV4_ADDRESS_LENGTH;
		address.address.long_words[0] = IPV4_LOOPBACK_ADDRESS;
		address.port = (word)config_integer("debug.telnet_console_port");
		if (!address.port)
			address.port = TELNET_CONSOLE_DEFAULT_PORT;

		if (bind_endpoint(telnet_console_globals.listening_endpoint, &address)==_transport_error_none)
		{
			if (listen_endpoint(telnet_console_globals.listening_endpoint)==_transport_error_none)
			{
				telnet_console_globals.initialized = TRUE;
			}
			else
			{
				error(2, "listen_endpoint() failed on telnet console endpoint");
				delete_transport_endpoint(telnet_console_globals.listening_endpoint);
				telnet_console_globals.listening_endpoint = NULL;
			}
		}
		else
		{
			error(2, "bind_endpoint() failed on telnet console endpoint");
			delete_transport_endpoint(telnet_console_globals.listening_endpoint);
			telnet_console_globals.listening_endpoint = NULL;
		}
	}
	else
	{
		error(2, "create_transport_endpoint() failed on telnet console endpoint");
	}

	return;
}

void telnet_console_dispose(
	void)
{
	if (telnet_console_globals.initialized)
	{
		if (telnet_console_globals.listening_endpoint)
			delete_transport_endpoint(telnet_console_globals.listening_endpoint);
		telnet_client_disconnect(telnet_console_globals.clients);
	}

	csmemset(&telnet_console_globals, 0, sizeof(telnet_console_globals));

	return;
}

void telnet_console_print(
	char *string)
{
	struct telnet_client *client = &telnet_console_globals.clients[0];

	/* (port: a failed write drops the client before it says so, since
	error() prints here again; a client that is not reading loses the line:
	telnet_client_write) */
	if (telnet_console_globals.initialized && string && string[0] && client->endpoint)
	{
		if (telnet_client_write(client, "\r\n", 2) &&
			telnet_client_write(client, string, csstrlen(string)) &&
			client->buffer[0])
		{
			telnet_client_write(client, client->buffer, csstrlen(client->buffer));
		}
	}

	return;
}

void telnet_console_process(
	void)
{
	if (telnet_console_globals.initialized)
	{
		char buffer[32];
		long count;

		if (endpoint_readable(telnet_console_globals.listening_endpoint, 0))
		{
			struct transport_endpoint *endpoint = accept_endpoint(telnet_console_globals.listening_endpoint);

			/* (a client that stops reading does not stall the game) */
			if (endpoint && set_endpoint_blocking(endpoint, FALSE) != _transport_error_none)
			{
				delete_transport_endpoint(endpoint);
				endpoint = NULL;
			}
			if (endpoint)
			{
				long client_index;

				for (client_index = 0; client_index<MAXIMUM_TELNET_CLIENTS; client_index++)
				{
					if (!telnet_console_globals.clients[client_index].endpoint)
					{
						if (write_endpoint(
							endpoint,
							"Would you like to play a game?\r\n",
							csstrlen("Would you like to play a game?\r\n"))<=0)
						{
							delete_transport_endpoint(endpoint);
						}
						else
						{
							telnet_console_globals.clients[client_index].endpoint = endpoint;
							telnet_console_globals.clients[client_index].buffer[0] = 0;
						}

						break;
					}
				}

				if (client_index==MAXIMUM_TELNET_CLIENTS)
				{
					write_endpoint(
						endpoint,
						"sorry - the maximum number of clients are already connected. goodbye!\r\n",
						csstrlen("sorry - the maximum number of clients are already connected. goodbye!\r\n"));
					delete_transport_endpoint(endpoint);
				}
			}
		}

		if (telnet_console_globals.clients[0].endpoint &&
			endpoint_readable(telnet_console_globals.clients[0].endpoint, 0))
		{
			count = read_endpoint(telnet_console_globals.clients[0].endpoint, buffer, sizeof(buffer));
			if (count>0)
			{
				if (!process_telnet_client_buffer(buffer, count, telnet_console_globals.clients))
				{
					telnet_client_disconnect(telnet_console_globals.clients);
					error(2, "error processing telnet client");
				}
			}
			else if (count!=_transport_result_operation_would_block)
			{
				/* (the client is dropped before error() prints to it) */
				telnet_client_disconnect(telnet_console_globals.clients);
				error(2, "connection lost to telnet client ('%s')", transport_error_to_string((short)count));
			}
		}
	}

	return;
}

/* ---------- private code */

/* port: the client's socket does not block (a client that stops reading
does not stall the game). What it cannot take now is dropped (a line of
text), a write sent in part goes on with the rest, and a failed write drops
the client before error() says so: error() prints to the console, and so
here again. TRUE when all of it was sent. */
static boolean telnet_client_write(
	struct telnet_client *client,
	char const *string,
	long length)
{
	long written = 0;

	while (client->endpoint && written<length)
	{
		long result = write_endpoint(client->endpoint, string+written, length-written);

		if (result>0)
		{
			written += result;
		}
		else if (result==_transport_result_operation_would_block)
		{
			return FALSE;
		}
		else
		{
			telnet_client_disconnect(client);
			error(2, "failed to write to telnet client ('%s')",
				transport_error_to_string((short)result));
			return FALSE;
		}
	}

	return client->endpoint && written==length;
}

/* (the endpoint is let go of before it is deleted: anything the deletion
prints finds no client) */
static void telnet_client_disconnect(
	struct telnet_client *client)
{
	struct transport_endpoint *endpoint = client->endpoint;

	client->endpoint = NULL;
	client->buffer[0] = 0;
	if (endpoint)
		delete_transport_endpoint(endpoint);

	return;
}

/* FALSE when the client was lost (it is then dropped) */
static boolean process_telnet_client_buffer(
	char *buffer,
	long size,
	struct telnet_client *client)
{
	long index;

	/* (a script run, or an error, prints to the client, which may lose it) */
	for (index = 0; client->endpoint && index<size; index++)
	{
		char *character = buffer+index;
		long length;

		if ((unsigned char)*character>0x7f)
			continue;

		if (isalnum(*character) || ispunct(*character) || *character==' ')
		{
			length = csstrlen(client->buffer)+1;
			if (length>=TELNET_CLIENT_BUFFER_SIZE)
			{
				client->buffer[0] = 0;
				telnet_client_write(
					client,
					"\r\noverflowed client buffer; resetting buffer\r\n",
					csstrlen("\r\noverflowed client buffer; resetting buffer\r\n"));

				return client->endpoint!=NULL;
			}

			client->buffer[length-1] = *character;
			client->buffer[length] = 0;
		}
		else
		{
			switch (*character)
			{
			case 10:
			case 13:
				if (client->buffer[0])
				{
					char expression[TELNET_CLIENT_BUFFER_SIZE];

					csstrncpy(expression, client->buffer, TELNET_CLIENT_BUFFER_SIZE-1);
					expression[TELNET_CLIENT_BUFFER_SIZE-1] = 0;
					client->buffer[0] = 0;

#if defined(__linux__) || defined(HALO_NATIVE_DESKTOP)
					if (telnet_console_admin_command(expression, client))
						continue;
#endif
					if (hs_compile_and_evaluate(expression))
					{
						telnet_client_write(client, "\r\n", 2);
					}
				}
				continue;

			case 8:
				if (client->buffer[0])
				{
					length = csstrlen(client->buffer);
					if (length>0)
						client->buffer[length-1] = 0;
				}
				break;

			case 4:
				telnet_client_write(
					client,
					"\r\ngoodbye!\r\n",
					csstrlen("\r\ngoodbye!\r\n"));
				telnet_client_disconnect(client);

				return TRUE;

			default:
				continue;
			}
		}

		telnet_client_write(client, character, 1);
	}

	return client->endpoint!=NULL;
}

#if defined(__linux__) || defined(HALO_NATIVE_DESKTOP)
static void telnet_console_admin_reply(
	struct telnet_client *client,
	char const *response)
{
	telnet_client_write(client, "\r\n", 2);
	telnet_client_write(client, response, (long)csstrlen(response));
	telnet_client_write(client, "\r\n", 2);
}

static char const *telnet_console_gametype_name(
	struct game_variant const *variant)
{
	static char const *const names[] =
	{
		"race", "team_race", "rally", "slayer", "team_slayer", "elimination",
		"stalker", "team_oddball", "accumulation", "oddball", "ctf", "ironctf",
		"king", "team_king"
	};
	struct game_variant candidate;
	struct game_variant current;
	size_t index;

	if (!variant)
		return "unknown";
	current = *variant;
	csmemset(current.human_readable_game_description, 0,
		sizeof(current.human_readable_game_description));
	for (index = 0; index < NUMBEROF(names); index++)
	{
		game_engine_get_variant_by_name(&candidate, names[index]);
		csmemset(candidate.human_readable_game_description, 0,
			sizeof(candidate.human_readable_game_description));
		if (!csmemcmp(&current, &candidate, sizeof(current)))
			return names[index];
	}
	return "custom";
}

static boolean telnet_console_admin_command(
	char const *command,
	struct telnet_client *client)
{
	static char const *const map_aliases[] =
	{
		"beavercreek", "sidewinder", "damnation", "ratrace", "prisoner",
		"hangemhigh", "chillout", "carousel", "boardingaction", "bloodgulch",
		"wizard", "putput", "longest"
	};
	static char const *const game_types[] =
	{
		"race", "team_race", "rally", "slayer", "team_slayer", "elimination",
		"stalker", "team_oddball", "accumulation", "oddball", "ctf", "ironctf",
		"king", "team_king"
	};
	char response[512];
	char map_alias[64];
	char game_type_name[64];
	char extra[2];
	char const *map_name;
	char const *map_path = NULL;
	struct network_game_server *server = global_network_game_server_get();
	struct network_game *game = server ? network_game_server_get_game(server) : NULL;
	struct game_variant variant;
	long player_index;
	long written;
	size_t index;
	int argument_count;

	map_name = game ? game->map.name : main_get_multiplayer_map_name();
	if (game)
		variant = game->variant;
	else if (!player_ui_game_variant_specified(&variant))
		variant = *game_engine_get_variant();

	if (!strcmp(command, "status"))
	{
		char const *phase = !server ? "offline" :
			network_game_server_is_pregame(server) ? "lobby" :
			network_game_server_is_ingame(server) ? "in-game" :
			network_game_server_is_postgame(server) ? "postgame" : "unknown";
		snprintf(response, sizeof(response), "OK status=%s map=%s gametype=%s players=%d machines=%d",
			phase, map_name ? map_name : "unknown", telnet_console_gametype_name(&variant),
			network_game_server_get_player_count(server), network_game_server_get_machine_count(server));
		telnet_console_admin_reply(client, response);
		return TRUE;
	}
	if (!strcmp(command, "currentmap"))
	{
		char const *alias = map_name ? strrchr(map_name, '\\') : NULL;
		snprintf(response, sizeof(response), "OK currentmap=%s gametype=%s",
			alias ? alias + 1 : (map_name ? map_name : "unknown"),
			telnet_console_gametype_name(&variant));
		telnet_console_admin_reply(client, response);
		return TRUE;
	}
	if (!strcmp(command, "players"))
	{
		long player_index;
		long count = 0;

		if (!game)
		{
			telnet_console_admin_reply(client, "ERR no hosted game");
			return TRUE;
		}
		telnet_console_admin_reply(client, "OK players");
		for (player_index = 0; player_index < NUMBEROF(game->players); player_index++)
		{
			struct network_player *player = &game->players[player_index];
			char name[64];
			long datum_index;
			boolean has_biped = FALSE;
			struct data_iterator iterator;
			struct player_datum *datum;

			if (!network_player_is_valid(player))
				continue;
			wide_to_ascii(player->name, name, sizeof(name));
			if (!name[0])
				csstrcpy(name, "<unnamed>");
			data_iterator_new(&iterator, player_data);
			while ((datum = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
			{
				if (datum->network_player_data.machine_index == player->machine_index &&
					datum->network_player_data.controller_index == player->controller_index)
				{
					has_biped = datum->unit_index != NONE;
					break;
				}
			}
			snprintf(response, sizeof(response),
				"player id=%ld name=%s machine=%d controller=%d team=%d biped=%s",
				player_index, name, player->machine_index,
				player->controller_index, player->team_index,
				has_biped ? "yes" : "no");
			telnet_console_admin_reply(client, response);
			count++;
		}
		if (!count)
			telnet_console_admin_reply(client, "players=0");
		return TRUE;
	}
	if (!strncmp(command, "kick", 4) && (!command[4] || command[4] == ' ' || command[4] == '\t'))
	{
		char *end;
		short machine_index;
		command += 4;
		while (*command == ' ' || *command == '\t')
			command++;
		player_index = strtol(command, &end, 10);
		while (*end == ' ' || *end == '\t')
			end++;
		if (!*command || end == command || *end || player_index == NONE)
		{
			telnet_console_admin_reply(client, "ERR usage: kick <player-id>");
			return TRUE;
		}
		{
			struct network_game *game = server ? network_game_server_get_game(server) : NULL;
			if (!game || player_index < 0 || player_index >= NUMBEROF(game->players) ||
				!network_player_is_valid(&game->players[player_index]))
			{
				telnet_console_admin_reply(client, "ERR player id not found");
				return TRUE;
			}
			machine_index = game->players[player_index].machine_index;
		}
		if (!server || machine_index == 0)
		{
			telnet_console_admin_reply(client, "ERR cannot kick the host or no hosted game");
			return TRUE;
		}
		network_game_server_kick_machine(machine_index);
		snprintf(response, sizeof(response), "OK disconnect scheduled for machine=%d", machine_index);
		telnet_console_admin_reply(client, response);
		return TRUE;
	}
	if (!strcmp(command, "gametype"))
	{
		snprintf(response, sizeof(response), "OK gametype=%s", telnet_console_gametype_name(&variant));
		telnet_console_admin_reply(client, response);
		return TRUE;
	}
	if (!strncmp(command, "gametype ", 9))
	{
		game_type_name[0] = 0;
		extra[0] = 0;
		argument_count = sscanf(command + 9, "%63s %1s", game_type_name, extra);
		if (argument_count != 1)
		{
			telnet_console_admin_reply(client, "ERR usage: gametype <name>");
			return TRUE;
		}
		for (index = 0; game_type_name[index]; index++)
			game_type_name[index] = (char)tolower((unsigned char)game_type_name[index]);
		if (index >= NUMBEROF(game_types))
		{
			for (index = 0; index < NUMBEROF(game_types); index++)
			{
				if (!strcmp(game_type_name, game_types[index]))
					break;
			}
		}
		else
		{
			for (index = 0; index < NUMBEROF(game_types); index++)
			{
				if (!strcmp(game_type_name, game_types[index]))
					break;
			}
		}
		if (index == NUMBEROF(game_types))
		{
			telnet_console_admin_reply(client, "ERR unknown gametype");
			return TRUE;
		}
		game_engine_get_variant_by_name(&variant, game_type_name);
		if (server && !network_game_server_is_pregame(server))
		{
			if (!network_game_server_admin_queue_map(server, map_name, &variant, FALSE))
				telnet_console_admin_reply(client, "ERR map queue is full");
			else
				telnet_console_admin_reply(client, "OK gametype queued for next transition");
			return TRUE;
		}
		player_ui_set_game_variant(&variant);
		game_engine_override_game_variant(&variant);
		if (server)
			network_game_server_change_game_variant(server, &variant);
		telnet_console_admin_reply(client, "OK gametype updated");
		return TRUE;
	}
	if (!strcmp(command, "start"))
	{
		char const *failure_reason;

		if (!server || !network_game_server_is_pregame(server))
			telnet_console_admin_reply(client, "ERR start requires a hosted lobby");
		else if (network_game_server_admin_start_immediately(server, &failure_reason))
			telnet_console_admin_reply(client, "OK match start requested");
		else
		{
			snprintf(response, sizeof(response), "ERR start blocked: %s", failure_reason);
			telnet_console_admin_reply(client, response);
		}
		return TRUE;
	}
	if (!strcmp(command, "end"))
	{
		if (!network_game_server_admin_end_match(server))
			telnet_console_admin_reply(client, "ERR end requires an active match");
		else
		{
			telnet_console_admin_reply(client, "OK match ended; lobby reset in 15 seconds");
		}
		return TRUE;
	}
	if (!strcmp(command, "restart"))
	{
		if (!server)
			telnet_console_admin_reply(client, "ERR restart requires a hosted game");
		else if (network_game_server_is_ingame(server))
		{
			game_engine_switch_to_postgame();
			telnet_console_admin_reply(client, "OK restart requested after postgame");
		}
		else if (network_game_server_is_postgame(server))
			telnet_console_admin_reply(client, network_game_server_reset_to_pregame(server) ?
				"OK returned to lobby" : "ERR could not return to lobby");
		else
			telnet_console_admin_reply(client, "ERR restart requires an active or postgame match");
		return TRUE;
	}
	if (!strcmp(command, "mapqueue"))
	{
		long queue_count = network_game_server_admin_map_queue_count();
		if (!queue_count)
			telnet_console_admin_reply(client, "OK mapqueue empty");
		for (index = 0; index < (size_t)queue_count; index++)
		{
			char queued_path[sizeof(((struct network_game *)0)->map.name)];
			struct game_variant queued_variant;
			char const *alias;
			if (!network_game_server_admin_map_queue_get((long)index, queued_path,
				sizeof(queued_path), &queued_variant))
				continue;
			alias = strrchr(queued_path, '\\');
			snprintf(response, sizeof(response), "OK mapqueue[%ld]=%s gametype=%s",
				index + 1, alias ? alias + 1 : queued_path, telnet_console_gametype_name(&queued_variant));
			telnet_console_admin_reply(client, response);
		}
		return TRUE;
	}
	if (!strcmp(command, "nextmap"))
	{
		if (!network_game_server_admin_next_map(server))
			telnet_console_admin_reply(client, "ERR map queue empty or server cannot transition");
		else
			telnet_console_admin_reply(client, "OK map transition requested");
		return TRUE;
	}
	if (!strncmp(command, "map ", 4) || !strncmp(command, "queuemap ", 9))
	{
		boolean queue_only = !strncmp(command, "queuemap ", 9);
		char const *arguments = command + (queue_only ? 9 : 4);
		struct game_variant queued_variant;
		boolean has_variant = FALSE;
		if (sscanf(arguments, "%63s %63s %1s", map_alias, game_type_name, extra) > 2)
		{
			telnet_console_admin_reply(client, "ERR usage: map <alias> [gametype]");
			return TRUE;
		}
		argument_count = sscanf(arguments, "%63s %63s", map_alias, game_type_name);
		if (argument_count < 1)
		{
			telnet_console_admin_reply(client, "ERR usage: map <alias> [gametype]");
			return TRUE;
		}
		for (index = 0; index < NUMBEROF(map_aliases); index++)
		{
			if (!_stricmp(map_alias, map_aliases[index]))
			{
				static char const *const paths[] =
				{
					"levels\\test\\beavercreek\\beavercreek", "levels\\test\\sidewinder\\sidewinder",
					"levels\\test\\damnation\\damnation", "levels\\test\\ratrace\\ratrace",
					"levels\\test\\prisoner\\prisoner", "levels\\test\\hangemhigh\\hangemhigh",
					"levels\\test\\chillout\\chillout", "levels\\test\\carousel\\carousel",
					"levels\\test\\boardingaction\\boardingaction", "levels\\test\\bloodgulch\\bloodgulch",
					"levels\\test\\wizard\\wizard", "levels\\test\\putput\\putput",
					"levels\\test\\longest\\longest"
				};
				map_path = paths[index];
				break;
			}
		}
		if (!map_path)
		{
			telnet_console_admin_reply(client, "ERR unknown multiplayer map");
			return TRUE;
		}
		if (argument_count == 2)
		{
			for (index = 0; game_type_name[index]; index++)
				game_type_name[index] = (char)tolower((unsigned char)game_type_name[index]);
			game_engine_get_variant_by_name(&queued_variant, game_type_name);
			if (!queued_variant.flags)
			{
				telnet_console_admin_reply(client, "ERR unknown gametype");
				return TRUE;
			}
			has_variant = TRUE;
		}
		if (queue_only || (server && !network_game_server_is_pregame(server)))
		{
			if (!server || !network_game_server_admin_queue_map(server, map_path,
				has_variant ? &queued_variant : NULL, !queue_only))
				telnet_console_admin_reply(client, "ERR map queue unavailable or full");
			else if (!queue_only && !network_game_server_admin_next_map(server))
				telnet_console_admin_reply(client, "ERR map transition failed");
			else
				telnet_console_admin_reply(client, queue_only ? "OK map queued" : "OK map transition requested");
			return TRUE;
		}
		main_set_multiplayer_map_name(map_path);
		game_engine_override_map_name(map_path);
		if (has_variant)
		{
			player_ui_set_game_variant(&queued_variant);
			game_engine_override_game_variant(&queued_variant);
		}
		if (server)
		{
			network_game_server_change_map_name(server, map_path);
			if (has_variant)
				network_game_server_change_game_variant(server, &queued_variant);
		}
		telnet_console_admin_reply(client, "OK map set");
		return TRUE;
	}
	if (!strcmp(command, "listmaps"))
	{
		csstrcpy(response, "OK maps: beavercreek, sidewinder, damnation, ratrace, prisoner, hangemhigh, chillout, carousel, boardingaction, bloodgulch, wizard, putput, longest");
		telnet_console_admin_reply(client, response);
		return TRUE;
	}
	if (!strcmp(command, "say") || !strncmp(command, "say ", 4))
	{
		telnet_console_admin_reply(client, "ERR server-wide chat is not implemented");
		return TRUE;
	}
	return FALSE;
}
#endif
