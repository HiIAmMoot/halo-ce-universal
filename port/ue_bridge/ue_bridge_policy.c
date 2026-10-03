/*
UE_BRIDGE_POLICY.C

See ue_bridge_policy.h.
*/

#include "ue_bridge_policy.h"

int ue_bridge_exit_code_is_crash(uint32_t exit_code)
{
	return exit_code >= 0xC0000000u;
}

static enum ue_bridge_action hung_action(const struct ue_bridge_peer_view *peer)
{
	return peer->is_editor ? UE_BRIDGE_ACTION_PEER_HUNG_EDITOR : UE_BRIDGE_ACTION_PEER_HUNG;
}

int ue_bridge_peer_winding_down(const struct ue_bridge_peer_view *peer)
{
	return peer->crashing != 0 || peer->stopping != UE_BRIDGE_STOP_NONE;
}

enum ue_bridge_action ue_bridge_policy_decide(const struct ue_bridge_peer_view *peer, uint64_t now_qpc, uint64_t qpc_frequency)
{
	uint32_t timeout_ms;
	uint64_t age_ms;

	if (peer->process_exited)
	{
		if (peer->stopping == UE_BRIDGE_STOP_CRASH || ue_bridge_exit_code_is_crash(peer->exit_code))
			return UE_BRIDGE_ACTION_PEER_CRASHED;
		return UE_BRIDGE_ACTION_PEER_EXITED;
	}
	if (ue_bridge_peer_winding_down(peer))
	{
		if (peer->crashing_for_ms > UE_BRIDGE_CRASHING_LIMIT_MS && !peer->debugger_attached)
			return hung_action(peer);
		return peer->crashing || peer->stopping == UE_BRIDGE_STOP_CRASH ? UE_BRIDGE_ACTION_PEER_CRASHING : UE_BRIDGE_ACTION_NONE;
	}
	if (peer->debugger_attached || peer->busy || peer->heartbeat_qpc == 0 || qpc_frequency == 0)
		return UE_BRIDGE_ACTION_NONE;
	if (now_qpc <= peer->heartbeat_qpc)
		return UE_BRIDGE_ACTION_NONE;
	age_ms = (now_qpc - peer->heartbeat_qpc) / (qpc_frequency / 1000u ? qpc_frequency / 1000u : 1u);
	timeout_ms = peer->hang_timeout_ms ? peer->hang_timeout_ms : UE_BRIDGE_DEFAULT_HANG_TIMEOUT_MS;
	if (age_ms <= timeout_ms)
		return UE_BRIDGE_ACTION_NONE;
	return hung_action(peer);
}

int ue_bridge_policy_shuts_down(enum ue_bridge_action action, int continue_on_peer_exit)
{
	if (action == UE_BRIDGE_ACTION_NONE || action == UE_BRIDGE_ACTION_PEER_CRASHING)
		return 0;
	return !continue_on_peer_exit;
}

uint32_t ue_bridge_crashing_for_ms(struct ue_bridge_crash_clock *clock, int crashing, uint32_t now_ms)
{
	if (!crashing)
	{
		clock->active = 0;
		return 0;
	}
	if (!clock->active)
	{
		clock->active = 1;
		clock->since_ms = now_ms;
		return 0;
	}
	return now_ms - clock->since_ms;
}

const char *ue_bridge_action_name(enum ue_bridge_action action)
{
	switch (action)
	{
	case UE_BRIDGE_ACTION_NONE: return "none";
	case UE_BRIDGE_ACTION_PEER_EXITED: return "peer_exited";
	case UE_BRIDGE_ACTION_PEER_CRASHED: return "peer_crashed";
	case UE_BRIDGE_ACTION_PEER_CRASHING: return "peer_crashing";
	case UE_BRIDGE_ACTION_PEER_HUNG: return "peer_hung";
	case UE_BRIDGE_ACTION_PEER_HUNG_EDITOR: return "peer_hung_editor";
	}
	return "unknown";
}
