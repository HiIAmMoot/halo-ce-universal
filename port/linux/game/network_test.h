/*
NETWORK_TEST.H

The riding tests of the automated network sessions (network_test.c,
debug.network_test_ride): the vehicles moved and the riders scripted a
tick, and what every machine has of them logged. Each does nothing while
debug.network_test_ride is empty.
*/

#ifndef NETWORK_TEST_H
#define NETWORK_TEST_H

struct player_action;
struct vehicle_riding_deck;

/* each game tick, before and after the objects update (game.c) */
void network_test_ride_before_objects(void);
/* the player-to-unit boundary's decision, before level scripts run (players.c) */
void network_test_ride_control_delivered(long player_index, boolean delivered);
void network_test_ride_after_objects(void);
/* whether the riding test's setting is set and parsed (the host's re-board entitlement, network_distributed.c) */
boolean network_test_ride_running(void);
/* a local player's input this tick, the riding test's (player_queues_new.c) */
void network_test_ride_action(short local_player_index, struct player_action *action);
/* the capsule's actual support transaction, observed without changing physics */
void network_test_ride_contact_begin(long unit_index);
void network_test_ride_contact(long unit_index, long vehicle, short node, long surface,
	real_point3d const *point, real_vector3d const *normal, real radius);
void network_test_ride_contact_end(long unit_index, long support, boolean grounded);
void network_test_ride_launch(long unit_index);

/* the host collecting, a client receiving, a player's state (network_distributed.c) */
void network_test_ride_state(short player_index, long unit_index, long host_tick, boolean predicted,
	word predicted_low16, real_point3d const *world, boolean deck_present, struct vehicle_riding_deck const *deck);
/* a client having applied the host's state of that tick to its unit */
void network_test_ride_applied(short player_index, long unit_index, long host_tick);
/* a client noting its own unit's position of a tick (distributed_note_own_positions) */
void network_test_ride_own(short player_index, long tick, long unit_index);
/* the host's word on the prediction a client's player had (distributed_apply_predictions) */
void network_test_ride_prediction(short player_index, long unit_index, long host_tick, word predicted_low16,
	boolean eligible, boolean accepted);
/* a client's unit moved by the host's word (distributed_apply_state) */
void network_test_ride_correction(short player_index, long unit_index, real_point3d const *before,
	real_point3d const *after);
/* a client's noted position moved by a correction (distributed_correct_own_unit) */
void network_test_ride_history(short player_index, long tick, long unit_index, real_point3d const *before,
	real_point3d const *after, struct vehicle_riding_deck const *deck, long window, long air_episode);
/* the claim's grace a client's seat sync allows (distributed_handle_unit_state) */
void network_test_ride_grace(short player_index, short ticks);
/* a vehicle's pose the netcode wrote (network_objects_vehicle_written) */
void network_test_ride_vehicle_written(long vehicle_index, real_point3d const *before, real_point3d const *after, short kind);

#endif
