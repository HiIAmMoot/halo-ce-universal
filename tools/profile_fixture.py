"""A recording written by hand, as port/linux/src/profile_json.c writes
one, for tools/test_profile.py: a host of two clients over internet play,
six one-second intervals, in three parts or (for the merge test) in one."""

import json
from pathlib import Path

MESSAGE_TYPES = [
    [0, "batch_header", ""],
    [255, "short_message", ""],
    [2, "unit_states", "distributed_handle_unit_states"],
    [6, "object_states", "network_objects_handle_states"],
    [14, "player_inputs", "distributed_handle_inputs"],
    [5, "object_changes", "network_objects_handle_changes"],
]
SITES = [[0, "distributed_host_send_states", 1112], [1, "distributed_host_send_players", 2757],
         [2, "network_objects_send_changes", 700]]
FIELDS = [[0, "header", 0], [1, "position", 0], [2, "unit_index", 0]]
LAYOUTS = [[6, "object_index", 0, 4, "datum"], [6, "flags", 4, 1, ""], [6, "pad", 5, 1, ""],
           [6, "time", 6, 2, ""], [6, "position", 8, 12, ""], [6, "forward", 20, 6, ""], [6, "up", 26, 6, ""],
           [6, "translational_velocity", 32, 6, ""], [6, "angular_velocity", 38, 6, ""],
           [14, "player_index", 0, 1, "player"], [14, "buttons", 1, 3, ""]]
MACHINES = [[0, "100.64.0.2:2302", "203.0.113.7"], [1, "100.64.0.3:2302", "198.51.100.9"]]
CONNECTIONS = [[0, "0.0.0.0:2302", "server_datagrams"], [1, "100.64.0.2:2303", 0], [2, "100.64.0.3:2303", 1]]
# (a real datum handle has bit 31 set, so its key is negative)
HANDLE = 0x80010001 - 2**32
PLAYER = 0xE0000001 - 2**32
UNKNOWN = 0xE0000002 - 2**32
OBJECTS = [[HANDLE, "biped", "characters\\elite\\elite"], [0x20002, "vehicle", "vehicles\\ghost\\ghost"]]


def interval_rows(interval):
    """one second of a host sending to machines 0 and 1"""
    rows = {name: [] for name in ("messages", "received", "built", "entries", "field_bytes", "send_failures",
                                  "traffic", "queues", "tunnel", "pings", "simulated_loss", "datagrams")}
    for machine in (0, 1):
        rows["messages"].append([interval, "out", machine, 6, 0, 1320, 30, 30, False])
        rows["messages"].append([interval, "out", machine, 2, 1, 600, 30, 60, False])
        rows["messages"].append([interval, "out", machine, 0, -1, 240, 30, 0, False])
        rows["datagrams"].append([interval, machine, 2160, 30])
        rows["entries"].append([interval, "out", machine, 6, HANDLE, 660, 15])
        rows["entries"].append([interval, "out", machine, 6, 0x20002, 660, 15])
        rows["received"].append([interval, machine, 0, "", 240, 30, 0, ""])
        rows["received"].append([interval, machine, 14, "distributed_handle_inputs", 900, 30, 30, ""])
        rows["pings"].append([interval, machine, 95 + machine * 10])
    rows["messages"].append([interval, "out", 0, 5, 2, 500, 1, 4, True])
    rows["received"].append([interval, 1, 14, "distributed_handle_inputs", 60, 2, 2, "stale"])
    # (no key, "other", a handle no object row describes: a player's, and an unknown one)
    rows["entries"].append([interval, "out", 0, 6, "", 20, 1])
    rows["entries"].append([interval, "out", 0, 6, "other", 100, 5])
    rows["entries"].append([interval, "out", 0, 2, PLAYER, 60, 3])
    # (a fixed-size entry keyed by a player index, which no object row describes)
    rows["entries"].append([interval, "out", 0, 14, 1, 40, 4])
    rows["entries"].append([interval, "out", 0, 6, UNKNOWN, 44, 2])
    rows["built"].append([interval, 6, 0, 30])
    rows["built"].append([interval, 2, 1, 30])
    rows["built"].append([interval, 5, 2, 1])
    rows["field_bytes"].append([interval, "out", 2, 1, 720, 60])
    rows["field_bytes"].append([interval, "out", 2, 0, 480, 60])
    rows["traffic"].append([interval, "out", "datagram", 0, 4320, 60])
    rows["traffic"].append([interval, "out", "stream", 1, 520, 1])
    rows["traffic"].append([interval, "in", "datagram", 0, 2280, 62])
    rows["queues"].append([interval, 1, 120 * interval])
    rows["tunnel"].append([interval, "100.64.0.2", 2800, 1300, 40, 32, 1, 520, 560, 61])
    rows["tunnel"].append([interval, "100.64.0.3", 2800, 1300, 40, 31, 0, 0, 0, 71])
    return rows


def part(number, intervals, last, frames_before, ticks_before):
    tables = {name: [] for name in ("messages", "received", "built", "entries", "field_bytes", "send_failures",
                                    "traffic", "queues", "tunnel", "pings", "simulated_loss", "datagrams")}
    for interval in intervals:
        for name, rows in interval_rows(interval).items():
            tables[name].extend(rows)
    frames = 60 * len(intervals)
    ticks = 30 * len(intervals)
    header = {
        "format": 1, "build": "0 debug profile", "platform": "windows", "role": "host", "own_machine": -1,
        "map": "levels\\a30\\a30", "map_name": "a30", "gametype": "campaign",
        "players": 3 if number == 1 else 4, "players_most": 4, "start_utc": "2026-10-05 14:22:33",
        "recording": "profile_20261005-142233_host", "part": number, "last_part": last,
        "first_interval": intervals[0], "intervals": len(intervals), "first_frame": frames_before, "frames": frames,
        "first_tick": ticks_before, "ticks": ticks, "start_s": float(intervals[0]), "duration_s": float(len(intervals)),
        "stop_reason": "command" if last else "", "clock_read_ns": 21.5, "memory_used": 1000, "memory_limit": 16777216,
        "writer_wait_ms": 0.0, "foreign_scopes": 0, "deep_scopes": 0, "unbalanced_scopes": 0, "dropped_scopes": 0,
        "foreign_net_events": 0, "entry_keys_overflowed": 0, "dropped_rows": 0, "peers_dropped": 0,
        "machines_dropped": 0, "connections_overflowed": 0, "objects_overflowed": 0, "sites_overflowed": 0,
        "fields_overflowed": 0, "layouts_overflowed": 0, "batches_unbooked": 0,
    }
    halo = {"header": header}
    columns = {
        "message_types": ["id", "name", "handler"], "sites": ["id", "function", "line"],
        "fields": ["id", "name", "size"], "layouts": ["type", "member", "offset", "size", "key"],
        "machines": ["machine", "address", "tunnel_peer"], "connections": ["id", "address", "machine"],
        "objects": ["key", "object_type", "tag"],
        "intervals": ["interval", "start_s", "length_s", "tick", "ticks"],
        "messages": ["interval", "dir", "machine", "type", "site", "bytes", "messages", "entries", "reliable"],
        "received": ["interval", "machine", "type", "handler", "bytes", "messages", "entries", "dropped"],
        "built": ["interval", "type", "site", "messages"],
        "entries": ["interval", "dir", "machine", "type", "key", "bytes", "entries"],
        "field_bytes": ["interval", "dir", "type", "field", "bytes", "count"],
        "send_failures": ["interval", "machine", "reason", "sends", "bytes", "reliable"],
        "traffic": ["interval", "dir", "channel", "connection", "bytes", "packets"],
        "queues": ["interval", "connection", "bytes"],
        "tunnel": ["interval", "peer", "bytes_out", "bytes_in", "packets_out", "packets_in", "lost_in", "kcp_payload",
                   "kcp_output", "round_trip_ms"],
        "pings": ["interval", "machine", "ping_ms"], "simulated_loss": ["interval", "datagrams"],
        "datagrams": ["interval", "machine", "bytes", "datagrams"],
    }
    metadata = {"message_types": MESSAGE_TYPES, "sites": SITES, "fields": FIELDS, "layouts": LAYOUTS,
                "machines": MACHINES, "connections": CONNECTIONS, "objects": OBJECTS,
                "intervals": [[interval, float(interval), 1.0, 30 * interval, 30] for interval in intervals]}
    for name in columns:
        rows = metadata[name] if name in metadata else tables[name]
        halo[name] = {"columns": columns[name], "rows": rows}
    cpu_rows = [
        ["frame", frames, 8.3 * frames, 8.3, 41.2, 1.0, None],
        ["game_tick", ticks, 3.2 * ticks, 3.2, 6.012, 0.5, 3.2],
        ["game_tick.objects", ticks, 1.4 * ticks, 1.4, 12.1, 0.5, 1.4],
        ["network_distributed_tick", ticks, 0.6 * ticks, 0.6, 4.0, 0.5, 0.6],
        ["render", frames, 4.0 * frames, 4.0, 9.8, 1.0, None],
    ]
    worst = [[frames_before + 12, frames_before / 60.0 + 0.2, 41.2 - number, 2,
              [["game_tick.objects", 12.1], ["render", 9.8], ["network_distributed_tick", 4.0]]]]
    cpu = {"frames": frames, "ticks": ticks,
           "columns": ["name", "count", "total_ms", "mean_ms", "max_ms", "per_frame", "per_tick_ms"],
           "rows": cpu_rows, "worst_frames": {"columns": ["frame", "at_s", "frame_ms", "ticks", "longest"],
                                              "rows": worst}}
    return halo, cpu


def write_part(path, halo, cpu):
    """the writer's layout: one item per line, the events last"""
    lines = ['{"halo": {', '"header": ' + json.dumps(halo["header"])]
    for name, table in halo.items():
        if name == "header":
            continue
        lines[-1] += ","
        lines.append(f'"{name}": {{"columns": {json.dumps(table["columns"])}, "rows": [')
        lines.extend(json.dumps(row) + ("," if index < len(table["rows"]) - 1 else "")
                     for index, row in enumerate(table["rows"]))
        lines.append("]}")
    lines[-1] += "},"
    lines.append('"cpu_summary": ' + json.dumps(cpu) + ",")
    lines.append('"traceEvents": [')
    lines.append('{"ph":"M","name":"process_name","pid":1,"args":{"name":"halo host windows"}}')
    lines.append("],")
    lines.append('"displayTimeUnit": "ms"}')
    Path(path).write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_recording(folder, name="profile_20261005-142233_host", split=True):
    """three parts of two intervals each, or the same six in one part"""
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    spans = [[0, 1], [2, 3], [4, 5]] if split else [[0, 1, 2, 3, 4, 5]]
    for index, intervals in enumerate(spans):
        halo, cpu = part(index + 1, intervals, index == len(spans) - 1, intervals[0] * 60, intervals[0] * 30)
        write_part(folder / f"{name}.part{index + 1}.json", halo, cpu)
    return folder / name


def write_client(folder, name="profile_20261005-142240_client"):
    """machine 1's own recording of the same seconds, as one part"""
    halo, cpu = part(1, [0, 1, 2, 3, 4, 5], True, 0, 0)
    halo["header"]["role"] = "client"
    halo["header"]["own_machine"] = 1
    for table in ("messages", "received", "datagrams", "entries", "pings"):
        for row in halo[table]["rows"]:
            row[2 if table != "received" and table != "datagrams" and table != "pings" else 1] = "host"
    halo["machines"]["rows"] = []
    write_part(Path(folder) / f"{name}.part1.json", halo, cpu)
    return Path(folder) / name
