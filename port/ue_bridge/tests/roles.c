/*
ROLES.C

ue_bridge_roles.exe --role=<role> [options]: stand-in processes for the UE
bridge's cross-process tests (tools/test_ue_bridge.py, and HaloCEUE's
Scripts/pair_test.py).

fake-game: the real core and Windows layer (win32_ue_bridge.c), heartbeating
  and publishing like the game.
  --log PATH          the debug.txt stand-in (its folder takes self-dumps)
  --ready-file PATH   written with the PID once the bridge is up
  --run-ms N          exits normally after N ms (default 30000)
  --crash-after-ms N  raises an access violation after N ms
  --hang-after-ms N   stops heartbeating and publishing after N ms (stays alive)
  --overflow-after-ms N  overflows the stack after N ms
  --heap-lock-crash-after-ms N  takes the process heap's lock, then raises an
                      access violation, after N ms (heap corruption faults inside
                      the heap with its lock held)
  --exit-when PATH    exits normally once PATH exists
  --crash-when PATH   raises an access violation once PATH exists
  --hang-when PATH    stops heartbeating and publishing once PATH exists
  --crash-after-stop  stops the bridge (the crash hook stays installed), raises on this
                      thread and then on a second one, and exits 0 when both returned
                      (9 when the second thread is parked)
  --refresh-hz N      published as the display's refresh rate (default 0: unknown)
  --frame-target-hz N published as the frame-rate target (default 0: uncapped)
  --target-file PATH  while PATH exists, its number is republished as the target
                      whenever it changes (not while hanging); the numbers are
                      clamped to >= 0
  --continue          on_peer_exit = continue
  --disabled          ue_bridge.enabled = false
  --cycles N          starts and stops the bridge N times in a row (no watcher,
                      crash hook or heartbeat), then exits
  --go-file PATH      with --cycles: waits for this file after --ready-file
  --hold-ms N         with --cycles: stays N ms inside every directory lock, and
                      counts the times another game was inside it too (exit 6)

fake-ue: a renderer that attaches through the directory.
  --session-dir PATH  created and published as ue_session_dir
  --ready-file PATH   written with the PID once attached
  --run-ms N          exits normally after N ms (default 30000)
  --exit-after-ms N   publishes ue_stopping = exit and exits after N ms
  --crash-after-ms N  publishes ue_stopping = crash after N ms, stops
                      heartbeating, waits --crash-linger-ms (default 0) as a
                      crash reporter would, then raises an access violation
  --crash-linger-ms N
  --hang-after-ms N   stops heartbeating after N ms (stays alive)
  --busy-flag         published as ue_busy
  --hang-timeout-ms N published as ue_hang_timeout_ms
  --editor            published as ue_is_editor
  --debugger-flag     published as ue_debugger_attached
  --dump-on-crashing  dumps the game to game_crash.dmp when it is crashing,
                      then sets ue_dump_done (without it, ignores the crash)
  It exits when the game's process does.

probe-directory: exits 0 when no bridge directory exists, 1 when one does.
  --print             also prints the entry as key=value lines (game_pid,
                      session_id, section, game_log_path)
  --check-ms N        instead: creates the directory itself and reads the entry
                      in a loop (at most N ms, or until --stop-file exists),
                      counting reads that are neither empty nor a game's own
                      name; prints reads, incoherent and final (empty or the
                      pid); exits 5 when any read was incoherent
  --stop-file PATH
  --ready-file PATH   written with the PID once reading
*/

#include "../../linux/src/ue_bridge_platform.h"
#include "ue_bridge_ring.h"

#include <windows.h>
#include <dbghelp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the smallest section the game allows: every role that starts the core
and the reader that maps it agree on this size */
#define ROLE_SECTION_SIZE (UE_BRIDGE_MIN_SECTION_MB << 20)
#define ROLE_TICK_SLOT_SIZE 0x10000u

static FILE *role_log;

/* the real game's platform_log is stderr: it never reaches debug.txt, so here it doesn't reach the log file either */
void platform_log(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vfprintf(stdout, format, arguments);
	va_end(arguments);
	fputc('\n', stdout);
	fflush(stdout);
}

/* errors.c's, the game's debug.txt: the role's log file */
void write_to_error_file(char *string, unsigned char date)
{
	(void)date;
	if (role_log)
	{
		fputs(string, role_log);
		fflush(role_log);
	}
}

/* a line the fake game itself writes to its debug.txt, as the game's error() does */
static void game_debug_line(const char *format, ...)
{
	char line[256];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	write_to_error_file(line, 1);
	write_to_error_file("\r\n", 1);
}

static const char *option_text(int argc, char **argv, const char *name)
{
	int index;

	for (index = 2; index + 1 < argc; index++)
	{
		if (strcmp(argv[index], name) == 0)
			return argv[index + 1];
	}
	return 0;
}

static long option_number(int argc, char **argv, const char *name, long fallback)
{
	const char *text = option_text(argc, argv, name);

	return text ? strtol(text, 0, 10) : fallback;
}

static int option_flag(int argc, char **argv, const char *name)
{
	int index;

	for (index = 2; index < argc; index++)
	{
		if (strcmp(argv[index], name) == 0)
			return 1;
	}
	return 0;
}

static long non_negative(long value)
{
	return value < 0 ? 0 : value;
}

static long read_number_file(const char *path, long fallback)
{
	FILE *file = path ? fopen(path, "r") : 0;
	long value = fallback;

	if (!file)
		return fallback;
	if (fscanf(file, "%ld", &value) != 1)
		value = fallback;
	fclose(file);
	return non_negative(value);
}

static int file_exists(const char *path)
{
	return path && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static void write_ready_file(const char *path)
{
	FILE *file;

	if (!path)
		return;
	file = fopen(path, "w");
	if (file)
	{
		fprintf(file, "%lu\n", GetCurrentProcessId());
		fclose(file);
	}
}

/* continuable (flags 0), so that a filter returning EXCEPTION_CONTINUE_EXECUTION resumes after it */
static void raise_access_violation(void)
{
	RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, 0);
}

static int (*volatile recurse_target)(int);

static int recurse(int depth)
{
	volatile char padding[512];

	padding[0] = (char)depth;
	return recurse_target(depth + 1) + padding[0];
}

/* the game's own unhandled-exception filter, which the bridge's hook must chain to */
static LONG WINAPI game_filter(EXCEPTION_POINTERS *exception)
{
	game_debug_line("crash: %08lx", (unsigned long)exception->ExceptionRecord->ExceptionCode);
	return EXCEPTION_CONTINUE_EXECUTION;
}

/* ---------- fake-game */

static DWORD WINAPI raise_on_this_thread(void *unused)
{
	(void)unused;
	raise_access_violation();
	return 0;
}

/* The hook outlives the bridge (a failed watcher start, at_exit). Both faults find no header, so
the filter returns at once; the first must release the crash ownership, or the second thread parks. */
static int crash_after_stop(void)
{
	HANDLE thread;

	raise_access_violation();
	thread = CreateThread(NULL, 0, raise_on_this_thread, NULL, 0, NULL);
	if (!thread)
		return 8;
	return WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0 ? 0 : 9;
}

static volatile LONG quit_requested;

static void fake_game_request_quit(void)
{
	InterlockedExchange(&quit_requested, 1);
}

static const struct ue_bridge_os *real_os;
static struct ue_bridge_os counting_os;
static volatile LONG *inside_count;
static long hold_ms;
static long overlaps;

/* every game's directory lock goes through one counter shared by the games: it
reads above 1 only when two are inside the lock at once */
static void counting_lock(void)
{
	real_os->lock_directory();
	if (InterlockedIncrement(inside_count) != 1)
		overlaps++;
	Sleep((DWORD)hold_ms);
}

static void counting_unlock(void)
{
	InterlockedDecrement(inside_count);
	real_os->unlock_directory();
}

static int fake_game_cycles(const struct ue_bridge_settings *settings, long cycles, long hold, const char *ready_file, const char *go_file)
{
	const struct ue_bridge_os *os = ue_bridge_platform_os();
	long cycle;

	if (hold > 0)
	{
		HANDLE counter = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(LONG), "Local\\HaloCEUE.Roles.InsideCount");

		inside_count = counter ? (volatile LONG *)MapViewOfFile(counter, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LONG)) : NULL;
		if (!inside_count)
			return 5;
		real_os = os;
		counting_os = *os;
		counting_os.lock_directory = counting_lock;
		counting_os.unlock_directory = counting_unlock;
		os = &counting_os;
		hold_ms = hold;
	}
	write_ready_file(ready_file);
	/* both games of a concurrency test must start cycling together */
	while (go_file && GetFileAttributesA(go_file) == INVALID_FILE_ATTRIBUTES)
		Sleep(1);
	for (cycle = 0; cycle < cycles; cycle++)
	{
		if (!ue_bridge_start(settings, os))
			return 3;
		ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
	}
	if (overlaps)
	{
		game_debug_line("fake-game: %ld directory lock overlaps", overlaps);
		return 6;
	}
	return 0;
}

static int fake_game(int argc, char **argv)
{
	struct ue_bridge_settings settings;
	struct ue_bridge_watch_config watch;
	const char *log = option_text(argc, argv, "--log");
	long run_ms = option_number(argc, argv, "--run-ms", 30000);
	long crash_after = option_number(argc, argv, "--crash-after-ms", -1);
	long hang_after = option_number(argc, argv, "--hang-after-ms", -1);
	long overflow_after = option_number(argc, argv, "--overflow-after-ms", -1);
	long heap_lock_crash_after = option_number(argc, argv, "--heap-lock-crash-after-ms", -1);
	long cycles = option_number(argc, argv, "--cycles", 0);
	const char *exit_when = option_text(argc, argv, "--exit-when");
	const char *crash_when = option_text(argc, argv, "--crash-when");
	const char *hang_when = option_text(argc, argv, "--hang-when");
	const char *target_file = option_text(argc, argv, "--target-file");
	long refresh_hz = non_negative(option_number(argc, argv, "--refresh-hz", 0));
	long target_hz = non_negative(option_number(argc, argv, "--frame-target-hz", 0));
	/* latched: the hang outlives its trigger file being deleted */
	int hanging = 0;
	const char *ended = "run time over";
	DWORD start = GetTickCount();
	DWORD last_tick = start;
	uint64_t tick = 0;
	uint64_t frame = 0;

	if (log)
		role_log = fopen(log, "w");
	settings.enabled = !option_flag(argc, argv, "--disabled");
	settings.log_path = log;
	settings.max_objects = 8192;
	settings.section_size = ROLE_SECTION_SIZE;
	settings.tick_slot_size = ROLE_TICK_SLOT_SIZE;
	if (cycles > 0)
		return fake_game_cycles(&settings, cycles, option_number(argc, argv, "--hold-ms", 0), option_text(argc, argv, "--ready-file"), option_text(argc, argv, "--go-file"));
	SetUnhandledExceptionFilter(game_filter);
	if (settings.enabled)
	{
		if (!ue_bridge_start(&settings, ue_bridge_platform_os()))
			return 3;
		ue_bridge_platform_install_crash_hook();
		watch.request_quit = fake_game_request_quit;
		watch.continue_on_peer_exit = option_flag(argc, argv, "--continue");
		if (!ue_bridge_platform_start_watcher(&watch))
			return 4;
		ue_bridge_publish_frame_rate((uint32_t)refresh_hz, (uint32_t)target_hz);
	}
	if (option_flag(argc, argv, "--crash-after-stop"))
	{
		ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
		return crash_after_stop();
	}
	write_ready_file(option_text(argc, argv, "--ready-file"));
	while (!quit_requested && (long)(GetTickCount() - start) < run_ms)
	{
		long elapsed = (long)(GetTickCount() - start);

		if (file_exists(exit_when))
		{
			ended = "exit file";
			break;
		}
		if ((crash_after >= 0 && elapsed >= crash_after) || file_exists(crash_when))
		{
			raise_access_violation();
			/* the bridge's hook must end the process; execution only returns when it didn't */
			return 7;
		}
		if (heap_lock_crash_after >= 0 && elapsed >= heap_lock_crash_after)
		{
			HeapLock(GetProcessHeap());
			raise_access_violation();
			return 7;
		}
		if (overflow_after >= 0 && elapsed >= overflow_after)
		{
			recurse_target = recurse;
			recurse(0);
			return 7;
		}
		if (file_exists(hang_when))
			hanging = 1;
		if (!hanging && (hang_after < 0 || elapsed < hang_after))
		{
			ue_bridge_heartbeat();
			if (file_exists(target_file))
			{
				long wanted = read_number_file(target_file, target_hz);

				if (wanted != target_hz)
				{
					target_hz = wanted;
					ue_bridge_publish_frame_rate((uint32_t)refresh_hz, (uint32_t)target_hz);
				}
			}
			ue_bridge_publish_frame(++frame, 0.5f);
			if (GetTickCount() - last_tick >= 33)
			{
				ue_bridge_publish_tick(++tick);
				last_tick = GetTickCount();
			}
		}
		Sleep(16);
	}
	ue_bridge_platform_stop_watcher();
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
	game_debug_line("fake-game: exiting (%s)", quit_requested ? "quit requested" : ended);
	return 0;
}

/* ---------- fake-ue */

static void dump_process(HANDLE process, DWORD pid, const char *directory, const char *name)
{
	char path[MAX_PATH];
	HANDLE file;

	snprintf(path, sizeof(path), "%s\\%s", directory, name);
	file = CreateFileA(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
	if (file == INVALID_HANDLE_VALUE)
		return;
	MiniDumpWriteDump(process, pid, file, MiniDumpWithThreadInfo, 0, 0, 0);
	CloseHandle(file);
}

static volatile struct ue_bridge_header *attach(const struct ue_bridge_os *os, volatile struct ue_bridge_directory *directory, void **section_handle)
{
	DWORD start = GetTickCount();

	while (GetTickCount() - start < 5000)
	{
		uint32_t before = ueb_load_u32(&directory->sequence);

		if (!(before & 1u) && directory->session_id)
		{
			char section_name[UE_BRIDGE_NAME_CHARS];

			memcpy(section_name, (const void *)(uintptr_t)directory->section_name, sizeof(section_name));
			section_name[sizeof(section_name) - 1] = 0;
			if (ueb_load_u32(&directory->sequence) == before)
				return (volatile struct ue_bridge_header *)os->map_section(section_name, ROLE_SECTION_SIZE, section_handle);
		}
		Sleep(20);
	}
	return 0;
}

static int fake_ue(int argc, char **argv)
{
	const struct ue_bridge_os *os = ue_bridge_platform_os();
	const char *session_dir = option_text(argc, argv, "--session-dir");
	long run_ms = option_number(argc, argv, "--run-ms", 30000);
	long exit_after = option_number(argc, argv, "--exit-after-ms", -1);
	long crash_after = option_number(argc, argv, "--crash-after-ms", -1);
	long hang_after = option_number(argc, argv, "--hang-after-ms", -1);
	long crash_linger = option_number(argc, argv, "--crash-linger-ms", 0);
	long exit_linger = option_number(argc, argv, "--exit-linger-ms", 0);
	int dump_on_crashing = option_flag(argc, argv, "--dump-on-crashing");
	volatile struct ue_bridge_directory *directory;
	volatile struct ue_bridge_header *header;
	void *directory_handle;
	void *section_handle;
	HANDLE game;
	DWORD start;
	int handled_crash = 0;

	directory = (volatile struct ue_bridge_directory *)os->map_section(UE_BRIDGE_DIRECTORY_NAME, UE_BRIDGE_DIRECTORY_SIZE, &directory_handle);
	if (!directory)
		return 3;
	header = attach(os, directory, &section_handle);
	if (!header)
		return 4;
	game = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE, FALSE, header->game_pid);
	if (session_dir)
	{
		size_t index;

		CreateDirectoryA(session_dir, 0);
		for (index = 0; index + 1 < UE_BRIDGE_PATH_BYTES && session_dir[index]; index++)
			header->ue_session_dir[index] = session_dir[index];
		header->ue_session_dir[index] = 0;
	}
	header->ue_pid = GetCurrentProcessId();
	header->ue_is_editor = option_flag(argc, argv, "--editor") ? 1u : 0u;
	header->ue_debugger_attached = option_flag(argc, argv, "--debugger-flag") ? 1u : 0u;
	header->ue_busy = option_flag(argc, argv, "--busy-flag") ? 1u : 0u;
	header->ue_hang_timeout_ms = (uint32_t)option_number(argc, argv, "--hang-timeout-ms", 0);
	ueb_store_u64(&header->ue_heartbeat_qpc, os->qpc());
	/* last: the game's watcher reads the fields above once this is set */
	ueb_store_u32(&header->ue_attached, 1);
	write_ready_file(option_text(argc, argv, "--ready-file"));
	start = GetTickCount();
	for (;;)
	{
		long elapsed = (long)(GetTickCount() - start);

		if (elapsed >= run_ms)
			break;
		if (exit_after >= 0 && elapsed >= exit_after)
		{
			ueb_store_u32(&header->ue_stopping, UE_BRIDGE_STOP_EXIT);
			/* a shutdown still writing its config: alive, silent, EXIT published */
			Sleep((DWORD)exit_linger);
			break;
		}
		if (crash_after >= 0 && elapsed >= crash_after)
		{
			ueb_store_u32(&header->ue_stopping, UE_BRIDGE_STOP_CRASH);
			Sleep((DWORD)crash_linger);
			raise_access_violation();
		}
		if (hang_after < 0 || elapsed < hang_after)
			ueb_store_u64(&header->ue_heartbeat_qpc, os->qpc());
		if (dump_on_crashing && !handled_crash && ueb_load_u32(&header->game_crashing))
		{
			if (game && session_dir)
				dump_process(game, header->game_pid, session_dir, "game_crash.dmp");
			ueb_store_u32(&header->ue_dump_done, 1);
			handled_crash = 1;
		}
		if (game && WaitForSingleObject(game, 0) == WAIT_OBJECT_0)
			break;
		Sleep(20);
	}
	if (game)
		CloseHandle(game);
	os->unmap_section((void *)(uintptr_t)header, section_handle);
	os->unmap_section((void *)(uintptr_t)directory, directory_handle);
	return 0;
}

/* ---------- probe-directory */

struct directory_entry
{
	uint32_t game_pid;
	uint64_t session_id;
	char section_name[UE_BRIDGE_NAME_CHARS];
};

static void read_entry(volatile struct ue_bridge_directory *directory, struct directory_entry *entry)
{
	for (;;)
	{
		uint32_t before = ueb_load_u32(&directory->sequence);

		if (!(before & 1u))
		{
			entry->game_pid = directory->game_pid;
			entry->session_id = directory->session_id;
			memcpy(entry->section_name, (const void *)(uintptr_t)directory->section_name, sizeof(entry->section_name));
			entry->section_name[sizeof(entry->section_name) - 1] = 0;
			if (ueb_load_u32(&directory->sequence) == before)
				return;
		}
		Sleep(0);
	}
}

/* an empty entry, or one whose pid, session and section name all belong to one game */
static int entry_is_coherent(const struct directory_entry *entry)
{
	char expected[UE_BRIDGE_NAME_CHARS];

	if (!entry->session_id)
		return !entry->game_pid && !entry->section_name[0];
	snprintf(expected, sizeof(expected), "Local\\HaloCEUE.Bridge.%lu.%016llx",
		(unsigned long)entry->game_pid, (unsigned long long)entry->session_id);
	return strcmp(expected, entry->section_name) == 0;
}

static int check_directory(int argc, char **argv)
{
	long limit_ms = option_number(argc, argv, "--check-ms", 30000);
	const char *stop_file = option_text(argc, argv, "--stop-file");
	void *handle;
	volatile struct ue_bridge_directory *directory = (volatile struct ue_bridge_directory *)ue_bridge_platform_os()->map_section(
		UE_BRIDGE_DIRECTORY_NAME, UE_BRIDGE_DIRECTORY_SIZE, &handle);
	struct directory_entry entry;
	unsigned long reads = 0;
	unsigned long incoherent = 0;
	DWORD start = GetTickCount();

	if (!directory)
		return 3;
	write_ready_file(option_text(argc, argv, "--ready-file"));
	while ((long)(GetTickCount() - start) < limit_ms && !(stop_file && GetFileAttributesA(stop_file) != INVALID_FILE_ATTRIBUTES))
	{
		read_entry(directory, &entry);
		reads++;
		if (!entry_is_coherent(&entry))
			incoherent++;
	}
	read_entry(directory, &entry);
	printf("reads=%lu\nincoherent=%lu\n", reads, incoherent);
	if (entry.session_id)
		printf("final=%lu\n", (unsigned long)entry.game_pid);
	else
		printf("final=empty\n");
	fflush(stdout);
	return incoherent ? 5 : 0;
}

static int probe_directory(int argc, char **argv)
{
	HANDLE mapping;
	volatile struct ue_bridge_directory *directory;

	if (option_text(argc, argv, "--check-ms"))
		return check_directory(argc, argv);
	mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, UE_BRIDGE_DIRECTORY_NAME);
	if (!mapping)
		return 0;
	directory = (volatile struct ue_bridge_directory *)MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, UE_BRIDGE_DIRECTORY_SIZE);
	if (directory && option_flag(argc, argv, "--print"))
	{
		struct directory_entry entry;
		HANDLE section = NULL;
		volatile struct ue_bridge_header *header = NULL;

		read_entry(directory, &entry);
		printf("game_pid=%lu\nsession_id=%016llx\nsection=%s\n", (unsigned long)entry.game_pid,
			(unsigned long long)entry.session_id, entry.section_name);
		if (entry.session_id)
			section = OpenFileMappingA(FILE_MAP_READ, FALSE, entry.section_name);
		if (section)
			header = (volatile struct ue_bridge_header *)MapViewOfFile(section, FILE_MAP_READ, 0, 0, UE_BRIDGE_HEADER_SIZE);
		if (header)
			printf("game_log_path=%s\n", (const char *)(uintptr_t)header->game_log_path);
		fflush(stdout);
		if (header)
			UnmapViewOfFile((const void *)header);
		if (section)
			CloseHandle(section);
	}
	if (directory)
		UnmapViewOfFile((const void *)directory);
	CloseHandle(mapping);
	return 1;
}

int main(int argc, char **argv)
{
	const char *role = argc >= 2 && strncmp(argv[1], "--role=", 7) == 0 ? argv[1] + 7 : "";

	/* a crashing role must exit at once, not wait on a Windows Error Reporting dialog */
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
	if (strcmp(role, "fake-game") == 0)
		return fake_game(argc, argv);
	if (strcmp(role, "fake-ue") == 0)
		return fake_ue(argc, argv);
	if (strcmp(role, "probe-directory") == 0)
		return probe_directory(argc, argv);
	fprintf(stderr, "usage: ue_bridge_roles.exe --role=fake-game|fake-ue|probe-directory [options]\n");
	return 2;
}
