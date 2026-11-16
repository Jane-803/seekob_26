/*
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "cuvs_hnsw_builder.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <ostream>

#ifdef OB_USE_CUVS
#include <cuda_runtime_api.h>
#include <cuvs/neighbors/cagra.hpp>
#include <raft/core/host_mdspan.hpp>
#include <raft/core/resource/cuda_stream.hpp>
#include <raft/core/resources.hpp>
#endif

namespace oceanbase {
namespace common {
namespace obvsag {
namespace {

const char *backend_setting()
{
  const char *value = std::getenv("SEEKDB_HNSW_BUILD_BACKEND");
  return value == nullptr ? "auto" : value;
}

bool is_cuda_backend(const char *backend)
{
  return std::strcmp(backend, "cuda") == 0 || std::strcmp(backend, "cuvs") == 0;
}

int64_t minimum_rows()
{
  static constexpr int64_t DEFAULT_MIN_ROWS = 10000;
  const char *value = std::getenv("SEEKDB_CUVS_MIN_ROWS");
  if (value == nullptr) {
    return DEFAULT_MIN_ROWS;
  }
  char *end = nullptr;
  const long long parsed = std::strtoll(value, &end, 10);
  return end != value && *end == '\0' && parsed > 0 ? parsed : DEFAULT_MIN_ROWS;
}

template <typename T>
void write_value(std::ostream &output, const T &value)
{
  output.write(reinterpret_cast<const char *>(&value), sizeof(T));
}

} // namespace

bool CuvsHnswBuilder::should_attempt(int64_t rows)
{
  const char *backend = backend_setting();
  if (std::strcmp(backend, "cpu") == 0) {
    return false;
  }
  return is_cuda_backend(backend) ||
         (std::strcmp(backend, "auto") == 0 && rows >= minimum_rows());
}

bool CuvsHnswBuilder::is_forced()
{
  return is_cuda_backend(backend_setting());
}

bool CuvsHnswBuilder::estimate_peak_gpu_bytes(int64_t rows,
                                               int64_t dim,
                                               int64_t graph_degree,
                                               int64_t intermediate_degree,
                                               uint64_t &bytes)
{
  bytes = 0;
  if (rows <= 0 || dim <= 0 || graph_degree <= 0 || intermediate_degree < graph_degree) {
    return false;
  }

  const uint64_t row_count = static_cast<uint64_t>(rows);
  const uint64_t dimension = static_cast<uint64_t>(dim);
  const uint64_t output_degree = static_cast<uint64_t>(graph_degree);
  const uint64_t working_degree = static_cast<uint64_t>(intermediate_degree);
  const uint64_t max_value = std::numeric_limits<uint64_t>::max();
  if (dimension > max_value / sizeof(float) ||
      working_degree > max_value / (2 * sizeof(uint32_t)) ||
      output_degree > max_value / sizeof(uint32_t)) {
    return false;
  }

  const uint64_t vector_bytes_per_row = dimension * sizeof(float);
  const uint64_t working_bytes_per_row = working_degree * 2 * sizeof(uint32_t);
  const uint64_t output_bytes_per_row = output_degree * sizeof(uint32_t);
  if (vector_bytes_per_row > max_value / row_count ||
      working_bytes_per_row > max_value / row_count ||
      output_bytes_per_row > max_value / row_count) {
    return false;
  }

  const uint64_t vector_bytes = row_count * vector_bytes_per_row;
  const uint64_t working_bytes = row_count * working_bytes_per_row;
  const uint64_t output_bytes = row_count * output_bytes_per_row;
  const uint64_t workspace_bytes = 512ULL * 1024 * 1024;
  if (vector_bytes > max_value / 2 ||
      working_bytes > (max_value - 2 * vector_bytes) / 2 ||
      output_bytes > max_value - 2 * vector_bytes - 2 * working_bytes ||
      workspace_bytes > max_value - 2 * vector_bytes - 2 * working_bytes - output_bytes) {
    return false;
  }
  bytes = 2 * vector_bytes + 2 * working_bytes + output_bytes + workspace_bytes;
  return true;
}

int CuvsHnswBuilder::serialize_cache(const int64_t *ids,
                                     int64_t rows,
                                     int64_t degree,
                                     const std::vector<uint32_t> &graph,
                                     std::ostream &cache)
{
  if (ids == nullptr || rows <= 0 || degree <= 0 ||
      static_cast<uint64_t>(rows) > std::numeric_limits<uint32_t>::max() ||
      static_cast<uint64_t>(degree) > std::numeric_limits<size_t>::max() /
                                          static_cast<uint64_t>(rows) ||
      graph.size() != static_cast<size_t>(rows) * static_cast<size_t>(degree)) {
    return -1;
  }

  const uint64_t source_count = static_cast<uint64_t>(rows);
  write_value(cache, source_count);
  for (int64_t row = 0; row < rows && cache.good(); ++row) {
    const std::string source_id = std::to_string(ids[row]);
    const uint64_t source_id_size = source_id.size();
    write_value(cache, source_id_size);
    cache.write(source_id.data(), source_id.size());

    // VSAG expects the source vector's internal id before its neighbors.
    uint64_t valid_neighbors = 1;
    for (int64_t col = 0; col < degree; ++col) {
      const uint32_t neighbor = graph[row * degree + col];
      if (neighbor < static_cast<uint32_t>(rows) && neighbor != static_cast<uint32_t>(row)) {
        ++valid_neighbors;
      }
    }
    write_value(cache, valid_neighbors);
    const uint32_t self = static_cast<uint32_t>(row);
    write_value(cache, self);
    for (int64_t col = 0; col < degree; ++col) {
      const uint32_t neighbor = graph[row * degree + col];
      if (neighbor < static_cast<uint32_t>(rows) && neighbor != self) {
        write_value(cache, neighbor);
      }
    }
  }
  return cache.good() ? 0 : -1;
}

int CuvsHnswBuilder::build_cache(const float *vectors,
                                 const int64_t *ids,
                                 int64_t rows,
                                 int64_t dim,
                                 int64_t max_degree,
                                 std::ostream &cache,
                                 CuvsBuildStats &stats)
{
  stats.attempted_ = true;
  if (vectors == nullptr || ids == nullptr || rows < 64 || dim <= 0 || max_degree <= 0 ||
      static_cast<uint64_t>(rows) > std::numeric_limits<uint32_t>::max()) {
    stats.fallback_reason_ = "invalid CUDA/cuVS build input";
    return -1;
  }

#ifndef OB_USE_CUVS
  (void)cache;
  stats.fallback_reason_ = "seekdb was built without OB_USE_CUVS";
  return -1;
#else
  try {
    const uint32_t graph_degree = static_cast<uint32_t>(std::min<int64_t>(max_degree, rows - 1));
    const uint64_t padded_intermediate_degree = std::max<uint64_t>(
        ((static_cast<uint64_t>(graph_degree) * 2 + 31) / 32) * 32, 64);
    if (padded_intermediate_degree > std::numeric_limits<uint32_t>::max()) {
      stats.fallback_reason_ = "cuVS graph degree exceeds supported limits";
      return -1;
    }
    const uint32_t intermediate_degree = static_cast<uint32_t>(padded_intermediate_degree);
    if (intermediate_degree >= static_cast<uint64_t>(rows)) {
      stats.fallback_reason_ = "dataset is too small for the configured cuVS graph degree";
      return -1;
    }
    if (!estimate_peak_gpu_bytes(rows, dim, graph_degree, intermediate_degree,
                                 stats.estimated_gpu_bytes_)) {
      stats.fallback_reason_ = "CUDA/cuVS peak memory estimate overflowed";
      return -1;
    }

    size_t free_bytes = 0;
    size_t total_bytes = 0;
    const cudaError_t memory_info_result = cudaMemGetInfo(&free_bytes, &total_bytes);
    if (memory_info_result != cudaSuccess) {
      stats.fallback_reason_ = cudaGetErrorString(memory_info_result);
      return -1;
    }
    const uint64_t safety_reserve = std::max<uint64_t>(
        512ULL * 1024 * 1024, static_cast<uint64_t>(total_bytes) / 10);
    stats.available_gpu_bytes_ = free_bytes > safety_reserve
                                     ? static_cast<uint64_t>(free_bytes) - safety_reserve
                                     : 0;
    if (stats.estimated_gpu_bytes_ > stats.available_gpu_bytes_) {
      stats.fallback_reason_ = "estimated cuVS peak memory exceeds the available GPU budget";
      return -1;
    }

    raft::resources resources;
    cuvs::neighbors::cagra::index_params params;
    params.graph_degree = graph_degree;
    params.intermediate_graph_degree = intermediate_degree;
    auto graph_params = cuvs::neighbors::cagra::graph_build_params::nn_descent_params(
        params.intermediate_graph_degree);
    graph_params.return_distances = false;
    params.graph_build_params = graph_params;
    params.attach_dataset_on_build = false;
    auto dataset = raft::make_host_matrix_view<const float, int64_t>(vectors, rows, dim);
    const auto stream = raft::resource::get_cuda_stream(resources);

    const auto build_start = std::chrono::steady_clock::now();
    auto index = cuvs::neighbors::cagra::build(resources, params, dataset);
    const cudaError_t build_sync_result = cudaStreamSynchronize(stream.value());
    if (build_sync_result != cudaSuccess) {
      stats.fallback_reason_ = cudaGetErrorString(build_sync_result);
      return -1;
    }
    const auto build_end = std::chrono::steady_clock::now();
    stats.gpu_build_us_ = std::chrono::duration_cast<std::chrono::microseconds>(
        build_end - build_start).count();

    const auto graph_view = index.graph();
    const int64_t degree = static_cast<int64_t>(index.graph_degree());
    std::vector<uint32_t> host_graph(static_cast<size_t>(rows) * static_cast<size_t>(degree));
    const auto copy_start = std::chrono::steady_clock::now();
    const cudaError_t copy_result = cudaMemcpyAsync(host_graph.data(),
                                                    graph_view.data_handle(),
                                                    host_graph.size() * sizeof(uint32_t),
                                                    cudaMemcpyDeviceToHost,
                                                    stream.value());
    const cudaError_t copy_sync_result = copy_result == cudaSuccess
                                             ? cudaStreamSynchronize(stream.value())
                                             : copy_result;
    if (copy_result != cudaSuccess || copy_sync_result != cudaSuccess) {
      stats.fallback_reason_ = cudaGetErrorString(
          copy_result != cudaSuccess ? copy_result : copy_sync_result);
      return -1;
    }
    const auto copy_end = std::chrono::steady_clock::now();
    stats.graph_copy_us_ = std::chrono::duration_cast<std::chrono::microseconds>(
        copy_end - copy_start).count();
    if (serialize_cache(ids, rows, degree, host_graph, cache) != 0) {
      stats.fallback_reason_ = "failed to serialize the cuVS graph";
      return -1;
    }
    stats.used_gpu_ = true;
    return 0;
  } catch (const std::exception &error) {
    stats.fallback_reason_ = error.what();
    return -1;
  } catch (...) {
    stats.fallback_reason_ = "unknown cuVS exception";
    return -1;
  }
#endif
}

} // namespace obvsag
} // namespace common
} // namespace oceanbase
