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

/* ---------- suspension (sleep and resume) */

/* one pass of a watcher at the given time, with the peer's heartbeat as it read it */
static int suspend_pass(struct ue_bridge_suspend_guard *guard, uint64_t now_qpc, uint64_t *heartbeat_qpc)
{
	return ue_bridge_suspend_guard_pass(guard, now_qpc, FREQUENCY, heartbeat_qpc);
}

static void policy_suspend_the_first_pass_is_never_a_suspension(void)
{
	struct ue_bridge_suspend_guard guard;
	uint64_t heartbeat = SECONDS(1);

	memset(&guard, 0, sizeof(guard));
	UEB_CHECK(suspend_pass(&guard, SECONDS(5000), &heartbeat) == 0);
	UEB_CHECK(heartbeat == SECONDS(1));
}

static void policy_suspend_ordinary_passes_change_nothing(void)
{
	struct ue_bridge_suspend_guard guard;
	uint64_t heartbeat = SECONDS(100);

	memset(&guard, 0, sizeof(guard));
	UEB_CHECK(suspend_pass(&guard, SECONDS(100), &heartbeat) == 0);
	UEB_CHECK(suspend_pass(&guard, SECONDS(100) + FREQUENCY / 4, &heartbeat) == 0);
	UEB_CHECK(suspend_pass(&guard, SECONDS(100) + FREQUENCY / 2, &heartbeat) == 0);
	UEB_CHECK(heartbeat == SECONDS(100));
}

static void policy_suspend_a_gap_at_the_threshold_is_not_one_and_a_tick_over_is(void)
{
	struct ue_bridge_suspend_guard guard;
	uint64_t heartbeat = SECONDS(1);
	uint64_t threshold = FREQUENCY / 1000u * UE_BRIDGE_SUSPEND_GAP_MS;

	memset(&guard, 0, sizeof(guard));
	UEB_CHECK(suspend_pass(&guard, SECONDS(100), &heartbeat) == 0);
	UEB_CHECK(suspend_pass(&guard, SECONDS(100) + threshold, &heartbeat) == 0);
	UEB_CHECK(heartbeat == SECONDS(1));
	UEB_CHECK(suspend_pass(&guard, SECONDS(100) + threshold + threshold + 1, &heartbeat) == 1);
}

static void policy_suspend_a_large_gap_rebases_the_peers_heartbeat_to_the_resume(void)
{
	struct ue_bridge_suspend_guard guard;
	uint64_t heartbeat = SECONDS(100);
	uint64_t resume = SECONDS(100 + 3600);
	struct ue_bridge_peer_view peer = alive_peer();

	memset(&guard, 0, sizeof(guard));
	UEB_CHECK(suspend_pass(&guard, SECONDS(100), &heartbeat) == 0);
	UEB_CHECK(suspend_pass(&guard, resume, &heartbeat) == 1);
	UEB_CHECK(heartbeat == resume);
	/* the peer judged by it has a whole timeout from the resume, and no longer */
	peer.heartbeat_qpc = heartbeat;
	UEB_CHECK(ue_bridge_policy_decide(&peer, resume + SECONDS(9), FREQUENCY) == UE_BRIDGE_ACTION_NONE);
	UEB_CHECK(ue_bridge_policy_decide(&peer, resume + SECONDS(11), FREQUENCY) == UE_BRIDGE_ACTION_PEER_HUNG);
}

static void policy_suspend_the_rebase_holds_on_the_passes_after_the_resume(void)
{
	struct ue_bridge_suspend_guard guard;
	uint64_t heartbeat = SECONDS(100);
	uint64_t resume = SECONDS(5000);

	memset(&guard, 0, sizeof(guard));
	suspend_pass(&guard, SECONDS(100), &heartbeat);
	UEB_CHECK(suspend_pass(&guard, resume, &heartbeat) == 1);
	/* the peer still shows its old stamp: the next pass is no gap, but must not undo the rebase */
	heartbeat = SECONDS(100);
	UEB_CHECK(suspend_pass(&guard, resume + FREQUENCY / 4, &heartbeat) == 0);
	UEB_CHECK(heartbeat == resume);
	/* a peer that beat after the resume keeps its own, newer stamp */
	heartbeat = resume + FREQUENCY / 2;
	UEB_CHECK(suspend_pass(&guard, resume + FREQUENCY / 2, &heartbeat) == 0);
	UEB_CHECK(heartbeat == resume + FREQUENCY / 2);
}

static void policy_suspend_a_peer_without_a_heartbeat_stays_without_one(void)
{
	struct ue_bridge_suspend_guard guard;
	uint64_t heartbeat = 0;

	memset(&guard, 0, sizeof(guard));
	suspend_pass(&guard, SECONDS(100), &heartbeat);
	UEB_CHECK(suspend_pass(&guard, SECONDS(9000), &heartbeat) == 1);
	UEB_CHECK(heartbeat == 0);
}

static void policy_suspend_is_wrap_safe(void)
{
	struct ue_bridge_suspend_guard guard;
	uint64_t heartbeat = 1;
	uint64_t before_wrap = ~(uint64_t)0 - 10u;

	memset(&guard, 0, sizeof(guard));
	suspend_pass(&guard, before_wrap, &heartbeat);
	/* 111 ticks later across the wrap: no gap */
	UEB_CHECK(suspend_pass(&guard, 100u, &heartbeat) == 0);
	UEB_CHECK(heartbeat == 1);
	/* and a big one across it */
	UEB_CHECK(suspend_pass(&guard, 100u + SECONDS(60), &heartbeat) == 1);
}

static void policy_suspend_a_zero_frequency_never_reports_one(void)
{
	struct ue_bridge_suspend_guard guard;
	uint64_t heartbeat = SECONDS(1);

	memset(&guard, 0, sizeof(guard));
	UEB_CHECK(ue_bridge_suspend_guard_pass(&guard, SECONDS(1), 0, &heartbeat) == 0);
	UEB_CHECK(ue_bridge_suspend_guard_pass(&guard, SECONDS(9000), 0, &heartbeat) == 0);
	UEB_CHECK(heartbeat == SECONDS(1));
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
	{ "policy_suspend_the_first_pass_is_never_a_suspension", policy_suspend_the_first_pass_is_never_a_suspension },
	{ "policy_suspend_ordinary_passes_change_nothing", policy_suspend_ordinary_passes_change_nothing },
	{ "policy_suspend_a_gap_at_the_threshold_is_not_one_and_a_tick_over_is", policy_suspend_a_gap_at_the_threshold_is_not_one_and_a_tick_over_is },
	{ "policy_suspend_a_large_gap_rebases_the_peers_heartbeat_to_the_resume", policy_suspend_a_large_gap_rebases_the_peers_heartbeat_to_the_resume },
	{ "policy_suspend_the_rebase_holds_on_the_passes_after_the_resume", policy_suspend_the_rebase_holds_on_the_passes_after_the_resume },
	{ "policy_suspend_a_peer_without_a_heartbeat_stays_without_one", policy_suspend_a_peer_without_a_heartbeat_stays_without_one },
	{ "policy_suspend_is_wrap_safe", policy_suspend_is_wrap_safe },
	{ "policy_suspend_a_zero_frequency_never_reports_one", policy_suspend_a_zero_frequency_never_reports_one },
	{ 0, 0 }
};
