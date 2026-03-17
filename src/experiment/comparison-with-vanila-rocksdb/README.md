# Experiment: Vanilla RocksDB vs BitLSM Performance Comparison

## Objective

Compare the **write** and **read** performance of **Vanilla RocksDB** and **BitLSM** under identical workload conditions.
BitLSM integrates a bitmap index into the LSM-Tree. The write experiment measures the overhead of maintaining that index; the read experiment measures the query speedup it provides.

---

## Subjects

| Method | Binary | Source |
|---|---|---|
| Vanilla RocksDB | `vanila-rocksdb` | `vanila-rocksdb.cpp` |
| BitLSM | `bit-lsm` | `bit-lsm.cpp` |

Both methods use the same record format, defined by a **schema JSON file** (`schema/` directory):
- **PK**: 8-character random alphanumeric string (`[A-Za-z0-9]`)
- **Attributes**: type (categorical/continuous), cardinality, range — all defined per-attr in schema
- **Payload**: random alphanumeric string, size defined by `payload_bytes` in schema

All writes are performed single-threaded. Progress is checkpointed every 1,000,000 records.

---

## Schema

Schema files live in `schema/` and define the DB's physical structure:

```json
{
  "attrs": [
    { "type": "categorical", "cardinality": 100 },
    { "type": "continuous" },
    { "type": "continuous", "min": -1.0, "max": 1.0 }
  ],
  "payload_bytes": 32
}
```

- `categorical`: values `"0"` to `"(cardinality-1)"`. Default cardinality: 100
- `continuous`: uniform double in `[min, max)`. Default: `[0.0, 100.0)`
- `payload_bytes`: payload size per record. Default: 32

Shared helper: `schema_loader.h` parses these files into `Schema` structs used by both binaries.

---

## Experiment Types

### `seq_write` — Sequential Write

Inserts N records using a single writer thread with `Put` calls.

### `seq_read_continuous` / `seq_read_categorical` — Sequential Read

Opens a pre-populated DB (created by `seq_write`) and executes a single range/equality query, scanning all matching records.

- **Mode trigger**: `--exp_type read_seq` switches the binary to read mode.
- **Vanilla RocksDB**: Full table scan — iterates every record and checks the query condition.
- **BitLSM**: Bitmap-indexed scan via `BitLSM::NewIterator(query)` — reads only qualifying blocks.
- **Query generation**: For each attribute index in `--query_attr_indices`:
  - **Continuous** attribute: range query `[lo, lo + selectivity × range)` with random `lo` (requires `--selectivity`)
  - **Categorical** attribute: equality query on a random value from `"0"`–`"(cardinality-1)"` (`--selectivity` not needed)
- **Out-of-bound handling**: If any query attribute index ≥ attr count in schema, the binary prints a SKIP message and exits 0 without producing output.

---

## Parameters

### Common

| Flag | Type | Default | Description |
|---|---|---|---|
| `--exp_label` | string | `seq_write` | Experiment label for output filename |
| `--exp_type` | string | `write_seq` | Experiment type: `write_seq` or `read_seq` |
| `-n` | uint64 | (required) | Total number of records |
| `--schema` | string | (required) | Path to schema JSON file (defines attrs + payload) |
| `-d, --db_path` | string | (required) | DB storage path |
| `-o, --output_dir` | string | `./result` | Directory for CSV output |

### Read-specific (`seq_read_*`)

| Flag | Type | Default | Description |
|---|---|---|---|
| `--query_attr_indices` | string | (none) | Comma-separated attribute indices for query (e.g. `0,1,3`) |
| `--selectivity` | double | (none) | Per-attribute selectivity; required only when querying continuous attrs |

### BitLSM-specific

| Flag | Type | Default | Description |
|---|---|---|---|
| `--rho` | double | `0.1` | BitLSM rho threshold |

---

## Build

From the project root:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target vanila-rocksdb bit-lsm -j$(nproc)
```

Outputs: `build/bin/vanila-rocksdb`, `build/bin/bit-lsm`

---

## Running

```bash
# Write — Vanilla RocksDB
./build/bin/vanila-rocksdb \
  --exp_label seq_write --exp_type write_seq \
  -n 100000000 --schema schema/default_a16.json \
  -d /scratch/vanila-exp \
  -o src/experiment/comparison-with-vanila-rocksdb/result

# Write — BitLSM
./build/bin/bit-lsm \
  --exp_label seq_write --exp_type write_seq \
  -n 100000000 --schema schema/default_a16.json --rho 0.1 \
  -d /scratch/bitlsm-exp \
  -o src/experiment/comparison-with-vanila-rocksdb/result

# Read — Vanilla RocksDB (full scan)
./build/bin/vanila-rocksdb \
  --exp_label seq_read_continuous --exp_type read_seq \
  -n 100000000 --schema schema/default_a16.json \
  --selectivity 0.01 --query_attr_indices 1,3 \
  -d /scratch/vanila-exp \
  -o src/experiment/comparison-with-vanila-rocksdb/result

# Read — BitLSM (bitmap-indexed scan)
./build/bin/bit-lsm \
  --exp_label seq_read_continuous --exp_type read_seq \
  -n 100000000 --schema schema/default_a16.json --rho 0.1 \
  --selectivity 0.01 --query_attr_indices 1,3 \
  -d /scratch/bitlsm-exp \
  -o src/experiment/comparison-with-vanila-rocksdb/result
```

---

## Results

### Output Directory

```
src/experiment/comparison-with-vanila-rocksdb/result/
```

Auto-created by run.py (derived from experiment directory).

### Filename Convention

```
{exp_label}_{method}_n{N}_schema_{schema_name}[_rho{R}][_selectivity{S}_query_attr_indices{I}].csv
```

| Method | Example filename |
|---|---|
| Vanilla (write) | `seq_write_vanila_n100000000_schema_default_a16.csv` |
| BitLSM (write) | `seq_write_bitlsm_n100000000_schema_default_a16_rho0.1.csv` |
| Vanilla (read) | `seq_read_continuous_vanila_n100000000_schema_default_a16_selectivity0.01_query_attr_indices1,3.csv` |

### CSV Schema — Write

```csv
time_elapsed_ms,records_written
1234,1000000
2501,2000000
...
```

- `time_elapsed_ms`: wall-clock time elapsed since experiment start (ms)
- `records_written`: cumulative records inserted up to this checkpoint
- Checkpoint interval: every 1,000,000 records

### CSV Schema — Read

```csv
time_elapsed_ms,records_matched,selectivity_actual
4523,1000000,0.01
```

- `time_elapsed_ms`: total query execution time (ms)
- `records_matched`: number of records matching the query
- `selectivity_actual`: `records_matched / n`
- One row per experiment run (single query per run)

---

## Examples

### Using run.py (recommended)

`--hw-reset` uses `sync`, `drop_caches`, and `fstrim`, so the **entire script must be run as root**.
**`--hw-reset` deletes the previous DB directory between runs by default.** Use `--keep-db` to preserve DB directories.
Use `--daemon` to run in the background without tmux, or use `tmux` to keep the session alive after terminal disconnect.

```bash
# dry-run to preview all commands
python3 src/experiment/run.py \
  src/experiment/comparison-with-vanila-rocksdb/param_set/seq_write_default.json \
  --dry-run

# background run with --daemon (no tmux needed)
sudo python3 src/experiment/run.py \
  src/experiment/comparison-with-vanila-rocksdb/param_set/seq_write_default.json \
  --hw-reset --cooldown 60 --daemon

# full sweep with SSD hardware reset + 1-min cooldown (DB deleted between runs by default)
sudo tmux new -d -s sweep 'sudo python3 src/experiment/run.py \
  src/experiment/comparison-with-vanila-rocksdb/param_set/seq_write_default.json \
  --hw-reset --cooldown 60'

# hw-reset without deleting DB between runs (e.g., for preparing read test)
sudo tmux new -d -s sweep 'sudo python3 src/experiment/run.py \
  src/experiment/comparison-with-vanila-rocksdb/param_set/seq_write_default.json \
  --hw-reset --cooldown 60 --keep-db'

# attach to see live output (tmux only)
sudo tmux attach -t sweep

# terminate experiment (tmux only)
sudo tmux kill-session -t sweep
```

### Write then read workflow (recommended)

```bash
# 1. Write with --keep-db to preserve DB
sudo python3 src/experiment/run.py \
  src/experiment/comparison-with-vanila-rocksdb/param_set/seq_write_default.json \
  --hw-reset --cooldown 60 --keep-db

# 2. Read on the same DB (DB_PARAMS ensures matching paths)
sudo python3 src/experiment/run.py \
  src/experiment/comparison-with-vanila-rocksdb/param_set/seq_read_continuous.json \
  --hw-reset --cooldown 60
```

### Manual sweep

```bash
# Vary rho for BitLSM
for rho in 0.05 0.1 0.2 0.5; do
  ./build/bin/bit-lsm \
    --exp_type write_seq -n 100000000 \
    --schema src/experiment/comparison-with-vanila-rocksdb/schema/default_a16.json \
    --rho $rho \
    -d /scratch/bitlsm-rho$rho \
    -o src/experiment/comparison-with-vanila-rocksdb/result
done

# Vary selectivity for read
for s in 0.1 0.01 0.001; do
  ./build/bin/vanila-rocksdb \
    --exp_type read_seq -n 100000000 \
    --schema src/experiment/comparison-with-vanila-rocksdb/schema/default_a16.json \
    --selectivity $s --query_attr_indices 1,3 \
    -d /scratch/vanila-a16 \
    -o src/experiment/comparison-with-vanila-rocksdb/result
done
```
