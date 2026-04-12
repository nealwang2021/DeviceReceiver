#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Migrate legacy SQL recording schema to the new aligned wide-table schema.

Legacy schema:
  - frames
  - frame_samples

New schema:
  - aligned_frames
  - stage_pose_log (optional, preserved from legacy frame stage fields)

The migration is designed to match the CSV layout used by the array-eddy-current
heatmap reference data: one row per frame, with Pos00..Pos39 expanded across columns.

Usage:
  python scripts/migrate_sql_to_aligned.py --source old.db
  python scripts/migrate_sql_to_aligned.py --source old.db --dest new.db
  python scripts/migrate_sql_to_aligned.py --source old.db --overwrite
  python scripts/migrate_sql_to_aligned.py --source old.db --drop-stage
"""

from __future__ import annotations

import argparse
import sqlite3
from pathlib import Path
from typing import Sequence


SQL_ALIGNED_CHANNEL_COUNT = 40


def connect(path: Path) -> sqlite3.Connection:
    conn = sqlite3.connect(str(path))
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA foreign_keys = ON")
    conn.execute("PRAGMA journal_mode = WAL")
    conn.execute("PRAGMA synchronous = NORMAL")
    conn.execute("PRAGMA busy_timeout = 3000")
    return conn


def table_exists(conn: sqlite3.Connection, name: str) -> bool:
    row = conn.execute(
        "SELECT 1 FROM sqlite_master WHERE type IN ('table','view') AND name = ?",
        (name,),
    ).fetchone()
    return row is not None


def table_columns(conn: sqlite3.Connection, table: str) -> set[str]:
    rows = conn.execute(f"PRAGMA table_info({table})").fetchall()
    return {row[1] for row in rows}


def pick_column(columns: set[str], candidates: Sequence[str]) -> str | None:
    for name in candidates:
        if name in columns:
            return name
    return None


def ensure_overwrite(dest: Path, overwrite: bool) -> None:
    if dest.exists() and overwrite:
        dest.unlink()
        wal = dest.with_name(dest.name + "-wal")
        shm = dest.with_name(dest.name + "-shm")
        if wal.exists():
            wal.unlink()
        if shm.exists():
            shm.unlink()


def create_aligned_schema(conn: sqlite3.Connection, preserve_stage: bool) -> None:
    cols: list[str] = [
        "id INTEGER PRIMARY KEY AUTOINCREMENT",
        "frame_sequence INTEGER NOT NULL",
        "timestamp_unix_ms INTEGER NOT NULL",
        "cell_count INTEGER NOT NULL",
        "detect_mode INTEGER NOT NULL DEFAULT 0",
        "source_tag TEXT DEFAULT ''",
        "created_at_ms INTEGER NOT NULL DEFAULT 0",
    ]
    for pos in range(SQL_ALIGNED_CHANNEL_COUNT):
        prefix = f"pos{pos:02d}"
        cols.extend([
            f"{prefix}_amp REAL",
            f"{prefix}_phase REAL",
            f"{prefix}_x REAL",
            f"{prefix}_y REAL",
            f"{prefix}_source_channel INTEGER",
        ])

    ddl = [
        f"CREATE TABLE IF NOT EXISTS aligned_frames ({', '.join(cols)})",
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_aligned_frames_sequence ON aligned_frames(frame_sequence)",
        "CREATE INDEX IF NOT EXISTS idx_aligned_frames_timestamp ON aligned_frames(timestamp_unix_ms)",
    ]

    if preserve_stage:
        ddl.extend([
            "CREATE TABLE IF NOT EXISTS stage_pose_log ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "frame_sequence INTEGER NOT NULL,"
            "timestamp_unix_ms INTEGER NOT NULL,"
            "stage_timestamp_ms INTEGER,"
            "stage_x_mm REAL, stage_y_mm REAL, stage_z_mm REAL,"
            "stage_x_pulse INTEGER, stage_y_pulse INTEGER, stage_z_pulse INTEGER"
            ")",
            "CREATE INDEX IF NOT EXISTS idx_stage_pose_log_sequence ON stage_pose_log(frame_sequence)",
            "CREATE INDEX IF NOT EXISTS idx_stage_pose_log_timestamp ON stage_pose_log(timestamp_unix_ms)",
        ])

    with conn:
        for sql in ddl:
            conn.execute(sql)


def build_insert_sql() -> str:
    columns = [
        "frame_sequence",
        "timestamp_unix_ms",
        "cell_count",
        "detect_mode",
        "source_tag",
        "created_at_ms",
    ]
    values = ["?" for _ in columns]
    for pos in range(SQL_ALIGNED_CHANNEL_COUNT):
        prefix = f"pos{pos:02d}"
        columns.extend([
            f"{prefix}_amp",
            f"{prefix}_phase",
            f"{prefix}_x",
            f"{prefix}_y",
            f"{prefix}_source_channel",
        ])
        values.extend(["?", "?", "?", "?", "?"])

    return f"INSERT INTO aligned_frames({', '.join(columns)}) VALUES({', '.join(values)})"


def build_stage_insert_sql() -> str:
    return (
        "INSERT INTO stage_pose_log(" 
        "frame_sequence, timestamp_unix_ms, stage_timestamp_ms, stage_x_mm, stage_y_mm, stage_z_mm, "
        "stage_x_pulse, stage_y_pulse, stage_z_pulse"
        ") VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)"
    )


def migrate_legacy_db(
    src: sqlite3.Connection,
    dst: sqlite3.Connection,
    source_tag: str,
    preserve_stage: bool,
) -> tuple[int, int]:
    if not table_exists(src, "frames") or not table_exists(src, "frame_samples"):
        raise RuntimeError("source database does not contain legacy frames + frame_samples tables")

    frame_cols = table_columns(src, "frames")
    sample_cols = table_columns(src, "frame_samples")

    frame_id_col = pick_column(frame_cols, ["id"])
    timestamp_col = pick_column(frame_cols, ["timestamp_ms", "timestamp_unix_ms"])
    sequence_col = pick_column(frame_cols, ["sequence", "frame_sequence"])
    frame_no_col = pick_column(frame_cols, ["frame_id", "frame_no"])
    detect_mode_col = pick_column(frame_cols, ["detect_mode"])
    channel_count_col = pick_column(frame_cols, ["channel_count", "cell_count"])

    has_stage_col = pick_column(frame_cols, ["has_stage"])
    stage_ts_col = pick_column(frame_cols, ["stage_timestamp_ms"])
    stage_x_mm_col = pick_column(frame_cols, ["stage_x_mm"])
    stage_y_mm_col = pick_column(frame_cols, ["stage_y_mm"])
    stage_z_mm_col = pick_column(frame_cols, ["stage_z_mm"])
    stage_x_pulse_col = pick_column(frame_cols, ["stage_x_pulse"])
    stage_y_pulse_col = pick_column(frame_cols, ["stage_y_pulse"])
    stage_z_pulse_col = pick_column(frame_cols, ["stage_z_pulse"])

    sample_index_col = pick_column(sample_cols, ["channel_index"])
    sample_source_col = pick_column(sample_cols, ["source_channel", "channel_id"])
    sample_amp_col = pick_column(sample_cols, ["amp"])
    sample_phase_col = pick_column(sample_cols, ["phase"])
    sample_x_col = pick_column(sample_cols, ["x"])
    sample_y_col = pick_column(sample_cols, ["y"])

    if not all([frame_id_col, timestamp_col, sequence_col, frame_no_col, detect_mode_col, channel_count_col]):
        raise RuntimeError("legacy frames table missing required columns")
    if not all([sample_index_col, sample_source_col, sample_amp_col, sample_phase_col, sample_x_col, sample_y_col]):
        raise RuntimeError("legacy frame_samples table missing required columns")

    insert_aligned_sql = build_insert_sql()
    insert_stage_sql = build_stage_insert_sql()

    aligned_count = 0
    stage_count = 0

    frame_rows = src.execute(
        f"SELECT * FROM frames ORDER BY {frame_id_col} ASC"
    ).fetchall()

    with dst:
        dst.execute("PRAGMA user_version = 2")
        aligned_cur = dst.cursor()
        stage_cur = dst.cursor()

        for frame in frame_rows:
            frame_row_id = frame[frame_id_col]
            timestamp_ms = frame[timestamp_col]
            sequence = frame[sequence_col]
            frame_no = frame[frame_no_col]
            detect_mode = frame[detect_mode_col]
            channel_count = frame[channel_count_col] or 0

            samples = src.execute(
                f"SELECT * FROM frame_samples WHERE frame_row_id = ? ORDER BY {sample_index_col} ASC",
                (frame_row_id,),
            ).fetchall()

            row: list[object | None] = [
                sequence if sequence is not None else frame_no,
                timestamp_ms,
                int(channel_count) if channel_count else min(len(samples), SQL_ALIGNED_CHANNEL_COUNT),
                int(detect_mode) if detect_mode is not None else 0,
                source_tag,
                int(timestamp_ms) if timestamp_ms is not None else 0,
            ]
            row.extend([None] * (SQL_ALIGNED_CHANNEL_COUNT * 5))

            for sample in samples:
                pos = int(sample[sample_index_col])
                if pos < 0 or pos >= SQL_ALIGNED_CHANNEL_COUNT:
                    continue

                base = 6 + pos * 5
                amp = sample[sample_amp_col]
                phase = sample[sample_phase_col]
                x = sample[sample_x_col]
                y = sample[sample_y_col]
                src_ch = sample[sample_source_col]

                row[base + 0] = amp
                row[base + 1] = phase
                row[base + 2] = x
                row[base + 3] = y
                row[base + 4] = src_ch if src_ch is not None else pos

            aligned_cur.execute(insert_aligned_sql, row)
            aligned_count += 1

            if preserve_stage and has_stage_col and stage_ts_col:
                has_stage = int(frame[has_stage_col] or 0)
                if has_stage:
                    stage_row = [
                        sequence if sequence is not None else frame_no,
                        timestamp_ms,
                        frame[stage_ts_col] if stage_ts_col else None,
                        frame[stage_x_mm_col] if stage_x_mm_col else None,
                        frame[stage_y_mm_col] if stage_y_mm_col else None,
                        frame[stage_z_mm_col] if stage_z_mm_col else None,
                        frame[stage_x_pulse_col] if stage_x_pulse_col else None,
                        frame[stage_y_pulse_col] if stage_y_pulse_col else None,
                        frame[stage_z_pulse_col] if stage_z_pulse_col else None,
                    ]
                    stage_cur.execute(insert_stage_sql, stage_row)
                    stage_count += 1

    return aligned_count, stage_count


def copy_aligned_db(src: sqlite3.Connection, dst: sqlite3.Connection, source_tag: str, preserve_stage: bool) -> tuple[int, int]:
    if not table_exists(src, "aligned_frames"):
        raise RuntimeError("source database does not contain aligned_frames table")

    with dst:
        dst.execute("PRAGMA user_version = 2")
        rows = src.execute("SELECT * FROM aligned_frames ORDER BY frame_sequence ASC").fetchall()
        cols = [info[1] for info in src.execute("PRAGMA table_info(aligned_frames)").fetchall()]
        if not cols:
            raise RuntimeError("aligned_frames table has no columns")

        # Recreate destination schema if source already matches it.
        insert_sql = f"INSERT INTO aligned_frames({', '.join(cols)}) VALUES({', '.join(['?'] * len(cols))})"
        for row in rows:
            values = [row[col] for col in cols]
            # keep provenance if source DB lacks it
            if "source_tag" in cols:
                idx = cols.index("source_tag")
                if not values[idx]:
                    values[idx] = source_tag
            dst.execute(insert_sql, values)

        stage_count = 0
        if preserve_stage and table_exists(src, "stage_pose_log"):
            stage_rows = src.execute("SELECT * FROM stage_pose_log ORDER BY frame_sequence ASC").fetchall()
            stage_cols = [info[1] for info in src.execute("PRAGMA table_info(stage_pose_log)").fetchall()]
            if stage_cols:
                stage_insert_sql = f"INSERT INTO stage_pose_log({', '.join(stage_cols)}) VALUES({', '.join(['?'] * len(stage_cols))})"
                for row in stage_rows:
                    dst.execute(stage_insert_sql, [row[col] for col in stage_cols])
                    stage_count += 1

    return len(rows), stage_count


def resolve_paths(source: str, dest: str | None, overwrite: bool) -> tuple[Path, Path]:
    src = Path(source).expanduser().resolve()
    if not src.exists():
        raise FileNotFoundError(f"source database not found: {src}")

    if dest:
        dst = Path(dest).expanduser().resolve()
    else:
        suffix = src.stem + "_aligned"
        dst = src.with_name(suffix + src.suffix)

    if src == dst:
        raise ValueError("source and destination must be different; use --dest or copy the file first")

    if dst.exists():
        ensure_overwrite(dst, overwrite)
        if dst.exists() and not overwrite:
            raise FileExistsError(f"destination exists: {dst} (use --overwrite)")

    dst.parent.mkdir(parents=True, exist_ok=True)
    return src, dst


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Migrate realtime SQL databases to the aligned wide-table schema")
    parser.add_argument("--source", required=True, help="Path to legacy or aligned SQLite database")
    parser.add_argument("--dest", help="Destination SQLite database path (default: <source>_aligned.db)")
    parser.add_argument("--overwrite", action="store_true", help="Overwrite destination database if it exists")
    parser.add_argument("--source-tag", default="legacy_migrated", help="Value stored in aligned_frames.source_tag")
    parser.add_argument("--drop-stage", action="store_true", help="Do not create or copy stage_pose_log")
    args = parser.parse_args(argv)

    try:
        src_path, dst_path = resolve_paths(args.source, args.dest, args.overwrite)
    except Exception as e:
        print(f"[ERROR] {e}")
        return 2

    preserve_stage = not args.drop_stage

    try:
        with connect(src_path) as src_conn, connect(dst_path) as dst_conn:
            create_aligned_schema(dst_conn, preserve_stage=preserve_stage)

            if table_exists(src_conn, "aligned_frames") and not table_exists(src_conn, "frames"):
                aligned_count, stage_count = copy_aligned_db(src_conn, dst_conn, args.source_tag, preserve_stage)
            else:
                aligned_count, stage_count = migrate_legacy_db(src_conn, dst_conn, args.source_tag, preserve_stage)

        print(f"[OK] migrated -> {dst_path}")
        print(f"[OK] aligned frames: {aligned_count}")
        if preserve_stage:
            print(f"[OK] stage pose rows: {stage_count}")
        return 0
    except Exception as e:
        print(f"[ERROR] migration failed: {e}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())