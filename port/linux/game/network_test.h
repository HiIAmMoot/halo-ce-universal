/*
NETWORK_TEST.H

The input tape of the automated tests (network_test.c, debug.input_tape): a
local player's input recorded each tick, and given back from a recording.
*/

#ifndef NETWORK_TEST_H
#define NETWORK_TEST_H

struct player_action;

/* a local player's input this tick (player_queues_new.c) */
void network_test_tape_action(short local_player_index, struct player_action *action);
/* tape_record, tape_stop, tape_play and tape_list (hs.c): TRUE when the expression was one of them */
boolean network_test_tape_command(char const *expression);

#endif
