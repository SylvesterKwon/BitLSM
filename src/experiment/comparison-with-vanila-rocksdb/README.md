# Experiment: Vanilla RocksDB vs BitLSM Write Performance Comparison

## Objective

Compare the write performance of **Vanilla RocksDB** and **BitLSM** under identical workload conditions.
BitLSM integrates a bitmap index into the LSM-Tree. This experiment measures the write overhead introduced by maintaining that index.

---

## Subjects

| Method | Binary | Source |
|---|---|---|
| Vanilla RocksDB | `vanila-rocksdb` | `vanila-rocksdb.cpp` |
| BitLSM | `bit-lsm` | `bit-lsm.cpp` |

Both methods use the same record format:
- **PK**: auto-increment integer (stored as string)
- **Attributes**: even-indexed → categorical (integer string in [0, 99]), odd-indexed → continuous (double in [0.0, 100.0])
- **Payload**: random alphanumeric string of fixed length

---

## Experiment Types

### `seq_write` — Sequential Write

Inserts N records using T concurrent writer threads.
Each thread atomically claims the next PK via a shared counter and performs independent `Put` calls.
Progress is checkpointed every 1,000,000 records.

---

## Parameters

### Common

| Flag | Type | Default | Description |
|---|---|---|---|
| `--exp_type` | string | `seq_write` | Experiment type |
| `-n` | uint64 | (required) | Total number of records to insert |
| `-t, --threads` | uint32 | `4` | Number of writer threads |
| `-p, --payload_size` | uint32 | `32` | Payload size in bytes |
| `-a, --attr_num` | uint32 | `16` | Number of attributes per record |
| `-d, --db_path` | string | (required) | DB storage path |
| `-o, --output_dir` | string | `./result` | Directory for CSV output |

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
# Vanilla RocksDB
./build/bin/vanila-rocksdb \
  --exp_type seq_write \
  -n 100000000 \
  -t 4 \
  -p 32 \
  -a 16 \
  -d /scratch/data/vanila-exp \
  -o src/experiment/comparison-with-vanila-rocksdb/result

# BitLSM
./build/bin/bit-lsm \
  --exp_type seq_write \
  -n 100000000 \
  -t 4 \
  -p 32 \
  -a 16 \
  --rho 0.1 \
  -d /scratch/data/bitlsm-exp \
  -o src/experiment/comparison-with-vanila-rocksdb/result
```

---

## Results

### Output Directory

```
src/experiment/comparison-with-vanila-rocksdb/result/
```

Created automatically if it does not exist.

### Filename Convention

```
{exp_type}_{method}_{parameters}.csv
```

| Method | Example filename |
|---|---|
| Vanilla RocksDB | `seq_write_vanila_n100000000_p32_t4_a16.csv` |
| BitLSM | `seq_write_bitlsm_n100000000_p32_t4_a16_rho0.1.csv` |

Parameter prefixes in filename:

| Prefix | Meaning |
|---|---|
| `n` | Total record count |
| `p` | Payload size (bytes) |
| `t` | Number of writer threads |
| `a` | Number of attributes |
| `rho` | BitLSM rho value (BitLSM only) |

### CSV Schema

```csv
time_elapsed_ms,records_written
1234,1000000
2501,2000000
...
```

- `time_elapsed_ms`: wall-clock time elapsed since experiment start (ms)
- `records_written`: cumulative records inserted up to this checkpoint
- Checkpoint interval: every 1,000,000 records

---

## Examples

### Using run.py (recommended)

`--hw-reset` uses `sync`, `drop_caches`, and `fstrim`, so the **entire script must be run as root**.
Use `tmux` to keep the session alive after terminal disconnect.

```bash
# dry-run to preview all commands
python3 src/experiment/run.py \
  src/experiment/comparison-with-vanila-rocksdb/param_set/thread_comparison.json \
  --dry-run

# full sweep with SSD hardware reset + 5-min cooldown (tmux detached session)
sudo tmux new -d -s sweep 'sudo python3 src/experiment/run.py \
  src/experiment/comparison-with-vanila-rocksdb/param_set/thread_comparison.json \
  --hw-reset --cooldown 60'

# attach to see live output
sudo tmux attach -t sweep

# terminate experiment
sudo tmux kill-session -t sweep
```

### Manual sweep

```bash
# Vary rho for BitLSM
for rho in 0.05 0.1 0.2 0.5; do
  ./build/bin/bit-lsm -n 100000000 -t 4 -p 32 -a 16 --rho $rho \
    -d /scratch/data/bitlsm-rho$rho \
    -o src/experiment/comparison-with-vanila-rocksdb/result
done

# Vary thread count
for t in 1 2 4 8; do
  ./build/bin/vanila-rocksdb -n 100000000 -t $t -p 32 -a 16 \
    -d /scratch/data/vanila-t$t \
    -o src/experiment/comparison-with-vanila-rocksdb/result

  ./build/bin/bit-lsm -n 100000000 -t $t -p 32 -a 16 --rho 0.1 \
    -d /scratch/data/bitlsm-t$t \
    -o src/experiment/comparison-with-vanila-rocksdb/result
done
```
