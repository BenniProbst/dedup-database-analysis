#!/usr/bin/env python3
"""
PostgreSQL maintenance operations for dedup experiment Stage 3.
Executes VACUUM FULL to reclaim storage and make deleted space visible to Longhorn.
"""

import argparse
import subprocess
import time


def kubectl_exec_psql(namespace: str, db_name: str, sql: str) -> str:
    cmd = [
        "kubectl", "exec", "-n", namespace,
        f"deploy/{db_name}", "--",
        "psql", "-U", "dedup", "-d", "dedup", "-t", "-A", "-c", sql
    ]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
    if result.returncode != 0:
        print(f"SQL Error: {result.stderr}")
    return result.stdout.strip()


def run_maintenance(namespace: str, db_name: str):
    """Run PostgreSQL maintenance operations."""
    print("=== PostgreSQL Maintenance ===")

    # Pre-maintenance sizes
    print("\nPre-maintenance table sizes:")
    sizes = kubectl_exec_psql(namespace, db_name, """
        SELECT relname, pg_total_relation_size(oid) as total_bytes,
               pg_relation_size(oid) as data_bytes
        FROM pg_class WHERE relname IN ('bulk_data', 'files')
        ORDER BY relname;
    """)
    print(sizes)

    # VACUUM FULL (rewrites table, reclaims space, requires exclusive lock)
    print("\nRunning VACUUM FULL on bulk_data...")
    start = time.monotonic()
    kubectl_exec_psql(namespace, db_name, "VACUUM FULL bulk_data;")
    elapsed = time.monotonic() - start
    print(f"  VACUUM FULL bulk_data: {elapsed:.1f}s")

    print("Running VACUUM FULL on files...")
    start = time.monotonic()
    kubectl_exec_psql(namespace, db_name, "VACUUM FULL files;")
    elapsed = time.monotonic() - start
    print(f"  VACUUM FULL files: {elapsed:.1f}s")

    # REINDEX to reclaim index bloat
    print("\nRunning REINDEX...")
    kubectl_exec_psql(namespace, db_name, "REINDEX DATABASE dedup;")

    # Post-maintenance sizes
    print("\nPost-maintenance table sizes:")
    sizes = kubectl_exec_psql(namespace, db_name, """
        SELECT relname, pg_total_relation_size(oid) as total_bytes,
               pg_relation_size(oid) as data_bytes
        FROM pg_class WHERE relname IN ('bulk_data', 'files')
        ORDER BY relname;
    """)
    print(sizes)

    # Checkpoint to flush WAL
    print("\nForcing checkpoint...")
    kubectl_exec_psql(namespace, db_name, "CHECKPOINT;")

    print("\nMaintenance complete. Waiting for filesystem sync...")
    time.sleep(10)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--namespace", default="dedup-research")
    parser.add_argument("--db-name", default="postgresql")
    args = parser.parse_args()
    run_maintenance(args.namespace, args.db_name)


if __name__ == "__main__":
    main()
