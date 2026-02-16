#!/usr/bin/env python3
"""
Generate test datasets with controlled duplication degrees (U0/U50/U90).

Usage:
    python3 generate_datasets.py --output-dir /tmp/datasets \
        --dup-grades U0 U50 U90 \
        --payload-types text image event json uuid bank
"""

import argparse
import hashlib
import json
import os
import random
import string
import struct
import uuid
from datetime import datetime, timedelta
from pathlib import Path


def generate_text_payload(size_kb: int = 10) -> bytes:
    """Generate random text payload."""
    words = ["lorem", "ipsum", "dolor", "sit", "amet", "consectetur",
             "adipiscing", "elit", "sed", "do", "eiusmod", "tempor"]
    text = " ".join(random.choice(words) for _ in range(size_kb * 100))
    return text.encode("utf-8")[:size_kb * 1024]


def generate_json_payload() -> bytes:
    """Generate a JSON document with semi-structured data."""
    doc = {
        "id": str(uuid.uuid4()),
        "timestamp": datetime.now().isoformat(),
        "user": {
            "name": "".join(random.choices(string.ascii_letters, k=8)),
            "email": f"user{random.randint(1,10000)}@example.com",
            "age": random.randint(18, 80),
        },
        "metadata": {
            "tags": random.sample(["python", "rust", "cpp", "java", "go",
                                    "sql", "nosql", "dedup", "storage"], k=3),
            "score": round(random.uniform(0, 100), 2),
            "active": random.choice([True, False]),
        },
        "payload": "".join(random.choices(string.ascii_letters + string.digits, k=500)),
    }
    return json.dumps(doc, indent=2).encode("utf-8")


def generate_uuid_payload() -> bytes:
    """Generate UUID-based payload (high entropy)."""
    uuids = [str(uuid.uuid4()) for _ in range(50)]
    return "\n".join(uuids).encode("utf-8")


def generate_event_payload() -> bytes:
    """Generate GitHub-style event payload."""
    event_types = ["PushEvent", "PullRequestEvent", "IssuesEvent",
                   "CreateEvent", "DeleteEvent", "WatchEvent"]
    event = {
        "id": str(random.randint(10**9, 10**10)),
        "type": random.choice(event_types),
        "actor": {"id": random.randint(1, 100000), "login": f"user{random.randint(1,1000)}"},
        "repo": {"id": random.randint(1, 500000), "name": f"org/repo-{random.randint(1,100)}"},
        "created_at": (datetime.now() - timedelta(hours=random.randint(0, 720))).isoformat(),
        "payload": {"size": random.randint(1, 50), "distinct_size": random.randint(1, 50)},
    }
    return json.dumps(event).encode("utf-8")


def generate_bank_payload() -> bytes:
    """Generate synthetic bank transaction payload."""
    txn = {
        "transaction_id": str(uuid.uuid4()),
        "timestamp": (datetime.now() - timedelta(seconds=random.randint(0, 86400*30))).isoformat(),
        "sender_iban": f"DE{random.randint(10**16, 10**17)}",
        "receiver_iban": f"DE{random.randint(10**16, 10**17)}",
        "amount": round(random.uniform(0.01, 50000.00), 2),
        "currency": random.choice(["EUR", "USD", "GBP"]),
        "reference": "".join(random.choices(string.ascii_uppercase + string.digits, k=20)),
    }
    return json.dumps(txn).encode("utf-8")


PAYLOAD_GENERATORS = {
    "text": generate_text_payload,
    "json": generate_json_payload,
    "uuid": generate_uuid_payload,
    "event": generate_event_payload,
    "bank": generate_bank_payload,
}


def generate_dataset(output_dir: Path, payload_type: str, dup_grade: str,
                     num_files: int = 1000):
    """
    Generate a dataset with the specified duplication degree.

    U0:  0% duplicates (all unique)
    U50: 50% duplicates (every 2nd file is a copy of a previous one)
    U90: 90% duplicates (every 10th file is unique, rest are copies)
    """
    grade_dir = output_dir / dup_grade / payload_type
    grade_dir.mkdir(parents=True, exist_ok=True)

    dup_ratio = {"U0": 0.0, "U50": 0.5, "U90": 0.9}[dup_grade]

    generator = PAYLOAD_GENERATORS[payload_type]
    unique_payloads = []

    for i in range(num_files):
        if dup_ratio > 0 and unique_payloads and random.random() < dup_ratio:
            # Duplicate: pick a random existing payload
            payload = random.choice(unique_payloads)
        else:
            # Unique: generate new payload
            payload = generator()
            unique_payloads.append(payload)

        sha256 = hashlib.sha256(payload).hexdigest()[:16]
        filename = f"{i:06d}_{sha256}.dat"
        (grade_dir / filename).write_bytes(payload)

    print(f"  {dup_grade}/{payload_type}: {num_files} files "
          f"({len(unique_payloads)} unique, {num_files - len(unique_payloads)} duplicates)")


def main():
    parser = argparse.ArgumentParser(description="Generate dedup test datasets")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--dup-grades", nargs="+", default=["U0", "U50", "U90"])
    parser.add_argument("--payload-types", nargs="+",
                        default=["text", "json", "uuid", "event", "bank"])
    parser.add_argument("--num-files", type=int, default=1000,
                        help="Number of files per payload type per grade")
    args = parser.parse_args()

    print(f"Generating datasets in {args.output_dir}")
    print(f"Grades: {args.dup_grades}")
    print(f"Payload types: {args.payload_types}")
    print(f"Files per type per grade: {args.num_files}")
    print()

    for grade in args.dup_grades:
        print(f"=== {grade} ===")
        for ptype in args.payload_types:
            generate_dataset(args.output_dir, ptype, grade, args.num_files)

    print("\nDone!")


if __name__ == "__main__":
    main()
