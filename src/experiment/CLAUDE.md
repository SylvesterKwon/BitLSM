# Experiment Conventions

## Directory Layout

```
src/experiment/
├── run.py                  # shared sweep runner
├── <experiment-name>/
│   ├── benchmark_experiment.h  # CRTP base class (CLI, data gen, progress, CSV output)
│   ├── <method>.cpp        # one binary per method — derives from BenchmarkExperiment (auto-compiled by CMake → build/bin/<method>)
│   ├── params.json         # sweep parameter space
│   ├── README.md           # experiment spec (English, required)
│   └── result/             # CSV outputs (auto-created at runtime)
```

## CLI Flags (cxxopts)

Common: `--exp_label`, `--exp_type` (`write_seq`|`read_seq`), `-n`, `--schema`, `-d/--db_path`, `-o/--output_dir`
Read-only: `--query_attr_indices` (comma-sep), `--selectivity` (double)
Method-specific flags: defined per experiment in its README.md.

## Result Files

- Path: `<exp_dir>/result/`
- Filename: `{exp_label}_{method}_{param1}{val1}_{param2}{val2}_...csv` — common params first, then method-specific
- Every CSV has `time_elapsed_ms`; additional columns defined per experiment type in README.md

## Progress Logging

`ProgressLog` struct and progress logging logic live in the CRTP base class `BenchmarkExperiment` (`benchmark_experiment.h`). Each derived `.cpp` inherits this automatically — no per-file logging code needed. Checkpoint interval: every 1,000,000 records.

## Sweep: params.json

Values are lists; runner computes cartesian product. `schema` values are filenames only (resolved to `<exp_dir>/schema/`). `db_path` is auto-generated as `{db_path_base}/{method}/{encoded_params}` using `DB_PARAMS = ["n", "schema", "rho"]`.

```json
{
  "exp_label": "...",
  "exp_type": "write_seq",
  "db_path_base": "/scratch",
  "common_params": { "n": [100000000], "schema": ["default_a16.json"] },
  "methods": [
    { "name": "...", "binary": "build/bin/...", "params": { "<flag>": [0.05, 0.1] } }
  ]
}
```

## Sweep: run.py

```bash
python3 src/experiment/run.py <params.json>                          # run all
python3 src/experiment/run.py <params.json> --dry-run                # print only
python3 src/experiment/run.py <params.json> --methods no-index,bitlsm  # subset
python3 src/experiment/run.py <params.json> --hw-reset --cooldown 300         # SSD deterministic (deletes DB by default)
python3 src/experiment/run.py <params.json> --hw-reset --cooldown 300 --keep-db  # hw-reset without DB deletion
python3 src/experiment/run.py <params.json> --hw-reset --cooldown 60 --daemon    # background run (no tmux needed)
python3 src/experiment/run.py <params.json> --warmup                             # warmup per-DB page cache before read_seq
```

Between-run sequence: `[warmup: 1 dummy query per unique db_path (read_seq only)] → experiment → [hw-reset: rm DB (unless --keep-db or read_seq), sync, drop_caches, fstrim] → [cooldown] → next`

## README.md (Required per experiment)

Write in English. Sections: **Objective**, **Subjects** (methods/binaries), **Experiment Types**, **Parameters** (all flags with types/defaults), **Build**, **Running**, **Results** (dir, filename, CSV columns), **Examples**.