#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""One-shot migrate + export + validate aligned SQL data.

What it does
  1) Migrate a legacy SQLite database into the aligned wide-table schema.
  2) Export aligned_frames to a CSV file matching the reference layout.
  3) Validate row counts, required columns, and per-row channel alignment.

Typical usage
  python scripts/export_and_validate_aligned_sql.py --source data/device_realtime_xxx.db
  python scripts/export_and_validate_aligned_sql.py --source old.db --dest out.db --csv out.csv
  python scripts/export_and_validate_aligned_sql.py --source old.db --report out/report.txt

The script prints PASS/FAIL and also writes a compact report file when --report is used.
"""

from __future__ import annotations

import argparse
import csv
import sqlite3
import sys
from pathlib import Path
from typing import Iterable, Sequence


SCRIPT_DIR = Path(__file__).resolve().parent
ROOT_DIR = SCRIPT_DIR.parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from migrate_sql_to_aligned import (  # noqa: E402
    SQL_ALIGNED_CHANNEL_COUNT,
    connect,
    copy_aligned_db,
    create_aligned_schema,
    migrate_legacy_db,
    table_exists,
)


def resolve_default_paths(source: Path, dest: Path | None, csv_path: Path | None) -> tuple[Path, Path, Path]:
    source = source.expanduser().resolve()
    if dest is None:
        dest = source.with_name(source.stem + "_aligned.db")
    else:
        dest = dest.expanduser().resolve()

    if csv_path is None:
        csv_path = dest.with_suffix(".csv")
    else:
        csv_path = csv_path.expanduser().resolve()

    return source, dest, csv_path


def ensure_parent(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)


def export_aligned_csv(conn: sqlite3.Connection, csv_path: Path) -> int:
    if not table_exists(conn, "aligned_frames"):
        raise RuntimeError("aligned_frames table not found")

    cols = [row[1] for row in conn.execute("PRAGMA table_info(aligned_frames)").fetchall()]
    if not cols:
        raise RuntimeError("aligned_frames has no columns")

    rows = conn.execute("SELECT * FROM aligned_frames ORDER BY frame_sequence ASC").fetchall()

    ensure_parent(csv_path)
    with csv_path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.writer(f)
        writer.writerow([
            "FrameSequence",
            "TimestampUnixMs",
            "CellCount",
            *[
                item
                for pos in range(SQL_ALIGNED_CHANNEL_COUNT)
                for item in (
                    f"Pos{pos:02d}_Amp",
                    f"Pos{pos:02d}_Phase",
                    f"Pos{pos:02d}_X",
                    f"Pos{pos:02d}_Y",
                    f"Pos{pos:02d}_SourceChannel",
                )
            ],
        ])

        for row in rows:
            out = [
                row["frame_sequence"],
                row["timestamp_unix_ms"],
                row["cell_count"],
            ]
            for pos in range(SQL_ALIGNED_CHANNEL_COUNT):
                prefix = f"pos{pos:02d}"
                out.extend([
                    row[f"{prefix}_amp"],
                    row[f"{prefix}_phase"],
                    row[f"{prefix}_x"],
                    row[f"{prefix}_y"],
                    row[f"{prefix}_source_channel"],
                ])
            writer.writerow(out)

    return len(rows)


def validate_exported_csv(conn: sqlite3.Connection, csv_path: Path) -> list[str]:
    issues: list[str] = []
    if not csv_path.exists():
        return [f"CSV not found: {csv_path}"]

    db_count = conn.execute("SELECT COUNT(*) FROM aligned_frames").fetchone()[0]
    with csv_path.open("r", encoding="utf-8", newline="") as f:
        reader = csv.reader(f)
        try:
            header = next(reader)
        except StopIteration:
            return ["CSV is empty"]
        csv_rows = list(reader)

    expected_header = [
        "FrameSequence",
        "TimestampUnixMs",
        "CellCount",
        *[
            item
            for pos in range(SQL_ALIGNED_CHANNEL_COUNT)
            for item in (
                f"Pos{pos:02d}_Amp",
                f"Pos{pos:02d}_Phase",
                f"Pos{pos:02d}_X",
                f"Pos{pos:02d}_Y",
                f"Pos{pos:02d}_SourceChannel",
            )
        ],
    ]

    if header != expected_header:
        issues.append("CSV header does not match aligned reference layout")

    if len(csv_rows) != db_count:
        issues.append(f"row count mismatch: db={db_count}, csv={len(csv_rows)}")

    # Quick structural check on the first/last row lengths.
    if csv_rows:
        expected_len = len(expected_header)
        first_len = len(csv_rows[0])
        last_len = len(csv_rows[-1])
        if first_len != expected_len or last_len != expected_len:
            issues.append(f"row width mismatch: expected={expected_len}, first={first_len}, last={last_len}")

    return issues


def write_report(report_path: Path, lines: Sequence[str]) -> None:
    ensure_parent(report_path)
    report_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Migrate SQL, export aligned CSV, and validate result in one step")
    ap.add_argument("--source", required=True, help="Source SQLite db path (legacy or already aligned)")
    ap.add_argument("--dest", help="Destination aligned SQLite db path")
    ap.add_argument("--csv", dest="csv_path", help="Export CSV path")
    ap.add_argument("--report", help="Write a text report to this path")
    ap.add_argument("--overwrite", action="store_true", help="Overwrite destination files if present")
    ap.add_argument("--source-tag", default="legacy_migrated", help="source_tag stored in aligned_frames")
    ap.add_argument("--drop-stage", action="store_true", help="Do not keep stage_pose_log")
    args = ap.parse_args(argv)

    source, dest, csv_path = resolve_default_paths(Path(args.source), Path(args.dest) if args.dest else None,
                                                  Path(args.csv_path) if args.csv_path else None)

    if source == dest:
        print("[ERROR] source and destination must be different")
        return 2

    if dest.exists() and not args.overwrite:
        print(f"[ERROR] destination exists: {dest} (use --overwrite)")
        return 2
    if csv_path.exists() and not args.overwrite:
        print(f"[ERROR] csv exists: {csv_path} (use --overwrite)")
        return 2

    report_lines: list[str] = []
    preserve_stage = not args.drop_stage

    try:
        with connect(source) as src_conn, connect(dest) as dst_conn:
            create_aligned_schema(dst_conn, preserve_stage=preserve_stage)
            if table_exists(src_conn, "aligned_frames") and not table_exists(src_conn, "frames"):
                aligned_count, stage_count = copy_aligned_db(src_conn, dst_conn, args.source_tag, preserve_stage)
            else:
                aligned_count, stage_count = migrate_legacy_db(src_conn, dst_conn, args.source_tag, preserve_stage)

            csv_count = export_aligned_csv(dst_conn, csv_path)
            issues = validate_exported_csv(dst_conn, csv_path)

        report_lines.append(f"source={source}")
        report_lines.append(f"dest={dest}")
        report_lines.append(f"csv={csv_path}")
        report_lines.append(f"aligned_rows={aligned_count}")
        report_lines.append(f"stage_rows={stage_count}")
        report_lines.append(f"csv_rows={csv_count}")
        if issues:
            report_lines.append("result=FAIL")
            report_lines.extend([f"issue={item}" for item in issues])
            print("[RESULT] FAIL")
            for item in issues:
                print(f"[ISSUE] {item}")
            if args.report:
                write_report(Path(args.report), report_lines)
            return 1

        report_lines.append("result=PASS")
        print("[RESULT] PASS")
        print(f"[OK]   migrated db : {dest}")
        print(f"[OK]   exported csv: {csv_path}")
        print(f"[OK]   aligned rows: {aligned_count}")
        print(f"[OK]   csv rows    : {csv_count}")
        if preserve_stage:
            print(f"[OK]   stage rows  : {stage_count}")
        if args.report:
            write_report(Path(args.report), report_lines)
        return 0
    except Exception as e:
        report_lines.append("result=FAIL")
        report_lines.append(f"error={e}")
        if args.report:
            write_report(Path(args.report), report_lines)
        print(f"[ERROR] {e}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())