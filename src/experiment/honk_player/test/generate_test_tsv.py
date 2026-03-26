#!/usr/bin/env python3
"""Generate a small test TSV workload for honk_player correctness testing."""

import json
import random
import uuid
import sys
import os

random.seed(42)

NUM_WRITES = 100
NUM_READS = 10
PAUSE_SECONDS = 0.0  # no pause for test

# Taxi column value generators
def gen_record():
    return {
        "VendorID": random.choice([1, 2]),
        "passenger_count": random.randint(0, 6),
        "trip_distance": round(random.uniform(0.1, 30.0), 2),
        "RatecodeID": random.choice([1, 2, 3, 4, 5, 99]),
        "store_and_fwd_flag": random.choice(["Y", "N"]),
        "PULocationID": random.randint(1, 265),
        "DOLocationID": random.randint(1, 265),
        "payment_type": random.choice([1, 2, 3, 4]),
        "fare_amount": round(random.uniform(2.5, 200.0), 2),
        "extra": round(random.choice([0.0, 0.5, 1.0, 2.5, 4.5]), 2),
        "mta_tax": 0.5,
        "tip_amount": round(random.uniform(0.0, 50.0), 2),
        "tolls_amount": round(random.choice([0.0, 0.0, 0.0, 6.55, 12.5]), 2),
        "improvement_surcharge": 1.0,
        "total_amount": round(random.uniform(3.0, 300.0), 2),
        "congestion_surcharge": round(random.choice([0.0, 2.5]), 2),
        "airport_fee": round(random.choice([0.0, 1.75]), 2),
        "cbd_congestion_fee": round(random.choice([0.0, 2.75]), 2),
        "tpep_pickup_datetime": f"2024-01-{random.randint(1,28):02d} {random.randint(0,23):02d}:{random.randint(0,59):02d}:00",
        "tpep_dropoff_datetime": f"2024-01-{random.randint(1,28):02d} {random.randint(0,23):02d}:{random.randint(0,59):02d}:00",
    }


# Queries designed to have non-trivial selectivity on 100 records
def gen_filters():
    filters = []

    # 1. eq on payment_type (should match ~25%)
    filters.append({"filters": [{"attr": "payment_type", "op": "eq", "value": 1}]})

    # 2. eq on VendorID (should match ~50%)
    filters.append({"filters": [{"attr": "VendorID", "op": "eq", "value": 2}]})

    # 3. range on fare_amount
    filters.append({"filters": [{"attr": "fare_amount", "op": "range", "lo": 10.0, "hi": 50.0}]})

    # 4. range on trip_distance
    filters.append({"filters": [{"attr": "trip_distance", "op": "range", "lo": 1.0, "hi": 10.0}]})

    # 5. eq + range (2 attrs)
    filters.append({"filters": [
        {"attr": "payment_type", "op": "eq", "value": 1},
        {"attr": "fare_amount", "op": "range", "lo": 5.0, "hi": 100.0},
    ]})

    # 6. in on passenger_count
    filters.append({"filters": [{"attr": "passenger_count", "op": "in", "values": [1, 2]}]})

    # 7. range on tip_amount
    filters.append({"filters": [{"attr": "tip_amount", "op": "range", "lo": 0.0, "hi": 10.0}]})

    # 8. eq on store_and_fwd_flag
    filters.append({"filters": [{"attr": "store_and_fwd_flag", "op": "eq", "value": "N"}]})

    # 9. multi-attr: VendorID + trip_distance range
    filters.append({"filters": [
        {"attr": "VendorID", "op": "eq", "value": 1},
        {"attr": "trip_distance", "op": "range", "lo": 0.0, "hi": 15.0},
    ]})

    # 10. range on total_amount
    filters.append({"filters": [{"attr": "total_amount", "op": "range", "lo": 20.0, "hi": 150.0}]})

    return filters


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "test_workload.tsv"
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)

    with open(out_path, "w") as f:
        # Write 100 records
        for _ in range(NUM_WRITES):
            pk = str(uuid.uuid4())
            record = gen_record()
            f.write(f"w\t{pk}\t{json.dumps(record)}\n")

        # 10 read queries
        for filt in gen_filters():
            f.write(f"r\t{json.dumps(filt)}\n")

    print(f"Generated {out_path}: {NUM_WRITES} writes, {NUM_READS} reads")


if __name__ == "__main__":
    main()
