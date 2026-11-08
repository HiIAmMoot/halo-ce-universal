#include "harness.h"
typedef unsigned short word;
enum { _game_connection_local, _game_connection_network_client, _game_connection_network_server };
#include "message_ids.inc"
enum { _distributed_to_clients_reliably };
struct distributed_message_header { word header; byte type, count; long game_time; };
#define GET_MESSAGE_TYPE(h) (((h)>>2)&3)
static int connection = _game_connection_network_server, playing = 1, menu, unreliable, sends;
static short map_value = NONE, received = NONE;
static byte wire[32];
static word wire_size;
static long last_machine = NONE;
static boolean network_connection_last_read_was_unreliable(void) { return unreliable; }
static int game_connection(void) { return connection; }
static int game_in_progress(void) { return playing; }
static int main_menu_is_active(void) { return menu; }
static short vehicle_riding_map(void) { return map_value; }
static void vehicle_riding_map_received(short map) { received = map; }
static void distributed_send(void *data, byte type, int count, word size, int destination)
{
    struct distributed_message_header *h = (void *)wire;
    CHECK(destination == _distributed_to_clients_reliably, "not reliable broadcast");
    CHECK(size <= sizeof(wire), "wire overflow");
    memcpy(wire, data, size); h->header = 8; h->type = type; h->count = count; wire_size = size; sends++;
}
static void distributed_send_to_machine_reliably(long machine, void *data, byte type, int count, word size)
{ last_machine = machine; distributed_send(data, type, count, size, _distributed_to_clients_reliably); }
#include "under_test.inc"
static void receive(int from, int value, int count, int size, int reserved)
{
    struct riding_map_message m = {{8, _distributed_message_vehicle_riding_map, count, 0}, value, reserved};
    network_riding_map_handle_message(from, (word *)&m, size);
}
int main(int argc, char **argv)
{
    CHECK(argc == 2, "case");
    char const *case_name = argv[1];
    network_riding_map_reset();
    CASE("replication") {
        network_riding_map_update(); network_riding_map_send(7); CHECK(sends == 0, "sent while none");
        map_value = TRUE; network_riding_map_update();
        CHECK(sends == 1 && wire_size == 12 && ((struct riding_map_message *)wire)->map == 2, "on wire layout");
        CHECK(network_riding_map_handles_message((word *)wire, wire_size), "dispatch missing");
        CHECK(!network_riding_map_handles_message((word *)wire, 7), "short dispatch");
        network_riding_map_update(); CHECK(sends == 1, "unchanged spam");
        network_riding_map_send(7); CHECK(sends == 2 && last_machine == 7, "late join missing");
        map_value = FALSE; network_riding_map_update();
        CHECK(sends == 3 && ((struct riding_map_message *)wire)->map == 1 && !((struct riding_map_message *)wire)->reserved, "off wire layout");
        connection = _game_connection_network_client; network_riding_map_update(); network_riding_map_send(7);
        CHECK(sends == 3, "client sent");
        connection = _game_connection_network_server; map_value = TRUE; playing = 0; network_riding_map_update();
        playing = 1; menu = 1; network_riding_map_update(); CHECK(sends == 3, "outside game sent");
    }
    else CASE("admission") {
        connection = _game_connection_network_client;
        receive(NONE, 1, 1, 12, 0); CHECK(received == FALSE, "off not received");
        receive(NONE, 2, 1, 12, 0); CHECK(received == TRUE, "on not received");
        received = NONE; unreliable = 1; receive(NONE, 2, 1, 12, 0); CHECK(received == NONE, "datagram accepted");
        unreliable = 0; receive(3, 2, 1, 12, 0); CHECK(received == NONE, "client spoof accepted");
        receive(NONE, 2, 0, 12, 0); CHECK(received == NONE, "count zero accepted");
        receive(NONE, 2, 2, 12, 0); CHECK(received == NONE, "count two accepted");
        receive(NONE, 3, 1, 12, 0); CHECK(received == NONE, "invalid map accepted");
        receive(NONE, 2, 1, 12, 1); CHECK(received == NONE, "reserved accepted");
        for (int n = 0; n < 12; n++) { receive(NONE, 2, 1, n, 0); CHECK(received == NONE, "truncation accepted"); }
        receive(NONE, 2, 1, 13, 0); CHECK(received == NONE, "trailing bytes accepted");
        connection = _game_connection_network_server; receive(NONE, 2, 1, 12, 0); CHECK(received == NONE, "host accepted");
        connection = _game_connection_network_client; playing = 0; receive(NONE, 2, 1, 12, 0); CHECK(received == NONE, "outside game accepted");
        playing = 1; menu = 1; receive(NONE, 2, 1, 12, 0); CHECK(received == NONE, "menu accepted");
        menu = 0; receive(NONE, 0, 1, 12, 0); CHECK(received == NONE, "none decode");
    }
    else CASE("reset") {
        connection = _game_connection_network_client; receive(NONE, 2, 1, 12, 0);
        CHECK(received == TRUE, "setup"); network_riding_map_reset(); CHECK(received == NONE, "client not reset");
        connection = _game_connection_network_server; map_value = TRUE; network_riding_map_update();
        CHECK(sends == 1, "host setup"); network_riding_map_reset(); network_riding_map_update(); CHECK(sends == 2, "host not reset");
    }
    else return 2;
    return 0;
}
