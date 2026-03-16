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

Both methods use the same record format:
- **PK**: 8-character random alphanumeric string (`[A-Za-z0-9]`)
- **Attributes**: even-indexed → categorical (integer string in [0, 99]), odd-indexed → continuous (double in [0.0, 100.0])
- **Payload**: random alphanumeric string of fixed length

All writes are performed single-threaded. Progress is checkpointed every 1,000,000 records.

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
  - **Continuous** attribute: range query `[lo, lo + selectivity × 100.0)` with random `lo` (requires `--selectivity`)
  - **Categorical** attribute: equality query on a random value from `"0"`–`"99"` (`--selectivity` not needed)
- **Out-of-bound handling**: If any query attribute index ≥ `attr_num`, the binary prints a SKIP message and exits 0 without producing output.

---

## Parameters

### Common

| Flag | Type | Default | Description |
|---|---|---|---|
| `--exp_label` | string | `seq_write` | Experiment label for output filename |
| `--exp_type` | string | `write_seq` | Experiment type: `write_seq` or `read_seq` |
| `-n` | uint64 | (required) | Total number of records |
| `-p, --payload_size` | uint32 | `32` | Payload size in bytes |
| `-a, --attr_num` | uint32 | `16` | Number of attributes per record |
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
  --exp_label seq_write \
  -n 100000000 -p 32 -a 16 \
  -d /scratch/vanila-exp \
  -o src/experiment/comparison-with-vanila-rocksdb/result

# Write — BitLSM
./build/bin/bit-lsm \
  --exp_label seq_write \
  -n 100000000 -p 32 -a 16 --rho 0.1 \
  -d /scratch/bitlsm-exp \
  -o src/experiment/comparison-with-vanila-rocksdb/result

# Read — Vanilla RocksDB (full scan)
./build/bin/vanila-rocksdb \
  --exp_label seq_read_continuous \
  -n 100000000 -p 32 -a 16 \
  --selectivity 0.01 --query_attr_indices 1,3 \
  -d /scratch/vanila-exp \
  -o src/experiment/comparison-with-vanila-rocksdb/result

# Read — BitLSM (bitmap-indexed scan)
./build/bin/bit-lsm \
  --exp_label seq_read_continuous \
  -n 100000000 -p 32 -a 16 --rho 0.1 \
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

### Filename Convention

```
{exp_type}_{method}_{parameters}.csv
```

| Method | Example filename |
|---|---|
| Vanilla RocksDB | `seq_write_vanila_n100000000_p32_a16.csv` |
| BitLSM | `seq_write_bitlsm_n100000000_p32_a16_rho0.1.csv` |

Parameter prefixes in filename:

| Prefix | Meaning |
|---|---|
| `n` | Total record count |
| `p` | Payload size (bytes) |
| `a` | Number of attributes |
| `rho` | BitLSM rho value (BitLSM only) |

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
Use `tmux` to keep the session alive after terminal disconnect.

```bash
# dry-run to preview all commands
python3 src/experiment/run.py \
  src/experiment/comparison-with-vanila-rocksdb/param_set/attr_num_comparison.json \
  --dry-run

# full sweep with SSD hardware reset + 1-min cooldown (DB deleted between runs by default)
sudo tmux new -d -s sweep 'sudo python3 src/experiment/run.py \
  src/experiment/comparison-with-vanila-rocksdb/param_set/attr_num_comparison.json \
  --hw-reset --cooldown 60'

# hw-reset without deleting DB between runs (e.g., for preparing read test)
sudo tmux new -d -s sweep 'sudo python3 src/experiment/run.py \
  src/experiment/comparison-with-vanila-rocksdb/param_set/attr_num_comparison.json \
  --hw-reset --cooldown 60 --keep-db'

# attach to see live output
sudo tmux attach -t sweep

# terminate experiment
sudo tmux kill-session -t sweep
```

### Write then read workflow (recommended)

```bash
# 1. Write with --keep-db to preserve DB
sudo python3 src/experiment/run.py \
  src/experiment/comparison-with-vanila-rocksdb/param_set/seq_write.json \
  --hw-reset --cooldown 60 --keep-db

# 2. Read on the same DB (db_params ensures matching paths)
sudo python3 src/experiment/run.py \
  src/experiment/comparison-with-vanila-rocksdb/param_set/seq_read_continuous.json \
  --hw-reset --cooldown 60
```

### Manual sweep

```bash
# Vary rho for BitLSM
for rho in 0.05 0.1 0.2 0.5; do
  ./build/bin/bit-lsm -n 100000000 -p 32 -a 16 --rho $rho \
    -d /scratch/bitlsm-rho$rho \
    -o src/experiment/comparison-with-vanila-rocksdb/result
done

# Vary selectivity for read
for s in 0.1 0.01 0.001; do
  ./build/bin/vanila-rocksdb -n 100000000 -p 32 -a 16 \
    --selectivity $s --query_attr_indices 1,3 \
    -d /scratch/vanila-a16 \
    -o src/experiment/comparison-with-vanila-rocksdb/result
done
```
