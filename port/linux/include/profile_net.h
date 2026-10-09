/*
PROFILE_NET.H

Expose profiling network accounting and layout registration.
*/

#ifndef __PROFILE_NET_H
#define __PROFILE_NET_H

#ifdef HALO_PROFILE

#include "halo_port_limits.h"

/* ---------- constants */

enum
{
	/* the machine of a client's messages to its host, and of the host's
	server datagram connection */
	PROFILE_NET_HOST = -2,
	PROFILE_NET_SERVER_DATAGRAMS = -3,
	PROFILE_NET_NO_MACHINE = -1,

	/* the pseudo message type of a batch's 8-byte header */
	PROFILE_NET_BATCH_HEADER = 0,
	/* where a distributed message's type is (struct distributed_message_header) */
	PROFILE_NET_MESSAGE_TYPE_OFFSET = 2,
	PROFILE_NET_MESSAGE_HEADER_SIZE = 8,

	MAXIMUM_PROFILE_NET_TYPES = 128,
	MAXIMUM_PROFILE_NET_SITES = 1024,
	MAXIMUM_PROFILE_NET_FIELDS = 256,
	/* the netcode's machines, and the tunnel's peers (a host and the rest
	of them: P2P_MAXIMUM_PEERS, which p2p.c asserts this holds) */
	MAXIMUM_PROFILE_NET_MACHINES = HALO_PORT_MAXIMUM_NETWORK_MACHINES,
	MAXIMUM_PROFILE_NET_PEERS = HALO_PORT_MAXIMUM_NETWORK_MACHINES - 1,
	MAXIMUM_PROFILE_NET_CONNECTIONS = 256,
	MAXIMUM_PROFILE_NET_OBJECTS = 8192,
	/* an interval's distinct entry keys; past them entries go to an
	"other" row, so totals stay exact */
	MAXIMUM_PROFILE_NET_INTERVAL_KEYS = 16384,
	/* the key of that row, and of entries with no key */
	PROFILE_NET_KEY_OTHER = -2,
	PROFILE_NET_KEY_NONE = -1,
};

enum profile_net_direction
{
	_profile_net_out = 0,
	_profile_net_in,
};

enum profile_net_channel
{
	_profile_net_datagram = 0,
	_profile_net_stream,
};

/* why network_distributed_handle_message dropped a message (0: handled) */
enum profile_net_drop
{
	_profile_net_drop_handled = 0,
	_profile_net_drop_not_in_game,
	_profile_net_drop_bad_type,
	_profile_net_drop_bad_size,
	_profile_net_drop_wrong_direction,
	_profile_net_drop_stale,
	_profile_net_drop_fast_clock,
	NUMBER_OF_PROFILE_NET_DROPS,
};

/* what profile_net_received counts: a message that came alone, one that
came in a batch, or a batch's datagram (its header) */
enum profile_net_received_kind
{
	_profile_net_received_message = 0,
	_profile_net_received_inner,
	_profile_net_received_batch,
};

/* why a send never reached the socket layer */
enum profile_net_failure
{
	_profile_net_failure_none = 0,
	_profile_net_failure_no_server,
	_profile_net_failure_no_machine,
	_profile_net_failure_no_connection,
	_profile_net_failure_too_large,
	_profile_net_failure_write_failed,
	NUMBER_OF_PROFILE_NET_FAILURES,
};

/* what a fixed-size entry's member is a key of */
enum profile_net_key_kind
{
	_profile_net_key_none = 0,
	/* a datum index (long): an object */
	_profile_net_key_datum,
	/* a player index (byte) */
	_profile_net_key_player,
};

/* ---------- structures */

/* a fixed-size entry's member (the netcode files' layout tables) */
struct profile_net_layout_member
{
	const char *name;
	unsigned short offset;
	unsigned short size;
	unsigned char key;
};

/* a layout table's rows (the netcode files' X-macros of their entry
structs), and the sum of the members' sizes, which each file asserts ends
where the last member does and the struct ends within its padding: a member
added or changed then breaks the profiling build instead of misreporting */
#define PROFILE_NET_MEMBER(type, member, key) \
	{ #member, (unsigned short)offsetof(type, member), (unsigned short)sizeof(((type *)0)->member), key },
#define PROFILE_NET_MEMBER_SIZE(type, member, key) + sizeof(((type *)0)->member)
#define PROFILE_NET_END(type, member) (offsetof(type, member) + sizeof(((type *)0)->member))
/* what each layout asserts: the sizes summed (sum) end where the last listed
member does, and no more is left of the struct than padding (less than its
alignment): a member added after the last one listed would otherwise be
reported as tail_pad */
#define PROFILE_NET_LAYOUT_OK(sum, type, last) \
	((sum) == PROFILE_NET_END(type, last) && sizeof(type) - PROFILE_NET_END(type, last) < __alignof__(type))

/* a message type's name and the function that handles it */
struct profile_net_message_name
{
	int type;
	const char *name;
	const char *handler;
};

/* one peer of internet play's tunnel, its counts since it was made (p2p.c) */
struct profile_net_tunnel_peer
{
	unsigned long virtual_address;
	unsigned long long bytes_out;
	unsigned long long bytes_in;
	unsigned long long packets_out;
	unsigned long long packets_in;
	unsigned long long highest_in;
	unsigned long long kcp_payload;
	unsigned long long kcp_output;
	unsigned long round_trip;
};

/* the overlay's totals since the start (bytes and datagrams by direction) */
struct profile_net_live
{
	unsigned long long game_bytes[2];
	unsigned long long game_packets[2];
	unsigned long long wire_bytes[2];
	unsigned long long wire_packets[2];
	/* the last second's */
	long ping_ms;
	long wire_round_trip_ms;
	double loss_percent;
};

/* ---------- globals */

/* recording: the funnel's detail; counting: recording or the overlay on
(the connection layer's totals, the tunnel and the pings); connections:
connections known, whose close must be seen */
extern int profile_net_recording;
extern int profile_net_counting;
extern int profile_net_connections;

/* ---------- prototypes/PROFILE_NET.C */

/* start-up (the game's profile_console.c) */
void profile_net_message_names(const struct profile_net_message_name *names, int count);
/* entry_size: the struct's, past its last member a tail of padding */
void profile_net_layout(int type, const struct profile_net_layout_member *members, int count,
	unsigned short entry_size);
void profile_net_set_object_describe(
	int (*describe)(long key, char *object_type, int object_type_size, char *tag, int tag_size));
void profile_net_set_peer_describe(unsigned long (*endpoint)(unsigned long virtual_address));
void profile_net_set_queue_reader(long (*queued_bytes)(void const *connection));
void profile_net_set_overlay(int on);

/* the netcode's sending functions: a site is a function and a line; the
outermost pushed is the message's */
int profile_net_site(const char *function, long line);
void profile_net_site_push(int site);
void profile_net_site_pop(void);

/* the funnel (network_distributed.c); sender is the netcode's batch index, 0 to HALO_PORT_MAXIMUM_NETWORK_MACHINES (the host) */
void profile_net_built(int type);
void profile_net_batch_append(int sender, long machine, int type, unsigned long bytes, int entries);
void profile_net_batch_flush(int sender, long machine, unsigned long size, int sent);
void profile_net_batch_discard(int sender);
void profile_net_send_failed(int failure);
void profile_net_entries(long machine, int type, void const *entries, int count, unsigned long entry_size);
int profile_net_field(const char *expression);
void profile_net_field_put(int field, unsigned long size);
void profile_net_packed_entry(long machine, int type, long key, unsigned long size, int added);
void profile_net_reliable(long machine, void const *message, unsigned long size, int sent);
/* netcode IPv4 inputs are host order; tunnel peer addresses are network order */
void profile_net_machine_address(long machine, unsigned long ipv4, unsigned short port);
void profile_net_received(long machine, void const *message, unsigned long size, int kind, int drop);

/* the connection layer (network_connection.c) */
void profile_net_traffic(void const *connection, int direction, int channel, unsigned long bytes,
	unsigned long ipv4, unsigned short port);
void profile_net_connection_closed(void const *connection);
void profile_net_connection_machine(void const *connection, long machine);
/* debug.network_loss's dropped datagrams (any thread) */
void profile_net_simulated_loss(void);

/* once a second (profile_console.c) */
void profile_net_tunnel(const struct profile_net_tunnel_peer *peers, int count);
void profile_net_ping(long machine, long milliseconds);
void profile_net_sample_queues(void);

void profile_net_live(struct profile_net_live *live);

#endif

#endif
