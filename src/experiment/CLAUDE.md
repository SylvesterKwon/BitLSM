# Experiment Conventions

## Architecture

All experiment methods (bitlsm, no-index, si-ck, si-lu, si-eager) share a **Binding** virtual interface (`bindings/binding.h`). Each binding implements `Open`, `Put`, `Scan`, `Close`. Two drivers consume bindings:

1. **BenchmarkExperiment** (`benchmark/benchmark_experiment.h`) — synthetic workload driver. Generates random KVPs from a schema, runs write/read experiments.
2. **HonkPlayer** (`honk_player/honk_player.cpp`) — real-world workload driver. Replays taxi data TSV traces.

Both drivers are method-agnostic; the binding is selected at runtime via `CreateBinding(name)`.

## Directory Layout

```
src/experiment/
├── run_common.py              # shared runner utilities (logging, hw-reset, daemon, etc.)
├── run.py                     # synthetic benchmark sweep runner
├── bindings/                  # Binding interface + implementations (static library)
│   ├── binding.h              # virtual interface: Open/Put/Scan/Close
│   ├── binding_factory.cpp    # CreateBinding() factory
│   ├── bitlsm_binding.*      # BitLSM binding
│   ├── no_index_binding.*     # No-Index (plain RocksDB) binding
│   ├── si_ck_binding.*        # SI-CK (concatenated key) binding
│   ├── si_lu_binding.*        # SI-LU (list union + merge operator) binding
│   └── si_eager_binding.*     # SI-Eager (eager sorted insert) binding
├── benchmark/
│   ├── benchmark_experiment.h # BenchmarkExperiment class (uses Binding)
│   ├── si_benchmark_common.h  # shared SI utilities (TransactionDB, scan helpers)
│   ├── methods/               # one-liner main() per method → build/bin/<method>
│   ├── schema/                # schema JSON files
│   ├── exp_set/               # sweep parameter JSON files
│   └── result/                # CSV outputs (auto-created)
└── honk_player/
    ├── honk_player.cpp        # real-world workload driver (→ build/bin/honk_player)
    ├── honk_run.py            # honk_player sweep runner
    ├── taxi_schema.h          # NYC taxi column definitions
    ├── json_record_parser.h   # JSON → Attr/Query conversion
    ├── tsv_parser.h           # TSV workload reader
    ├── exp_set/               # honk sweep parameter JSON files
    ├── test/                  # test workload + generator
    └── result/                # CSV outputs (auto-created)
```

## Methods

| Method | Binding | Binary | Description |
|---|---|---|---|
| bitlsm | `BitLSMBinding` | `build/bin/bit-lsm` | Bitmap-indexed LSM-Tree |
| no-index | `NoIndexBinding` | `build/bin/no-index` | Plain RocksDB (full table scan) |
| si-ck | `SICKBinding` | `build/bin/si-ck` | Secondary index — concatenated key |
| si-lu | `SILUBinding` | `build/bin/si-lu` | Secondary index — list union (merge operator) |
| si-eager | `SIEagerBinding` | `build/bin/si-eager` | Secondary index — eager sorted insert |

Each `methods/<method>.cpp` is a one-liner main:
```cpp
#include "benchmark_experiment.h"
#include "binding.h"
int main(int argc, char* argv[]) {
  return benchmark::BenchmarkExperiment(experiment::CreateBinding("bitlsm"))
      .Run(argc, argv);
}
```

## CLI Flags

### Benchmark binaries (common)

`--exp_label`, `--exp_type` (`write_seq`|`read_seq`), `-n`, `--schema`, `-d/--db_path`, `-o/--output_dir`

### Read-specific

`--query_attr_indices` (comma-sep), `--selectivity` (double)

### Method-specific

- `--rho` (double, bitlsm only)
- `--read_strategy` (`im`|`pf`, si-ck/si-lu/si-eager only)

### HonkPlayer

`--binding`, `--workload`, `--db_path`, `--output_dir`, plus method-specific flags passed through.

## Sweep Runners

### run.py (synthetic benchmark)

```bash
python3 src/experiment/run.py <params.json>                  # run all (daemon by default)
python3 src/experiment/run.py <params.json> --no-daemon      # foreground
python3 src/experiment/run.py <params.json> --dry-run        # print only
python3 src/experiment/run.py <params.json> --methods no-index,bitlsm
python3 src/experiment/run.py <params.json> --hw-reset --cooldown 300
python3 src/experiment/run.py <params.json> --start-from 5
```

### honk_run.py (real-world workload)

```bash
python3 src/experiment/honk_player/honk_run.py <params.json>            # daemon by default
python3 src/experiment/honk_player/honk_run.py <params.json> --no-daemon
python3 src/experiment/honk_player/honk_run.py <params.json> --dry-run
```

### Common runner options

| Flag | Default | Description |
|---|---|---|
| `--dry-run` | off | Print commands without executing |
| `--methods` | all | Comma-separated method filter |
| `--cooldown N` | 0 | Seconds between runs |
| `--hw-reset` | off | sync + drop_caches + fstrim between runs (sudo) |
| `--keep-db` | off | Preserve DB directories between runs |
| `--start-from N` | 1 | Resume from N-th experiment (1-indexed) |
| `--daemon` | **on** | Run in background via nohup |
| `--no-daemon` | off | Run in foreground |

Between-run sequence: `experiment → [hw-reset: rm DB (unless --keep-db), sync, drop_caches, fstrim] → [cooldown] → next`

Logs are saved to `logs/{timestamp}_{label}.log`.

## Performance: Hot-Path Allocation Rules

`Put()` is called up to 1e8 times per run. Heap allocation inside this loop directly impacts throughput.

**Rules for binding implementations:**
- `Put()` must not allocate heap memory per call. Buffers like `serialized_value_` must be class member variables, reused via `resize()`/`clear()`.
- `EncodeValue()` uses `out_value.resize(exact_size)` + `memcpy` — safe to reuse the same `string&`.

**Rules enforced in `BenchmarkExperiment`:**
- `vector<Attr>`, `payload`, `pk` are allocated once before the loop and reused per iteration.

When adding a new binding, follow the existing pattern in `no_index_binding.cpp` and `bitlsm_binding.cpp`.

## Result Files

- Write CSV: `time_elapsed_ms,records_written` (checkpoint every 1M records)
- Read CSV (benchmark): `RESULT:{elapsed_ms},{matched},{selectivity}` printed to stdout, collected by run.py into master CSV
- Read CSV (honk): `query_id,query_attr_num,filter_attrs,time_elapsed_ms,records_matched,records_total,selectivity_actual`
