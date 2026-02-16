#!/usr/bin/env python3
"""
PostgreSQL data loader for dedup experiments.
Supports bulk-insert, per-file insert, and per-file delete operations.
"""

import argparse
import csv
import hashlib
import os
import subprocess
import time
import uuid
from pathlib import Path


def get_db_connection_string(namespace: str, db_name: str) -> str:
    """Get PostgreSQL connection string via K8s service."""
    return f"host={db_name}.{namespace}.svc.cluster.local port=5432 dbname=dedup user=dedup password=dedup"


def kubectl_exec_psql(namespace: str, db_name: str, sql: str) -> str:
    """Execute SQL via kubectl exec into the PostgreSQL pod."""
    cmd = [
        "kubectl", "exec", "-n", namespace,
        f"deploy/{db_name}", "--",
        "psql", "-U", "dedup", "-d", "dedup", "-t", "-A", "-c", sql
    ]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    if result.returncode != 0:
        print(f"SQL Error: {result.stderr}")
    return result.stdout.strip()


def setup_schema(namespace: str, db_name: str):
    """Create the experiment tables."""
    # Bulk data table
    kubectl_exec_psql(namespace, db_name, """
        CREATE TABLE IF NOT EXISTS bulk_data (
            id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
            timestamp TIMESTAMPTZ DEFAULT now(),
            payload_type TEXT NOT NULL,
            dup_grade TEXT NOT NULL,
            data_text TEXT,
            data_json JSONB,
            data_bytes BYTEA
        );
    """)

    # Per-file table (Stufe 2)
    kubectl_exec_psql(namespace, db_name, """
        CREATE TABLE IF NOT EXISTS files (
            id UUID PRIMARY KEY,
            mime TEXT,
            size_bytes BIGINT,
            sha256 BYTEA,
            payload BYTEA
        );
    """)

    print("Schema created.")


def bulk_insert(namespace: str, db_name: str, data_dir: Path):
    """Stufe 1: Bulk-Insert aller Dateien."""
    setup_schema(namespace, db_name)

    file_count = 0
    for payload_dir in sorted(data_dir.iterdir()):
        if not payload_dir.is_dir():
            continue
        payload_type = payload_dir.name

        for f in sorted(payload_dir.glob("*.dat")):
            content = f.read_bytes()
            # Escape for SQL
            hex_content = content.hex()
            file_id = str(uuid.uuid4())

            kubectl_exec_psql(namespace, db_name, f"""
                INSERT INTO bulk_data (id, payload_type, dup_grade, data_bytes)
                VALUES ('{file_id}', '{payload_type}', '{data_dir.name}',
                        decode('{hex_content}', 'hex'));
            """)
            file_count += 1

            if file_count % 100 == 0:
                print(f"  Inserted {file_count} records...")

    # Report DB size
    db_size = kubectl_exec_psql(namespace, db_name,
        "SELECT pg_total_relation_size('bulk_data');")
    print(f"Bulk insert complete: {file_count} records, DB relation size: {db_size} bytes")


def perfile_insert(namespace: str, db_name: str, data_dir: Path, output: Path):
    """Stufe 2: Per-File Insert with latency tracking."""
    setup_schema(namespace, db_name)

    output.parent.mkdir(parents=True, exist_ok=True)
    latencies = []

    for payload_dir in sorted(data_dir.iterdir()):
        if not payload_dir.is_dir():
            continue

        for f in sorted(payload_dir.glob("*.dat")):
            content = f.read_bytes()
            file_id = str(uuid.uuid4())
            sha256 = hashlib.sha256(content).hexdigest()
            hex_content = content.hex()

            start = time.monotonic()
            kubectl_exec_psql(namespace, db_name, f"""
                INSERT INTO files (id, mime, size_bytes, sha256, payload)
                VALUES ('{file_id}', 'application/octet-stream', {len(content)},
                        decode('{sha256}', 'hex'), decode('{hex_content}', 'hex'));
            """)
            elapsed_ms = (time.monotonic() - start) * 1000
            latencies.append({"file": f.name, "size": len(content), "latency_ms": elapsed_ms})

    with open(output, "w", newline="") as csvfile:
        writer = csv.DictWriter(csvfile, fieldnames=["file", "size", "latency_ms"])
        writer.writeheader()
        writer.writerows(latencies)

    avg_latency = sum(l["latency_ms"] for l in latencies) / max(len(latencies), 1)
    print(f"Per-file insert: {len(latencies)} files, avg latency: {avg_latency:.1f}ms")


def perfile_delete(namespace: str, db_name: str, output: Path):
    """Stufe 3: Per-File Delete with latency tracking."""
    # Get all file IDs
    ids_raw = kubectl_exec_psql(namespace, db_name,
        "SELECT id FROM files ORDER BY id;")
    file_ids = [line.strip() for line in ids_raw.split("\n") if line.strip()]

    output.parent.mkdir(parents=True, exist_ok=True)
    latencies = []

    for file_id in file_ids:
        start = time.monotonic()
        kubectl_exec_psql(namespace, db_name,
            f"DELETE FROM files WHERE id = '{file_id}';")
        elapsed_ms = (time.monotonic() - start) * 1000
        latencies.append({"id": file_id, "latency_ms": elapsed_ms})

    with open(output, "w", newline="") as csvfile:
        writer = csv.DictWriter(csvfile, fieldnames=["id", "latency_ms"])
        writer.writeheader()
        writer.writerows(latencies)

    print(f"Deleted {len(latencies)} files")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--action", choices=["bulk-insert", "perfile-insert", "perfile-delete"],
                        required=True)
    parser.add_argument("--data-dir", type=Path)
    parser.add_argument("--namespace", default="dedup-research")
    parser.add_argument("--db-name", default="postgresql")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    if args.action == "bulk-insert":
        bulk_insert(args.namespace, args.db_name, args.data_dir)
    elif args.action == "perfile-insert":
        perfile_insert(args.namespace, args.db_name, args.data_dir, args.output)
    elif args.action == "perfile-delete":
        perfile_delete(args.namespace, args.db_name, args.output)


if __name__ == "__main__":
    main()
