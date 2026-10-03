/*
WIN32_UE_BRIDGE.C

The UE bridge's Windows layer (port/linux/src/ue_bridge_platform.h): named
sections and the clock for the core, a thread watching the renderer's
process, the crash hook and its minidump, and the game's part of the session
report (the HaloCEUE repository's Phase 0 design, section 8.1). Replaces
port/linux/src/ue_bridge_platform_null.c (port/windows/port.json).
*/

#include <windows.h>
#include <bcrypt.h>
#include <dbghelp.h>
#include <stdio.h>
#include <string.h>

#include "../../linux/src/ue_bridge_platform.h"
#include "../../ue_bridge/ue_bridge_policy.h"
#include "../../ue_bridge/ue_bridge_ring.h"

/* platform.h's (this file sees the Windows SDK, the platform layer the Xbox SDK's names) */
void platform_log(const char *format, ...);

#define HUNG_PEER_EXIT_CODE 0x48414E47u
#define WATCH_INTERVAL_MS 250
#define DIRECTORY_LOCK_NAME "Local\\HaloCEUE.Bridge.DirectoryLock"
#define DIRECTORY_LOCK_WAIT_MS 2000

/* ---------- the core's operating system */

static void *map_section(const char *name, uint32_t size, void **handle)
{
	HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, size, name);
	void *view;

	if (!mapping)
		return NULL;
	view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
	if (!view)
	{
		CloseHandle(mapping);
		return NULL;
	}
	*handle = mapping;
	return view;
}

static void unmap_section(void *view, void *handle)
{
	UnmapViewOfFile(view);
	CloseHandle((HANDLE)handle);
}

static uint64_t qpc(void)
{
	LARGE_INTEGER value;

	QueryPerformanceCounter(&value);
	return (uint64_t)value.QuadPart;
}

static uint64_t qpc_frequency(void)
{
	LARGE_INTEGER value;

	QueryPerformanceFrequency(&value);
	return (uint64_t)value.QuadPart;
}

static uint32_t current_pid(void)
{
	return GetCurrentProcessId();
}

static uint64_t random64(void)
{
	uint64_t value = 0;

	if (!BCRYPT_SUCCESS(BCryptGenRandom(NULL, (PUCHAR)&value, sizeof(value), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
		value = qpc() ^ ((uint64_t)GetCurrentProcessId() << 32);
	return value;
}

static int debugger_present(void)
{
	return IsDebuggerPresent() != 0;
}

static void os_log(const char *message)
{
	platform_log("%s", message);
}

/* the port's paths are in the ANSI code page (CreateFileA and friends), and
relative when paths.data is unset ("./debug.txt"): UE would resolve a
relative one against its own folder */
static void path_to_utf8(const char *path, char *utf8, uint32_t capacity)
{
	char full[MAX_PATH * 2];
	wchar_t wide[MAX_PATH * 2];
	DWORD length = GetFullPathNameA(path, sizeof(full), full, NULL);

	utf8[0] = 0;
	if (length == 0 || length >= sizeof(full))
		return;
	if (!MultiByteToWideChar(CP_ACP, 0, full, -1, wide, MAX_PATH * 2))
		return;
	if (!WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, (int)capacity, NULL, NULL))
		utf8[0] = 0;
}

static HANDLE directory_mutex;
static int directory_lock_owned;

/* Several bridged games can share a machine (system link on one PC) and the
core's directory entry has one seqlock, so its writes need one writer at a time.
The wait is capped: a holder that hangs must never hang another game's start. */
static void lock_directory(void)
{
	DWORD result;

	if (!directory_mutex)
		directory_mutex = CreateMutexA(NULL, FALSE, DIRECTORY_LOCK_NAME);
	if (!directory_mutex)
	{
		platform_log("ue bridge: cannot create the directory lock: going on without it");
		return;
	}
	result = WaitForSingleObject(directory_mutex, DIRECTORY_LOCK_WAIT_MS);
	/* abandoned: the holder died. The lock is ours, and the directory's seqlock
	repair covers a write it tore. */
	if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED)
	{
		directory_lock_owned = 1;
		return;
	}
	platform_log("ue bridge: directory lock %s: going on without it", result == WAIT_TIMEOUT ? "timed out" : "failed");
}

static void unlock_directory(void)
{
	if (!directory_lock_owned)
		return;
	directory_lock_owned = 0;
	ReleaseMutex(directory_mutex);
}

static const struct ue_bridge_os windows_os =
{
	map_section,
	unmap_section,
	qpc,
	qpc_frequency,
	current_pid,
	random64,
	debugger_present,
	os_log,
	path_to_utf8,
	/* never NULL here: NULL is for the single-game unit tests */
	lock_directory,
	unlock_directory
};

const struct ue_bridge_os *ue_bridge_platform_os(void)
{
	return &windows_os;
}

/* ---------- report files */

static void header_string(const volatile char *field, char *text, size_t capacity)
{
	size_t index;

	for (index = 0; index + 1 < capacity && field[index]; index++)
		text[index] = field[index];
	text[index] = 0;
}

/* UE publishes ue_attached after its session folder, so the folder is only read once it is set */
static int ue_session_published(volatile struct ue_bridge_header *header)
{
	return ueb_load_u32(&header->ue_attached) != 0 && header->ue_session_dir[0] != 0;
}

/* where the game's files go: UE's session folder, or the folder of debug.txt when no UE attached */
static void report_directory(volatile struct ue_bridge_header *header, char *directory, size_t capacity)
{
	char log_path[UE_BRIDGE_PATH_BYTES];
	const char *slash;
	const char *backslash;
	const char *end;
	size_t length;

	directory[0] = 0;
	if (ue_session_published(header))
		header_string(header->ue_session_dir, directory, capacity);
	if (directory[0])
		return;
	header_string(header->game_log_path, log_path, sizeof(log_path));
	slash = strrchr(log_path, '/');
	backslash = strrchr(log_path, '\\');
	end = slash;
	if (!end || (backslash && backslash > end))
		end = backslash;
	length = end ? (size_t)(end - log_path) : 0;
	if (length >= capacity)
		length = 0;
	memcpy(directory, log_path, length);
	directory[length] = 0;
}

/* directory (UTF-8) \ name, as UTF-16; 0 when there is no directory or it is too long */
static int file_path(const char *directory, const char *name, wchar_t *path, int capacity)
{
	int length;

	if (!directory[0])
		return 0;
	length = MultiByteToWideChar(CP_UTF8, 0, directory, -1, path, capacity);
	if (length <= 0 || length + (int)strlen(name) + 1 >= capacity)
		return 0;
	path[length - 1] = L'\\';
	return MultiByteToWideChar(CP_UTF8, 0, name, -1, path + length, capacity - length) > 0;
}

static void write_text_file(const char *directory, const char *name, const char *text)
{
	wchar_t path[MAX_PATH * 2];
	HANDLE file;
	DWORD written;

	if (!file_path(directory, name, path, MAX_PATH * 2))
		return;
	file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return;
	WriteFile(file, text, (DWORD)strlen(text), &written, NULL);
	CloseHandle(file);
}

static void write_header_snapshot(const char *directory, volatile struct ue_bridge_header *header)
{
	wchar_t path[MAX_PATH * 2];
	HANDLE file;
	DWORD written;

	if (!file_path(directory, "header.bin", path, MAX_PATH * 2))
		return;
	file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return;
	WriteFile(file, (const void *)(uintptr_t)header, UE_BRIDGE_HEADER_SIZE, &written, NULL);
	CloseHandle(file);
}

/* UE can write the header, so the ring's geometry comes from the format's
constants and only its write count is read live (ue_bridge_ring_read_newest) */
static uint64_t newest_id(volatile struct ue_bridge_header *header, volatile struct ue_bridge_ring_desc *live,
	uint32_t offset, uint32_t slot_count)
{
	struct ue_bridge_slot slot;
	struct ue_bridge_ring_desc ring;
	enum ue_bridge_read_result result;

	ring.offset = offset;
	ring.slot_size = UE_BRIDGE_SLOT_SIZE;
	ring.slot_count = slot_count;
	ring.published = ueb_load_u32(&live->published);
	result = ue_bridge_ring_read_newest((const volatile uint8_t *)header, &ring, &slot, sizeof(slot), NULL);

	return result == UE_BRIDGE_READ_NEWEST || result == UE_BRIDGE_READ_PREVIOUS ? slot.id : 0;
}

static void write_game_report(enum ue_bridge_action action, const struct ue_bridge_peer_view *peer,
	volatile struct ue_bridge_header *header, const char *note)
{
	char directory[UE_BRIDGE_PATH_BYTES];
	char crashes[UE_BRIDGE_PATH_BYTES + 32];
	char text[2048];

	report_directory(header, directory, sizeof(directory));
	/* UE's crash reporter writes under <project>/Saved/Crashes; the session
	folder is <project>/Saved/HaloBridge/Sessions/<session> */
	crashes[0] = 0;
	if (ue_session_published(header))
		snprintf(crashes, sizeof(crashes), "%s\\..\\..\\..\\Crashes", directory);
	snprintf(text, sizeof(text),
		"side=game\n"
		"session_id=%016llx\n"
		"peer_action=%s\n"
		"peer_exit_code=0x%08lx\n"
		"peer_stopping=%lu\n"
		"last_tick=%llu\n"
		"last_frame=%llu\n"
		"load_epoch=%lu\n"
		"state_epoch=%lu\n"
		"ue_crashes_dir=%s\n"
		"note=%s\n",
		(unsigned long long)header->session_id,
		ue_bridge_action_name(action),
		(unsigned long)peer->exit_code,
		(unsigned long)peer->stopping,
		(unsigned long long)newest_id(header, &header->tick_ring, UE_BRIDGE_HEADER_SIZE, UE_BRIDGE_TICK_SLOTS),
		(unsigned long long)newest_id(header, &header->frame_ring, UE_BRIDGE_HEADER_SIZE + UE_BRIDGE_SLOT_SIZE * UE_BRIDGE_TICK_SLOTS, UE_BRIDGE_FRAME_SLOTS),
		(unsigned long)header->load_epoch,
		(unsigned long)header->state_epoch,
		crashes,
		note);
	write_text_file(directory, "game_report.txt", text);
	write_header_snapshot(directory, header);
}

/* ---------- the crash hook */

typedef BOOL (WINAPI *minidump_write_function)(HANDLE process, DWORD pid, HANDLE file, MINIDUMP_TYPE type,
	PMINIDUMP_EXCEPTION_INFORMATION exception, PMINIDUMP_USER_STREAM_INFORMATION user, PMINIDUMP_CALLBACK_INFORMATION callback);

#define DUMP_THREAD_WAIT_MS 10000
#define CRASH_STACK_GUARANTEE_BYTES 65536

static minidump_write_function minidump_write;
static LPTOP_LEVEL_EXCEPTION_FILTER previous_filter;
static volatile LONG crash_hook_installed;
/* the renderer's process while it is attached; the crash hook reads it from the crashing thread */
static void *volatile watched_peer;
/* the thread inside the crash filter, 0 when none */
static volatile LONG crash_owner;
static HANDLE dump_thread;
static HANDLE dump_requested;
static HANDLE dump_finished;
static EXCEPTION_POINTERS *volatile dump_exception;
static volatile struct ue_bridge_header *volatile dump_header;
static volatile DWORD dump_crashing_thread;

static void write_self_dump(EXCEPTION_POINTERS *exception, volatile struct ue_bridge_header *header, DWORD crashing_thread)
{
	char directory[UE_BRIDGE_PATH_BYTES];
	wchar_t path[MAX_PATH * 2];
	MINIDUMP_EXCEPTION_INFORMATION information;
	HANDLE file;

	if (!minidump_write)
		return;
	report_directory(header, directory, sizeof(directory));
	if (!file_path(directory, "game_crash_self.dmp", path, MAX_PATH * 2))
		return;
	file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return;
	information.ThreadId = crashing_thread;
	information.ExceptionPointers = exception;
	information.ClientPointers = FALSE;
	minidump_write(GetCurrentProcess(), GetCurrentProcessId(), file,
		(MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory), &information, NULL, NULL);
	CloseHandle(file);
}

/* Writes the dump for the crash filter. A stack overflow leaves the crashing
thread with almost no stack, too little for MiniDumpWriteDump, so the dump is
written from this thread, which the filter signals and waits on. */
static DWORD WINAPI dump_thread_main(void *unused)
{
	(void)unused;
	for (;;)
	{
		if (WaitForSingleObject(dump_requested, INFINITE) != WAIT_OBJECT_0)
			return 0;
		write_self_dump(dump_exception, dump_header, dump_crashing_thread);
		SetEvent(dump_finished);
	}
}

static int process_alive(HANDLE process)
{
	return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
}

/* ends the process with the exception's code: left to Windows Error Reporting,
the process could sit on its dialog, and the renderer would wait on it */
static void terminate_self(DWORD code)
{
	TerminateProcess(GetCurrentProcess(), code);
}

static LONG WINAPI bridge_crash_filter(EXCEPTION_POINTERS *exception)
{
	DWORD code = exception->ExceptionRecord->ExceptionCode;
	DWORD self = GetCurrentThreadId();
	DWORD owner = (DWORD)InterlockedCompareExchange(&crash_owner, (LONG)self, 0);
	LONG result;
	volatile struct ue_bridge_header *header;
	HANDLE peer;
	DWORD start;

	/* A fault inside this filter (the game's own filter, the dump) enters it
	again on the same thread, and would loop through the same fault: end the
	process. Another thread crashing meanwhile must not race the first one's
	dump: it parks, since the first ends the process. */
	if (owner == self)
		terminate_self(code);
	if (owner)
		Sleep(INFINITE);
	/* the game's own filter first, so debug.txt has its crash lines before UE copies it */
	result = previous_filter ? previous_filter(exception) : EXCEPTION_CONTINUE_SEARCH;
	header = ue_bridge_header();
	peer = (HANDLE)watched_peer;
	if (!header)
	{
		/* Nothing to publish, and the previous filter may have let execution continue. The
		ownership is released so a later crash on another thread is not parked for ever. */
		InterlockedExchange(&crash_owner, 0);
		return result;
	}
	header->crash.exception_code = code;
	header->crash.exception_address = (uint32_t)(uintptr_t)exception->ExceptionRecord->ExceptionAddress;
	header->crash.thread_id = self;
	ueb_store_u32(&header->game_stopping, UE_BRIDGE_STOP_CRASH);
	/* always, and first: only this dump carries the exception record and the
	faulting 32-bit context (UE's 64-bit dump of this process can't) */
	if (dump_thread)
	{
		dump_exception = exception;
		dump_header = header;
		dump_crashing_thread = self;
		SetEvent(dump_requested);
		WaitForSingleObject(dump_finished, DUMP_THREAD_WAIT_MS);
	}
	else
	{
		write_self_dump(exception, header, self);
	}
	ueb_store_u32(&header->game_crashing, 1);
	if (ueb_load_u32(&header->ue_attached) && process_alive(peer))
	{
		start = GetTickCount();
		while (!ueb_load_u32(&header->ue_dump_done) && GetTickCount() - start < UE_BRIDGE_DUMP_WAIT_MS && process_alive(peer))
			Sleep(10);
	}
	ueb_store_u32(&header->game_crashing, 0);
	terminate_self(code);
	return result;
}

void ue_bridge_platform_install_crash_hook(void)
{
	HMODULE dbghelp;
	ULONG guarantee = CRASH_STACK_GUARANTEE_BYTES;

	if (InterlockedExchange(&crash_hook_installed, 1))
		return;
	/* loaded now: a crashing process can't be trusted to load a library */
	dbghelp = LoadLibraryExA("dbghelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (dbghelp)
		minidump_write = (minidump_write_function)(void *)GetProcAddress(dbghelp, "MiniDumpWriteDump");
	dump_requested = CreateEventA(NULL, FALSE, FALSE, NULL);
	dump_finished = CreateEventA(NULL, FALSE, FALSE, NULL);
	if (dump_requested && dump_finished)
		dump_thread = CreateThread(NULL, 0, dump_thread_main, NULL, 0, NULL);
	/* the filter and the signal to the dump thread run on what is left of the
	faulting thread's stack after an overflow */
	SetThreadStackGuarantee(&guarantee);
	previous_filter = SetUnhandledExceptionFilter(bridge_crash_filter);
}

/* ---------- the watcher */

static HANDLE watcher_thread;
static HANDLE watcher_stop;
static struct ue_bridge_watch_config watch_config;

static void act_on_peer(enum ue_bridge_action action, const struct ue_bridge_peer_view *view, HANDLE peer,
	volatile struct ue_bridge_header *header)
{
	const char *note = "";

	if (action == UE_BRIDGE_ACTION_PEER_HUNG)
	{
		/* the renderer dumped itself 2 s before this timeout (HaloCEUE's peer watcher) */
		TerminateProcess(peer, HUNG_PEER_EXIT_CODE);
		note = "terminated the hung renderer";
	}
	else if (action == UE_BRIDGE_ACTION_PEER_HUNG_EDITOR)
	{
		note = "the renderer is an editor: left running";
	}
	write_game_report(action, view, header, note);
	platform_log("ue bridge: renderer %s %s", ue_bridge_action_name(action), note);
}

static void read_peer_view(volatile struct ue_bridge_header *header, HANDLE peer, struct ue_bridge_peer_view *view)
{
	memset(view, 0, sizeof(*view));
	view->process_exited = WaitForSingleObject(peer, 0) == WAIT_OBJECT_0;
	if (view->process_exited)
	{
		DWORD code = 0;

		GetExitCodeProcess(peer, &code);
		view->exit_code = code;
	}
	view->stopping = ueb_load_u32(&header->ue_stopping);
	/* UE published a crash: its crash reporter is running and its game thread
	is gone; terminating it now would lose UE's own dump */
	view->crashing = view->stopping == UE_BRIDGE_STOP_CRASH;
	view->heartbeat_qpc = ueb_load_u64(&header->ue_heartbeat_qpc);
	view->hang_timeout_ms = ueb_load_u32(&header->ue_hang_timeout_ms);
	view->debugger_attached = ueb_load_u32(&header->ue_debugger_attached) != 0;
	view->busy = ueb_load_u32(&header->ue_busy) != 0;
	view->is_editor = ueb_load_u32(&header->ue_is_editor) != 0;
}

static DWORD WINAPI watcher_main(void *unused)
{
	DWORD peer_pid = 0;
	HANDLE peer = NULL;
	uint64_t session = 0;
	int was_attached = 0;
	DWORD open_failed_pid = 0;
	int acted = 0;
	struct ue_bridge_crash_clock crash_clock;

	(void)unused;
	memset(&crash_clock, 0, sizeof(crash_clock));
	while (WaitForSingleObject(watcher_stop, WATCH_INTERVAL_MS) == WAIT_TIMEOUT)
	{
		volatile struct ue_bridge_header *header = ue_bridge_header();
		struct ue_bridge_peer_view view;
		enum ue_bridge_action action;
		DWORD pid;
		int attached;

		if (!header)
			continue;
		/* the bridge was restarted: this is another header, with no renderer yet */
		if (header->session_id != session)
		{
			session = header->session_id;
			watched_peer = NULL;
			peer = NULL;
			peer_pid = 0;
			was_attached = 0;
			acted = 0;
			memset(&crash_clock, 0, sizeof(crash_clock));
		}
		pid = ueb_load_u32(&header->ue_pid);
		attached = ueb_load_u32(&header->ue_attached) != 0;
		/* a renderer attaches again with ue_attached going 0 then 1, or with a new
		PID; a PID alone would miss a new renderer that got the old one's PID. A
		failed OpenProcess is retried on the next poll. */
		if (attached && pid && (!peer || pid != peer_pid || !was_attached))
		{
			/* the superseded handle is not closed: the crash filter may be using it
			from another thread, and a re-attach is rare, so it is left to process exit */
			watched_peer = NULL;
			peer = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, pid);
			peer_pid = pid;
			acted = 0;
			memset(&crash_clock, 0, sizeof(crash_clock));
			watched_peer = peer;
			if (peer)
				platform_log("ue bridge: renderer %lu attached", (unsigned long)pid);
			else if (open_failed_pid != pid)
				platform_log("ue bridge: cannot open renderer %lu", (unsigned long)pid);
			open_failed_pid = peer ? 0 : pid;
		}
		was_attached = attached;
		if (!peer || acted)
			continue;
		read_peer_view(header, peer, &view);
		view.crashing_for_ms = ue_bridge_crashing_for_ms(&crash_clock, view.crashing, GetTickCount());
		action = ue_bridge_policy_decide(&view, qpc(), header->qpc_frequency);
		if (action == UE_BRIDGE_ACTION_NONE || action == UE_BRIDGE_ACTION_PEER_CRASHING)
			continue;
		acted = 1;
		act_on_peer(action, &view, peer, header);
		if (ue_bridge_policy_shuts_down(action, watch_config.continue_on_peer_exit))
			watch_config.request_quit();
		else
			platform_log("ue bridge: on_peer_exit = continue: waiting for a renderer to attach");
	}
	/* peer stays open: see above */
	watched_peer = NULL;
	return 0;
}

int ue_bridge_platform_start_watcher(const struct ue_bridge_watch_config *config)
{
	if (watcher_thread)
		return 1;
	watch_config = *config;
	watcher_stop = CreateEventA(NULL, TRUE, FALSE, NULL);
	if (!watcher_stop)
		return 0;
	watcher_thread = CreateThread(NULL, 0, watcher_main, NULL, 0, NULL);
	if (!watcher_thread)
	{
		CloseHandle(watcher_stop);
		watcher_stop = NULL;
		return 0;
	}
	return 1;
}

void ue_bridge_platform_stop_watcher(void)
{
	if (!watcher_thread)
		return;
	SetEvent(watcher_stop);
	WaitForSingleObject(watcher_thread, INFINITE);
	CloseHandle(watcher_thread);
	CloseHandle(watcher_stop);
	watcher_thread = NULL;
	watcher_stop = NULL;
}
