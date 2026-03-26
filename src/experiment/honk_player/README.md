# Honk Player: Real-World Taxi Workload Driver

## Objective

Replay real NYC taxi data workloads against all storage methods to compare performance on actual data distributions and query patterns, complementing the synthetic benchmark.

## How it works

1. Reads a TSV workload file containing interleaved WRITE and READ operations
2. WRITEs insert taxi records (JSON) into the DB via the selected binding
3. READs execute filter queries and measure latency + selectivity
4. Results are saved to per-binding CSV files

## Methods

Same five methods as the synthetic benchmark, selected at runtime via `--binding`:

`bitlsm`, `no-index`, `si-ck`, `si-lu`, `si-eager`

## Taxi Schema

20 columns defined in `taxi_schema.h`:

| Type | Columns |
|---|---|
| CATEGORICAL | VendorID, RatecodeID, store_and_fwd_flag, payment_type |
| CONTINUOUS | tpep_pickup_datetime, tpep_dropoff_datetime, passenger_count, trip_distance, PULocationID, DOLocationID, fare_amount, extra, mta_tax, tip_amount, tolls_amount, improvement_surcharge, total_amount, congestion_surcharge, Airport_fee, cbd_congestion_fee |

Datetime columns are stored as unix timestamps (numeric). `Airport_fee` uses capital A to match the official NYC taxi data.

## TSV Workload Format

Tab-separated, one operation per line:

```
w	<pk>	<json_record>
u	<pk>	<json_record>
r	<json_filter>
p	<seconds>
```

- `w`/`u`: write/update a record (PK + JSON with column values)
- `r`: read query (JSON with `filters` array)
- `p`: pause for N seconds

### Filter operators

```json
{"filters": [
  {"attr": "payment_type", "op": "eq", "value": 1},
  {"attr": "fare_amount", "op": "range", "lo": 10.0, "hi": 50.0},
  {"attr": "passenger_count", "op": "in", "values": [1, 2, 3]}
]}
```

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Binary: `build/bin/honk_player`

## Running

### Direct execution

```bash
build/bin/honk_player \
  --binding bitlsm \
  --workload path/to/workload.tsv \
  --db_path /scratch/honk/bitlsm/rho0.1 \
  --output_dir src/experiment/honk_player/result \
  --rho 0.1
```

### Sweep with honk_run.py (recommended)

```bash
# dry-run
python3 src/experiment/honk_player/honk_run.py exp_set/two_point_w1e7.json --dry-run

# run all (daemon by default, logs to logs/)
python3 src/experiment/honk_player/honk_run.py exp_set/two_point_w1e7.json

# foreground
python3 src/experiment/honk_player/honk_run.py exp_set/two_point_w1e7.json --no-daemon

# with hw-reset (requires sudo)
sudo python3 src/experiment/honk_player/honk_run.py exp_set/two_point_w1e7.json \
  --hw-reset --cooldown 60

# filter methods
python3 src/experiment/honk_player/honk_run.py exp_set/two_point_w1e7.json \
  --methods bitlsm,no-index

# monitor output
tail -f logs/honk_*.log
```

### params.json format

```json
{
  "db_path_base": "/scratch/honk",
  "workload": "/home/user/Datasets/workload.tsv",
  "output_dir": "src/experiment/honk_player/result",
  "methods": [
    {"name": "bitlsm",   "params": {"rho": [0.1]}},
    {"name": "no-index",  "params": {}},
    {"name": "si-ck",     "params": {"read_strategy": ["im"]}},
    {"name": "si-lu",     "params": {"read_strategy": ["im"]}},
    {"name": "si-eager",  "params": {"read_strategy": ["im"]}}
  ]
}
```

## Results

Output directory: `result/` (or `--output_dir`)

### Write log

`{binding}{suffix}_write_log.csv` — checkpointed every 1M records:

```csv
time_elapsed_ms,records_written
47773,1000000
95535,2000000
```

## Testing

```bash
# regenerate test workload (100 writes + 10 reads)
python3 src/experiment/honk_player/test/generate_test_tsv.py \
  src/experiment/honk_player/test/test_workload.tsv

# run test sweep
python3 src/experiment/honk_player/honk_run.py \
  src/experiment/honk_player/test/test_params.json --no-daemon

# verify correctness: all bindings should produce identical records_matched
```
