# GPU-accelerated HNSW construction

The competition backend is AMD Radeon + ROCm/hipVS. hipVS CAGRA builds the
distance-heavy neighbor graph; seekdb converts that graph to VSAG `BuildCache`,
then VSAG finishes the HNSW build and retains ownership of index persistence and
query semantics. An optional CUDA/cuVS build uses the same adapter on NVIDIA
systems, but does not replace the hipVS backend required for AMD testing.

## Build configuration

The default build has no GPU dependency and preserves the CPU path. Build the
AMD backend with the ROCm and hipVS development packages installed:

```bash
bazel build //src/... \
  --cxxopt=-DOB_USE_HIPVS \
  --cxxopt=-std=c++17 \
  --cxxopt=-I/opt/rocm/include \
  --linkopt=-L/opt/rocm/lib \
  --linkopt=-lcuvs \
  --linkopt=-lamdhip64
```

Use the include, library, and transitive link flags exported by the installed
hipVS package. The adapter uses the public `cuvs::neighbors::cagra` API and RAFT
resource/stream types exposed by hipVS. The matching VSAG build must provide
`Index::ImportCache` and `Dataset::SourceID`.

For local NVIDIA development only, build against CUDA/cuVS with
`OB_USE_CUVS`, CUDA/cuVS include and library paths, `-lcuvs`, and `-lcudart`.
Do not define both backend macros in one binary; the build intentionally rejects
that configuration. GPU libraries must match the compiler ABI and the CAGRA
headers used to compile the adapter.

## Runtime behavior

Set `SEEKDB_HNSW_BUILD_BACKEND` before starting seekdb:

| Value | Behavior |
| --- | --- |
| `auto` (default) | Use the backend compiled into the binary for eligible builds above 10,000 rows. Fall back to CPU when GPU memory is insufficient or GPU build fails. |
| `cpu` | Always use VSAG's original CPU build. |
| `hipvs` | Require the AMD HIP/hipVS backend; fail if the binary or device does not support it. |
| `cuda` or `cuvs` | Require the NVIDIA CUDA/cuVS backend; fail if the binary or device does not support it. |

`SEEKDB_GPU_HNSW_MIN_ROWS` controls the `auto` threshold. Legacy
`SEEKDB_HIPVS_MIN_ROWS` and `SEEKDB_CUVS_MIN_ROWS` are still accepted. Only dense
FP32/L2 HNSW and HGraph builds are eligible; other vector types and metrics keep
the existing CPU path.

The builder keeps CAGRA's supported graph-build selection rather than assigning
an algorithm-specific parameter type that may not exist in every hipVS release.
It bounds output/intermediate graph degree, checks integer overflow, estimates
peak memory for two dataset staging buffers, graph work buffers, output graph,
and a 512 MiB workspace, then requires that estimate to fit in currently free
device memory after reserving at least 512 MiB or 10% of total memory. `auto`
falls back to CPU if it will not fit; forced GPU modes fail with the estimate
and budget in the log. The estimate is conservative, not a substitute for
measuring peak VRAM on the target Radeon/ROCm/hipVS release.

Successful builds log the backend, GPU graph time, graph-copy time, estimated
and available GPU memory, and final VSAG build time. These timings must be
compared against CPU builds using the same data, index parameters, and database
state. Report median build time over at least five runs and validate Recall@10
against an exact L2 baseline. Do not claim acceleration without a real AMD GPU
run; an NVIDIA-only local test does not verify hipVS.
