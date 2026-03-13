# Experiment Conventions

## Directory Structure

Each experiment lives in its own subdirectory under `src/experiment/`:

```
src/experiment/
├── run.py                  # global sweep runner (shared)
├── <experiment-name>/
│   ├── <method-a>.cpp      # one binary per method being compared
│   ├── <method-b>.cpp
│   ├── params.json         # parameter space for sweep automation
│   ├── README.md           # experiment spec (English)
│   └── result/             # CSV outputs (auto-created at runtime)
```

Every `.cpp` file under `src/experiment/` is automatically compiled into its own binary by CMake (filename without extension = binary name). Output goes to `build/bin/`.

---

## CLI Parameters (cxxopts)

All experiment binaries use `cxxopts` for argument parsing. Include via `<cxxopts.hpp>`.

### Common flags

| Flag | Type | Description |
|---|---|---|
| `--exp_type` | string | Experiment type label; used in output filename |
| `-n` | uint64 | Total number of records |
| `-t, --threads` | uint32 | Number of worker threads |
| `-p, --payload_size` | uint32 | Payload size in bytes |
| `-a, --attr_num` | uint32 | Number of attributes per record |
| `-d, --db_path` | string | DB storage path |
| `-o, --output_dir` | string | CSV output directory (default: `./result`) |

Method-specific flags are defined per experiment and documented in the experiment's README.md.

---

## Result Files

### Output directory

Each experiment writes results to a `result/` subdirectory inside its own experiment directory.
Created automatically at runtime via `std::filesystem::create_directories`.

### Filename convention

```
{exp_type}_{method}_{param1}{val1}_{param2}{val2}_...csv
```

- `exp_type` first, then method name, then parameters in order
- Method name is a short identifier for the binary (e.g., `vanila`, `bitlsm`)
- Parameter encoding order: common params first (`n`, `p`, `t`, `a`), then method-specific

### CSV schema

Columns are defined per experiment type. Each row represents one measurement checkpoint.
Common columns across experiment types:

| Column | Description |
|---|---|
| `time_elapsed_ms` | Wall-clock ms since experiment start |

Additional columns (e.g., `records_written`, `response_time_us`) are defined per experiment type and documented in the experiment's README.md.

---

## Progress Logging Pattern

For multi-threaded experiments, collect checkpoints in a shared `vector<ProgressLog>` protected by a `mutex`.

Do **not** modify shared utility headers (e.g., `utils.h`) to add logging hooks. Write a local worker function inside the experiment's own `.cpp` file instead.

---

## Sweep Automation

### params.json

Each experiment directory must have a `params.json` that defines the parameter space.
Values are lists — the runner computes the cartesian product.

```json
{
  "exp_type": "seq_write",
  "output_dir": "<experiment-dir>/result",
  "db_path_base": "/scratch/data",
  "common_params": {
    "n":            [100000000],
    "threads":      [1, 2, 4, 8],
    "payload_size": [32],
    "attr_num":     [16]
  },
  "methods": [
    {
      "name":   "<method-name>",
      "binary": "build/bin/<binary-name>",
      "params": {}
    },
    {
      "name":   "<method-name>",
      "binary": "build/bin/<binary-name>",
      "params": {
        "<method-specific-flag>": [0.05, 0.1, 0.2]
      }
    }
  ]
}
```

- `common_params` keys map 1:1 to cxxopts long-form flag names (`--key value`)
- `db_path` is auto-generated as `{db_path_base}/{method_name}/{encoded_params}`
- Method-specific `params` are merged with `common_params` before computing the product

### run.py

Global runner at `src/experiment/run.py`. One runner for all experiments.

| Flag | Default | Description |
|---|---|---|
| `--dry-run` | off | Print commands without executing |
| `--methods m1,m2` | all | Comma-separated list of methods to run |
| `--cooldown SECONDS` | `0` | Wait between runs; for SSD SLC cache recovery |
| `--hw-reset` | off | Reset hardware state between runs (requires sudo): `sync` + `drop_caches` + `fstrim` on `db_path_base` |

```bash
# run all combinations
python3 src/experiment/run.py <path/to/params.json>

# dry-run: print commands without executing
python3 src/experiment/run.py <path/to/params.json> --dry-run

# run only specific methods
python3 src/experiment/run.py <path/to/params.json> --methods vanila,bitlsm

# SSD deterministic state: hw-reset + cooldown for GC
python3 src/experiment/run.py <path/to/params.json> --hw-reset --cooldown 300
```

Between-run sequence when enabled: `experiment → [hw-reset: sync, drop_caches, fstrim] → [cooldown: sleep Ns] → next experiment`

---

## README.md Requirements

**Every experiment directory must have a `README.md`.** When a new experiment is created or significantly modified, generate or update its README.md as part of the same task.

Write in **English**. Required sections:

| Section | Content |
|---|---|
| **Objective** | What is being measured and why |
| **Subjects** | Methods/binaries being compared, with source filenames |
| **Experiment Types** | Each `exp_type` value: what workload it runs and how |
| **Parameters** | Full table of flags — common and method-specific — with types, defaults, descriptions |
| **Build** | CMake commands to build the relevant binaries |
| **Running** | Concrete invocation example with all flags filled in |
| **Results** | Output directory, filename convention, CSV column definitions |
| **Examples** | Shell loops sweeping one parameter at a time |
