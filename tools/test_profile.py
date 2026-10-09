"""Profiling build, recorder, real output and stage-owned report tests.
For local WSL runs in a Windows worktree, set HALO_GIT=git.exe.
"""

import csv
import io
import json
import os
import re
import shlex
import shutil
import subprocess
import tarfile
from pathlib import Path
from types import SimpleNamespace

import pytest

from tools import linux_build, net_report, ninja_syntax, profile_fixture

ROOT = Path(__file__).resolve().parent.parent
GIT = os.environ.get("HALO_GIT", "git")


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

PROFILE_SOURCES = ["port/linux/src/profile_trace.c", "port/linux/src/profile_net.c", "port/linux/src/profile_json.c"]


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


def build_check(tmp_path, sanitizers):
    compiler = check_compiler()
    if not compiler:
        pytest.skip("needs a C compiler with POSIX threads (Linux, WSL)")
    program = tmp_path / ("profile_check" + "".join(f"_{name}" for name in sanitizers))
    macro = tmp_path / "distributed_put_macro.h"
    macro.write_text(distributed_put_macro(), encoding="utf-8")
    flags = [f"-fsanitize={','.join(sanitizers)}", "-fno-omit-frame-pointer"] if sanitizers else []
    built = subprocess.run([compiler, "-std=gnu11", "-Wall", "-Werror", "-DHALO_PROFILE", "-pthread", "-g", "-O1",
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


def test_check_program_has_no_leaks_or_memory_errors(tmp_path):
    """AddressSanitizer and LeakSanitizer over every recording the check
    makes: arenas, the writer's files, the parts"""
    run_check(build_check(tmp_path, ["address", "undefined"]), tmp_path / "out")


def test_check_program_has_no_data_races(tmp_path):
    """ThreadSanitizer: the p2p track, the writer hand-off, the recording
    flag"""
    run_check(build_check(tmp_path, ["thread"]), tmp_path / "out")


@pytest.fixture(scope="module")
def written_parts(tmp_path_factory):
    folder = tmp_path_factory.mktemp("written")
    run_check(build_check(tmp_path_factory.mktemp("build"), []), folder)
    recordings = {}
    for path in folder.glob("*.part*.json"):
        try:
            header = json.loads(path.read_text(encoding="utf-8"))["halo"]["header"]
        except (OSError, json.JSONDecodeError, KeyError):
            continue
        recordings.setdefault(header["recording"], []).append((header["part"], path))
    parts = max(recordings.values(), key=len)
    assert len(parts) >= 2, "the real C recording is split into parts"
    return [path for number, path in sorted(parts)]


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
    with trace_processor.TraceProcessor(trace=str(written_parts[0])) as processor:
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
    names = ("objects_overflowed", "sites_overflowed",
             "fields_overflowed", "layouts_overflowed", "batches_unbooked")
    for path in written_parts:
        header = json.loads(path.read_text(encoding="utf-8"))["halo"]["header"]
        assert all(isinstance(header[name], int) and header[name] >= 0 for name in names), header


def test_written_parts_read_by_the_report(written_parts):
    recording = net_report.Recording(written_parts[0])
    assert recording.complete and not recording.warnings
    cpu_rows = [dict(zip(summary["columns"], row)) for summary in
                [json.loads(path.read_text(encoding="utf-8"))["cpu_summary"] for path in written_parts]
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


# ---------- tools/net_report.py

EXPECTED_SUMMARY = ROOT / "tools" / "profile_fixture.summary.txt"


@pytest.fixture
def host(tmp_path):
    return profile_fixture.write_recording(tmp_path)


def test_cpu_only_duration_comes_from_each_part_header(tmp_path):
    recording = cpu_recording(tmp_path)
    assert recording.select() == (set(), 4.0)
    assert recording.select(0.5, 3.0) == (set(), 2.5)
    assert recording.select(5.0, 6.0) == (set(), 0.0)


def cpu_recording(tmp_path, role="host"):
    base = tmp_path / f"compat_{role}"
    for number, start, duration in ((1, 0.0, 1.25), (2, 1.25, 2.75)):
        halo, cpu = profile_fixture.part(number, [number - 1], number == 2, 0, 0)
        halo["header"].update(role=role, start_s=start, duration_s=duration)
        profile_fixture.write_part(Path(f"{base}.part{number}.json"), {"header": halo["header"]}, cpu)
    return net_report.Recording(base)


def test_cpu_only_csv_marks_all_message_and_field_tables_not_recorded(tmp_path):
    recording = cpu_recording(tmp_path)
    directory = tmp_path / "csv"
    net_report.write_csv(recording, directory, None, None, None)
    for name in ("message_types", "sending_functions", "handlers", "dropped", "object_types", "fields"):
        with (directory / f"{recording.name}.{name}.csv").open() as file:
            rows = list(csv.reader(file))
        assert len(rows) == 2 and set(rows[1]) == {"not recorded"}
    output = "\n".join(net_report.summary_lines(recording, None, None, 20, None)[0])
    assert "network tables: unavailable" in output



def test_report_summary_is_the_expected_one(host):
    assert net_report.main([str(host) + ".part2.json"]) == 0
    written = Path(str(host) + ".summary.txt").read_text(encoding="ascii")
    assert written == EXPECTED_SUMMARY.read_text(encoding="ascii")
    assert len(written.splitlines()) <= net_report.SUMMARY_LINE_LIMIT


def test_report_distinguishes_measured_idle_network_from_absent_tables(tmp_path, capsys):
    halo, cpu = profile_fixture.part(1, [0], True, 0, 0)
    for name, table in halo.items():
        if name not in ("header", "intervals"):
            table["rows"] = []
    path = tmp_path / (halo["header"]["recording"] + ".part1.json")
    profile_fixture.write_part(path, halo, cpu)
    assert net_report.main([str(path), "--no-summary"]) == 0
    output = capsys.readouterr().out
    assert "network tables: unavailable" not in output
    assert "## message types" in output


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
    assert net_report.main([str(paths[0]), "--no-summary"]) == 0


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
    for number, values in ((1, (6, 7, 2, 1, 2)), (2, (0, 9, 0, 0, 1))):
        path = Path(str(host) + f".part{number}.json")
        text = path.read_text(encoding="utf-8")
        names = ("objects_overflowed", "sites_overflowed",
                 "fields_overflowed", "layouts_overflowed", "batches_unbooked")
        for name, value in zip(names, values):
            assert text.count(f'"{name}": 0') == 1
            text = text.replace(f'"{name}": 0', f'"{name}": {value}')
        path.write_text(text, encoding="utf-8")
    assert net_report.main([str(host), "--no-summary"]) == 0
    problems = next(line for line in capsys.readouterr().out.splitlines() if line.startswith("# problems:"))
    for text in ("object keys past the table, per interval 6", "sending sites past the table 9", "field names past the table 2",
                 "layouts past the table 1", "batch flushes that did not add up (after the first second) 3"):
        assert text in problems, (text, problems)


def test_report_labels_entries_keyed_by_player_as_players(host):
    # (player_inputs is a fixed-size layout whose key is a player index: only the layouts say so)
    recording = net_report.Recording(host)
    columns, rows = net_report.object_type_table(recording, *recording.select())
    found = {(row[0], row[1]): row[2:] for row in rows}
    assert found[("player", "-")] == [7.0, 100.0]
    assert found[("unknown", "-")] == [2.0, 44.0]


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
    writer = (ROOT / "port/linux/src/profile_json.c").read_text()
    drops = re.search(r"profile_json_drops[^=]*=\s*\{([^}]+)\}", writer)[1]
    reasons = [reason for reason in re.findall(r'"([^\"]*)"', drops) if reason]
    assert all(reason in glossary["reason"] for reason in reasons)
    assert "batch_header" in glossary["reason"] and "cut short" in glossary["reason"]
    # (the base row of a received message counts it whether it was handled or not)
    assert "also counted" in glossary["dropped/s"]


def test_summary_file_ignores_top(host):
    assert net_report.main([str(host), "--top", "1"]) == 0
    written = Path(str(host) + ".summary.txt").read_text(encoding="ascii")
    assert written == EXPECTED_SUMMARY.read_text(encoding="ascii")
    assert len(written.splitlines()) <= net_report.SUMMARY_LINE_LIMIT


def test_report_top_limits_the_printed_tables(host, capsys):
    net_report.main([str(host), "--no-summary", "--top", "1"])
    assert "... 5 more rows (net_report.py --top N)" in capsys.readouterr().out


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


def source(path):
    return (ROOT / path).read_text(encoding="latin-1")


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


def test_windows_compile_command_parser_preserves_quoted_define_and_paths():
    command = ('clang -I"source/saved films" -include port\\windows\\include\\prefix.h '
               '-I"C:\\Program Files\\LLVM\\include" '
               '-DHALO_BUILD_FLAVOR=\\"release\\" source\\game.c -o build\\game.o')
    assert parse_compile_command(command, windows=True) == [
        "clang", "-Isource/saved films", "-include", "port/windows/include/prefix.h",
        "-IC:/Program Files/LLVM/include", '-DHALO_BUILD_FLAVOR="release"',
        "source/game.c", "-o", "build/game.o"]


def test_console_words_and_record_seconds_use_the_real_parser(tmp_path):
    compiler = check_compiler()
    if not compiler:
        pytest.skip("needs a C compiler (Linux, WSL)")
    text = (ROOT / "port/linux/game/profile_console.c").read_text()
    word = text[text.index("static char const *profile_console_word("):text.index("/* ---------- public code */")]
    command = text[text.index("boolean profile_console_command("):text.rindex("\n#endif")]
    program = tmp_path / "console_check"
    code = tmp_path / "console_check.c"
    # Compile the game's actual parsing functions; only their external game effects are stubbed.
    code.write_text(r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "profile_trace.h"
typedef int boolean;
#define TRUE 1
#define FALSE 0
static int requests;
static double accepted_seconds;
static void profile_console_install(void) {}
static long config_integer(const char *name) { (void)name; return 32; }
static void console_printf(boolean flag, const char *format, ...) { (void)flag; (void)format; }
int profile_trace_request_start(double seconds, int when, long memory) {
    assert(when == _profile_trace_when_now && memory == 32);
    requests++; accepted_seconds = seconds; return _profile_trace_answer_armed;
}
int profile_trace_request_stop(void) { return _profile_trace_answer_not_recording; }
void profile_trace_status(struct profile_trace_status *status) { memset(status, 0, sizeof(*status)); }
''' + word + command + r'''
int main(void) {
    assert(strcmp(profile_console_word(" (PROFILE_RECORD 1.5)", "profile_record"), "1.5)") == 0);
    assert(profile_console_word("profile_record_extra", "profile_record") == NULL);
    assert(profile_console_command("PROFILE_RECORD 1.5"));
    assert(requests == 1 && accepted_seconds == 1.5);
    assert(profile_console_command("profile_record x"));
    assert(requests == 1);
    assert(profile_console_command("profile_record -1"));
    assert(requests == 1);
    assert(profile_console_command("profile_record"));
    assert(requests == 2 && accepted_seconds == 0.0);
    assert(!profile_console_command("profile_record_extra 1"));
    return 0;
}
''')
    subprocess.run([compiler, "-std=gnu11", "-Wall", "-Werror", "-DHALO_PROFILE",
                    "-I", str(ROOT / "port/linux/include"), str(code), "-o", str(program)], check=True)
    subprocess.run([str(program)], check=True)


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
        return subprocess.run([GIT, *arguments], cwd=ROOT, capture_output=True, text=True, check=True).stdout.split()

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
    changed = subprocess.run([GIT, "diff", "--name-only", "--diff-filter=M", base, "HEAD", "--", "*.c"], cwd=ROOT,
                             capture_output=True, text=True, check=True).stdout.split()
    assert changed, "no C file changed since the base"
    changed += includers_of_changed_headers(base, changed)
    tree = tmp_path / "base"
    tree.mkdir()
    archive = subprocess.run([GIT, "archive", "--format=tar", base], cwd=ROOT, check=True, capture_output=True).stdout
    with tarfile.open(fileobj=io.BytesIO(archive)) as reference:
        reference.extractall(tree)
    for path in changed:
        flags = flags_for(path, flag_sets)
        assert preprocessed(tree, path, flags) == preprocessed(ROOT, path, flags), path


def test_new_files_are_empty_in_a_normal_build():
    flag_sets = preprocess_flags()
    # (found, not listed: a new profile_*.c that forgets its #ifdef shows up here)
    paths = sorted(str(path.relative_to(ROOT)).replace("\\", "/")
                   for directory in ("port/linux/src", "port/linux/game")
                   for path in (ROOT / directory).glob("profile_*.c"))
    assert set(paths) == {*PROFILE_SOURCES, "port/linux/game/profile_console.c"}, paths
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




def write_partial(tmp_path, missing):
    halo, cpu = profile_fixture.part(1, [0], True, 0, 0)
    for table in missing:
        halo.pop(table)
    path = tmp_path / "partial.part1.json"
    profile_fixture.write_part(path, halo, cpu)
    return net_report.Recording(path)


@pytest.mark.parametrize("missing,built,sent,size", [
    ("built", "not recorded", 60.0, 2640.0),
    ("messages", 30.0, "not recorded", "not recorded"),
])
def test_sending_metrics_require_their_own_source_in_text_and_csv(tmp_path, missing, built, sent, size):
    recording = write_partial(tmp_path, [missing])
    columns, rows = net_report.sending_function_table(recording, *recording.select())
    row = next(row for row in rows if row[0] == "distributed_host_send_states:1112")
    assert row[2:] == [built, sent, size]
    assert "not recorded" in "\n".join(net_report.format_table(columns, rows))
    directory = tmp_path / "csv"
    net_report.write_csv(recording, directory, None, None, None)
    with (directory / f"{recording.name}.sending_functions.csv").open() as file:
        exported = next(row for row in csv.DictReader(file)
                        if row["function:line"] == "distributed_host_send_states:1112")
    if missing == "built":
        assert exported["built/s"] == "not recorded" and exported["sent/s"] == "60.0"
    else:
        assert exported["built/s"] == "30.0" and exported["sent/s"] == exported["B/s"] == "not recorded"


@pytest.mark.parametrize("missing", ["messages", "datagrams", "send_failures"])
def test_missing_invariant_inputs_are_not_reported_as_corruption(tmp_path, capsys, missing):
    recording = write_partial(tmp_path, [missing])
    assert net_report.main([str(recording.base), "--no-summary"]) == 0
    captured = capsys.readouterr()
    assert "batch invariant: not recorded" in captured.out
    assert "batch bytes do not add up" not in captured.out
    assert "datagrams " not in captured.err
