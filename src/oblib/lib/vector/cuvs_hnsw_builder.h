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

#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace oceanbase {
namespace common {
namespace obvsag {

struct CuvsBuildStats {
  bool attempted_ = false;
  bool used_gpu_ = false;
  int64_t gpu_build_us_ = 0;
  int64_t graph_copy_us_ = 0;
  std::string fallback_reason_;
};

// Builds a CAGRA graph on an NVIDIA GPU and serializes it in VSAG's
// BuildCache format. VSAG still owns the final HNSW index and its persistence.
class CuvsHnswBuilder {
public:
  static bool should_attempt(int64_t rows);
  static bool is_forced();

  static int build_cache(const float *vectors,
                         const int64_t *ids,
                         int64_t rows,
                         int64_t dim,
                         int64_t max_degree,
                         std::ostream &cache,
                         CuvsBuildStats &stats);

  // Public so CPU-only tests can validate the cuVS-to-VSAG boundary.
  static int serialize_cache(const int64_t *ids,
                             int64_t rows,
                             int64_t degree,
                             const std::vector<uint32_t> &graph,
                             std::ostream &cache);
};

} // namespace obvsag
} // namespace common
} // namespace oceanbase
