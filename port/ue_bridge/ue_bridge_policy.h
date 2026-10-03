/*
UE_BRIDGE_POLICY.H

What one side of the bridge does about the other (Phase 0 design, section
8.1): both sides fill a ue_bridge_peer_view from the other's process handle
and header fields, and act on the action this returns. Pure: no clock, no
handles, so both sides and the tests run the same decision.
*/

#ifndef UE_BRIDGE_POLICY_H
#define UE_BRIDGE_POLICY_H

#include "ue_bridge_format.h"

#ifdef __cplusplus
extern "C" {
#endif

enum ue_bridge_action
{
	UE_BRIDGE_ACTION_NONE = 0,
	UE_BRIDGE_ACTION_PEER_EXITED,
	UE_BRIDGE_ACTION_PEER_CRASHED,
	UE_BRIDGE_ACTION_PEER_CRASHING,
	UE_BRIDGE_ACTION_PEER_HUNG,
	UE_BRIDGE_ACTION_PEER_HUNG_EDITOR
};

struct ue_bridge_peer_view
{
	/* the peer's process handle is signalled */
	int process_exited;
	/* GetExitCodeProcess; meaningful only when process_exited */
	uint32_t exit_code;
	/* the UE_BRIDGE_STOP_* the peer published */
	uint32_t stopping;
	/* the peer is crashing: the game's crash hook is waiting for a dump
	(game_crashing). A published stop (ue_stopping, game_stopping) is not
	needed here: ue_bridge_peer_winding_down counts it, and the policy takes a
	published CRASH as crashing from stopping alone */
	int crashing;
	/* how long the watcher has seen the peer winding down (crashing or stopping); it measures this itself */
	uint32_t crashing_for_ms;
	/* 0 until the peer's first heartbeat */
	uint64_t heartbeat_qpc;
	/* the peer's own timeout; 0 means UE_BRIDGE_DEFAULT_HANG_TIMEOUT_MS */
	uint32_t hang_timeout_ms;
	int debugger_attached;
	int busy;
	/* the peer is a UE editor, which can hold unsaved work */
	int is_editor;
};

/* a crash hook, crash reporter or shutdown still running after this long has hung itself */
#define UE_BRIDGE_CRASHING_LIMIT_MS (UE_BRIDGE_DUMP_WAIT_MS + 20000u)

enum ue_bridge_action ue_bridge_policy_decide(const struct ue_bridge_peer_view *peer, uint64_t now_qpc, uint64_t qpc_frequency);

/* 1 when the survivor shuts itself down after acting on action */
int ue_bridge_policy_shuts_down(enum ue_bridge_action action, int continue_on_peer_exit);

/* How long a watcher has seen its peer crashing (ue_bridge_peer_view.crashing_for_ms).
Zero-initialised to start. Takes one reading of the clock per call: a caller
that reads it twice, once to stamp and once to subtract, can see the later
reading come out behind the stamp. Wrap-safe, for a 32-bit millisecond tick. */
struct ue_bridge_crash_clock
{
	int active;
	uint32_t since_ms;
};

/* 0 while the peer is not crashing (and resets the clock), and on the first
sighting; then the milliseconds since it */
uint32_t ue_bridge_crashing_for_ms(struct ue_bridge_crash_clock *clock, int crashing, uint32_t now_ms);

/* 1 while a peer is crashing or has published a stop (UE_BRIDGE_STOP_EXIT or
CRASH): the watcher leaves such a live peer alone, up to UE_BRIDGE_CRASHING_LIMIT_MS
of its own clock, and a caller feeds ue_bridge_crashing_for_ms with this. An
exiting peer is still writing its config and caches: it has stopped heartbeating
but has not hung. */
int ue_bridge_peer_winding_down(const struct ue_bridge_peer_view *peer);

/* 1 for an NTSTATUS error code (0xC0000000 and above): an unhandled exception */
int ue_bridge_exit_code_is_crash(uint32_t exit_code);

const char *ue_bridge_action_name(enum ue_bridge_action action);

#ifdef __cplusplus
}
#endif

#endif
