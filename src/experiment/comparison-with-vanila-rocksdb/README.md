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
- **PK**: 8-character random alphanumeric string (`[A-Za-z0-9]`)
- **Attributes**: even-indexed → categorical (integer string in [0, 99]), odd-indexed → continuous (double in [0.0, 100.0])
- **Payload**: random alphanumeric string of fixed length

All writes are performed single-threaded. Progress is checkpointed every 1,000,000 records.

---

## Experiment Types

### `seq_write` — Sequential Write

Inserts N records using a single writer thread with `Put` calls.

---

## Parameters

### Common

| Flag | Type | Default | Description |
|---|---|---|---|
| `--exp_type` | string | `seq_write` | Experiment type |
| `-n` | uint64 | (required) | Total number of records to insert |
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
  -p 32 \
  -a 16 \
  -d /scratch/vanila-exp \
  -o src/experiment/comparison-with-vanila-rocksdb/result

# BitLSM
./build/bin/bit-lsm \
  --exp_type seq_write \
  -n 100000000 \
  -p 32 \
  -a 16 \
  --rho 0.1 \
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

### Manual sweep

```bash
# Vary rho for BitLSM
for rho in 0.05 0.1 0.2 0.5; do
  ./build/bin/bit-lsm -n 100000000 -p 32 -a 16 --rho $rho \
    -d /scratch/bitlsm-rho$rho \
    -o src/experiment/comparison-with-vanila-rocksdb/result
done

# Vary attribute count
for a in 1 2 4 8 16 32 64; do
  ./build/bin/vanila-rocksdb -n 100000000 -p 32 -a $a \
    -d /scratch/vanila-a$a \
    -o src/experiment/comparison-with-vanila-rocksdb/result

  ./build/bin/bit-lsm -n 100000000 -p 32 -a $a --rho 0.1 \
    -d /scratch/bitlsm-a$a \
    -o src/experiment/comparison-with-vanila-rocksdb/result
done
```
