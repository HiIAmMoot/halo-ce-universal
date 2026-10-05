/*
UEB_TEST.H

A minimal test runner for the UE bridge's C code (port/ue_bridge). Each test
file defines one table of tests ending in { 0, 0 }; test_main.c lists the
tables.
*/

#ifndef UEB_TEST_H
#define UEB_TEST_H

#include <stdio.h>

extern int ueb_test_failed;

#define UEB_CHECK(condition) \
	do \
	{ \
		if (!(condition)) \
		{ \
			printf("  check failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__); \
			ueb_test_failed = 1; \
			return; \
		} \
	} while (0)

typedef void (*ueb_test_function)(void);

struct ueb_test
{
	const char *name;
	ueb_test_function function;
};

extern const struct ueb_test ueb_format_tests[];
extern const struct ueb_test ueb_ring_tests[];
extern const struct ueb_test ueb_load_tests[];
extern const struct ueb_test ueb_policy_tests[];
extern const struct ueb_test ueb_frame_rate_tests[];
extern const struct ueb_test ueb_core_tests[];
extern const struct ueb_test ueb_game_tests[];

#endif
