/*
TEST_POLICY.C

ue_bridge_policy_decide: exit, crash, crashing, hang, and every condition that
must suppress a hang.
*/

#include "ueb_test.h"
#include "ue_bridge_policy.h"

#include <string.h>

#define FREQUENCY 10000000ull
#define SECONDS(s) ((uint64_t)(s) * FREQUENCY)

static struct ue_bridge_peer_view alive_peer(void)
{
	struct ue_bridge_peer_view peer;

	memset(&peer, 0, sizeof(peer));
	peer.heartbeat_qpc = SECONDS(100);
	return peer;
}

static void policy_alive_and_fresh_is_none(void)
{
	struct ue_bridge_peer_view peer = alive_peer();

	UEB_CHECK(ue_bridge_policy_decide(&peer, SECONDS(101), FREQUENCY) == UE_BRIDGE_ACTION_NONE);
}

static void policy_exit_code_zero_is_exited(void)
{
	struct ue_bridge_peer_view peer = alive_peer();

	peer.process_exited = 1;
	peer.exit_code = 0;
	UEB_CHECK(ue_bridge_policy_decide(&peer, SECONDS(101), FREQUENCY) == UE_BRIDGE_ACTION_PEER_EXITED);
}

static void policy_exit_code_one_is_exited(void)
{
	struct ue_bridge_peer_view peer = alive_peer();

	/* taskkill /F ends a process with exit code 1: an exit, not a crash */
	peer.process_exited = 1;
	peer.exit_code = 1;
	UEB_CHECK(ue_bridge_policy_decide(&peer, SECONDS(101), FREQUENCY) == UE_BRIDGE_ACTION_PEER_EXITED);
}

static void policy_access_violation_exit_is_crashed(void)
{
	struct ue_bridge_peer_view peer = alive_peer();

	peer.process_exited = 1;
	peer.exit_code = 0xC0000005u;
	UEB_CHECK(ue_bridge_policy_decide(&peer, SECONDS(101), FREQUENCY) == UE_BRIDGE_ACTION_PEER_CRASHED);
}

static void policy_published_crash_is_crashed_whatever_the_exit_code(void)
{
	struct ue_bridge_peer_view peer = alive_peer();

	peer.process_exited = 1;
	peer.exit_code = 3;
	peer.stopping = UE_BRIDGE_STOP_CRASH;
	UEB_CHECK(ue_bridge_policy_decide(&peer, SECONDS(101), FREQUENCY) == UE_BRIDGE_ACTION_PEER_CRASHED);
}

static void policy_exited_wins_over_a_stale_heartbeat(void)
{
	struct ue_bridge_peer_view peer = alive_peer();

	peer.process_exited = 1;
	UEB_CHECK(ue_bridge_policy_decide(&peer, SECONDS(1000), FREQUENCY) == UE_BRIDGE_ACTION_PEER_EXITED);
}

static void policy_crashing_peer_is_crashing(void)
{
	struct ue_bridge_peer_view peer = alive_peer();

	peer.crashing = 1;
	UEB_CHECK(ue_bridge_policy_decide(&peer, SECONDS(101), FREQUENCY) == UE_BRIDGE_ACTION_PEER_CRASHING);
	/* a crashing peer stops heartbeating while it waits: that is not a hang */
	UEB_CHECK(ue_bridge_policy_decide(&peer, SECONDS(200), FREQUENCY) == UE_BRIDGE_ACTION_PEER_CRASHING);
}

static void policy_crashing_too_long_is_hung(void)
{
	struct ue_bridge_peer_view peer = alive_peer();

	peer.crashing = 1;
	peer.crashing_for_ms = UE_BRIDGE_CRASHING_LIMIT_MS;
	UEB_CHECK(ue_bridge_policy_decide(&peer, SECONDS(101), FREQUENCY) == UE_BRIDGE_ACTION_PEER_CRASHING);
	peer.crashing_for_ms = UE_BRIDGE_CRASHING_LIMIT_MS + 1;
	UEB_CHECK(ue_bridge_policy_decide(&peer, SECONDS(101), FREQUENCY) == UE_BRIDGE_ACTION_PEER_HUNG);
	peer.is_editor = 1;
	UEB_CHECK(ue_bridge_policy_decide(&peer, SECONDS(101), FREQUENCY) == UE_BRIDGE_ACTION_PEER_HUNG_EDITOR);
}

static void policy_debugger_suppresses_crashing_too_long(void)
{
	struct ue_bridge_peer_view peer = alive_peer();

	/* a breakpoint in the crash hook must not shut the pair down */
	peer.crashing = 1;
	peer.crashing_for_ms = UE_BRIDGE_CRASHING_LIMIT_MS + 1;
	peer.debugger_attached = 1;
	UEB_CHECK(ue_bridge_policy_decide(&peer, SECONDS(101), FREQUENCY) == UE_BRIDGE_ACTION_PEER_CRASHING);
	peer.is_editor = 1;
	UEB_CHECK(ue_bridge_policy_decide(&peer, SECONDS(101), FREQUENCY) == UE_BRIDGE_ACTION_PEER_CRASHING);
}

static void policy_exiting_live_peer_is_left_alone_within_the_limit(void)
{
	struct ue_bridge_peer_view peer = alive_peer();

	/* a UE that published EXIT is still writing its config and caches: no heartbeats, not a hang */
	peer.stopping = UE_BRIDGE_STOP_EXIT;
	peer.crashing_for_ms = 0;
	UEB_CHECK(ue_bridge_policy_decide(&peer, SECONDS(1000), FREQUENCY) == UE_BRIDGE_ACTION_NONE);
	peer.crashing_for_ms = UE_BRIDGE_CRASHING_LIMIT_MS;
	UEB_CHECK(ue_bridge_policy_decide(&peer, SECONDS(1000), FREQUENCY) == UE_BRIDGE_ACTION_NONE);
}

static void policy_exiting_live_peer_past_the_limit_is_hung(void)
{
	struct ue_bridge_peer_view peer = alive_peer();

	peer.stopping = UE_BRIDGE_STOP_EXIT;
	peer.crashing_for_ms = UE_BRIDGE_CRASHING_LIMIT_MS + 1;
	UEB_CHECK(ue_bridge_policy_decide(&peer, SECONDS(1000), FREQUENCY) == UE_BRIDGE_ACTION_PEER_HUNG);
	peer.is_editor = 1;
	UEB_CHECK(ue_bridge_policy_decide(&peer, SECONDS(1000), FREQUENCY) == UE_BRIDGE_ACTION_PEER_HUNG_EDITOR);
}

static void policy_debugger_suppresses_an_exiting_peer_hang(void)
{
	struct ue_bridge_peer_view peer = alive_peer();

	peer.stopping = UE_BRIDGE_STOP_EXIT;
	peer.crashing_for_ms = UE_BRIDGE_CRASHING_LIMIT_MS + 1;
	peer.debugger_attached = 1;
	UEB_CHECK(ue_bridge_policy_decide(&peer, SECONDS(1000), FREQUENCY) == UE_BRIDGE_ACTION_NONE);
}

static void policy_exiting_peer_that_exited_is_judged_by_its_exit_code(void)
{
	struct ue_bridge_peer_view peer = alive_peer();

	peer.stopping = UE_BRIDGE_STOP_EXIT;
	peer.crashing_for_ms = UE_BRIDGE_CRASHING_LIMIT_MS + 1;
	peer.process_exited = 1;
	UEB_CHECK(ue_bridge_policy_decide(&peer, SECONDS(1000), FREQUENCY) == UE_BRIDGE_ACTION_PEER_EXITED);
	peer.exit_code = 0xC0000005u;
	UEB_CHECK(ue_bridge_policy_decide(&peer, SECONDS(1000), FREQUENCY) == UE_BRIDGE_ACTION_PEER_CRASHED);
}

static void policy_winding_down_is_crashing_or_stopping(void)
{
	struct ue_bridge_peer_view peer = alive_peer();

	UEB_CHECK(!ue_bridge_peer_winding_down(&peer));
	peer.crashing = 1;
	UEB_CHECK(ue_bridge_peer_winding_down(&peer));
	peer.crashing = 0;
	peer.stopping = UE_BRIDGE_STOP_EXIT;
	UEB_CHECK(ue_bridge_peer_winding_down(&peer));
	peer.stopping = UE_BRIDGE_STOP_CRASH;
	UEB_CHECK(ue_bridge_peer_winding_down(&peer));
}

static void policy_stale_heartbeat_is_hung(void)
{
	struct ue_bridge_peer_view peer = alive_peer();

	/* 10 001 ms: just past the 10 s default */
	UEB_CHECK(ue_bridge_policy_decide(&peer, SECONDS(110) + FREQUENCY / 1000u, FREQUENCY) == UE_BRIDGE_ACTION_PEER_HUNG);
}

static void policy_stall_below_peer_timeout_is_not_a_hang(void)
{
	struct ue_bridge_peer_view peer = alive_peer();

	/* exactly the timeout is not yet a hang */
	UEB_CHECK(ue_bridge_policy_decide(&peer, SECONDS(110), FREQUENCY) == UE_BRIDGE_ACTION_NONE);
	/* an editor publishes 60 s: a 30 s shader compile is not a hang */
	peer.hang_timeout_ms = 60000;
	peer.is_editor = 1;
	UEB_CHECK(ue_bridge_policy_decide(&peer, SECONDS(130), FREQUENCY) == UE_BRIDGE_ACTION_NONE);
	UEB_CHECK(ue_bridge_policy_decide(&peer, SECONDS(161), FREQUENCY) == UE_BRIDGE_ACTION_PEER_HUNG_EDITOR);
}

static void policy_debugger_suppresses_hang(void)
{
	struct ue_bridge_peer_view peer = alive_peer();

	peer.debugger_attached = 1;
	UEB_CHECK(ue_bridge_policy_decide(&peer, SECONDS(10000), FREQUENCY) == UE_BRIDGE_ACTION_NONE);
}

static void policy_busy_suppresses_hang(void)
{
	struct ue_bridge_peer_view peer = alive_peer();

	peer.busy = 1;
	UEB_CHECK(ue_bridge_policy_decide(&peer, SECONDS(10000), FREQUENCY) == UE_BRIDGE_ACTION_NONE);
}

static void policy_no_heartbeat_yet_is_not_a_hang(void)
{
	struct ue_bridge_peer_view peer = alive_peer();

	peer.heartbeat_qpc = 0;
	UEB_CHECK(ue_bridge_policy_decide(&peer, SECONDS(10000), FREQUENCY) == UE_BRIDGE_ACTION_NONE);
}

static void policy_heartbeat_ahead_of_now_is_not_a_hang(void)
{
	struct ue_bridge_peer_view peer = alive_peer();

	/* the watcher read now just before the peer's newer heartbeat */
	UEB_CHECK(ue_bridge_policy_decide(&peer, SECONDS(99), FREQUENCY) == UE_BRIDGE_ACTION_NONE);
}

static void policy_zero_frequency_is_never_a_hang(void)
{
	struct ue_bridge_peer_view peer = alive_peer();

	UEB_CHECK(ue_bridge_policy_decide(&peer, SECONDS(10000), 0) == UE_BRIDGE_ACTION_NONE);
}

static void policy_editor_hang_is_hung_editor(void)
{
	struct ue_bridge_peer_view peer = alive_peer();

	peer.is_editor = 1;
	UEB_CHECK(ue_bridge_policy_decide(&peer, SECONDS(110) + FREQUENCY / 1000u, FREQUENCY) == UE_BRIDGE_ACTION_PEER_HUNG_EDITOR);
}

static void policy_shutdown_follows_continue_mode(void)
{
	UEB_CHECK(!ue_bridge_policy_shuts_down(UE_BRIDGE_ACTION_NONE, 0));
	UEB_CHECK(!ue_bridge_policy_shuts_down(UE_BRIDGE_ACTION_PEER_CRASHING, 0));
	UEB_CHECK(ue_bridge_policy_shuts_down(UE_BRIDGE_ACTION_PEER_EXITED, 0));
	UEB_CHECK(ue_bridge_policy_shuts_down(UE_BRIDGE_ACTION_PEER_CRASHED, 0));
	UEB_CHECK(ue_bridge_policy_shuts_down(UE_BRIDGE_ACTION_PEER_HUNG, 0));
	UEB_CHECK(ue_bridge_policy_shuts_down(UE_BRIDGE_ACTION_PEER_HUNG_EDITOR, 0));
	UEB_CHECK(!ue_bridge_policy_shuts_down(UE_BRIDGE_ACTION_PEER_EXITED, 1));
	UEB_CHECK(!ue_bridge_policy_shuts_down(UE_BRIDGE_ACTION_PEER_HUNG, 1));
}

static void policy_exit_code_classification(void)
{
	UEB_CHECK(!ue_bridge_exit_code_is_crash(0));
	UEB_CHECK(!ue_bridge_exit_code_is_crash(1));
	UEB_CHECK(!ue_bridge_exit_code_is_crash(0xBFFFFFFFu));
	UEB_CHECK(ue_bridge_exit_code_is_crash(0xC0000000u));
	UEB_CHECK(ue_bridge_exit_code_is_crash(0xC0000409u));
}

static void policy_action_names(void)
{
	UEB_CHECK(strcmp(ue_bridge_action_name(UE_BRIDGE_ACTION_NONE), "none") == 0);
	UEB_CHECK(strcmp(ue_bridge_action_name(UE_BRIDGE_ACTION_PEER_HUNG_EDITOR), "peer_hung_editor") == 0);
	UEB_CHECK(strcmp(ue_bridge_action_name((enum ue_bridge_action)99), "unknown") == 0);
}

static void policy_crash_clock_first_sighting_at_an_even_time_is_zero(void)
{
	struct ue_bridge_crash_clock clock;

	memset(&clock, 0, sizeof(clock));
	UEB_CHECK(ue_bridge_crashing_for_ms(&clock, 1, 1000u) == 0);
	UEB_CHECK(ue_bridge_crashing_for_ms(&clock, 1, 1000u) == 0);
}

static void policy_crash_clock_elapsed_time_grows(void)
{
	struct ue_bridge_crash_clock clock;

	memset(&clock, 0, sizeof(clock));
	UEB_CHECK(ue_bridge_crashing_for_ms(&clock, 1, 5000u) == 0);
	UEB_CHECK(ue_bridge_crashing_for_ms(&clock, 1, 5250u) == 250u);
	UEB_CHECK(ue_bridge_crashing_for_ms(&clock, 1, 8000u) == 3000u);
}

static void policy_crash_clock_resets_when_crashing_stops(void)
{
	struct ue_bridge_crash_clock clock;

	memset(&clock, 0, sizeof(clock));
	UEB_CHECK(ue_bridge_crashing_for_ms(&clock, 1, 5000u) == 0);
	UEB_CHECK(ue_bridge_crashing_for_ms(&clock, 0, 6000u) == 0);
	UEB_CHECK(ue_bridge_crashing_for_ms(&clock, 1, 7000u) == 0);
	UEB_CHECK(ue_bridge_crashing_for_ms(&clock, 1, 7100u) == 100u);
}

static void policy_crash_clock_is_wrap_safe(void)
{
	struct ue_bridge_crash_clock clock;

	memset(&clock, 0, sizeof(clock));
	UEB_CHECK(ue_bridge_crashing_for_ms(&clock, 1, 0xFFFFFF00u) == 0);
	UEB_CHECK(ue_bridge_crashing_for_ms(&clock, 1, 0x00000100u) == 0x200u);
}

const struct ueb_test ueb_policy_tests[] =
{
	{ "policy_alive_and_fresh_is_none", policy_alive_and_fresh_is_none },
	{ "policy_exit_code_zero_is_exited", policy_exit_code_zero_is_exited },
	{ "policy_exit_code_one_is_exited", policy_exit_code_one_is_exited },
	{ "policy_access_violation_exit_is_crashed", policy_access_violation_exit_is_crashed },
	{ "policy_published_crash_is_crashed_whatever_the_exit_code", policy_published_crash_is_crashed_whatever_the_exit_code },
	{ "policy_exited_wins_over_a_stale_heartbeat", policy_exited_wins_over_a_stale_heartbeat },
	{ "policy_crashing_peer_is_crashing", policy_crashing_peer_is_crashing },
	{ "policy_crashing_too_long_is_hung", policy_crashing_too_long_is_hung },
	{ "policy_debugger_suppresses_crashing_too_long", policy_debugger_suppresses_crashing_too_long },
	{ "policy_exiting_live_peer_is_left_alone_within_the_limit", policy_exiting_live_peer_is_left_alone_within_the_limit },
	{ "policy_exiting_live_peer_past_the_limit_is_hung", policy_exiting_live_peer_past_the_limit_is_hung },
	{ "policy_debugger_suppresses_an_exiting_peer_hang", policy_debugger_suppresses_an_exiting_peer_hang },
	{ "policy_exiting_peer_that_exited_is_judged_by_its_exit_code", policy_exiting_peer_that_exited_is_judged_by_its_exit_code },
	{ "policy_winding_down_is_crashing_or_stopping", policy_winding_down_is_crashing_or_stopping },
	{ "policy_stale_heartbeat_is_hung", policy_stale_heartbeat_is_hung },
	{ "policy_stall_below_peer_timeout_is_not_a_hang", policy_stall_below_peer_timeout_is_not_a_hang },
	{ "policy_debugger_suppresses_hang", policy_debugger_suppresses_hang },
	{ "policy_busy_suppresses_hang", policy_busy_suppresses_hang },
	{ "policy_no_heartbeat_yet_is_not_a_hang", policy_no_heartbeat_yet_is_not_a_hang },
	{ "policy_heartbeat_ahead_of_now_is_not_a_hang", policy_heartbeat_ahead_of_now_is_not_a_hang },
	{ "policy_zero_frequency_is_never_a_hang", policy_zero_frequency_is_never_a_hang },
	{ "policy_editor_hang_is_hung_editor", policy_editor_hang_is_hung_editor },
	{ "policy_shutdown_follows_continue_mode", policy_shutdown_follows_continue_mode },
	{ "policy_exit_code_classification", policy_exit_code_classification },
	{ "policy_action_names", policy_action_names },
	{ "policy_crash_clock_first_sighting_at_an_even_time_is_zero", policy_crash_clock_first_sighting_at_an_even_time_is_zero },
	{ "policy_crash_clock_elapsed_time_grows", policy_crash_clock_elapsed_time_grows },
	{ "policy_crash_clock_resets_when_crashing_stops", policy_crash_clock_resets_when_crashing_stops },
	{ "policy_crash_clock_is_wrap_safe", policy_crash_clock_is_wrap_safe },
	{ 0, 0 }
};
