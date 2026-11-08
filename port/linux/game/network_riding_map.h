/* The map's riding choice, from the host to every client. */
#ifndef NETWORK_RIDING_MAP_H
#define NETWORK_RIDING_MAP_H

void network_riding_map_reset(void);
void network_riding_map_send(long machine_index);
void network_riding_map_update(void);
boolean network_riding_map_handles_message(word const *message, word size);
void network_riding_map_handle_message(long machine_index, word const *data, word size);

#endif
