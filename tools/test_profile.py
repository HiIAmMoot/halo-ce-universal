"""Tests of the profiling build (configure.py --profile): the build switch,
the recording's C units through tools/profile_check.c, the trace files they
write, tools/net_report.py on a hand-written recording, and the hooks in the
game's sources."""

import io
import json
import os
import re
import shlex
import shutil
import subprocess
from pathlib import Path
from types import SimpleNamespace

import pytest

from tools import linux_build, net_report, ninja_syntax, profile_fixture, profile_join

ROOT = Path(__file__).resolve().parent.parent


# ---------- the build switch


def test_configuration_defines():
    assert linux_build.configuration_defines(SimpleNamespace()) == []
    assert linux_build.configuration_defines(SimpleNamespace(port_release=True)) == ["-DHALO_RELEASE"]
    assert linux_build.configuration_defines(SimpleNamespace(port_release=True, port_profile=True)) == [
        "-DHALO_RELEASE", "-DHALO_PROFILE"]


def test_every_native_build_takes_the_configuration_defines():
    # (the guest, the game and the platform layer of all three: one place
    # decides what --release and --profile define)
    for name in ("linux_build.py", "windows_build.py", "android_build.py"):
        text = (ROOT / "tools" / name).read_text(encoding="utf-8")
        assert "configuration_defines(sln)" in text, name
        assert '["-DHALO_RELEASE"] if' not in text, name


def test_profile_refuses_pgo_training():
    with pytest.raises(ValueError, match="--pgo=train"):
        linux_build.check_profile_options(True, "train")
    linux_build.check_profile_options(True, "use")
    linux_build.check_profile_options(True, "off")
    linux_build.check_profile_options(False, "train")


def test_linux_build_compiles_with_the_profile_define(monkeypatch):
    monkeypatch.chdir(ROOT)
    out = io.StringIO()
    linux_build.generate_linux_build(ninja_syntax.Writer(out), SimpleNamespace(
        build_dir=Path("build"), linux_cc="clang", compiler_launcher=None, port_release=False, port_lto="off",
        port_portable=True, port_pgo="off", port_pgo_profile=None, port_profile=True))
    # (ninja_syntax wraps long lines with " $"; Windows paths have backslashes)
    text = re.sub(r" \$\n *", " ", out.getvalue()).replace("\\", "/")
    for obj in ("build/linux/obj/source/game/game.o", "build/linux/obj/port/linux/src/p2p.o"):
        block = text[text.index(f"build {obj}:"):]
        cflags = next(line for line in block.splitlines() if line.strip().startswith("cflags = "))
        assert "-DHALO_PROFILE" in cflags, obj


# ---------- the recording's C units (tools/profile_check.c)

PROFILE_SOURCES = ["port/linux/src/profile_trace.c", "port/linux/src/profile_net.c", "port/linux/src/profile_json.c",
                   "port/linux/src/profile_overlay_lines.c"]


def check_compiler():
    """a C compiler of this machine with POSIX threads, or None (Windows has
    them only through the port's own layer, so the check runs on Linux and
    in WSL)"""
    if os.name == "nt":
        return None
    for name in ("clang", "cc", "gcc"):
        if shutil.which(name):
            return name
    return None


def distributed_put_macro():
    """the profiling build's measured distributed_put, as network_distributed.c
    has it (the check compiles that text, not a copy of it)"""
    text = source("port/linux/game/network_distributed.c")
    start = text.index("#define distributed_put(cursor, data, size)")
    return text[start:text.index("\n#endif", start)] + "\n"


def build_check(tmp_path, sanitizers, keep_parts=False):
    compiler = check_compiler()
    if not compiler:
        pytest.skip("needs a C compiler with POSIX threads (Linux, WSL)")
    program = tmp_path / ("profile_check" + "".join(f"_{name}" for name in sanitizers))
    macro = tmp_path / "distributed_put_macro.h"
    macro.write_text(distributed_put_macro(), encoding="utf-8")
    flags = [f"-fsanitize={','.join(sanitizers)}", "-fno-omit-frame-pointer"] if sanitizers else []
    extra = ["-DPROFILE_CHECK"] + (["-DPROFILE_CHECK_KEEP_PARTS", "-Dremove=profile_check_remove"] if keep_parts else [])
    built = subprocess.run([compiler, "-std=gnu11", "-Wall", "-Werror", "-DHALO_PROFILE", *extra, "-pthread", "-g", "-O1",
                            *flags, "-idirafter", "port/linux/include", "-Iport/linux/src", f'-DDISTRIBUTED_PUT_MACRO="{macro}"',
                            "-o", str(program),
                            "tools/profile_check.c", *PROFILE_SOURCES],
                           cwd=ROOT, capture_output=True, text=True)
    if built.returncode != 0 and sanitizers and "sanitize" in built.stderr:
        pytest.skip(f"{compiler} cannot build with {sanitizers}")
    assert built.returncode == 0, built.stderr[-4000:]
    return program


def run_check(program, folder):
    folder.mkdir(exist_ok=True)
    result = subprocess.run([str(program), str(folder)], capture_output=True, text=True, timeout=300,
                            env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1", "TSAN_OPTIONS": "halt_on_error=1"})
    assert result.returncode == 0, result.stdout + result.stderr[-4000:]
    assert "PASS" in result.stdout


def test_check_program_passes(tmp_path):
    run_check(build_check(tmp_path, []), tmp_path / "out")


def test_per_object_aggregate_names_are_registered():
    source_text = source("source/cseries/profile.c")
    names = ("item_update", "weapon_update", "unit_update", "biped_update", "vehicle_update",
             "projectile_update", "render_structure_shadows", "render_structure_shadows_draw",
             "render_structure_diffuse_lights", "render_structure_diffuse_lights_draw",
             "render_structure_specular_lights", "render_structure_specular_lights_draw")
    for name in names:
        assert f'profile_trace_name_aggregate("{name}")' in source_text
    assert "MAXIMUM_PROFILE_TRACE_AGGREGATES = 32" in source("port/linux/include/profile_trace.h")


def test_check_program_has_no_leaks_or_memory_errors(tmp_path):
    """AddressSanitizer and LeakSanitizer over every recording the check
    makes: arenas, the writer's files, the parts"""
    run_check(build_check(tmp_path, ["address", "undefined"]), tmp_path / "out")


def test_check_program_has_no_data_races(tmp_path):
    """ThreadSanitizer: the p2p track, the writer hand-off, the recording
    flag"""
    run_check(build_check(tmp_path, ["thread"]), tmp_path / "out")


def test_python_join_is_byte_identical_to_the_c_writer(tmp_path):
    folder = tmp_path / "out"
    (tmp_path / "build").mkdir()
    run_check(build_check(tmp_path / "build", [], keep_parts=True), folder)
    c_joined = max((path for path in folder.glob("profile_*_host_*_*.json")
                    if "halo_parts" in json.loads(path.read_text())),
                   key=lambda path: len(json.loads(path.read_text())["halo_parts"]))
    part_base = json.loads(c_joined.read_text())["halo_parts"][0]["header"]["recording"]
    parts = sorted(folder.glob(part_base + ".part*.json"),
                   key=lambda path: int(re.search(r"part(\d+)", path.name)[1]))
    c_contents = c_joined.read_bytes()
    c_name = c_joined.name
    c_joined.unlink()
    python_joined = profile_join.join(parts)
    assert python_joined.name == c_name
    assert python_joined.read_bytes() == c_contents
    first = json.loads(parts[0].read_text(encoding="utf-8"))
    first["halo"]["header"]["map_name"] = ""
    first["halo"]["header"]["gametype"] = "none"
    profile_fixture.write_part(parts[0], first["halo"], first["cpu_summary"])
    python_joined.unlink()
    assert profile_join.join(parts).name == c_name


def test_joined_name_uses_sanitized_map_and_engine_type():
    name = profile_join.joined_name({"role": "client", "map_name": "bloodgulch", "gametype": "slayer"},
                                   "20261008-065527")
    assert name == "profile_20261008-065527_client_bloodgulch_slayer"
    assert profile_join.joined_name({"role": "host", "map_name": "", "gametype": "campaign"},
                                    "20261008-123009") == "profile_20261008-123009_host_nomap_campaign"
    assert profile_join.joined_name({"role": "client", "map_name": "bloodgulch", "gametype": "ctf"},
                                    "20261008-065527") == "profile_20261008-065527_client_bloodgulch_ctf"
    assert profile_join.joined_name({"role": "local", "map_name": "levels\\a-b!?", "gametype": "none"},
                                    "20261008-123009") == "profile_20261008-123009_local_a-b--_none"


def test_python_join_derives_suffixed_output_from_part_headers(tmp_path):
    base = profile_fixture.write_recording(tmp_path / "recording")
    paths = [Path(f"{base}.part{number}.json") for number in (1, 2, 3)]
    output = profile_join.join(paths)
    assert output.name == "profile_20261005-142233_host_a30_campaign.json"
    assert json.loads(output.read_text(encoding="utf-8"))["halo_parts"]


def test_python_join_names_from_last_part_and_legacy_map(tmp_path):
    base = profile_fixture.write_recording(tmp_path / "recording")
    paths = [Path(f"{base}.part{number}.json") for number in (1, 2, 3)]
    last = json.loads(paths[-1].read_text(encoding="utf-8"))
    last["halo"]["header"]["map_name"] = "bloodgulch"
    last["halo"]["header"]["gametype"] = "slayer"
    profile_fixture.write_part(paths[-1], last["halo"], last["cpu_summary"])
    output = profile_join.join(paths)
    assert output.name == "profile_20261005-142233_host_bloodgulch_slayer.json"

    legacy = tmp_path / "legacy"
    base = profile_fixture.write_recording(legacy)
    old = Path(f"{base}.part3.json")
    data = json.loads(old.read_text(encoding="utf-8"))
    data["halo"]["header"].pop("map_name")
    data["halo"]["header"].pop("gametype")
    profile_fixture.write_part(old, data["halo"], data["cpu_summary"])
    assert profile_join.joined_name(profile_join._part_header(old), "20261005-142233").endswith("_a30_none")


def test_python_join_uses_collision_suffix_for_derived_name(tmp_path):
    base = profile_fixture.write_recording(tmp_path / "recording")
    parts = [Path(f"{base}.part{number}.json") for number in (1, 2, 3)]
    occupied = tmp_path / "recording" / "profile_20261005-142233_host_a30_campaign.json"
    occupied.write_text("belongs to another recording", encoding="utf-8")
    output = profile_join.join(parts)
    assert output.name == "profile_20261005-142233_host_a30_campaign_2.json"
    assert occupied.read_text(encoding="utf-8") == "belongs to another recording"

def test_python_join_handles_one_part(tmp_path):
    base = profile_fixture.write_recording(tmp_path / "whole", split=False)
    path = Path(f"{base}.part1.json")
    output = profile_join.join([path], Path(f"{base}.json"))
    joined = json.loads(output.read_text(encoding="utf-8"))
    assert len(joined["halo_parts"]) == len(joined["cpu_summary_parts"]) == 1


def test_python_join_warns_and_refuses_to_delete_incomplete_parts(tmp_path):
    base = profile_fixture.write_recording(tmp_path / "incomplete", split=False)
    path = Path(f"{base}.part1.json")
    path.write_text(path.read_text(encoding="utf-8").replace('"last_part": true', '"last_part": false'),
                    encoding="utf-8")
    output = Path(f"{base}.json")
    with pytest.raises(ValueError, match="no part has last_part=true"):
        profile_join.join([path], output, delete_parts=True)
    assert path.exists() and not output.exists()
    with pytest.warns(RuntimeWarning, match="incomplete set"):
        profile_join.join([path], output)


def test_python_join_preserves_a_preexisting_temporary_file(tmp_path):
    base = profile_fixture.write_recording(tmp_path / "recording")
    parts = [Path(f"{base}.part{number}.json") for number in (1, 2, 3)]
    output = Path(f"{base}.json")
    temporary = output.with_name(output.name + ".tmp")
    temporary.write_bytes(b"belongs to another attempt")
    with pytest.raises(FileExistsError):
        profile_join.join(parts, output)
    assert temporary.read_bytes() == b"belongs to another attempt"
    assert not output.exists()


@pytest.fixture(scope="module")
def written_parts(tmp_path_factory):
    folder = tmp_path_factory.mktemp("written")
    run_check(build_check(tmp_path_factory.mktemp("build"), [], keep_parts=True), folder)
    candidates = []
    for joined_path in folder.glob("profile_*_host_*_*.json"):
        try:
            part_base = json.loads(joined_path.read_text(encoding="utf-8"))["halo_parts"][0]["header"]["recording"]
        except (OSError, json.JSONDecodeError, KeyError, IndexError):
            continue
        first = folder / f"{part_base}.part1.json"
        if not first.exists() or first.stat().st_size == 0:
            continue
        try:
            joined = json.loads(joined_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        candidates.append((len(joined.get("halo_parts", [])), joined_path))
    part_count, joined_path = max(candidates, default=(0, None), key=lambda candidate: candidate[0])
    assert part_count >= 2, "the real C recording is split into parts"
    part_base = json.loads(joined_path.read_text(encoding="utf-8"))["halo_parts"][0]["header"]["recording"]
    parts = [folder / f"{part_base}.part{number}.json" for number in range(1, part_count + 1)]
    assert all(path.exists() for path in parts)
    return parts


def test_written_parts_are_traces(written_parts):
    for path in written_parts:
        text = path.read_text(encoding="utf-8")
        trace = json.loads(text)
        assert list(trace) == ["halo", "cpu_summary", "traceEvents", "displayTimeUnit"]
        assert text.startswith('{"halo": {\n"header": {')
        assert trace["halo"]["header"]["last_part"] == (path == written_parts[-1])
        header = trace["halo"]["header"]
        assert (header["role"], header["own_machine"], header["players"], header["players_most"]) == (
            "host", -1, 2, 5)
        assert (header["map_name"], header["gametype"]) == ("b30", "campaign")
        lines = text.splitlines()
        events = lines.index('"traceEvents": [')
        # one event a line, the scopes in order of their start
        starts = [json.loads(line.rstrip(","))["ts"] for line in lines[events + 1:-2] if '"ph":"X"' in line]
        assert starts == sorted(starts)
        assert '"quote\\"back\\\\slash"' in text or path != written_parts[0]


def joined_path_for_parts(parts):
    recording = json.loads(parts[0].read_text(encoding="utf-8"))["halo"]["header"]["recording"]
    for path in parts[0].parent.glob("profile_*_*_*.json"):
        try:
            joined = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            continue
        if joined.get("halo_parts") and joined["halo_parts"][0]["header"]["recording"] == recording:
            return path
    raise AssertionError(f"no joined output for {recording}")


def test_written_parts_nest_on_tracks(written_parts):
    trace = json.loads(written_parts[0].read_text(encoding="utf-8"))
    names = {event["args"]["name"] for event in trace["traceEvents"] if event["ph"] == "M"}
    assert {"game", "p2p"} <= names
    scopes = [event for event in trace["traceEvents"] if event["ph"] == "X" and event["tid"] == 1]
    frame = next(event for event in scopes if event["name"] == "frame")
    inside = [event for event in scopes if frame["ts"] <= event["ts"] < frame["ts"] + frame["dur"] and event is not frame]
    assert inside and all(event["ts"] + event["dur"] <= frame["ts"] + frame["dur"] + 0.001 for event in inside)


def test_written_parts_import_in_perfetto(written_parts):
    """trace_processor's import, when the perfetto package is installed (run
    by hand on a real recording otherwise)"""
    trace_processor = pytest.importorskip("perfetto.trace_processor")
    joined_path = joined_path_for_parts(written_parts)
    with trace_processor.TraceProcessor(trace=str(joined_path)) as processor:
        errors = processor.query("select name, value from stats where severity = 'error' and value > 0")
        rows = list(errors)
    assert not rows, [(row.name, row.value) for row in rows]


def test_written_parts_carry_negative_datum_handles(written_parts):
    """a datum handle's salt is bit 31, so its key is negative; -1 and -2 are
    the "no key" and "other" rows"""
    trace = json.loads(written_parts[0].read_text(encoding="utf-8"))["halo"]
    keys = {row[4] for row in trace["entries"]["rows"] if isinstance(row[4], int) and row[4] < -2}
    assert keys == {0xE1740000 + number - 2**32 for number in range(5)}
    objects = {row[0]: row for row in trace["objects"]["rows"]}
    assert keys <= set(objects) and all(objects[key][1] == "biped" for key in keys)


def test_written_parts_say_what_the_fixed_tables_dropped(written_parts):
    names = ("peers_dropped", "machines_dropped", "connections_overflowed", "objects_overflowed", "sites_overflowed",
             "fields_overflowed", "layouts_overflowed", "batches_unbooked")
    for path in written_parts:
        header = json.loads(path.read_text(encoding="utf-8"))["halo"]["header"]
        assert all(isinstance(header[name], int) and header[name] >= 0 for name in names), header


def test_written_parts_read_by_the_report(written_parts):
    recording = net_report.Recording(written_parts[0])
    assert recording.complete and not recording.warnings
    joined_path = joined_path_for_parts(written_parts)
    joined = json.loads(joined_path.read_text(encoding="utf-8"))
    assert len(joined["halo_parts"]) == len(written_parts)
    joined_report = net_report.Recording(joined_path)
    assert joined_report.tables == recording.tables
    assert joined_report.cpu == recording.cpu
    starts = [event["ts"] for event in joined["traceEvents"] if event.get("ph") == "X"]
    assert starts == sorted(starts)
    cpu_rows = [dict(zip(summary["columns"], row)) for summary in joined["cpu_summary_parts"]
                for row in summary["rows"]]
    totals = {}
    for row in cpu_rows:
        totals[row["name"]] = totals.get(row["name"], 0) + row["count"]
    assert totals["texture"] == 2400
    assert totals["render_model"] == 2400
    assert totals["aggregate_child"] == 2400
    frames = sum(1 for path in written_parts for event in json.loads(path.read_text(encoding="utf-8"))["traceEvents"]
                 if event["ph"] == "X" and event["name"] == "frame")
    assert recording.cpu["frames"] == frames


def test_joined_file_validates_each_required_header_field(tmp_path):
    base = profile_fixture.write_recording(tmp_path / "recording")
    parts = [Path(f"{base}.part{number}.json") for number in (1, 2, 3)]
    joined_path = profile_join.join(parts, Path(f"{base}.json"))
    joined = json.loads(joined_path.read_text(encoding="utf-8"))
    del joined["halo_parts"][1]["header"]["role"]
    joined_path.write_text(json.dumps(joined), encoding="utf-8")
    with pytest.raises(net_report.ReportError, match='part 2: the header has no "role"'):
        net_report.Recording(joined_path)


# ---------- tools/net_report.py

EXPECTED_SUMMARY = ROOT / "tools" / "profile_fixture.summary.txt"


@pytest.fixture
def host(tmp_path):
    return profile_fixture.write_recording(tmp_path)


def test_report_summary_is_the_expected_one(host):
    assert net_report.main([str(host) + ".part2.json"]) == 0
    written = Path(str(host) + ".summary.txt").read_text(encoding="ascii")
    assert written == EXPECTED_SUMMARY.read_text(encoding="ascii")
    assert len(written.splitlines()) <= net_report.SUMMARY_LINE_LIMIT


def test_report_uses_players_most_and_accepts_old_headers(tmp_path):
    base = profile_fixture.write_recording(tmp_path / "legacy")
    paths = [Path(f"{base}.part{number}.json") for number in (1, 2, 3)]
    for index, path in enumerate(paths):
        data = json.loads(path.read_text(encoding="utf-8"))
        header = data["halo"]["header"]
        for key in ("map_name", "gametype", "players_most"):
            header.pop(key, None)
        header["players"] = 1 + index
        profile_fixture.write_part(path, data["halo"], data["cpu_summary"])
    old_parts_report = net_report.Recording(paths[0])
    old_summary, _ = net_report.summary_lines(old_parts_report, None, None, 20, None)
    assert any("players: 1 at start, 3 most" in line for line in old_summary)
    joined = profile_join.join(paths, tmp_path / "legacy.json")
    recording = net_report.Recording(joined)
    lines, _ = net_report.summary_lines(recording, None, None, 20, None)
    assert any("players: 1 at start, 3 most" in line for line in lines)
    assert net_report.main([str(joined), "--no-summary"]) == 0


def test_report_summary_uses_current_players_most(tmp_path):
    base = profile_fixture.write_recording(tmp_path / "current")
    last = Path(f"{base}.part3.json")
    data = json.loads(last.read_text(encoding="utf-8"))
    data["halo"]["header"]["players_most"] = 9
    profile_fixture.write_part(last, data["halo"], data["cpu_summary"])
    recording = net_report.Recording(Path(f"{base}.part1.json"))
    summary, _ = net_report.summary_lines(recording, None, None, 20, None)
    assert any("players: 3 at start, 9 most" in line for line in summary)


def test_report_merges_parts_as_one(tmp_path):
    split = profile_fixture.write_recording(tmp_path / "split")
    whole = profile_fixture.write_recording(tmp_path / "whole", split=False)
    assert net_report.main([str(split), "--no-summary", "--csv", str(tmp_path / "split_csv")]) == 0
    assert net_report.main([str(whole), "--no-summary", "--csv", str(tmp_path / "whole_csv")]) == 0
    for table in sorted((tmp_path / "split_csv").glob("*.csv")):
        if table.name.endswith(".worst_frames.csv"):
            continue
        assert table.read_text() == (tmp_path / "whole_csv" / table.name).read_text(), table.name


def test_python_join_keeps_parts_unless_requested_and_report_matches(tmp_path, capsys):
    base = profile_fixture.write_recording(tmp_path / "split")
    paths = [Path(f"{base}.part{number}.json") for number in (1, 2, 3)]
    output = profile_join.join(paths, Path(f"{base}.json"))
    joined = json.loads(output.read_text(encoding="utf-8"))
    assert len(joined["halo_parts"]) == len(joined["cpu_summary_parts"]) == 3
    assert all(path.exists() for path in paths)
    assert net_report.main([str(base), "--no-summary"]) == 0
    parts_report = capsys.readouterr().out
    assert net_report.main([str(output), "--no-summary"]) == 0
    assert capsys.readouterr().out == parts_report
    original = output.read_bytes()
    with pytest.raises(FileExistsError):
        profile_join.join(paths, output)
    assert output.read_bytes() == original
    assert not output.with_name(output.name + ".tmp").exists()
    output.unlink()
    missing_output = tmp_path / "missing" / "joined.json"
    with pytest.raises(FileNotFoundError):
        profile_join.join(paths, missing_output)
    assert all(path.exists() for path in paths)
    assert not missing_output.with_name(missing_output.name + ".tmp").exists()
    profile_join.join(paths, output, delete_parts=True)
    assert not any(path.exists() for path in paths)


def test_report_warns_about_a_missing_part(host, capsys):
    Path(str(host) + ".part2.json").unlink()
    net_report.main([str(host), "--no-summary"])
    captured = capsys.readouterr()
    assert "part 2 of profile_20261005-142233_host is missing" in captured.err
    assert "(incomplete)" in captured.out


def test_report_warns_about_no_last_part(host, capsys):
    Path(str(host) + ".part3.json").unlink()
    net_report.main([str(host), "--no-summary"])
    assert "has no last part: the recording is incomplete" in capsys.readouterr().err


def test_report_flags_a_broken_batch_invariant(host, capsys):
    path = Path(str(host) + ".part1.json")
    path.write_text(path.read_text().replace("[0, 0, 2160, 30]", "[0, 0, 2168, 30]"), encoding="utf-8")
    assert net_report.main([str(host), "--no-summary"]) == 2
    captured = capsys.readouterr()
    assert "interval 0, machine 0: datagrams 2168 B, messages 2160 B" in captured.err
    assert "batch bytes do not add up in 1 interval(s)" in captured.out


def test_report_problems_name_what_the_fixed_tables_dropped(host, capsys):
    # (the run's tables count over the whole run, so a part's header repeats the earlier ones: the largest counts)
    for number, values in ((1, (3, 4, 5, 6, 7, 2, 1, 2)), (2, (1, 0, 0, 0, 9, 0, 0, 1))):
        path = Path(str(host) + f".part{number}.json")
        text = path.read_text(encoding="utf-8")
        names = ("peers_dropped", "machines_dropped", "connections_overflowed", "objects_overflowed", "sites_overflowed",
                 "fields_overflowed", "layouts_overflowed", "batches_unbooked")
        for name, value in zip(names, values):
            assert text.count(f'"{name}": 0') == 1
            text = text.replace(f'"{name}": 0', f'"{name}": {value}')
        path.write_text(text, encoding="utf-8")
    assert net_report.main([str(host), "--no-summary"]) == 0
    problems = next(line for line in capsys.readouterr().out.splitlines() if line.startswith("# problems:"))
    for text in ("tunnel peer samples dropped 4", "machines dropped 4", "connection events past the table 5",
                 "object keys past the table, per interval 6", "sending sites past the table 9", "field names past the table 2",
                 "layouts past the table 1", "batch flushes that did not add up (after the first second) 3"):
        assert text in problems, (text, problems)


def test_report_labels_entries_keyed_by_player_as_players(host):
    # (player_inputs is a fixed-size layout whose key is a player index: only the layouts say so)
    recording = net_report.Recording(host)
    columns, rows = net_report.object_type_table(recording, *recording.select())
    found = {(row[0], row[1]): row[2:] for row in rows}
    assert found[("player", "-")] == [7.0, 100.0]
    assert found[("unknown", "-")] == [2.0, 44.0]


def test_report_matches_host_and_client(host, capsys):
    client = profile_fixture.write_client(host.parent)
    net_report.main([str(host), str(client), "--no-summary"])
    out = capsys.readouterr().out
    assert "## profile_20261005-142240_client is machine 1 of profile_20261005-142233_host" in out
    assert "host -> client: sent 2160 B/s" in out
    stranger = profile_fixture.write_client(host.parent, "profile_x_client")
    path = Path(str(stranger) + ".part1.json")
    path.write_text(path.read_text().replace('"own_machine": 1', '"own_machine": 7'), encoding="utf-8")
    net_report.main([str(host), str(stranger), "--no-summary"])
    assert "warning: profile_20261005-142233_host has no machine 7" in capsys.readouterr().out


def test_report_matches_suffixed_host_and_client_and_names_summary(host, tmp_path, capsys):
    host_parts = [Path(f"{host}.part{number}.json") for number in (1, 2, 3)]
    for path in host_parts:
        text = path.read_text(encoding="utf-8").replace('"map_name": "a30"', '"map_name": "bloodgulch"')
        path.write_text(text.replace('"gametype": "campaign"', '"gametype": "slayer"'), encoding="utf-8")
    host_joined = profile_join.join(host_parts)
    client_base = profile_fixture.write_client(tmp_path, "profile_20261005-142240_client")
    client_part = Path(f"{client_base}.part1.json")
    text = client_part.read_text(encoding="utf-8").replace('"map_name": "a30"', '"map_name": "bloodgulch"')
    client_part.write_text(text.replace('"gametype": "campaign"', '"gametype": "slayer"'), encoding="utf-8")
    client_joined = profile_join.join([client_part])
    assert host_joined.name.endswith("_bloodgulch_slayer.json")
    assert client_joined.name.endswith("_bloodgulch_slayer.json")
    assert net_report.main([str(host_joined), str(client_joined)]) == 0
    out = capsys.readouterr().out
    assert f"## {client_joined.stem} is machine 1 of {host_joined.stem}" in out
    assert "client -> host: sent" in out
    assert host_joined.with_name(host_joined.stem + ".summary.txt").exists()


def test_a_clients_host_is_its_busiest_tunnel_peer(host, capsys):
    # (a client's machines table has no host address, and its tunnel may still
    # hold a peer of the previous game: the host is the peer that sent the most)
    client = profile_fixture.write_client(host.parent)
    recording = net_report.Recording(client)
    columns, rows = net_report.machine_table(recording, *recording.select())
    row = dict(zip(columns, next(row for row in rows if row[0] == "host")))
    assert row["wire_rtt_ms"] == 61
    assert round(row["loss_in%"], 1) == 3.0
    net_report.main([str(host), str(client), "--no-summary"])
    assert "client inbound loss 3.0%" in capsys.readouterr().out


def test_fixture_send_failures_matches_writer_schema(host):
    failures = json.loads(Path(str(host) + ".part1.json").read_text(encoding="utf-8"))["halo"]["send_failures"]
    assert failures["columns"] == ["interval", "machine", "reason", "sends", "bytes", "reliable"]
    assert all(len(row) == len(failures["columns"]) for row in failures["rows"])


def test_report_selects_seconds(host, capsys):
    net_report.main([str(host), "--no-summary", "--from", "2", "--to", "4"])
    out = capsys.readouterr().out
    assert "6.0 s, 180 ticks, 360 frames" in out
    assert "out  object_states     60.0       60.0  2640" in out


def test_report_counts_failed_sends_in_the_batch_invariant(host):
    # (a datagram the socket layer refused is still bytes the batch built)
    path = Path(str(host) + ".part1.json")
    text = path.read_text(encoding="utf-8").replace("[0, 0, 2160, 30]", "[0, 0, 2168, 30]")
    marker = '"send_failures": {"columns": ["interval", "machine", "reason", "sends", "bytes", "reliable"], "rows": [\n'
    assert marker in text
    path.write_text(text.replace(marker, marker + '[0, 0, "would_block", 1, 8, false],\n', 1).replace(
        '[0, 0, "would_block", 1, 8, false],\n]}', '[0, 0, "would_block", 1, 8, false]\n]}'), encoding="utf-8")
    assert net_report.main([str(host), "--no-summary"]) == 0


def test_report_keeps_reliable_failures_out_of_the_datagram_invariant(host):
    path = Path(str(host) + ".part1.json")
    trace = json.loads(path.read_text(encoding="utf-8"))
    failures = trace["halo"]["send_failures"]
    failures["columns"].append("reliable")
    for row in failures["rows"]:
        row.append(False)
    failures["rows"].append([0, 0, "write_failed", 1, 128, True])
    path.write_text("{\n" + ",\n".join(json.dumps(key) + ": " + json.dumps(value)
                                      for key, value in trace.items()) + "\n}", encoding="utf-8")
    assert net_report.main([str(host), "--no-summary"]) == 0


def test_summary_glossary_has_one_line_per_printed_column(host):
    net_report.main([str(host)])
    lines = Path(str(host) + ".summary.txt").read_text(encoding="ascii").splitlines()
    start = lines.index("## columns") + 1
    end = lines.index("", start)
    glossary = [re.split(r"\s{2,}", line, maxsplit=1)[0] for line in lines[start:end]]
    printed = set()
    for index, line in enumerate(lines[end:], end):
        if line.startswith("## ") and index + 1 < len(lines) and lines[index + 1] not in ("", "(none)"):
            printed.update(re.split(r"\s{2,}", lines[index + 1].strip()))
    assert printed and len(glossary) == len(set(glossary))
    assert set(glossary) == printed


def test_summary_glossary_says_what_the_data_counts(host):
    net_report.main([str(host)])
    glossary = {}
    for line in Path(str(host) + ".summary.txt").read_text(encoding="ascii").splitlines():
        parts = re.split(r"\s{2,}", line, maxsplit=1)
        if len(parts) == 2:
            glossary[parts[0]] = parts[1]
    # every reason the writer has, and what a batch's bad_size is
    drops = re.search(r"profile_json_drops\[NUMBER_OF_PROFILE_NET_DROPS\] =\s*\{([^}]*)\}", source("port/linux/src/profile_json.c"))
    reasons = re.findall(r'"(\w+)"', drops.group(1))
    assert len(reasons) == 6
    assert all(reason in glossary["reason"] for reason in reasons)
    assert "batch_header" in glossary["reason"] and "cut short" in glossary["reason"]
    # (the base row of a received message counts it whether it was handled or not)
    assert "discarded messages included" in glossary["in_B/s"] and "also counted" in glossary["dropped/s"]
    assert "reliable" in glossary["failed"]


def test_summary_file_ignores_top(host):
    assert net_report.main([str(host), "--top", "1"]) == 0
    written = Path(str(host) + ".summary.txt").read_text(encoding="ascii")
    assert written == EXPECTED_SUMMARY.read_text(encoding="ascii")
    assert len(written.splitlines()) <= net_report.SUMMARY_LINE_LIMIT


def test_report_top_limits_the_printed_tables(host, capsys):
    net_report.main([str(host), "--no-summary", "--top", "1"])
    assert "... 1 more rows (net_report.py --top N)" in capsys.readouterr().out


def test_report_says_cpu_tables_cover_the_whole_recording(host, capsys):
    net_report.main([str(host), "--no-summary", "--from", "2", "--to", "4"])
    out = capsys.readouterr().out
    assert "# window: 2.0 s selected (seconds 2 to 4)" in out
    assert "cpu tables, ticks and frames cover the whole recording" in out
    assert "## cpu scopes (top 40 by total_ms, whole recording)" in out
    assert "## worst frames (10, whole recording)" in out


def test_report_names_the_broken_part(host, capsys):
    path = Path(str(host) + ".part2.json")
    path.write_text(path.read_text().replace('"cpu_summary"', '"cpu_summery"'), encoding="utf-8")
    assert net_report.main([str(host), "--no-summary"]) == 1
    err = capsys.readouterr().err
    assert err.startswith("net_report: ") and "part2" in err and "cpu_summary" in err
    path.write_text(Path(str(host) + ".part1.json").read_text().replace('"last_part"', '"last_prt"'), encoding="utf-8")
    assert net_report.main([str(host), "--no-summary"]) == 1
    assert "last_part" in capsys.readouterr().err


# ---------- the game's side (the sources)


def source(path):
    return (ROOT / path).read_text(encoding="latin-1")


def function_body(text, signature):
    start = text.index(signature)
    return text[start:text.index("\n}\n", start)]


def test_profile_settings_are_in_port_config():
    names = set(re.findall(r'"(debug\.profile_\w+)"', source("port/linux/game/profile_console.c")))
    assert names == {"debug.profile_record", "debug.profile_record_when", "debug.profile_overlay",
                     "debug.profile_memory"}
    config = source("port/linux/src/port_config.c")
    for name in names | {"debug.profile_overlay"}:
        assert re.search(r'\{ "' + re.escape(name) + r'", _config_', config), name
    block = config[config.index('{ "debug.profile_record"') - 60:config.index('{ "debug.profile_record"')]
    assert "#ifdef HALO_PROFILE" in block


def test_profile_launch_logs_the_compiled_network_version():
    body = function_body(source("port/linux/game/profile_console.c"), "void profile_console_launch(")
    assert 'platform_log("profile: network version %d", HALO_PORT_NETWORK_VERSION);' in body


def test_rejected_nested_batches_have_a_received_drop_row():
    text = source("port/linux/game/network_distributed.c")
    start = text.index("/* (no batch in a batch) */")
    end = text.index("/* (a length that does not fit:", start)
    nested = text[start:end]
    assert re.search(r'#ifdef HALO_PROFILE\s+else if \(profile_net_recording\)', nested)
    assert "_profile_net_received_inner, _profile_net_drop_bad_type" in nested


def test_console_commands_come_before_the_script_compiler():
    body = function_body(source("source/hs/hs.c"),
                         "static boolean hs_compile_and_evaluate_command(\n\tchar const *expression)\n{")
    assert body.index("profile_console_command(expression)") < body.index("hs_playing_in_anothers_game()")


def test_map_load_hook_is_in_game_load():
    # (main_new_map's loads and network_game_manager.c's both call it)
    body = function_body(source("source/game/game.c"), "boolean game_load(\n\tstruct game_options *options)\n{")
    assert "profile_console_map_loaded(options->map_name);" in body


def test_console_keeps_the_profiler_on_in_the_profiling_build():
    console = source("source/main/console.c")
    found = 0
    for match in re.finditer(r"profile_global_enable = FALSE;", console):
        before = console[:match.start()].rstrip().rsplit("\n", 1)[-1]
        assert before.strip() == "#ifndef HALO_PROFILE", before
        found += 1
    assert found == 2


def test_quits_shut_the_recording_down():
    assert "profile_trace_shutdown();" in function_body(source("source/main/main.c"),
                                                         "static void main_exit(\n\tvoid)\n{")
    assert "atexit(profile_trace_shutdown);" in source("port/linux/game/profile_console.c")


def test_sections_feed_the_trace():
    profile = source("source/cseries/profile.c")
    assert "profile_trace_begin(profile_trace_section_names[section->section_index]);" in function_body(
        profile, "void profile_enter_private(\n\tstruct profile_section *section)\n{")
    assert "profile_trace_end(profile_trace_section_names[section->section_index]);" in function_body(
        profile, "void profile_exit_private(\n\tstruct profile_section *section)\n{")
    assert "profile_trace_frame_boundary();" in function_body(profile, "void profile_frame_start(\n\tvoid)\n{")


def test_seams_are_installed_before_a_command_is_handled():
    # (init.txt's profile_record runs before the first frame: the recording's
    # name is chosen from the session then, so a default one would send the
    # parts to the working directory)
    console = source("port/linux/game/profile_console.c")
    command = function_body(console, "boolean profile_console_command(")
    assert command.index("profile_console_install();") < command.index("profile_console_word(")
    assert "profile_trace_set_session(profile_console_session);" in function_body(
        console, "static void profile_console_install(")
    assert "profile_console_install();" in function_body(console, "void profile_console_launch(")


def test_recordings_go_to_their_own_folder_in_the_data_root():
    console = source("port/linux/game/profile_console.c")
    session = function_body(console, "static void profile_console_session(")
    assert '"%s/profiles"' in session and "platform_data_root()" in session
    assert "posix_make_directory(" in session
    # (a folder that cannot be made must not lose the recording: the parts go
    # to the data root itself then)
    assert "posix_directory_open(" in session
    assert "strncpy(session->folder, platform_data_root()" in session


def test_the_memory_default_is_256_mb_everywhere():
    config = source("port/linux/src/port_config.c")
    row = config[config.index('"debug.profile_memory"'):]
    assert row.split(",")[2].strip() == '"256"'
    assert "DEFAULT_PROFILE_TRACE_MEMORY = 256," in source("port/linux/include/profile_trace.h")


def test_android_docs_name_the_profiles_folder():
    for path in ("README.md",):
        text = source(path)
        assert "com.halo.decomp/files/profiles/" in text, path
        assert "com.halo.decomp/files/`" not in text and "com.halo.decomp/files/<name>" not in text, path


def test_console_words_ignore_case_and_seconds_are_checked():
    console = source("port/linux/game/profile_console.c")
    word = function_body(console, "static char const *profile_console_word(")
    assert "'A'" in word and "'Z'" in word
    assert "strtod" in console and "atof" not in console


def test_the_overlay_command_answers_a_wrong_word_with_an_error():
    command = function_body(source("port/linux/game/profile_console.c"), "boolean profile_console_command(")
    assert "profile_overlay_switch(rest)" in command and "strncmp" not in command
    assert "profile: profile_overlay takes on or off" in command


GAME_TICK_STEPS = ("cheats_enforce", "remove_quitting_players", "allegiance", "units", "ai", "actors_drive",
                   "players_before", "effects", "antennas", "first_person", "game_engine", "editor", "hs",
                   "recorded_animations", "objects", "players_after", "hud", "player_effect")
DISTRIBUTED_TICK_STEPS = ("apply_predictions", "apply_vehicle_predictions", "objects_host_tick", "plan_players",
                          "damage_host_tick", "send_statistics", "send_pings", "send_structure_bsp", "send_players",
                          "actors_host_tick", "coop_host_tick", "send_pickups", "send_game_state",
                          "note_own_positions", "send_inputs", "send_predictions", "objects_client_tick",
                          "damage_client_tick", "coop_client_tick", "batches_flush")
SPEC_SECTIONS = {
    "input", "platform_events", "network_start_frame", "main_update_time", "ui_update", "network_end_frame",
    "game_time_update", "non_deterministic_update", "throttle", "present",
    "network_distributed_tick", "render_interpolation_tick", "game_frame",
    *("game_tick." + step for step in GAME_TICK_STEPS),
    *("network_distributed_tick." + step for step in DISTRIBUTED_TICK_STEPS),
    *("network_objects_host_tick." + step for step in ("update_objects", "find_viewers", "send_states",
                                                       "send_inventories", "send_damage_animations")),
}


def game_sources_text():
    return {path: path.read_text(encoding="latin-1")
            for path in [*ROOT.glob("source/**/*.c"), *ROOT.glob("port/linux/game/*.c")]}


def test_every_section_of_the_spec_is_declared():
    declared = set()
    for text in game_sources_text().values():
        declared |= set(re.findall(r'PROFILE_SECTION\(\w+, "([\w.]+)"\)', text))
    assert SPEC_SECTIONS <= declared, sorted(SPEC_SECTIONS - declared)
    p2p = source("port/linux/src/p2p.c")
    assert {"p2p.pass", "p2p.tunnel_receive", "p2p.kcp_update", "p2p.streams"} <= set(
        re.findall(r'profile_trace_name\("(p2p\.\w+)"\)', p2p))


def test_sections_are_entered_and_left_in_pairs():
    for path, text in game_sources_text().items():
        enters = re.findall(r"profile_scope_enter\((\w+)\)", text)
        exits = re.findall(r"profile_scope_exit\((\w+)\)", text)
        for name in set(enters):
            assert exits.count(name) >= enters.count(name), (path.name, name)


def test_no_section_holds_the_console():
    # (outside the game tick today: a section around them would be cut off
    # mid-frame in a build that still turned the profiler off there)
    main = source("source/main/main.c")
    for match in re.finditer(r"profile_scope_enter\((\w+)\)", main):
        inside = main[match.end():main.index(f"profile_scope_exit({match.group(1)})", match.end())]
        assert "console_update" not in inside and "console_open" not in inside, match.group(1)


def test_the_p2p_track_records_under_its_lock():
    console = source("port/linux/game/profile_console.c")
    assert "profile_trace_set_track_lock(p2p_profile_lock, p2p_profile_unlock);" in function_body(
        console, "static void profile_console_install(")
    p2p = source("port/linux/src/p2p.c")
    thread = p2p[p2p.index("static void *p2p_thread(void *unused)"):p2p.index("void p2p_initialize(")]
    relock = thread.index("pthread_mutex_lock(&p2p_lock);", thread.index("posix_socket_select("))
    begin = thread.index("profile_trace_begin(p2p_profile_names.pass);")
    end = thread.index("profile_trace_end(p2p_profile_names.pass);")
    assert relock < begin < end


def enum_messages():
    header = source("port/linux/game/network_distributed.h")
    body = header[header.index("enum\n{\n\t/* a client's own players' units"):]
    body = body[:body.index("NUMBER_OF_DISTRIBUTED_MESSAGES")]
    values, value = {}, 0
    for name, explicit in re.findall(r"\t_distributed_message_(\w+)(?: = (\d+))?,", body):
        value = int(explicit) if explicit else value + 1
        values[name] = value
    return values


def dispatch_handlers():
    """the receive switch's handler of each message type: the function its
    case calls first, or network_distributed_handle_message when the case
    handles it inline (a block, or an if, first)"""
    text = source("port/linux/game/network_distributed.c")
    lines = text[text.rindex("switch (header.type)"):].splitlines()[2:]
    handlers, pending = {}, []
    for line in lines:
        if line == "}":
            break
        case = re.match(r"\tcase _distributed_message_(\w+):\s*(.*)", line)
        if case:
            pending.append(case.group(1))
            line = case.group(2)
            if not line:
                continue
        statement = line.strip()
        if not pending or not statement or statement.startswith(("/*", "break;", "}")):
            continue
        call = re.match(r"(\w+)\(", statement)
        inline = statement == "{" or not call or call.group(1) in ("if", "for", "while", "switch")
        for name in pending:
            handlers[name] = "network_distributed_handle_message" if inline else call.group(1)
        pending = []
    return handlers


def test_message_names_cover_the_enum_and_the_switch():
    table = {name: (text_name, handler) for name, text_name, handler in re.findall(
        r'\{ _distributed_message_(\w+), "(\w+)", "(\w*)" \}', source("port/linux/game/network_distributed.c"))}
    assert set(table) == set(enum_messages())
    handlers = dispatch_handlers()
    assert len(handlers) >= 30
    for name, (text_name, handler) in table.items():
        assert text_name == name
        # (a batch is taken apart at the function's top; a retired number has
        # no handler)
        assert handler == handlers.get(name, "network_distributed_handle_message" if name == "batch" else ""), name


def test_a_new_game_discards_the_profilers_pending_rows_with_the_batches():
    body = function_body(source("port/linux/game/network_distributed.c"), "void network_distributed_new_game(")
    assert ("\t\tdistributed_batches[sender].size = 0;\n#ifdef HALO_PROFILE\n\t\tprofile_net_batch_discard(sender);\n"
            "#endif\n") in body.replace("\r", "")


def test_forwarding_helpers_note_their_callers():
    """a function whose body is one call of a sender forwards its caller's
    message: in the profiling build it is a site macro too"""
    found = 0
    for path in sorted((ROOT / "port/linux/game").glob("*.c")):
        text = path.read_text(encoding="latin-1")
        for match in re.finditer(r"\n(?:static )?void (\w+)\(\n[^{;]*?\)\n\{\n\t(distributed_send\w*)\([^;]*;\n\}\n", text):
            name = match.group(1)
            found += 1
            assert f"#define {name}(" in text and f"PROFILE_NET_SITE(({name})(" in text, f"{path.name}: {name}"
    assert found >= 1


def test_senders_are_counted_where_they_hand_on():
    distributed = source("port/linux/game/network_distributed.c")
    assert "void PROFILE_FUNCTION_NAME(distributed_send)(" in distributed
    assert "void PROFILE_FUNCTION_NAME(distributed_send_to_machine)(" in distributed
    assert "void PROFILE_FUNCTION_NAME(distributed_send_to_machine_reliably)(" in distributed
    for signature, hook in (("static void distributed_batch_flush(", "profile_net_batch_flush("),
                            ("static void distributed_batch_append(", "profile_net_batch_append("),
                            ("static void distributed_fill_header(", "profile_net_built("),
                            ("static void distributed_packer_add(", "profile_net_packed_entry(")):
        body = distributed[distributed.index(signature):]
        assert hook in body[:body.index("\n}\n")], signature
    server = source("source/networking/network_server_message_handler.c")
    for signature in ("boolean network_distributed_server_send_to_machine(",
                      "boolean network_distributed_server_send_to_machine_reliably(",
                      "boolean network_distributed_server_send_to_all_reliably("):
        body = server[server.index(signature):]
        assert "profile_net_" in body[:body.index("\n}\n")], signature
    globals_text = source("source/networking/network_game_globals.c")
    body = globals_text[globals_text.index("boolean network_distributed_client_send_reliably("):]
    assert "profile_net_reliable(" in body[:body.index("\n}\n")]


def test_every_early_return_of_the_receive_has_a_reason():
    text = source("port/linux/game/network_distributed.c")
    body = text[text.index("void network_distributed_handle_message(\n\tlong machine_index,\n\tword const *message,\n"
                           "\tword size)\n{\n\tstruct distributed_message_header header;"):]
    body = body[:body.index("\n}\n")]
    last_stage = None
    returns = 0
    for number, line in enumerate(body.splitlines()):
        if "distributed_receive_stage =" in line:
            last_stage = number
        if line.strip() == "return;":
            returns += 1
            assert last_stage is not None and number - last_stage <= 24, f"return on body line {number}"
    assert returns >= 8
    assert "distributed_receive_stage = _profile_net_drop_handled;" in body
    assert "_profile_net_received_inner" in body
    assert "PROFILE_SECTION(distributed_receive_section, \"network_distributed_receive\")" in text


def test_the_receive_is_renamed_only_in_the_profiling_build():
    # (the public name stays the one the message handlers call; the rename
    # and the wrapper are under HALO_PROFILE, the wire and the normal build
    # are the body's alone)
    text = source("port/linux/game/network_distributed.c")
    start = text.index("#ifdef HALO_PROFILE\n/* how far network_distributed_handle_message got")
    wrapper = text[start:text.index("#endif\n", start)]
    assert wrapper.count("distributed_handle_message(machine_index, message, size);") == 1
    assert "#define network_distributed_handle_message distributed_handle_message" in wrapper
    assert text.count("#define network_distributed_handle_message") == 1


RECEIVE_STAGES = [
    # (the stage in force at each `return;` of the receive, in source order: the brief's table)
    "size < sizeof(header) ? _profile_net_drop_bad_size : _profile_net_drop_not_in_game",
    "batch_fits ? _profile_net_drop_handled : _profile_net_drop_bad_size",
    "header.type == 0 || header.type >= NUMBER_OF_DISTRIBUTED_MESSAGES ? _profile_net_drop_bad_type : "
    "_profile_net_drop_bad_size",
    "header.type == 0 || header.type >= NUMBER_OF_DISTRIBUTED_MESSAGES ? _profile_net_drop_bad_type : "
    "_profile_net_drop_bad_size",
    "_profile_net_drop_wrong_direction",
    "_profile_net_drop_wrong_direction",
    "_profile_net_drop_stale",
    "_profile_net_drop_fast_clock",
]


def receive_stage_at_each_return(text):
    start = text.index("void network_distributed_handle_message(\n\tlong machine_index,\n\tword const *message,\n"
                       "\tword size)\n{\n\tstruct distributed_message_header header;")
    body = text[start:]
    body = body[:body.index("\n}\n")]
    stages = [(match.start(), " ".join(match.group(1).split()))
              for match in re.finditer(r"distributed_receive_stage = ([^;]*);", body)]
    found = []
    for match in re.finditer(r"^\s*return;$", body, re.M):
        before = [stage for position, stage in stages if position < match.start()]
        found.append(before[-1] if before else None)
    dispatch = body.index("\tswitch (header.type)\n\t{\n\tcase _distributed_message_player_prediction:\n\t\tdistributed_handle_predictions")
    handled = [stage for position, stage in stages if position < dispatch][-1]
    return found, handled


def test_each_early_return_of_the_receive_has_the_reason_of_the_table():
    # (the batch's `batch_fits` is cleared where a length does not fit, so a
    # batch that fit is handled)
    found, handled = receive_stage_at_each_return(source("port/linux/game/network_distributed.c"))
    assert found == RECEIVE_STAGES
    assert handled == "_profile_net_drop_handled"


NETCODE_FILES = ("port/linux/game/network_distributed.c", "port/linux/game/network_objects.c",
                 "port/linux/game/network_actors.c", "port/linux/game/network_damage.c",
                 "port/linux/game/network_coop.c")


def entry_structs():
    """the structs of the netcode's fixed-size entries: those its
    *_entry_size functions and the receive switch measure"""
    names = set()
    for path in NETCODE_FILES:
        for match in re.finditer(r"\nword (\w+_entry_size)\(.*?\n\}\n", source(path), re.S):
            names |= set(re.findall(r"sizeof\(struct (\w+)\)", match.group(0)))
    text = source("port/linux/game/network_distributed.c")
    switch = text[text.index("entries of a size of their own: their least here"):]
    names |= set(re.findall(r"sizeof\(struct (\w+)\)", switch[:switch.index("\n\t}\n")]))
    return names


def entry_struct_of_each_message():
    """the struct each message's entries are, as the receive switch and the
    *_entry_size functions pair them: {message name: struct name}"""
    sizes = {}
    for path in NETCODE_FILES:
        for match in re.finditer(r"\nword (\w+_entry_size)\(\s*(?:void|byte type)\)\n\{\n(.*?)\n\}\n", source(path), re.S):
            body = match.group(2)
            only = re.fullmatch(r"\treturn sizeof\(struct (\w+)\);", body)
            if only:
                sizes[match.group(1)] = only.group(1)
    pairs = {}
    label = r"((?:\s*case _distributed_message_\w+:)+)"
    # (network_objects_entry_size's switch, network_damage_entry_size's choice of two)
    objects = source("port/linux/game/network_objects.c")
    switch = objects[objects.index("word network_objects_entry_size("):]
    switch = switch[:switch.index("\n}\n")]
    for labels, struct in re.findall(label + r"\s*return sizeof\(struct (\w+)\);", switch):
        for message in re.findall(r"case _distributed_message_(\w+):", labels):
            pairs[message] = struct
    damage = source("port/linux/game/network_damage.c")
    choice = re.search(r"return type == _distributed_message_(\w+) \?\s*sizeof\(struct (\w+)\) : sizeof\(struct (\w+)\);", damage)
    pairs[choice.group(1)] = choice.group(2)
    pairs["hit_reports"] = choice.group(3)
    text = source("port/linux/game/network_distributed.c")
    receive = text[text.index("entries of a size of their own: their least here"):]
    receive = receive[:receive.index("\n\t}\n")]
    for labels, value in re.findall(label + r"\s*entry_size = ([^;]+);", receive):
        direct = re.fullmatch(r"sizeof\(struct (\w+)\)", value)
        struct = direct.group(1) if direct else sizes.get(value.removesuffix("()"))
        for message in re.findall(r"case _distributed_message_(\w+):", labels):
            if struct:
                pairs[message] = struct
    return pairs


def test_every_fixed_size_entry_has_a_layout():
    structs = entry_structs()
    assert "distributed_object_state" in structs and len(structs) >= 20, sorted(structs)
    netcode = "".join(source(path) for path in NETCODE_FILES)
    for name in structs:
        assert f"{name}_layout[]" in netcode, name
    for function in ("network_objects_profile_register();", "network_actors_profile_register();",
                     "network_damage_profile_register();", "network_coop_profile_register();"):
        assert function in source("port/linux/game/network_distributed.c"), function


def test_each_layout_is_registered_under_the_message_of_its_struct():
    """a layout under another message's type would key and size that
    message's entries by the wrong struct"""
    pairs = entry_struct_of_each_message()
    assert pairs["vehicle_prediction"] == "distributed_object_state" and pairs["hit_reports"] == "distributed_hit_report"
    assert pairs["coop_screen_effect"] == "rasterizer_screen_effect_port_state" and len(pairs) >= 24, pairs
    assert set(pairs.values()) == entry_structs()
    netcode = "".join(source(path) for path in NETCODE_FILES)
    registrations = re.findall(r"profile_net_layout\(_distributed_message_(\w+), (\w+)_layout,", netcode)
    registered = dict(registrations)
    assert len(registered) == len(registrations), "a message type is registered twice"
    assert registered == pairs


def test_every_layout_asserts_that_the_struct_ends_in_its_last_member():
    """the sum of the members and the padding after the last (less than the
    struct's alignment), so a member added at the end breaks the build"""
    netcode = "".join(source(path) for path in NETCODE_FILES)
    names = re.findall(r"typedef char (\w+_layout_assert)\[PROFILE_NET_LAYOUT_OK\(", netcode)
    assert len(names) == len(re.findall(r"typedef char \w+_layout_assert\[", netcode)) == len(entry_structs())
    assert "sizeof(type) - PROFILE_NET_END(type, last) < __alignof__(type)" in source("port/linux/include/profile_net.h")


def test_the_object_describer_takes_only_handles_and_only_while_objects_exist():
    body = function_body(source("port/linux/game/profile_console.c"), "static int profile_console_describe(")
    assert "(key >> 16) == 0" in body and "object_header_data->valid" in body
    assert body.index("object_header_data->valid") < body.index("object_try_and_get(key)")


def test_packed_fields_are_measured():
    text = source("port/linux/game/network_distributed.c")
    definition = text.index("static byte *distributed_put(")
    macro = text.index("#define distributed_put(cursor, data, size)")
    assert definition < macro < text.index("static word distributed_unit_state_write(")
    assert "profile_net_set_object_describe(profile_console_describe);" in source("port/linux/game/profile_console.c")


def test_written_parts_carry_layouts_with_their_tail_and_the_fields_put(written_parts):
    """a layout's members as the compiler laid them out, the padding after
    the last one as tail_pad, and the bytes a packed entry's fields came to"""
    trace = json.loads(written_parts[0].read_text(encoding="utf-8"))["halo"]
    layouts = [row for row in trace["layouts"]["rows"] if row[0] == 7]
    assert [row[1] for row in layouts] == ["object_index", "player_index", "pad", "count", "position", "last", "tail_pad"]
    assert layouts[0][2:] == [0, 4, "datum"] and layouts[-1][2:] == [21, 3, ""]
    # (a field name is shared by the message types that put it: no type of its own)
    assert trace["fields"]["columns"] == ["id", "name", "size"]
    fields = {row[0]: row[1] for row in trace["fields"]["rows"]}
    assert "position" in fields.values()
    counted = {}
    for row in trace["field_bytes"]["rows"]:
        if row[2] == 2:
            counted[fields[row[3]]] = counted.get(fields[row[3]], 0) + row[4]
    assert counted.get("position", 0) > 0 and counted.get("header", 0) > 0, counted


def test_written_parts_emit_dotted_connection_addresses_and_tunnel_peers(written_parts):
    trace = json.loads(written_parts[0].read_text(encoding="utf-8"))["halo"]
    assert [row[1] for row in trace["machines"]["rows"] if row[0] == 201] == ["127.0.0.201:5151"]
    assert [row[1] for row in trace["connections"]["rows"] if row[1].endswith(":5151")] == [
        "127.0.0.201:5151"]
    assert "100.64.0.1" in [row[1] for row in trace["tunnel"]["rows"]]
    machine = next(row for row in trace["machines"]["rows"] if row[0] == 202)
    assert machine[2] == "100.64.0.1"


def test_connection_layer_counts_every_byte_event():
    text = source("source/networking/network_connection.c")
    helper = text[text.index("static void network_connection_profile_traffic("):]
    helper = helper[:helper.index("\n}\n")]
    for event in ("datagram_sent", "datagram_received", "stream_bytes_sent", "stream_bytes_received"):
        assert f"_network_connection_traffic_event_{event}:" in helper, event
    log = function_body(text, "static void network_connection_log_traffic_event(\n\tenum network_connection_traffic_event "
                              "event,\n\tlong amount,\n\tstruct network_connection *connection)\n{")
    # (the close is seen before the early return for amounts of nothing)
    assert log.index("profile_net_connection_closed(connection)") < log.index("if (amount <= 0)")
    # (and before the connection is freed: profile_net.c keeps its pointer)
    delete = function_body(text, "void network_connection_delete(")
    assert delete.index("_network_connection_traffic_event_close") < delete.index("match_free(")
    queued = function_body(text, "long network_connection_profile_queued_bytes(")
    assert "circular_queue_size(queued->reliable_outgoing_queue)" in queued


def test_connections_are_told_their_machines():
    text = source("source/networking/network_server_message_handler.c")
    learnt = "profile_net_connection_machine(network_game_server_get_connection(server), PROFILE_NET_SERVER_DATAGRAMS);"
    assert learnt in text
    # (after the write: the connection has its address from its first traffic event)
    write = "written = network_game_server_write(network_game_server_get_connection(server), buffer, size, &address, 0);"
    assert text.index(write) < text.index(learnt)
    assert text.count("profile_net_connection_machine(connection, machine_index);") >= 1


def test_tunnel_counts_and_once_a_second_sampling():
    p2p = source("port/linux/src/p2p.c")
    for counter in ("profile_bytes_out", "profile_bytes_in", "profile_packets_out", "profile_packets_in",
                    "profile_kcp_payload", "profile_kcp_output"):
        assert p2p.count(counter) >= 3, counter
    assert "int p2p_profile_statistics(struct profile_net_tunnel_peer *peers, int maximum)" in p2p
    # (the peer's counts are the p2p thread's: read under its lock)
    statistics = function_body(p2p, "int p2p_profile_statistics(")
    assert statistics.index("pthread_mutex_lock(&p2p_lock);") < statistics.index("peer->profile_bytes_out")
    assert statistics.index("peer->profile_kcp_output") < statistics.index("pthread_mutex_unlock(&p2p_lock);")
    console = source("port/linux/game/profile_console.c")
    for call in ("profile_net_tunnel(", "profile_net_ping(", "profile_net_sample_queues();",
                 "profile_net_set_peer_describe(p2p_peer_endpoint_address);",
                 "profile_net_set_queue_reader(network_connection_profile_queued_bytes);"):
        assert call in console, call
    # (the seams before any recording, which init.txt's profile_record can start)
    install = function_body(console, "static void profile_console_install(")
    assert "profile_net_set_peer_describe(p2p_peer_endpoint_address);" in install
    assert "profile_net_set_queue_reader(network_connection_profile_queued_bytes);" in install
    frame = function_body(console, "void profile_console_frame(")
    assert "profile_net_sample_queues();" in frame and "profile_console_tunnel();" in frame
    assert "profile_net_simulated_loss();" in source("port/linux/src/xnet.c")


def test_one_ping_is_taken_for_each_machine_and_the_queue_reader_is_const():
    console = source("port/linux/game/profile_console.c")
    pings = function_body(console, "static void profile_console_pings(")
    assert "taken[machine]" in pings and "taken[machine] = 1;" in pings
    assert "struct network_connection const *queued" in source("source/networking/network_connection.c")


def test_the_tunnel_peers_start_over_when_counting_does():
    net = source("port/linux/src/profile_net.c")
    begin = function_body(net, "void profile_net_recording_begin(")
    assert begin.index("if (!__atomic_load_n(&profile_net_counting") < begin.index("profile_net_forget_peers();") < begin.index("__atomic_store_n(&profile_net_counting, 1")
    overlay = function_body(net, "void profile_net_set_overlay(")
    assert "profile_net_forget_peers();" in overlay


def test_the_counting_flag_is_atomic_where_other_threads_read_it():
    net = source("port/linux/src/profile_net.c")
    assert not re.search(r"profile_net_counting\s*=[^=]", net)
    assert "if (!profile_net_counting" not in net


def test_the_overlay_is_fed_and_drawn():
    profile = source("source/cseries/profile.c")
    start = function_body(profile, "void profile_frame_start(\n\tvoid)\n{")
    assert start.index("profile_overlay_feed();") < start.index("profile_internal_step();")
    assert "profile_global_enable = profile_trace_recording() || profile_overlay_visible();" in start
    draw = function_body(source("source/interface/interface.c"), "void interface_draw_fullscreen_overlays(\n\tvoid)\n{")
    assert draw.index("render_debug_profile();") < draw.index("profile_overlay_render();")
    assert re.search(r"#ifdef HALO_PROFILE\n\tprofile_overlay_render\(\);\n#endif", draw)
    assert 'profile_console_word(expression, "profile_overlay")' in source("port/linux/game/profile_console.c")


def test_the_overlay_is_right_justified_at_the_screen_bottom_right():
    render = function_body(source("port/linux/game/profile_overlay.c"), "void profile_overlay_render(\n\tvoid)\n{")
    assert "render.camera.window_bounds" in render
    assert "font_definition_get(interface_get_tag_index(_interface_font_terminal))" in render
    assert "PROFILE_OVERLAY_CONTINUATION_LINE][0] != 0" in render
    assert re.search(r"interface_set_bitmap_text_draw_mode\([^\n]*, 1 /\* right \*/,", render)
    assert "rasterizer_draw_string(&bounds, NULL, NULL, 0, profile_overlay_text);" in render
    assert "bounds.y0 = bounds.y1 - line_count * line_height;" in render


def test_the_overlay_costs_nothing_while_hidden():
    render = function_body(source("port/linux/game/profile_overlay.c"), "void profile_overlay_render(\n\tvoid)\n{")
    assert render.index("if (!profile_overlay_on)\n\t\treturn;") < render.index("profile_trace_clock()")
    feed = function_body(source("source/cseries/profile.c"), "static void profile_overlay_feed(\n\tvoid)\n{")
    assert "if (!profile_overlay_visible() || !profile_global_enable)\n\t\treturn;" in feed
    # (the text is made once a second, in a buffer of the render's own)
    assert "malloc" not in render and "pool_new" not in render


def test_the_overlay_command_and_setting_are_on_the_game_thread_seams():
    console = source("port/linux/game/profile_console.c")
    command = function_body(console, "boolean profile_console_command(\n\tchar const *expression)\n{")
    assert command.index("profile_console_install();") < command.index('"profile_overlay"')
    assert 'console_printf(FALSE, "profile: overlay %s", on ? "on" : "off");' in command
    launch = function_body(console, "void profile_console_launch(\n\tvoid)\n{")
    assert launch.index("profile_console_install();") < launch.index('profile_overlay_toggle(config_boolean("debug.profile_overlay"));')
    toggle = function_body(source("port/linux/game/profile_overlay.c"), "void profile_overlay_toggle(\n\tboolean on)\n{")
    assert "profile_net_set_overlay(on);" in toggle


def test_the_overlay_is_in_the_profiling_build_only():
    for path in ("port/linux/game/profile_overlay.c", "port/linux/src/profile_overlay_lines.c"):
        text = source(path)
        assert text.index("#ifdef HALO_PROFILE") < text.index("#include")
        assert text.rstrip().endswith("#endif"), path


def test_the_main_menu_says_profiling_enabled_in_the_profiling_build_only():
    text = source("source/interface/ui_widget_game_data_input_functions.c")
    assert text.count("profiling enabled") == 1
    body = function_body(text, "static void set_textbox_to_build_number(\n\tstruct widget_instance *widget)\n{")
    assert re.search(r"#ifdef HALO_PROFILE\n(?:(?!#endif)[^\n]*\n)*?[^\n]*profiling enabled[^\n]*\n(?:(?!#endif)[^\n]*\n)*?#else\n", body)
    tags = source("port/linux/game/menu_tags.c")
    assert "HALO_PROFILE_WIDGET(definition->bounds.x1 = (short)(source->left + (source->has_width ? source->width : 640)));" in tags
    end = tags[tags.index("\n#ifdef HALO_PROFILE\n/* at the end of the file"):]
    assert '"main_menu/build_number"' in end and "+ 40);" in end and end.rstrip().endswith("#endif")
    # (nothing that the normal build expands __LINE__ on is below the profiling code)
    assert "malloc(" not in end and "free(" not in end and "allocate(" not in end
    # (the line is under the orange version: main_menu.xml's build_number text, 20 high, one line)
    menu = source("port/assets/menus/ce/main_menu.xml")
    assert 'name="main_menu/build_number"' in menu and 'color="#FFFF8000"' in menu


def test_windows_compile_command_parser_preserves_quoted_define_and_paths():
    command = ('clang -I"source/saved films" -include port\\windows\\include\\prefix.h '
               '-I"C:\\Program Files\\LLVM\\include" '
               '-DHALO_BUILD_FLAVOR=\\"release\\" source\\game.c -o build\\game.o')
    assert parse_compile_command(command, windows=True) == [
        "clang", "-Isource/saved films", "-include", "port/windows/include/prefix.h",
        "-IC:/Program Files/LLVM/include", '-DHALO_BUILD_FLAVOR="release"',
        "source/game.c", "-o", "build/game.o"]


def test_overlay_box_uses_six_or_seven_font_pitches_and_hud_safe_bounds():
    render = function_body(source("port/linux/game/profile_overlay.c"),
                           "void profile_overlay_render(\n\tvoid)\n{")
    assert "render.camera.window_bounds" in render
    assert re.search(r"font_definition_get\([^\n]*_interface_font_terminal", render)
    assert re.search(r"ascending_height\s*\+\s*[^;]*descending_height\s*\+\s*[^;]*leading_height", render)
    lines_header = source("port/linux/include/profile_overlay_lines.h")
    assert re.search(r"PROFILE_OVERLAY_LINE_COUNT\s*=\s*7\s*,", lines_header)
    assert "PROFILE_OVERLAY_CONTINUATION_LINE][0] != 0" in render
    assert "bounds.y0 = bounds.y1 - line_count * line_height;" in render
    assert not re.search(r"bounds\.y0\s*=\s*bounds\.y1\s*-\s*\d+", render)
    assert re.search(r"interface_set_bitmap_text_draw_mode\([^\n]*,\s*1\s*/\* right \*/,\s*0,", render)


def test_network_loss_docs_explain_host_unconnected_socket_scope():
    expected = "debug.network_loss drops only datagrams read by an unconnected socket (the host); a client's connected socket is not affected, so set it on the host."
    readme = source("port/linux/README.md")
    assert expected in readme


# ---------- a normal build is unchanged

PROFILE_PREFIXES = ("profile_trace_", "profile_net_", "profile_json_", "profile_overlay_")


def parse_compile_command(command, windows=False):
    """Split a Ninja C command, normalizing Windows path separators without changing escaped quotes."""
    if windows:
        command = re.sub(r'\\(?!")', "/", command)
    return shlex.split(command, posix=True)


def unavailable(reason):
    """a run that asks for the check (HALO_PROFILE_BASE or HALO_PROFILE_OBJECTS) must not skip it silently"""
    if os.environ.get("HALO_PROFILE_BASE") or os.environ.get("HALO_PROFILE_OBJECTS"):
        pytest.fail(reason)
    pytest.skip(reason)


def preprocess_flags():
    """the game's or the platform layer's compile flags for this computer's
    build, as ninja has them, without HALO_PROFILE and without what makes
    an object; absolute where they name a generated file"""
    missing = [name for name in ("clang", "ninja") if not shutil.which(name)]
    if not (ROOT / "build.ninja").is_file():
        missing.append("a configured build (build.ninja)")
    if missing:
        unavailable("needs " + ", ".join(missing))
    # (HALO_PROFILE_OBJECTS names another configured flavour's object folder, such as build/android/guest/obj,
    # where this computer has no build of its own)
    objects = os.environ.get("HALO_PROFILE_OBJECTS") or f"build/{'windows' if os.name == 'nt' else 'linux'}/obj"

    def flags(object_path):
        listing = subprocess.run(["ninja", "-t", "commands", object_path], cwd=ROOT, capture_output=True, text=True)
        if listing.returncode != 0:
            unavailable(f"needs a build.ninja that has {object_path}")
        command = listing.stdout.strip().splitlines()[-1]
        words = parse_compile_command(command, windows=os.name == "nt")
        while words and not words[0].startswith("-"):
            words = words[1:]
        # (the Android guest's command is a pipeline whose first compile makes assembly)
        words = words[:words.index("&&")] if "&&" in words else words
        kept, skip = [], False
        for word in words:
            if skip:
                skip = False
            elif word in ("-MF", "-o", "-c"):
                skip = True
            elif word in ("-MMD", "-S") or word == "-DHALO_PROFILE" or word.startswith(("-flto", "-fprofile-use")):
                continue
            elif word.endswith(".c") and not word.startswith("-"):
                continue
            elif word.startswith(("build/", "build\\")):
                kept.append(str(ROOT / word))
            elif word.startswith(("-Ibuild/", "-Ibuild\\")):
                kept.append("-I" + str(ROOT / word[2:]))
            else:
                kept.append(word)
        return kept

    flag_sets = {
        "game": flags(f"{objects}/source/game/game.o"),
        "platform": flags(f"{objects}/port/linux/src/p2p.o"),
        "port_game": flags(f"{objects}/port/linux/game/network_coop.o"),
    }
    # (only a missing 32-bit C library is "no build here"; any other error is a wrong command, which must not hide)
    probe = subprocess.run(["clang", *flag_sets["game"], "-E", "-P", "-x", "c", "-"], cwd=ROOT,
                           input="#include <stddef.h>\n#include <stdio.h>\n",
                           capture_output=True, text=True)
    if probe.returncode != 0:
        reason = "the game's flags do not preprocess: " + next((line for line in probe.stderr.splitlines() if "error" in line), "")[:200]
        if "bits/libc-header-start.h" in probe.stderr:
            pytest.skip("needs a 32-bit C library (gcc-multilib); " + reason)
        pytest.fail(reason)
    return flag_sets


def flags_for(path, flag_sets):
    if path.startswith("port/linux/src/"):
        return flag_sets["platform"]
    if path.startswith("port/linux/game/"):
        return flag_sets["port_game"]
    return flag_sets["game"]


def preprocessed(tree, path, flags):
    # (path None: an empty file, which is all that the forced includes leave of a file that is empty)
    arguments = ["-x", "c", "-"] if path is None else [path]
    # (__DATE__ and __TIME__ pinned: main.c prints them, and the two trees can be preprocessed a second apart)
    pinned = ["-Wno-builtin-macro-redefined", '-D__DATE__="Jan  1 2000"', '-D__TIME__="00:00:00"']
    result = subprocess.run(["clang", *flags, *pinned, "-E", "-P", *arguments], cwd=tree,
                            input="" if path is None else None,
                            capture_output=True, text=True)
    assert result.returncode == 0, f"{path}: {result.stderr[-2000:]}"
    # (only the tokens: blank lines and layout differ where #ifdef blocks were)
    return " ".join(result.stdout.split())


def includers_of_changed_headers(base, already):
    """the C files that include a header that changed since the base: a header's #ifdef can change them too"""
    def names(*arguments):
        return subprocess.run(["git", *arguments], cwd=ROOT, capture_output=True, text=True, check=True).stdout.split()

    added = set(names("diff", "--name-only", "--diff-filter=A", base, "HEAD", "--", "*.c"))
    found = set()
    for header in names("diff", "--name-only", "--diff-filter=M", base, "HEAD", "--", "*.h"):
        pattern = r'#[ 	]*include[ 	]*[<"]([^">]*/)?' + re.escape(header.rsplit("/", 1)[-1]) + '[">]'
        for path in names("grep", "-l", "-E", pattern, "HEAD", "--", "*.c"):
            path = path.split(":", 1)[1]
            if path.startswith(("source/", "port/linux/src/", "port/linux/game/")) and path not in added:
                found.add(path)
    return sorted(found - set(already))


@pytest.mark.skipif(not os.environ.get("HALO_PROFILE_BASE"), reason="HALO_PROFILE_BASE names the base commit")
def test_normal_build_preprocesses_as_before(tmp_path):
    base = os.environ["HALO_PROFILE_BASE"]
    flag_sets = preprocess_flags()
    changed = subprocess.run(["git", "diff", "--name-only", "--diff-filter=M", base, "HEAD", "--", "*.c"], cwd=ROOT,
                             capture_output=True, text=True, check=True).stdout.split()
    assert changed, "no C file changed since the base"
    changed += includers_of_changed_headers(base, changed)
    tree = tmp_path / "base"
    subprocess.run(["git", "worktree", "add", "--detach", str(tree), base], cwd=ROOT, check=True, capture_output=True)
    try:
        for path in changed:
            flags = flags_for(path, flag_sets)
            assert preprocessed(tree, path, flags) == preprocessed(ROOT, path, flags), path
    finally:
        subprocess.run(["git", "worktree", "remove", "--force", str(tree)], cwd=ROOT, check=True, capture_output=True)


def test_new_files_are_empty_in_a_normal_build():
    flag_sets = preprocess_flags()
    # (found, not listed: a new profile_*.c that forgets its #ifdef shows up here)
    paths = sorted(str(path.relative_to(ROOT)).replace("\\", "/")
                   for directory in ("port/linux/src", "port/linux/game")
                   for path in (ROOT / directory).glob("profile_*.c"))
    assert len(paths) >= 6, paths
    for path in paths:
        flags = flags_for(path, flag_sets)
        assert preprocessed(ROOT, path, flags) == preprocessed(ROOT, None, flags), path


def test_normal_build_has_no_profiling_symbols():
    ninja_file = ROOT / "build.ninja"
    if not ninja_file.is_file():
        pytest.skip("needs a configured build (build.ninja)")
    if "-DHALO_PROFILE" in ninja_file.read_text(encoding="utf-8"):
        pytest.skip("a profiling build is configured")
    nm = shutil.which("llvm-nm") or shutil.which("nm")
    if os.name == "nt" or os.environ.get("HALO_PROFILE_OBJECTS"):
        folder = ROOT / (os.environ.get("HALO_PROFILE_OBJECTS") or "build/windows/obj")
        objects = sorted(str(path) for path in folder.rglob("*.o"))
    else:
        objects = [str(ROOT / "build/linux/halo")] if (ROOT / "build/linux/halo").is_file() else []
    if not nm:
        unavailable("needs nm")
    if not objects:
        unavailable("needs a built normal build (build/windows/obj/**/*.o, build/linux/halo, or the objects in HALO_PROFILE_OBJECTS)")
    found, words, defined = set(), set(), 0
    for start in range(0, len(objects), 200):
        listing = subprocess.run([nm, "--defined-only", *objects[start:start + 200]], capture_output=True, text=True)
        # (an nm that cannot read the objects, such as bitcode from --lto, lists nothing and would pass)
        assert listing.returncode == 0, listing.stderr[-1000:]
        words |= {word.lstrip("_") for word in listing.stdout.split()}
        defined += sum(len(line.split()) == 3 for line in listing.stdout.splitlines())
    found = {word for word in words if word.startswith(PROFILE_PREFIXES)}
    # (a count, not a named symbol: link-time optimization inlines the functions that have one caller. The game's
    # objects list thousands; an unreadable or a near-empty set lists none)
    assert defined >= 300, f"nm lists {defined} defined symbols: it cannot read these objects"
    assert not found, sorted(found)[:20]


def test_worst_frames_rank_without_allocating():
    # (an allocation per part, near debug.profile_memory = 1024, could fail and quietly rank by the wrong time)
    body = function_body(source("port/linux/src/profile_json.c"), "static void profile_json_cpu_summary(")
    assert "malloc" not in body and "free(" not in body
