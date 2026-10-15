#!/usr/bin/env python3
"""Generate the E001 dataset: a `reviews` table with labelled text.

    python experiments/E001-semantic-filter-ordering/gen_data.py --rows 10000 --seed 42

Writes to --out (default: data/ next to this script, git-ignored):
    reviews.sql     CREATE TABLE + batched INSERTs, ready to pipe into the CLI
    labels.csv      id,complains_about_shipping   (ground truth, 0/1)
    manifest.json   seed, sizes, exact selectivities, sha256 of both files

Table:
    reviews(id INT32 NOT NULL, rating INT32 NOT NULL, year INT32 NOT NULL,
            helpful INT32 NOT NULL, body VARCHAR(300) NOT NULL)

The cheap columns are built so selectivities are *exact*, not just expected:
    helpful  each value 0..99 appears rows/100 times, so `helpful < k` keeps
             exactly k% of the rows (k = 1, 5, 20, 50 give E001's 0.01 .. 0.5)
    rating   1..5, each exactly 20%
    year     2015..2024, each exactly 10%
    label    exactly --positive-rate of the rows complain about shipping
Each column is shuffled independently, so they are uncorrelated.

Bodies come from a template pool. Positives complain about shipping; the
negatives include "hard" ones that mention shipping without complaining about
it, so a real model is actually tested, not just keyword matching.

Standard library only. Same --rows/--seed/--positive-rate always produces
byte-identical files (checked by the sha256 in manifest.json).
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import random
import sys
from pathlib import Path

GENERATOR_VERSION = 1
BATCH = 500  # rows per INSERT statement

COURIERS = ["the courier", "the delivery company", "the post office", "the carrier"]
PRODUCTS = ["blender", "lamp", "backpack", "kettle", "headset", "desk chair", "jacket"]
DELAYS = ["two weeks late", "a month late", "after the promised date", "much later than promised"]
DAMAGE = ["crushed", "soaked", "torn open", "dented"]

POSITIVE = [
    "The package arrived {delay} and nobody told me why.",
    "Shipping took forever, the {product} came {delay}.",
    "The box was {damage} when it arrived, {courier} clearly did not care.",
    "{courier_cap} lost my order and I had to wait for a replacement.",
    "Tracking said delivered but the {product} never showed up.",
    "Terrible delivery, the {product} arrived {delay} in a {damage} box.",
    "I paid for express shipping and it still came {delay}.",
    "Wrong address on the label, {courier} sent my {product} to another city.",
]

NEGATIVE = [
    "The {product} works great and looks even better than the photos.",
    "Good value for the price, I would buy this {product} again.",
    "The {product} stopped working after a week, very disappointed.",
    "Customer service was rude when I asked about the warranty.",
    "The {product} is smaller than I expected but does the job.",
    "Battery life on the {product} is shorter than advertised.",
    "Instructions were confusing, it took an hour to set up the {product}.",
    "Solid build quality, the {product} feels premium.",
]

# Mention shipping, but the complaint (if any) is not about shipping.
HARD_NEGATIVE = [
    "Shipping was fast, but the {product} broke on the first day.",
    "Arrived early and well packed, sadly the {product} is too loud.",
    "Delivery was on time. The {product} itself is mediocre.",
    "No problems with the courier, my issue is the {product} color.",
]


def render(template: str, rng: random.Random) -> str:
    courier = rng.choice(COURIERS)
    text = template.format(
        product=rng.choice(PRODUCTS),
        delay=rng.choice(DELAYS),
        damage=rng.choice(DAMAGE),
        courier=courier,
        courier_cap=courier[0].upper() + courier[1:],
    )
    # The SQL loader quotes with '...'. Keep the pool apostrophe-free rather
    # than rely on '' escaping.
    assert "'" not in text, text
    return text


def exact_column(values: list[int], rows: int, rng: random.Random) -> list[int]:
    """Repeat `values` to `rows` entries, each value equally often, then shuffle."""
    if rows % len(values) != 0:
        raise SystemExit(f"--rows must be a multiple of {len(values)} for exact selectivities")
    col = values * (rows // len(values))
    rng.shuffle(col)
    return col


def build(rows: int, seed: int, positive_rate: float):
    rng = random.Random(seed)

    helpful = exact_column(list(range(100)), rows, rng)
    rating = exact_column([1, 2, 3, 4, 5], rows, rng)
    year = exact_column(list(range(2015, 2025)), rows, rng)

    positives = round(rows * positive_rate)
    labels = [1] * positives + [0] * (rows - positives)
    rng.shuffle(labels)

    bodies = []
    for label in labels:
        if label:
            bodies.append(render(rng.choice(POSITIVE), rng))
        else:
            pool = HARD_NEGATIVE if rng.random() < 0.25 else NEGATIVE
            bodies.append(render(rng.choice(pool), rng))

    return [
        {"id": i + 1, "rating": rating[i], "year": year[i], "helpful": helpful[i],
         "body": bodies[i], "label": labels[i]}
        for i in range(rows)
    ]


def write_sql(path: Path, data: list[dict]) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as f:
        f.write("CREATE TABLE reviews (id INT32 NOT NULL, rating INT32 NOT NULL, "
                "year INT32 NOT NULL, helpful INT32 NOT NULL, body VARCHAR(300) NOT NULL);\n")
        for start in range(0, len(data), BATCH):
            chunk = data[start:start + BATCH]
            values = ", ".join(
                f"({r['id']}, {r['rating']}, {r['year']}, {r['helpful']}, '{r['body']}')"
                for r in chunk)
            f.write(f"INSERT INTO reviews VALUES {values};\n")


def write_labels(path: Path, data: list[dict]) -> None:
    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["id", "complains_about_shipping"])
        for r in data:
            w.writerow([r["id"], r["label"]])


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--rows", type=int, default=10000, help="multiple of 100 (default 10000)")
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--positive-rate", type=float, default=0.3,
                    help="share of rows that complain about shipping (default 0.3)")
    ap.add_argument("--out", type=Path, default=Path(__file__).resolve().parent / "data")
    args = ap.parse_args()

    if not 0.0 <= args.positive_rate <= 1.0:
        raise SystemExit("--positive-rate must be in [0, 1]")

    data = build(args.rows, args.seed, args.positive_rate)
    args.out.mkdir(parents=True, exist_ok=True)
    sql_path, labels_path = args.out / "reviews.sql", args.out / "labels.csv"
    write_sql(sql_path, data)
    write_labels(labels_path, data)

    manifest = {
        "experiment": "E001",
        "generator_version": GENERATOR_VERSION,
        "seed": args.seed,
        "rows": args.rows,
        "positives": sum(r["label"] for r in data),
        "selectivity": {
            "helpful < k": "exactly k/100",
            "rating = v": 0.2,
            "year = v": 0.1,
        },
        "sha256": {"reviews.sql": sha256(sql_path), "labels.csv": sha256(labels_path)},
    }
    with (args.out / "manifest.json").open("w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    print(f"wrote {args.rows} rows ({manifest['positives']} positives) to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
