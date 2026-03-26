# Synthetic Benchmark: Multi-Method Performance Comparison

## Objective

Compare **write** and **read** performance across five storage methods under identical synthetic workloads.

## Methods

| Method | Binary | Key Feature |
|---|---|---|
| No-Index RocksDB | `build/bin/no-index` | Plain RocksDB, full table scan for reads |
| BitLSM | `build/bin/bit-lsm` | Bitmap-indexed LSM-Tree, `--rho` controls index density |
| SI-CK | `build/bin/si-ck` | Secondary index with concatenated keys, prefix bloom filter |
| SI-LU | `build/bin/si-lu` | Secondary index with merge operator (lazy updates) |
| SI-Eager | `build/bin/si-eager` | Secondary index with eager sorted insert on write |

All methods use the shared **Binding** interface (`bindings/`). Each `methods/<name>.cpp` is a one-liner main that creates a binding and delegates to `BenchmarkExperiment`.

Record format is defined by **schema JSON files** (`schema/` directory):
- **PK**: 8-character random alphanumeric string
- **Attributes**: categorical (string) or continuous (double), defined per-attr in schema
- **Payload**: random alphanumeric string, size defined by `payload_bytes`

## Schema

```json
{
  "attrs": [
    { "type": "categorical", "cardinality": 1000 },
    { "type": "continuous" },
    { "type": "continuous", "min": -1.0, "max": 1.0 }
  ],
  "payload_bytes": 32
}
```

- `categorical`: values `"0"` to `"(cardinality-1)"`. Default cardinality: 100
- `continuous`: uniform double in `[min, max)`. Default: `[0.0, 100.0)`
- `payload_bytes`: payload size per record. Default: 32

## Experiment Types

### `write_seq` — Sequential Write

Inserts N records using single-threaded `Put` calls. Progress checkpointed every 1M records.

### `read_seq` — Sequential Read

Opens a pre-populated DB and executes a single query.

- **No-Index**: full table scan with condition check on every record
- **BitLSM**: bitmap-indexed scan via `BitLSM::NewIterator(query)`
- **SI methods**: secondary index lookup → PK list → MultiGet + optional post-filter
- Query generation: continuous attrs get range queries scaled by `--selectivity`; categorical attrs get equality queries with `K = round(selectivity * cardinality)` OR'd values

## Parameters

### Common

| Flag | Type | Default | Description |
|---|---|---|---|
| `--exp_label` | string | `seq_write` | Experiment label for output filename |
| `--exp_type` | string | `write_seq` | `write_seq` or `read_seq` |
| `-n` | uint64 | required | Total number of records |
| `--schema` | string | required | Path to schema JSON |
| `-d, --db_path` | string | required | DB storage path |
| `-o, --output_dir` | string | `./result` | CSV output directory |

### Read-specific

| Flag | Type | Description |
|---|---|---|
| `--query_attr_indices` | string | Comma-separated attribute indices (e.g. `0,1,3`) |
| `--selectivity` | double | Per-attribute selectivity |

### Method-specific

| Flag | Methods | Default | Description |
|---|---|---|---|
| `--rho` | bitlsm | 0.1 | BitLSM rho threshold |
| `--read_strategy` | si-ck, si-lu, si-eager | `im` | `im` (index merge) or `pf` (post filter) |

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Running with run.py

```bash
# dry-run
python3 src/experiment/run.py src/experiment/benchmark/exp_set/seq_write_all.json --dry-run

# full sweep (daemon by default, logs to logs/)
python3 src/experiment/run.py src/experiment/benchmark/exp_set/seq_write_all.json

# foreground run
python3 src/experiment/run.py src/experiment/benchmark/exp_set/seq_write_all.json --no-daemon

# with hw-reset + cooldown (requires sudo)
sudo python3 src/experiment/run.py src/experiment/benchmark/exp_set/seq_write_all.json \
  --hw-reset --cooldown 60

# filter methods
python3 src/experiment/run.py src/experiment/benchmark/exp_set/seq_write_all.json \
  --methods no-index,bitlsm

# resume from specific run
python3 src/experiment/run.py src/experiment/benchmark/exp_set/seq_write_all.json \
  --start-from 5

# monitor daemon output
tail -f logs/<timestamp>_*.log
```

### Write then read workflow

```bash
# 1. Write with --keep-db
sudo python3 src/experiment/run.py exp_set/seq_write_for_read.json \
  --hw-reset --cooldown 60 --keep-db

# 2. Read on the same DB (--warmup populates page cache first)
sudo python3 src/experiment/run.py exp_set/seq_read_cont.json --warmup
```

## Results

### Write CSV

```
src/experiment/benchmark/result/{exp_label}_{method}_n{N}_schema_{name}{param_suffix}.csv
```

```csv
time_elapsed_ms,records_written
1234,1000000
2501,2000000
```

### Read output

Each read binary prints `RESULT:{elapsed_ms},{matched},{selectivity}` to stdout. `run.py` collects these into a master CSV.

## exp_set JSON format

```json
{
  "exp_label": "seq_write_all",
  "exp_type": "write_seq",
  "db_path_base": "/scratch/benchmark",
  "common_params": { "n": [100000000], "schema": ["default_a16_c100.json"] },
  "methods": [
    { "name": "no-index", "binary": "build/bin/no-index", "params": {} },
    { "name": "bitlsm",  "binary": "build/bin/bit-lsm",  "params": { "rho": [0.05, 0.1, 0.2] } },
    { "name": "si-ck",   "binary": "build/bin/si-ck",     "params": { "read_strategy": ["im"] } },
    { "name": "si-lu",   "binary": "build/bin/si-lu",     "params": { "read_strategy": ["im"] } },
    { "name": "si-eager","binary": "build/bin/si-eager",   "params": { "read_strategy": ["im"] } }
  ]
}
```

Values are lists; the runner computes their cartesian product.
