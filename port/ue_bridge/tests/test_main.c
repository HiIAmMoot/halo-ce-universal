/*
TEST_MAIN.C

ue_bridge_tests.exe [filter]: runs every test whose name contains filter (all
of them without one). Exits 0 when all pass, 1 when any fails, 2 when the
filter matched nothing (so a typo in a mutant's test name can't pass).
*/

#include "ueb_test.h"

#include <string.h>

int ueb_test_failed;

static const struct ueb_test *const suites[] =
{
	ueb_format_tests,
	ueb_ring_tests,
	0
};

int main(int argc, char **argv)
{
	const char *filter = argc >= 2 ? argv[1] : 0;
	int suite_index;
	int run = 0;
	int failures = 0;

	for (suite_index = 0; suites[suite_index]; suite_index++)
	{
		const struct ueb_test *test;

		for (test = suites[suite_index]; test->name; test++)
		{
			if (filter && !strstr(test->name, filter))
				continue;
			ueb_test_failed = 0;
			test->function();
			run++;
			failures += ueb_test_failed;
			printf("%s %s\n", ueb_test_failed ? "FAIL" : "PASS", test->name);
		}
	}
	printf("%d run, %d failed\n", run, failures);
	if (failures)
		return 1;
	return run ? 0 : 2;
}
