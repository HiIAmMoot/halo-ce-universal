#!/usr/bin/env python3
"""Join leftover Halo profiling parts without loading their event data."""

import argparse
import json
import os
import re
import sys
import warnings
from pathlib import Path

CHUNK = 65536
TAIL = b'\n],\n"displayTimeUnit": "ms"}'


def _find(file, marker, start=0):
    file.seek(start)
    overlap = b""
    while block := file.read(CHUNK):
        data = overlap + block
        found = data.find(marker)
        if found >= 0:
            return file.tell() - len(data) + found
        overlap = data[-len(marker) + 1:]
    raise ValueError(f"marker not found: {marker!r}")


def _value_end(file, start):
    file.seek(start)
    first = file.read(1)
    if first not in (b"{", b"["):
        raise ValueError("expected JSON object or array")
    stack = [b"}" if first == b"{" else b"]"]
    quoted = escaped = False
    while block := file.read(CHUNK):
        for offset, byte in enumerate(block):
            if quoted:
                if escaped:
                    escaped = False
                elif byte == 92:
                    escaped = True
                elif byte == 34:
                    quoted = False
            elif byte == 34:
                quoted = True
            elif byte in (123, 91):
                stack.append(b"}" if byte == 123 else b"]")
            elif byte in (125, 93):
                if not stack or stack.pop() != bytes((byte,)):
                    raise ValueError("malformed JSON delimiters")
                if not stack:
                    return file.tell() - len(block) + offset + 1
    raise ValueError("unterminated JSON value")


def byte_ranges(path):
    with Path(path).open("rb") as file:
        halo_marker = b'{"halo": '
        cpu_marker = b',\n"cpu_summary": '
        events_marker = b',\n"traceEvents": [\n'
        halo_start = _find(file, halo_marker) + len(halo_marker)
        halo_end = _value_end(file, halo_start)
        cpu_start = _find(file, cpu_marker, halo_end) + len(cpu_marker)
        cpu_end = _value_end(file, cpu_start)
        events_start = _find(file, events_marker, cpu_end) + len(events_marker)
        file.seek(0, os.SEEK_END)
        end = file.tell()
        tail_start = max(0, end - len(TAIL) - CHUNK)
        close = _find(file, TAIL, tail_start)
        return halo_start, halo_end, cpu_start, cpu_end, events_start, close


def _is_last_part(path, events_start):
    with Path(path).open("rb") as file:
        prefix = file.read(events_start)
    marker = prefix.find(b'"traceEvents"')
    if marker < 0:
        raise ValueError(f"{path}: no traceEvents array")
    head_text = prefix[:marker].rstrip().rstrip(b",") + b"}"
    try:
        head = json.loads(head_text)
        return bool(head["halo"]["header"]["last_part"])
    except (KeyError, TypeError, ValueError) as error:
        raise ValueError(f"{path}: invalid part header ({error})") from None


def _copy(source, target, start, end):
    source.seek(start)
    while remaining := end - start:
        block = source.read(min(CHUNK, remaining))
        if not block:
            raise OSError("unexpected end of part")
        target.write(block)
        start += len(block)


def joined_name(header, stamp):
    def slug(value, fallback, path_component=False):
        text = str(value or "")
        if path_component:
            text = re.split(r"[\\/]", text)[-1]
        result = []
        for character in text.encode("utf-8"):
            if 65 <= character <= 90:
                character += 32
            result.append(chr(character) if 97 <= character <= 122 or 48 <= character <= 57 or character in (45, 95)
                          else "-")
        return "".join(result) or fallback

    component = slug(header.get("map_name") or header.get("map", ""), "nomap", path_component=True)
    role = slug(header.get("role", "local"), "local")
    gametype = slug(header.get("gametype", "none"), "none")
    return f"profile_{stamp}_{role}_{component}_{gametype}"


def _part_header(path):
    with Path(path).open("rb") as file:
        start = _find(file, b'{"halo": ') + len(b'{"halo": ')
        end = _value_end(file, start)
        file.seek(start)
        return json.loads(file.read(end - start))["header"]


def _collision_free(output):
    output = Path(output)
    stem, suffix = output.stem, output.suffix
    candidate = output
    number = 2
    while candidate.exists() or candidate.with_name(candidate.name + ".tmp").exists():
        candidate = output.with_name(f"{stem}_{number}{suffix}")
        number += 1
    return candidate


def join(parts, output=None, delete_parts=False):
    parts = [Path(path) for path in parts]
    if not parts:
        raise ValueError("no recording parts")
    if output is None:
        header = _part_header(parts[-1])
        stamp = re.match(r"^profile_(\d{8}-\d{6})_", parts[0].name)
        if not stamp:
            raise ValueError(f"cannot find recording timestamp in {parts[0].name}")
        base = joined_name(header, stamp.group(1))
        output = _collision_free(parts[0].parent / f"{base}.json")
    else:
        output = Path(output)
    temporary = output.with_name(output.name + ".tmp")
    ranges = [byte_ranges(path) for path in parts]
    complete = any(_is_last_part(path, offsets[4]) for path, offsets in zip(parts, ranges))
    if delete_parts and not complete:
        raise ValueError("refusing --delete-parts: no part has last_part=true; the recording may be incomplete")
    if not complete:
        warnings.warn("joining an incomplete set: no part has last_part=true", RuntimeWarning)
    temporary_created = False
    output_created = False
    try:
        with temporary.open("xb") as target:
            temporary_created = True
            target.write(b'{"halo_parts": [')
            for index, (path, offsets) in enumerate(zip(parts, ranges)):
                if index:
                    target.write(b", ")
                with path.open("rb") as source:
                    _copy(source, target, offsets[0], offsets[1])
            target.write(b'],\n"cpu_summary_parts": [')
            for index, (path, offsets) in enumerate(zip(parts, ranges)):
                if index:
                    target.write(b", ")
                with path.open("rb") as source:
                    _copy(source, target, offsets[2], offsets[3])
            target.write(b'],\n"traceEvents": [\n')
            for index, (path, offsets) in enumerate(zip(parts, ranges)):
                if index:
                    target.write(b",\n")
                with path.open("rb") as source:
                    _copy(source, target, offsets[4], offsets[5])
            target.write(b'\n],\n"displayTimeUnit": "ms"}\n')
        descriptor = os.open(output, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o666)
        output_created = True
        with os.fdopen(descriptor, "wb") as target, temporary.open("rb") as source:
            while block := source.read(CHUNK):
                target.write(block)
            target.flush()
            os.fsync(target.fileno())
        temporary.unlink()
    except Exception:
        if temporary_created:
            temporary.unlink(missing_ok=True)
        if output_created:
            output.unlink(missing_ok=True)
        raise
    if delete_parts:
        for path in parts:
            path.unlink()
    return output


def part_paths(recording):
    path = Path(recording)
    match = re.match(r"^(.*)\.part(\d+)\.json$", path.name)
    base = path.parent / (match.group(1) if match else path.stem)
    numbered = {}
    for candidate in base.parent.glob(base.name + ".part*.json"):
        match = re.match(r"^" + re.escape(base.name) + r"\.part(\d+)\.json$", candidate.name)
        if match:
            numbered[int(match.group(1))] = candidate
    if not numbered or sorted(numbered) != list(range(1, max(numbered) + 1)):
        raise ValueError(f"missing or no parts for {base}")
    return base, [numbered[index] for index in sorted(numbered)]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recording")
    parser.add_argument("--delete-parts", action="store_true")
    args = parser.parse_args(argv)
    try:
        base, parts = part_paths(args.recording)
        join(parts, delete_parts=args.delete_parts)
    except (OSError, ValueError) as error:
        print(f"profile_join: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
