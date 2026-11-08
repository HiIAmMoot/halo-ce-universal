/* The map's riding choice overrides the host's pregame switch for this map.
The host sends it reliably when it changes and when a client becomes ready;
without a map choice, the pregame switch still decides and nothing is sent. */
#include "cseries.h"
#include "game/game.h"
#include "interface/ui_widget.h"
#include "main/main.h"
#include "networking/network_game_globals.h"
#include "network_distributed.h"
#include "network_riding_map.h"
#include "vehicle_riding.h"
#include <string.h>

static short sent_map = NONE;

struct riding_map_message
{
	struct distributed_message_header header;
	word map;
	word reserved;
};

void network_riding_map_reset(void)
{
	vehicle_riding_map_received(NONE);
	sent_map = NONE;
}

void network_riding_map_send(long machine_index)
{
	struct riding_map_message message;
	short map;

	if (game_connection() != _game_connection_network_server)
		return;
	map = vehicle_riding_map();
	if (map == NONE)
		return;
	memset(&message, 0, sizeof(message));
	message.map = (word)(map + 1);
	distributed_send_to_machine_reliably(machine_index, &message, _distributed_message_vehicle_riding_map, 1,
		sizeof(message));
}

void network_riding_map_update(void)
{
	struct riding_map_message message;
	short map;

	if (!game_in_progress() || main_menu_is_active() || game_connection() != _game_connection_network_server)
		return;
	map = vehicle_riding_map();
	if (map != NONE && !(sent_map == map))
	{
		sent_map = map;
		memset(&message, 0, sizeof(message));
		message.map = (word)(map + 1);
		distributed_send(&message, _distributed_message_vehicle_riding_map, 1, sizeof(message),
			_distributed_to_clients_reliably);
	}
}

boolean network_riding_map_handles_message(word const *message, word size)
{
	struct distributed_message_header header;

	if (size < sizeof(header))
		return FALSE;
	memcpy(&header, message, sizeof(header));
	return header.type == _distributed_message_vehicle_riding_map;
}

/* only the host's reliable stream, whole, while the game is running */
void network_riding_map_handle_message(long machine_index, word const *data, word size)
{
	struct riding_map_message message;

	if (machine_index != NONE || game_connection() != _game_connection_network_client ||
		!game_in_progress() || main_menu_is_active() || network_connection_last_read_was_unreliable() || size != sizeof(message))
		return;
	memcpy(&message, data, sizeof(message));
	if (GET_MESSAGE_TYPE(message.header.header) != 2 || message.header.type != _distributed_message_vehicle_riding_map ||
		message.header.count != 1 || message.map > 2 || message.reserved != 0)
		return;
	vehicle_riding_map_received((short)message.map - 1);
}
