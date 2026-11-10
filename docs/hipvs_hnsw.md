# hipVS-accelerated HNSW construction

This integration uses an AMD Radeon GPU for the distance-heavy graph
initialization stage of seekdb's dense FP32/L2 HNSW/HGraph build. hipVS CAGRA builds a
k-nearest-neighbor graph, the adapter converts it to VSAG's `BuildCache`
representation, and VSAG completes the HNSW build. VSAG remains responsible for
the final index, so persistence, filtering, incremental inserts, and query
behavior use the same seekdb format as the CPU path.

## Build

The default build has no ROCm dependency and retains the existing CPU behavior.
To include acceleration, install ROCm and hipVS on the build machine, then add
the define, include paths, and libraries to the normal seekdb Bazel invocation:

```bash
bazel build //src/... \
  --cxxopt=-DOB_USE_HIPVS \
  --cxxopt=-I/opt/rocm/include \
  --linkopt=-L/opt/rocm/lib \
  --linkopt=-lcuvs --linkopt=-lamdhip64
```

The exact hipVS library name can vary by package release; use the imported CMake
target or library flags supplied by the installed hipVS package. The code uses
the public `cuvs::neighbors::cagra` API from current hipVS releases.

VSAG must provide `Index::ImportCache` and `Dataset::SourceID`, as the dependency
version used by this branch does. These APIs form the compatibility boundary
between hipVS graph construction and the seekdb index.

## Runtime selection and fallback

Set `SEEKDB_HNSW_BUILD_BACKEND` before starting seekdb:

| Value | Behavior |
| --- | --- |
| `auto` (default) | Try hipVS for eligible indexes with at least 10,000 rows; otherwise use CPU. Any hipVS error falls back to CPU. |
| `cpu` | Always use the original VSAG CPU build. |
| `hipvs` | Require the hipVS path. An unavailable GPU, rejected cache, or build error fails the operation instead of silently changing backends. |

`SEEKDB_HIPVS_MIN_ROWS` changes the `auto` threshold. Acceleration is restricted
to dense FP32/L2 HNSW or HGraph indexes. Sparse, quantized, inner-product, and cosine
indexes retain the established CPU implementation so distance semantics cannot
silently change.

Logs contain `HNSW graph initialized with hipVS` together with row count,
dimension, GPU graph-build time, device-to-host graph-copy time, and VSAG final
build time. CPU-only binaries log `HNSW index built with CPU`. hipVS exceptions
and device errors include an explicit fallback reason.

## Reproducible performance and quality check

Use the same checkout, release flags, data file, and HNSW parameters for both
runs. Restart seekdb between runs, drop the test table/index, and clear only the
database state associated with the benchmark before rebuilding it.

1. Start seekdb with `SEEKDB_HNSW_BUILD_BACKEND=cpu`, import the vectors, create
   the HNSW index, and record wall-clock DDL time and the CPU timing log.
2. Repeat with `SEEKDB_HNSW_BUILD_BACKEND=hipvs` and record wall-clock DDL time
   plus the three hipVS timing fields.
3. Run the same held-out query vectors against both indexes. Compute Recall@10
   against an exact L2 top-10 baseline and compare query latency.
4. Repeat each build at least five times and report the median. Record vector
   count/dimension, HNSW degree and `ef_construction`, CPU, Radeon model, ROCm,
   hipVS, compiler, and seekdb commit.

Report `speedup = median CPU build time / median hipVS build time`. Do not count
data generation or database startup in index build time. A valid result must
show that Recall@10 does not materially regress; if it does, increase graph
degree/intermediate degree before claiming the optimization.

Suggested result table:

| Backend | Rows x dimensions | Median build | Speedup | Recall@10 | p50 query |
| --- | --- | ---: | ---: | ---: | ---: |
| VSAG CPU | actual | actual | 1.00x | actual | actual |
| hipVS + VSAG | same | actual | actual | actual | actual |

No performance number is checked into this repository because it would be
misleading without an AMD GPU run. The forced `hipvs` mode is intended for CI
or competition hardware: it prevents a benchmark from accidentally measuring
the fallback path.
