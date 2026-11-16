# CUDA/cuVS-accelerated HNSW construction

This integration accelerates seekdb dense FP32/L2 HNSW and HGraph construction
on NVIDIA GPUs. NVIDIA cuVS CAGRA builds the distance-heavy k-nearest-neighbor
graph, the adapter converts it to VSAG's `BuildCache` representation, and VSAG
finishes the HNSW build. VSAG remains responsible for the final index, so the
on-disk format, filtering, incremental inserts, and query path are unchanged.

## Dependencies and build

The normal build has no CUDA dependency and keeps the existing CPU behavior.
The GPU build requires a supported NVIDIA driver, CUDA Toolkit, and cuVS C++
development packages. Add the feature define, include paths, and libraries to
the normal seekdb Bazel invocation:

```bash
bazel build //src/... \
  --cxxopt=-DOB_USE_CUVS \
  --cxxopt=-std=c++17 \
  --cxxopt=-I/usr/local/cuda/include \
  --cxxopt=-I/opt/rapids/include \
  --linkopt=-L/usr/local/cuda/lib64 \
  --linkopt=-L/opt/rapids/lib \
  --linkopt=-lcuvs \
  --linkopt=-lcudart
```

Adjust the cuVS include and library paths and any package-specific transitive
link dependencies for the selected package. The code uses the public
`cuvs::neighbors::cagra` C++ API and RAFT resource/stream types. The graph
parameter configuration is compatible with the cuVS 26.10 C++ API; compile
against matching cuVS/RAFT headers and libraries.
VSAG must provide `Index::ImportCache` and `Dataset::SourceID`, as the dependency
version in the competition branch does.

## Runtime selection and fallback

Set `SEEKDB_HNSW_BUILD_BACKEND` before starting seekdb:

| Value | Behavior |
| --- | --- |
| `auto` (default) | Try CUDA/cuVS for eligible indexes with at least 10,000 rows; otherwise use CPU. A cuVS or CUDA error falls back to CPU. |
| `cpu` | Always use the original VSAG CPU build. |
| `cuda` or `cuvs` | Require CUDA/cuVS. An unavailable GPU, rejected cache, or build error fails the operation. |

`SEEKDB_CUVS_MIN_ROWS` changes the `auto` threshold. The graph builder is
explicitly configured for cuVS NN-Descent, with distance output disabled and
the intermediate degree rounded up to a multiple of 32. Before building, seekdb
estimates peak GPU memory for two FP32 dataset buffers (input staging), two
working-graph buffers, the output graph, and a 512 MiB workspace. It also
reserves at least 512 MiB or 10%
of device memory, whichever is larger. `auto` falls back to CPU if the estimated
peak does not fit; forced GPU mode fails with the estimated and available budget
in the log instead of proceeding toward an OOM. The estimate is conservative,
but should still be checked against peak-memory telemetry on the target cuVS
release and GPU.

Acceleration is limited to dense FP32/L2 HNSW and HGraph indexes. Sparse,
quantized, inner-product, and cosine indexes keep the CPU path so metric
semantics cannot silently change.

Successful GPU builds log `HNSW graph initialized with CUDA/cuVS`, including row
count, dimension, GPU graph-build time, device-to-host copy time, and final VSAG
build time. CPU builds log `HNSW index built with CPU`. Fallback logs contain the
cuVS exception or CUDA runtime error.

## Reproducible performance and quality check

Use identical release flags, input vectors, HNSW parameters, and database state
for both backends. Restart seekdb and recreate the table/index between runs.

1. Set `SEEKDB_HNSW_BUILD_BACKEND=cpu`, import vectors, create the index, and
   record DDL wall time plus the CPU timing log.
2. Repeat with `SEEKDB_HNSW_BUILD_BACKEND=cuda` and record DDL wall time plus
   the three CUDA/cuVS timing fields.
3. Run the same held-out queries against both indexes and compute Recall@10
   against an exact L2 top-10 baseline.
4. Repeat each build at least five times and report the median. Record rows,
   dimension, graph degree, `ef_construction`, CPU, NVIDIA GPU, driver, CUDA,
   cuVS, compiler, and seekdb commit.

Report `speedup = median CPU build time / median CUDA build time`. Do not include
data generation or server startup. A valid result must show no material
Recall@10 regression; otherwise increase graph and intermediate graph degrees
before claiming acceleration.

| Backend | Rows x dimensions | Median build | Speedup | Recall@10 | p50 query |
| --- | --- | ---: | ---: | ---: | ---: |
| VSAG CPU | actual | actual | 1.00x | actual | actual |
| CUDA/cuVS + VSAG | same | actual | actual | actual | actual |

No benchmark number is checked in without a real NVIDIA GPU run. Forced `cuda`
mode is intended for competition hardware because it prevents accidentally
benchmarking the CPU fallback.
