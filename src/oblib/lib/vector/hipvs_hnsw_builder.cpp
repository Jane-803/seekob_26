/*
 * Copyright (c) 2025 OceanBase.
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

#include "hipvs_hnsw_builder.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <ostream>

#ifdef OB_USE_HIPVS
#include <cuvs/neighbors/cagra.hpp>
#include <hip/hip_runtime_api.h>
#include <raft/core/host_mdspan.hpp>
#include <raft/core/resources.hpp>
#include <raft/core/resource/cuda_stream.hpp>
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

int64_t minimum_rows()
{
  static constexpr int64_t DEFAULT_MIN_ROWS = 10000;
  const char *value = std::getenv("SEEKDB_HIPVS_MIN_ROWS");
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

bool HipvsHnswBuilder::should_attempt(int64_t rows)
{
  const char *backend = backend_setting();
  if (std::strcmp(backend, "cpu") == 0) {
    return false;
  }
  return std::strcmp(backend, "hipvs") == 0 ||
         (std::strcmp(backend, "auto") == 0 && rows >= minimum_rows());
}

bool HipvsHnswBuilder::is_forced()
{
  return std::strcmp(backend_setting(), "hipvs") == 0;
}

int HipvsHnswBuilder::serialize_cache(const int64_t *ids,
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

    // VSAG expects the source inner id as the first item.
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

int HipvsHnswBuilder::build_cache(const float *vectors,
                                  const int64_t *ids,
                                  int64_t rows,
                                  int64_t dim,
                                  int64_t max_degree,
                                  std::ostream &cache,
                                  HipvsBuildStats &stats)
{
  stats.attempted_ = true;
  if (vectors == nullptr || ids == nullptr || rows <= 1 || dim <= 0 || max_degree <= 0) {
    stats.fallback_reason_ = "invalid hipVS build input";
    return -1;
  }

#ifndef OB_USE_HIPVS
  stats.fallback_reason_ = "seekdb was built without OB_USE_HIPVS";
  return -1;
#else
  try {
    raft::resources resources;
    cuvs::neighbors::cagra::index_params params;
    params.graph_degree = static_cast<uint32_t>(std::min<int64_t>(max_degree, rows - 1));
    params.intermediate_graph_degree = std::max<uint32_t>(params.graph_degree * 2, 64);
    params.attach_dataset_on_build = false;
    auto dataset = raft::make_host_matrix_view<const float, int64_t>(vectors, rows, dim);
    const auto stream = raft::resource::get_cuda_stream(resources);

    const auto build_start = std::chrono::steady_clock::now();
    auto index = cuvs::neighbors::cagra::build(resources, params, dataset);
    if (hipStreamSynchronize(stream.value()) != hipSuccess) {
      stats.fallback_reason_ = "failed to synchronize the hipVS build stream";
      return -1;
    }
    const auto build_end = std::chrono::steady_clock::now();
    stats.gpu_build_us_ = std::chrono::duration_cast<std::chrono::microseconds>(
        build_end - build_start).count();

    const auto graph_view = index.graph();
    const int64_t degree = static_cast<int64_t>(index.graph_degree());
    std::vector<uint32_t> host_graph(static_cast<size_t>(rows * degree));
    const auto copy_start = std::chrono::steady_clock::now();
    hipError_t copy_result = hipMemcpyAsync(host_graph.data(),
                                            graph_view.data_handle(),
                                            host_graph.size() * sizeof(uint32_t),
                                            hipMemcpyDeviceToHost,
                                            stream.value());
    const hipError_t sync_result = copy_result == hipSuccess
                                       ? hipStreamSynchronize(stream.value())
                                       : copy_result;
    if (copy_result != hipSuccess || sync_result != hipSuccess) {
      stats.fallback_reason_ = hipGetErrorString(copy_result != hipSuccess ? copy_result : sync_result);
      return -1;
    }
    const auto copy_end = std::chrono::steady_clock::now();
    stats.graph_copy_us_ = std::chrono::duration_cast<std::chrono::microseconds>(
        copy_end - copy_start).count();
    if (serialize_cache(ids, rows, degree, host_graph, cache) != 0) {
      stats.fallback_reason_ = "failed to serialize the hipVS graph";
      return -1;
    }
    stats.used_gpu_ = true;
    return 0;
  } catch (const std::exception &error) {
    stats.fallback_reason_ = error.what();
    return -1;
  } catch (...) {
    stats.fallback_reason_ = "unknown hipVS exception";
    return -1;
  }
#endif
}

} // namespace obvsag
} // namespace common
} // namespace oceanbase
