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
  --ready-after-ms N  once the game's export for its load_epoch is published
                      (export_epoch == load_epoch), waits N ms, heartbeating,
                      then writes ue_ready = load_epoch, once per epoch; N < 0
                      and the default never write it
  --dump-on-crashing  dumps the game to game_crash.dmp when it is crashing,
                      then sets ue_dump_done (without it, ignores the crash)
  It exits when the game's process does.

fake-world: fake-game's start-up, then a synthetic map exported through the shared
  writers (one quad model, two definitions, one BSP), a tick every 33 ms with two
  objects (one moving along X), and a frame every 16 ms with a camera at (-3, 0, 1)
  looking along +X. Options: --log, --ready-file, --run-ms (default 60000), --continue.

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

read-world: attaches read-only to the live bridge, as UE does, copies the load
region under the load sequence and prints one JSON object (the export's
counts, limits, per-BSP figures and winding agreement); tools/test_ue_bridge_maps.py.
  Exits 3 (no directory), 4 (no section), 5 (a region outside the section)
  or 6 (the region stayed mid-export for 30 s).
*/

#include "../../linux/src/ue_bridge_platform.h"
#include "ue_bridge_ring.h"
#include "ue_bridge_bsp.h"
#include "ue_bridge_model.h"
#include "ue_bridge_tick.h"

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

/* ---------- fake-world */

/* The limits the real game publishes (model_definitions.h, object_definitions.h); roles.c is
built without the game's headers, so they are named once here, as FHaloFakeWorld's GameMax* are. */
#define WORLD_MAX_NODES_PER_MODEL 64u
#define WORLD_MAX_REGIONS_PER_MODEL 32u
#define WORLD_MAX_PERMUTATIONS_PER_REGION 32u
#define WORLD_MAX_REGIONS_PER_OBJECT 8u

#define WORLD_PACKED_UP (511u << 22)

static struct ue_bridge_matrix world_matrix(float x, float y, float z)
{
	struct ue_bridge_matrix matrix;

	memset(&matrix, 0, sizeof(matrix));
	matrix.scale = 1.0f;
	matrix.forward[0] = 1.0f;
	matrix.left[1] = 1.0f;
	matrix.up[2] = 1.0f;
	matrix.position[0] = x;
	matrix.position[1] = y;
	matrix.position[2] = z;
	return matrix;
}

/* FHaloFakeModel (HaloFakeWorld.h): region 0 has two permutations, 0 a unit quad at every detail
level, 1 a smaller quad at levels 3 and 4 */
struct world_model
{
	struct ue_bridge_compressed_model_vertex quad_vertices[4];
	struct ue_bridge_compressed_model_vertex small_vertices[4];
	uint16_t strip[4];
	struct ue_bridge_model_source_part parts[2];
	struct ue_bridge_model_source_geometry geometries[2];
	int16_t permutations[2][UE_BRIDGE_DETAIL_LEVELS];
	struct ue_bridge_model_source_region region;
	struct ue_bridge_model_source_node node;
	int32_t shader;
	struct ue_bridge_model_source source;
};

static void world_model_build(struct world_model *model)
{
	static const float corners[4][2] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 1.0f }, { 1.0f, 1.0f } };
	static const int16_t permutations[2][UE_BRIDGE_DETAIL_LEVELS] = { { 0, 0, 0, 0, 0 }, { -1, -1, -1, 1, 1 } };
	static const float cutoffs[UE_BRIDGE_DETAIL_LEVELS] = { 0.0f, 10.0f, 40.0f, 120.0f, 300.0f };
	uint32_t index;

	memset(model, 0, sizeof(*model));
	for (index = 0; index < 4; index++)
	{
		model->strip[index] = (uint16_t)index;
		model->quad_vertices[index].position[0] = corners[index][0];
		model->quad_vertices[index].position[1] = corners[index][1];
		model->quad_vertices[index].normal = WORLD_PACKED_UP;
		model->quad_vertices[index].node_weight = 32767;
		model->small_vertices[index] = model->quad_vertices[index];
		model->small_vertices[index].position[0] *= 0.5f;
		model->small_vertices[index].position[1] *= 0.5f;
	}
	model->parts[0].vertices = model->quad_vertices;
	model->parts[0].vertex_count = 4;
	model->parts[0].strip = model->strip;
	model->parts[0].strip_length = 4;
	model->parts[1] = model->parts[0];
	model->parts[1].vertices = model->small_vertices;
	model->geometries[0].parts = &model->parts[0];
	model->geometries[0].part_count = 1;
	model->geometries[1].parts = &model->parts[1];
	model->geometries[1].part_count = 1;
	memcpy(model->permutations, permutations, sizeof(permutations));
	model->region.permutations = model->permutations;
	model->region.permutation_count = 2;
	model->node.parent = -1;
	model->node.default_local = world_matrix(0.0f, 0.0f, 0.0f);
	model->node.default_inverse = world_matrix(0.0f, 0.0f, 0.0f);
	model->shader = 0x00AA00AA;
	model->source.tag_index = 0x00110011;
	for (index = 0; index < UE_BRIDGE_DETAIL_LEVELS; index++)
	{
		model->source.detail_cutoff_pixels[index] = cutoffs[index];
		model->source.node_counts[index] = 1;
	}
	model->source.nodes = &model->node;
	model->source.node_count = 1;
	model->source.regions = &model->region;
	model->source.region_count = 1;
	model->source.geometries = model->geometries;
	model->source.geometry_count = 2;
	model->source.shader_tags = &model->shader;
	model->source.shader_count = 1;
}

/* FHaloFakeBsp (HaloFakeWorld.h): 4 surfaces in 2 clusters, one shader, every normal +Z */
struct world_bsp
{
	struct ue_bridge_compressed_environment_vertex vertices[6];
	uint16_t surfaces[4][3];
	struct ue_bridge_bsp_source_material material;
	int32_t cluster0[2];
	int32_t cluster1[2];
	const int32_t *clusters[2];
	uint32_t counts[2];
	struct ue_bridge_bsp_source source;
};

static void world_bsp_build(struct world_bsp *bsp)
{
	static const uint16_t surfaces[4][3] = { { 0, 1, 2 }, { 2, 1, 3 }, { 2, 3, 4 }, { 4, 3, 5 } };
	uint32_t index;

	memset(bsp, 0, sizeof(*bsp));
	for (index = 0; index < 6; index++)
	{
		bsp->vertices[index].position[0] = (float)(index % 2);
		bsp->vertices[index].position[1] = (float)(index / 2);
		bsp->vertices[index].normal = WORLD_PACKED_UP;
	}
	memcpy(bsp->surfaces, surfaces, sizeof(surfaces));
	bsp->material.shader_tag = 0x00BB00BB;
	bsp->material.first_surface = 0;
	bsp->material.surface_count = 4;
	bsp->material.vertices = bsp->vertices;
	bsp->material.vertex_count = 6;
	bsp->cluster0[1] = 1;
	bsp->cluster1[0] = 2;
	bsp->cluster1[1] = 3;
	bsp->clusters[0] = bsp->cluster0;
	bsp->clusters[1] = bsp->cluster1;
	bsp->counts[0] = 2;
	bsp->counts[1] = 2;
	bsp->source.surfaces = &bsp->surfaces[0][0];
	bsp->source.surface_count = 4;
	bsp->source.materials = &bsp->material;
	bsp->source.material_count = 1;
	bsp->source.cluster_surfaces = bsp->clusters;
	bsp->source.cluster_surface_counts = bsp->counts;
	bsp->source.cluster_count = 2;
}

/* The map export, laid out as ue_bridge_world_export_map lays it out: the root, a one-entry BSP
table right after it (a later BSP appends into its entry), then the definitions and the model. */
static int fake_world_export(void)
{
	static struct world_model model;
	static struct world_bsp bsp;
	struct ue_bridge_load_writer *writer;
	struct ue_bridge_load_root *root;
	struct ue_bridge_definition definitions[2];
	struct ue_bridge_model exported_model;
	struct ue_bridge_bsp_entry *entry;
	uint32_t bsps_offset, definitions_offset, models_offset;

	world_model_build(&model);
	world_bsp_build(&bsp);
	ue_bridge_bump_load_epoch();
	writer = ue_bridge_load_begin();
	root = ue_bridge_load_root();
	if (!writer || !root)
		return 0;
	ue_bridge_load_reserve(writer, 1, sizeof(struct ue_bridge_load_root));
	root->magic = UE_BRIDGE_LOAD_MAGIC;
	root->load_epoch = ue_bridge_load_epoch();
	strcpy(root->map_name, "fake_world");
	root->max_nodes_per_model = WORLD_MAX_NODES_PER_MODEL;
	root->max_regions_per_model = WORLD_MAX_REGIONS_PER_MODEL;
	root->max_permutations_per_region = WORLD_MAX_PERMUTATIONS_PER_REGION;
	root->max_regions_per_object = WORLD_MAX_REGIONS_PER_OBJECT;
	bsps_offset = ue_bridge_load_reserve(writer, 1, sizeof(struct ue_bridge_bsp_entry));
	memset(ue_bridge_load_pointer(writer, bsps_offset), 0, sizeof(struct ue_bridge_bsp_entry));
	root->bsps.offset = bsps_offset;
	root->bsps.count = 1;
	ue_bridge_load_set_bsp_table(bsps_offset, 1);
	definitions_offset = ue_bridge_load_reserve(writer, 2, sizeof(struct ue_bridge_definition));
	models_offset = ue_bridge_load_reserve(writer, 1, sizeof(struct ue_bridge_model));
	if (writer->overflow || !ue_bridge_model_export(writer, &model.source, &exported_model))
	{
		/* an odd load sequence would hold every reader for ever */
		ue_bridge_load_end(0);
		return 0;
	}
	memset(definitions, 0, sizeof(definitions));
	definitions[0].tag_index = 0x00010000;
	definitions[0].model = 0;
	definitions[0].animation_graph_tag = -1;
	definitions[0].bounding_radius = 1.0f;
	definitions[1] = definitions[0];
	definitions[1].tag_index = 0x00010001;
	definitions[1].animation_graph_tag = 0x00330033;
	memcpy(ue_bridge_load_pointer(writer, definitions_offset), definitions, sizeof(definitions));
	memcpy(ue_bridge_load_pointer(writer, models_offset), &exported_model, sizeof(exported_model));
	root->definitions.offset = definitions_offset;
	root->definitions.count = 2;
	root->models.offset = models_offset;
	root->models.count = 1;
	entry = ue_bridge_load_bsp_slot(0);
	entry->tag_index = 0x00220022;
	if (!ue_bridge_bsp_export(writer, &bsp.source, entry))
	{
		ue_bridge_load_end(0);
		return 0;
	}
	ue_bridge_load_publish_bsp(0, 1);
	ue_bridge_load_end(1);
	return 1;
}

/* One node and one region at permutation 0: an empty permutation list would not match the
default combination, and the static mesh would not be placed. */
static void fake_world_add_object(struct ue_bridge_tick_writer *writer, uint32_t datum_index, uint16_t definition, float x)
{
	static const uint8_t permutation = 0;
	struct ue_bridge_matrix node = world_matrix(x, 0.0f, 0.0f);

	ue_bridge_tick_writer_add(writer, datum_index, definition, 0, &permutation, 1, &node, 1);
}

static int fake_world(int argc, char **argv)
{
	struct ue_bridge_settings settings;
	struct ue_bridge_watch_config watch;
	const char *log = option_text(argc, argv, "--log");
	long run_ms = option_number(argc, argv, "--run-ms", 60000);
	DWORD start = GetTickCount();
	DWORD last_tick = 0;
	uint64_t tick = 0;
	uint64_t frame = 0;
	int have_tick = 0;

	if (log)
		role_log = fopen(log, "w");
	settings.enabled = 1;
	settings.log_path = log;
	settings.max_objects = 8192;
	settings.section_size = ROLE_SECTION_SIZE;
	settings.tick_slot_size = ROLE_TICK_SLOT_SIZE;
	if (!ue_bridge_start(&settings, ue_bridge_platform_os()))
		return 3;
	ue_bridge_platform_install_crash_hook();
	watch.request_quit = fake_game_request_quit;
	watch.continue_on_peer_exit = option_flag(argc, argv, "--continue");
	if (!ue_bridge_platform_start_watcher(&watch))
		return 4;
	ue_bridge_publish_frame_rate(0, 0);
	ue_bridge_bump_state_epoch();
	if (!fake_world_export())
		return 5;
	write_ready_file(option_text(argc, argv, "--ready-file"));
	while (!quit_requested && (long)(GetTickCount() - start) < run_ms)
	{
		DWORD now = GetTickCount();
		struct ue_bridge_camera camera;

		ue_bridge_heartbeat();
		if (!have_tick || now - last_tick >= 33)
		{
			struct ue_bridge_tick_writer *writer = ue_bridge_tick_begin(++tick);

			if (writer)
			{
				fake_world_add_object(writer, 0x00010002, 0, (float)(now - start) / 1000.0f);
				fake_world_add_object(writer, 0x00010004, 1, 0.0f);
				ue_bridge_tick_end(0);
			}
			last_tick = now;
			have_tick = 1;
		}
		memset(&camera, 0, sizeof(camera));
		camera.position[0] = -3.0f;
		camera.position[2] = 1.0f;
		camera.forward[0] = 1.0f;
		camera.up[2] = 1.0f;
		camera.vertical_fov = 1.0f;
		camera.z_near = 0.0625f;
		camera.z_far = 1024.0f;
		ue_bridge_publish_frame_camera(++frame, 0.5f, tick, &camera);
		Sleep(16);
	}
	ue_bridge_platform_stop_watcher();
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
	game_debug_line("fake-world: exiting (%s)", quit_requested ? "quit requested" : "run time over");
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
	long ready_after = option_number(argc, argv, "--ready-after-ms", -1);
	int dump_on_crashing = option_flag(argc, argv, "--dump-on-crashing");
	uint32_t readied_epoch = 0, pending_epoch = 0;
	DWORD pending_since = 0;
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
		if (ready_after >= 0)
		{
			uint32_t epoch = ueb_load_u32(&header->export_epoch);

			/* epoch 0 is no map: ue_ready starts at 0 and would already read as ready */
			if (epoch && epoch == ueb_load_u32(&header->load_epoch) && epoch != readied_epoch)
			{
				if (pending_epoch != epoch)
				{
					pending_epoch = epoch;
					pending_since = GetTickCount();
				}
				if ((long)(GetTickCount() - pending_since) >= ready_after)
				{
					ueb_store_u32(&header->ue_ready, epoch);
					readied_epoch = epoch;
				}
			}
		}
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

/* --role=read-world: the live bridge as JSON on stdout (tools/test_ue_bridge_maps.py) */
static double dot3(const float *a, const float *b)
{
	return (double)a[0] * b[0] + (double)a[1] * b[1] + (double)a[2] * b[2];
}

/* 1 when the face normal (b - a) x (c - a), right-handed, points the way the
three vertex normals do on the whole */
static int face_agrees(const float *a, const float *b, const float *c, const float *na, const float *nb, const float *nc)
{
	float u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
	float v[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
	float face[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
	float sum[3] = { na[0] + nb[0] + nc[0], na[1] + nb[1] + nc[1], na[2] + nb[2] + nc[2] };

	return dot3(face, sum) > 0.0;
}

/* a JSON string: scenario names hold backslashes */
static void print_json_string(const char *text)
{
	putchar('"');
	for (; *text; text++)
	{
		if (*text == '"' || *text == '\\')
			putchar('\\');
		putchar(*text);
	}
	putchar('"');
}

static int role_read_world(void)
{
	HANDLE directory_mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, UE_BRIDGE_DIRECTORY_NAME);
	const volatile struct ue_bridge_directory *directory;
	HANDLE section_mapping;
	const volatile uint8_t *view;
	const volatile struct ue_bridge_header *header;
	MEMORY_BASIC_INFORMATION info;
	uint8_t *region;
	const struct ue_bridge_load_root *root;
	uint32_t before, size, bsp, index;
	unsigned long model_triangles = 0, model_agree = 0, weights_out = 0;
	ULONGLONG started = GetTickCount64();

	if (!directory_mapping)
		return 3;
	directory = (const volatile struct ue_bridge_directory *)MapViewOfFile(directory_mapping, FILE_MAP_READ, 0, 0, 0);
	section_mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, (const char *)directory->section_name);
	if (!section_mapping)
		return 4;
	view = (const volatile uint8_t *)MapViewOfFile(section_mapping, FILE_MAP_READ, 0, 0, 0);
	VirtualQuery((LPCVOID)view, &info, sizeof(info));
	header = (const volatile struct ue_bridge_header *)view;
	size = header->load_region.size;
	if ((uint64_t)header->load_region.offset + size > info.RegionSize)
		return 5;
	region = (uint8_t *)malloc(size);
	do
	{
		/* a game that halted mid-export leaves the sequence odd for ever */
		if (GetTickCount64() - started > 30000u)
			return 6;
		before = ueb_load_u32(&header->load_sequence);
		memcpy(region, (const void *)(uintptr_t)(view + header->load_region.offset), size);
		ueb_fence();
	} while ((before & 1u) || ueb_load_u32(&header->load_sequence) != before);
	root = (const struct ue_bridge_load_root *)region;
	printf("{\"load_epoch\": %lu, \"ue_ready\": %lu, \"export_epoch\": %lu, \"export_complete\": %lu, \"missing\": %lu, \"map\": ",
		(unsigned long)header->load_epoch, (unsigned long)header->ue_ready,(unsigned long)header->export_epoch, (unsigned long)header->export_complete,
		(unsigned long)root->missing);
	print_json_string(root->magic == UE_BRIDGE_LOAD_MAGIC ? root->map_name : "");
	printf(", \"limits\": [%lu, %lu, %lu, %lu]", (unsigned long)root->max_nodes_per_model, (unsigned long)root->max_regions_per_model,
		(unsigned long)root->max_permutations_per_region, (unsigned long)root->max_regions_per_object);
	printf(", \"definitions\": %lu, \"models\": %lu, ", (unsigned long)root->definitions.count, (unsigned long)root->models.count);
	{
		const struct ue_bridge_definition *definitions = (const struct ue_bridge_definition *)(region + root->definitions.offset);
		unsigned long static_count = 0;

		for (index = 0; index < root->definitions.count; index++)
			static_count += definitions[index].animation_graph_tag == -1 && definitions[index].model >= 0;
		printf("\"static_definitions\": %lu, ", static_count);
	}
	for (index = 0; index < root->models.count; index++)
	{
		const struct ue_bridge_model *model = (const struct ue_bridge_model *)(region + root->models.offset) + index;
		const struct ue_bridge_geometry *geometries = (const struct ue_bridge_geometry *)(region + model->geometries.offset);
		uint32_t geometry, part;

		for (geometry = 0; geometry < model->geometries.count; geometry++)
		{
			for (part = 0; part < geometries[geometry].parts.count; part++)
			{
				const struct ue_bridge_part *p = (const struct ue_bridge_part *)(region + geometries[geometry].parts.offset) + part;
				const struct ue_bridge_model_vertex *v = (const struct ue_bridge_model_vertex *)(region + p->vertices.offset);
				const ue_bridge_model_index *i = (const ue_bridge_model_index *)(region + p->indices.offset);
				uint32_t k;

				for (k = 0; k < p->vertices.count; k++)
					weights_out += !(v[k].weight >= 0.0f && v[k].weight <= 1.0f);
				for (k = 0; k + 2 < p->indices.count; k += 3)
				{
					if (i[k] >= p->vertices.count || i[k + 1] >= p->vertices.count || i[k + 2] >= p->vertices.count)
						continue;
					model_triangles++;
					model_agree += face_agrees(v[i[k]].position, v[i[k + 1]].position, v[i[k + 2]].position,
						v[i[k]].normal, v[i[k + 1]].normal, v[i[k + 2]].normal);
				}
			}
		}
	}
	printf("\"model_triangles\": %lu, \"model_normals_agree\": %.4f, \"weights_out_of_range\": %lu, \"bsps\": [",
		model_triangles, model_triangles ? (double)model_agree / model_triangles : 0.0, weights_out);
	for (bsp = 0; bsp < root->bsps.count; bsp++)
	{
		const struct ue_bridge_bsp_entry *entry = (const struct ue_bridge_bsp_entry *)(region + root->bsps.offset) + bsp;
		unsigned long triangles = 0, agree = 0;
		/* FNV-1a over every batch's header, vertices and indices: equal exports are equal bytes */
		uint32_t hash = 2166136261u;

		for (index = 0; entry->ready && index < entry->batches.count; index++)
		{
			const struct ue_bridge_bsp_batch *batch = (const struct ue_bridge_bsp_batch *)(region + entry->batches.offset) + index;
			const struct ue_bridge_bsp_vertex *v = (const struct ue_bridge_bsp_vertex *)(region + batch->vertices.offset);
			const ue_bridge_bsp_index *i = (const ue_bridge_bsp_index *)(region + batch->indices.offset);
			uint32_t k;
			const uint8_t *bytes = (const uint8_t *)batch;

			for (k = 0; k < sizeof(*batch); k++)
				hash = (hash ^ bytes[k]) * 16777619u;
			bytes = (const uint8_t *)v;
			for (k = 0; k < batch->vertices.count * sizeof(struct ue_bridge_bsp_vertex); k++)
				hash = (hash ^ bytes[k]) * 16777619u;
			bytes = (const uint8_t *)i;
			for (k = 0; k < batch->indices.count * sizeof(ue_bridge_bsp_index); k++)
				hash = (hash ^ bytes[k]) * 16777619u;
			for (k = 0; k + 2 < batch->indices.count; k += 3)
			{
				triangles++;
				agree += face_agrees(v[i[k]].position, v[i[k + 1]].position, v[i[k + 2]].position,
					v[i[k]].normal, v[i[k + 1]].normal, v[i[k + 2]].normal);
			}
		}
		printf("%s{\"index\": %lu, \"ready\": %lu, \"batches\": %lu, \"clusters\": %lu, \"unclustered\": %lu, \"duplicates\": %lu, "
			"\"triangles\": %lu, \"normals_agree\": %.4f, \"hash\": %lu}",
			bsp ? ", " : "", (unsigned long)bsp, (unsigned long)entry->ready, (unsigned long)entry->batches.count,
			(unsigned long)entry->cluster_count, (unsigned long)entry->unclustered_surfaces, (unsigned long)entry->duplicate_surfaces,
			triangles, triangles ? (double)agree / triangles : 0.0, (unsigned long)hash);
	}
	printf("]");
	{
		static uint8_t tick[UE_BRIDGE_TICK_SLOT_SIZE];
		struct ue_bridge_frame_slot frame;
		uint32_t bytes = 0, offset, index, at_rest = 0, hidden = 0, ascending = 1, previous = 0;
		const struct ue_bridge_tick_header *tick_header = (const struct ue_bridge_tick_header *)tick;
		struct ue_bridge_ring_desc ring = *(const struct ue_bridge_ring_desc *)&header->tick_ring;

		if (ue_bridge_ring_read_newest_used(view, &ring, offsetof(struct ue_bridge_tick_header, used), tick, sizeof(tick), &bytes, 0)
			== UE_BRIDGE_READ_NONE)
		{
			memset(tick, 0, sizeof(struct ue_bridge_tick_header));
		}
		for (index = 0, offset = sizeof(struct ue_bridge_tick_header); index < tick_header->object_count; index++)
		{
			const struct ue_bridge_object_record *record = (const struct ue_bridge_object_record *)(tick + offset);

			/* a record that runs past what was copied would be read out of the buffer */
			if (offset + sizeof(*record) > bytes || record->size < sizeof(*record) || offset + record->size > bytes)
				break;
			at_rest += (record->flags & UE_BRIDGE_OBJECT_AT_REST) != 0;
			hidden += (record->flags & UE_BRIDGE_OBJECT_HIDDEN) != 0;
			if (index && (record->datum_index & (UE_BRIDGE_DATUM_ABSOLUTE_LIMIT - 1u)) <= previous)
				ascending = 0;
			previous = record->datum_index & (UE_BRIDGE_DATUM_ABSOLUTE_LIMIT - 1u);
			offset += record->size;
		}
		printf(", \"tick\": {\"id\": %llu, \"objects\": %lu, \"truncated\": %lu, \"ascending\": %lu, \"at_rest\": %lu, \"hidden\": %lu, "
			"\"active_bsp\": %d, \"load_epoch\": %lu, \"used\": %lu}",
			(unsigned long long)tick_header->slot.id, (unsigned long)tick_header->object_count,
			(unsigned long)(tick_header->flags & UE_BRIDGE_TICK_TRUNCATED), (unsigned long)ascending, (unsigned long)at_rest,
			(unsigned long)hidden, (int)tick_header->active_bsp, (unsigned long)tick_header->load_epoch, (unsigned long)tick_header->used);
		memset(&frame, 0, sizeof(frame));
		ring = *(const struct ue_bridge_ring_desc *)&header->frame_ring;
		ue_bridge_ring_read_newest(view, &ring, &frame, sizeof(frame), 0);
		printf(", \"frame\": {\"id\": %llu, \"camera_valid\": %lu, \"position\": [%f, %f, %f], \"vertical_fov\": %f, \"z_far\": %f, \"game_holding\": %lu}",
			(unsigned long long)frame.slot.id, (unsigned long)frame.camera_valid, frame.camera_position[0], frame.camera_position[1],
			frame.camera_position[2], frame.vertical_fov, frame.z_far, (unsigned long)header->game_holding);
	}
	printf("}\n");
	free(region);
	return 0;
}

int main(int argc, char **argv)
{
	const char *role = argc >= 2 && strncmp(argv[1], "--role=", 7) == 0 ? argv[1] + 7 : "";

	/* a crashing role must exit at once, not wait on a Windows Error Reporting dialog */
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
	if (strcmp(role, "fake-game") == 0)
		return fake_game(argc, argv);
	if (strcmp(role, "fake-world") == 0)
		return fake_world(argc, argv);
	if (strcmp(role, "fake-ue") == 0)
		return fake_ue(argc, argv);
	if (strcmp(role, "probe-directory") == 0)
		return probe_directory(argc, argv);
	if (strcmp(role, "read-world") == 0)
		return role_read_world();
	fprintf(stderr, "usage: ue_bridge_roles.exe --role=fake-game|fake-world|fake-ue|probe-directory|read-world [options]\n");
	return 2;
}
