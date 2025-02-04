// Copyright (c) 2023 dingodb.com, Inc. All Rights Reserved
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "vector/vector_reader.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/helper.h"
#include "fmt/core.h"
#include "proto/common.pb.h"
#include "server/server.h"
#include "vector/codec.h"
#include "vector/vector_index.h"

namespace dingodb {

butil::Status VectorReader::QueryVectorWithId(uint64_t partition_id, uint64_t vector_id, bool with_vector_data,
                                              pb::common::VectorWithId& vector_with_id) {
  std::string key;
  VectorCodec::EncodeVectorData(partition_id, vector_id, key);

  std::string value;
  auto status = reader_->KvGet(key, value);
  if (!status.ok()) {
    return status;
  }

  if (with_vector_data) {
    pb::common::Vector vector;
    if (!vector.ParseFromString(value)) {
      return butil::Status(pb::error::EINTERNAL, "Parse proto from string error");
    }
    vector_with_id.mutable_vector()->Swap(&vector);
  }

  vector_with_id.set_id(vector_id);

  return butil::Status();
}

butil::Status VectorReader::SearchVector(
    uint64_t partition_id, std::shared_ptr<VectorIndex> vector_index, pb::common::Range region_range,
    const std::vector<pb::common::VectorWithId>& vector_with_ids, const pb::common::VectorSearchParameter& parameter,
    std::vector<pb::index::VectorWithDistanceResult>& vector_with_distance_results) {
  if (vector_with_ids.empty()) {
    DINGO_LOG(WARNING) << "Empty vector with ids";
    return butil::Status();
  }

  auto vector_filter = parameter.vector_filter();
  auto vector_filter_type = parameter.vector_filter_type();

  bool with_vector_data = !(parameter.without_vector_data());
  std::vector<pb::index::VectorWithDistanceResult> tmp_results;

  uint64_t min_vector_id = VectorCodec::DecodeVectorId(region_range.start_key());
  uint64_t max_vector_id = VectorCodec::DecodeVectorId(region_range.end_key());
  DINGO_LOG(INFO) << fmt::format("vector id range [{}-{})", min_vector_id, max_vector_id);

  std::vector<std::shared_ptr<VectorIndex::FilterFunctor>> filters;
  if (vector_index->VectorIndexType() == pb::common::VECTOR_INDEX_TYPE_HNSW) {
    filters.push_back(std::make_shared<VectorIndex::RangeFilterFunctor>(min_vector_id, max_vector_id));
  } else if (vector_index->VectorIndexType() == pb::common::VECTOR_INDEX_TYPE_FLAT) {
    filters.push_back(std::make_shared<VectorIndex::FlatRangeFilterFunctor>(min_vector_id, max_vector_id));
  }

  // scalar post filter
  if (dingodb::pb::common::VectorFilter::SCALAR_FILTER == vector_filter &&
      dingodb::pb::common::VectorFilterType::QUERY_POST == vector_filter_type) {
    uint32_t top_n = parameter.top_n();
    if (BAIDU_UNLIKELY(vector_with_ids[0].scalar_data().scalar_data_size() == 0)) {
      butil::Status status =
          vector_index->Search(vector_with_ids, top_n, filters, vector_with_distance_results, with_vector_data);
      if (!status.ok()) {
        DINGO_LOG(ERROR) << fmt::format("vector_index::Search failed ");
        return status;
      }
    } else {
      top_n *= 10;

      butil::Status status = vector_index->Search(vector_with_ids, top_n, filters, tmp_results, with_vector_data);
      if (!status.ok()) {
        DINGO_LOG(ERROR) << fmt::format("vector_index::Search failed ");
        return status;
      }
      for (auto& vector_with_distance_result : tmp_results) {
        pb::index::VectorWithDistanceResult new_vector_with_distance_result;

        for (auto& temp_vector_with_distance : *vector_with_distance_result.mutable_vector_with_distances()) {
          uint64_t temp_id = temp_vector_with_distance.vector_with_id().id();
          bool compare_result = false;
          butil::Status status =
              CompareVectorScalarData(partition_id, temp_id, vector_with_ids[0].scalar_data(), compare_result);
          if (!status.ok()) {
            return status;
          }
          if (!compare_result) {
            continue;
          }

          new_vector_with_distance_result.add_vector_with_distances()->Swap(&temp_vector_with_distance);
          if (new_vector_with_distance_result.vector_with_distances_size() >= parameter.top_n()) {
            break;
          }
        }
        vector_with_distance_results.emplace_back(std::move(new_vector_with_distance_result));
      }
    }
  } else if (dingodb::pb::common::VectorFilter::VECTOR_ID_FILTER == vector_filter) {  // vector id array search
    butil::Status status = DoVectorSearchForVectorIdPreFilter(vector_index, vector_with_ids, parameter, filters,
                                                              vector_with_distance_results);
    if (!status.ok()) {
      DINGO_LOG(ERROR) << fmt::format("DoVectorSearchForVectorIdPreFilter failed");
      return status;
    }

  } else if (dingodb::pb::common::VectorFilter::SCALAR_FILTER == vector_filter &&
             dingodb::pb::common::VectorFilterType::QUERY_PRE == vector_filter_type) {  // scalar pre filter search

    butil::Status status = DoVectorSearchForScalarPreFilter(vector_index, region_range, vector_with_ids, parameter,
                                                            filters, vector_with_distance_results);
    if (!status.ok()) {
      DINGO_LOG(ERROR) << fmt::format("DoVectorSearchForScalarPreFilter failed ");
      return status;
    }

  } else if (dingodb::pb::common::VectorFilter::TABLE_FILTER ==
             vector_filter) {  //  table coprocessor pre filter search. not impl
    butil::Status status = DoVectorSearchForTableCoprocessor(vector_index, partition_id, vector_with_ids, parameter,
                                                             vector_with_distance_results);
    if (!status.ok()) {
      DINGO_LOG(ERROR) << fmt::format("DoVectorSearchForTableCoprocessor failed ");
      return status;
    }
  }

  // if vector index does not support restruct vector ,we restruct it using RocksDB
  if (with_vector_data) {
    for (auto& result : vector_with_distance_results) {
      for (auto& vector_with_distance : *result.mutable_vector_with_distances()) {
        if (vector_with_distance.vector_with_id().vector().float_values_size() > 0 ||
            vector_with_distance.vector_with_id().vector().binary_values_size() > 0) {
          continue;
        }

        pb::common::VectorWithId vector_with_id;
        auto status = QueryVectorWithId(partition_id, vector_with_distance.vector_with_id().id(), true, vector_with_id);
        if (!status.ok()) {
          return status;
        }
        vector_with_distance.mutable_vector_with_id()->Swap(&vector_with_id);
      }
    }
  }

  return butil::Status();
}

butil::Status VectorReader::QueryVectorTableData(uint64_t partition_id, pb::common::VectorWithId& vector_with_id) {
  std::string key, value;
  VectorCodec::EncodeVectorTable(partition_id, vector_with_id.id(), key);

  auto status = reader_->KvGet(key, value);
  if (!status.ok()) {
    return status;
  }

  pb::common::VectorTableData vector_table;
  if (!vector_table.ParseFromString(value)) {
    return butil::Status(pb::error::EINTERNAL, "Decode vector table data failed");
  }

  vector_with_id.mutable_table_data()->CopyFrom(vector_table);

  return butil::Status();
}

butil::Status VectorReader::QueryVectorTableData(uint64_t partition_id,
                                                 std::vector<pb::index::VectorWithDistanceResult>& results) {
  // get metadata by parameter
  for (auto& result : results) {
    for (auto& vector_with_distance : *result.mutable_vector_with_distances()) {
      pb::common::VectorWithId& vector_with_id = *(vector_with_distance.mutable_vector_with_id());
      QueryVectorTableData(partition_id, vector_with_id);
    }
  }

  return butil::Status();
}

butil::Status VectorReader::QueryVectorTableData(uint64_t partition_id,
                                                 std::vector<pb::common::VectorWithDistance>& vector_with_distances) {
  // get metadata by parameter
  for (auto& vector_with_distance : vector_with_distances) {
    pb::common::VectorWithId& vector_with_id = *(vector_with_distance.mutable_vector_with_id());
    QueryVectorTableData(partition_id, vector_with_id);
  }

  return butil::Status();
}

butil::Status VectorReader::QueryVectorScalarData(uint64_t partition_id, std::vector<std::string> selected_scalar_keys,
                                                  pb::common::VectorWithId& vector_with_id) {
  std::string key, value;
  VectorCodec::EncodeVectorScalar(partition_id, vector_with_id.id(), key);

  auto status = reader_->KvGet(key, value);
  if (!status.ok()) {
    return status;
  }

  pb::common::VectorScalardata vector_scalar;
  if (!vector_scalar.ParseFromString(value)) {
    return butil::Status(pb::error::EINTERNAL, "Decode vector scalar data failed");
  }

  auto* scalar = vector_with_id.mutable_scalar_data()->mutable_scalar_data();
  for (const auto& [key, value] : vector_scalar.scalar_data()) {
    if (!selected_scalar_keys.empty() &&
        std::find(selected_scalar_keys.begin(), selected_scalar_keys.end(), key) == selected_scalar_keys.end()) {
      continue;
    }

    scalar->insert({key, value});
  }

  return butil::Status();
}

butil::Status VectorReader::QueryVectorScalarData(uint64_t partition_id, std::vector<std::string> selected_scalar_keys,
                                                  std::vector<pb::index::VectorWithDistanceResult>& results) {
  // get metadata by parameter
  for (auto& result : results) {
    for (auto& vector_with_distance : *result.mutable_vector_with_distances()) {
      pb::common::VectorWithId& vector_with_id = *(vector_with_distance.mutable_vector_with_id());
      QueryVectorScalarData(partition_id, selected_scalar_keys, vector_with_id);
    }
  }

  return butil::Status();
}

butil::Status VectorReader::QueryVectorScalarData(uint64_t partition_id, std::vector<std::string> selected_scalar_keys,
                                                  std::vector<pb::common::VectorWithDistance>& vector_with_distances) {
  // get metadata by parameter
  for (auto& vector_with_distance : vector_with_distances) {
    pb::common::VectorWithId& vector_with_id = *(vector_with_distance.mutable_vector_with_id());
    QueryVectorScalarData(partition_id, selected_scalar_keys, vector_with_id);
  }

  return butil::Status();
}

butil::Status VectorReader::CompareVectorScalarData(uint64_t partition_id, uint64_t vector_id,
                                                    const pb::common::VectorScalardata& source_scalar_data,
                                                    bool& compare_result) {
  compare_result = false;
  std::string key, value;

  VectorCodec::EncodeVectorScalar(partition_id, vector_id, key);

  auto status = reader_->KvGet(key, value);
  if (!status.ok()) {
    DINGO_LOG(WARNING) << fmt::format("Get vector scalar data failed, vector_id: {} error: {} ", vector_id,
                                      status.error_str());
    return status;
  }

  pb::common::VectorScalardata vector_scalar;
  if (!vector_scalar.ParseFromString(value)) {
    return butil::Status(pb::error::EINTERNAL, "Decode vector scalar data failed");
  }

  for (const auto& [key, value] : source_scalar_data.scalar_data()) {
    auto it = vector_scalar.scalar_data().find(key);
    if (it == vector_scalar.scalar_data().end()) {
      compare_result = false;
      return butil::Status();
    }

    compare_result = Helper::IsEqualVectorScalarValue(value, it->second);
    if (!compare_result) {
      return butil::Status();
    }
  }

  compare_result = true;
  return butil::Status();
}

butil::Status VectorReader::VectorBatchSearch(std::shared_ptr<Engine::VectorReader::Context> ctx,
                                              std::vector<pb::index::VectorWithDistanceResult>& results) {  // NOLINT
  // Search vectors by vectors
  auto status = SearchVector(ctx->partition_id, ctx->vector_index, ctx->region_range, ctx->vector_with_ids,
                             ctx->parameter, results);
  if (!status.ok()) {
    return status;
  }

  if (ctx->parameter.with_scalar_data()) {
    // Get scalar data by parameter
    std::vector<std::string> selected_scalar_keys = Helper::PbRepeatedToVector(ctx->parameter.selected_keys());
    auto status = QueryVectorScalarData(ctx->partition_id, selected_scalar_keys, results);
    if (!status.ok()) {
      return status;
    }
  }

  if (ctx->parameter.with_table_data()) {
    // Get table data by parameter
    auto status = QueryVectorTableData(ctx->partition_id, results);
    if (!status.ok()) {
      return status;
    }
  }

  return butil::Status();
}

butil::Status VectorReader::VectorBatchQuery(std::shared_ptr<Engine::VectorReader::Context> ctx,
                                             std::vector<pb::common::VectorWithId>& vector_with_ids) {
  for (auto vector_id : ctx->vector_ids) {
    pb::common::VectorWithId vector_with_id;
    auto status = QueryVectorWithId(ctx->partition_id, vector_id, ctx->with_vector_data, vector_with_id);
    if (!status.ok()) {
      DINGO_LOG(WARNING) << fmt::format("Query vector_with_id failed, vector_id: {} error: {} ", vector_id,
                                        status.error_str());
    }

    // if the id is not exist, the vector_with_id will be empty, sdk client will handle this
    vector_with_ids.push_back(vector_with_id);
  }

  if (ctx->with_scalar_data) {
    for (auto& vector_with_id : vector_with_ids) {
      if (vector_with_id.ByteSizeLong() == 0) {
        continue;
      }

      auto status = QueryVectorScalarData(ctx->partition_id, ctx->selected_scalar_keys, vector_with_id);
      if (!status.ok()) {
        DINGO_LOG(WARNING) << fmt::format("Query vector scalar data failed, vector_id: {} error: {} ",
                                          vector_with_id.id(), status.error_str());
      }
    }
  }

  if (ctx->with_table_data) {
    for (auto& vector_with_id : vector_with_ids) {
      if (vector_with_id.ByteSizeLong() == 0) {
        continue;
      }

      auto status = QueryVectorTableData(ctx->partition_id, vector_with_id);
      if (!status.ok()) {
        DINGO_LOG(WARNING) << fmt::format("Query vector table data failed, vector_id: {} error: {} ",
                                          vector_with_id.id(), status.error_str());
      }
    }
  }

  return butil::Status::OK();
}

butil::Status VectorReader::VectorGetBorderId(const pb::common::Range& region_range, bool get_min,
                                              uint64_t& vector_id) {
  auto status = GetBorderId(region_range, get_min, vector_id);
  if (!status.ok()) {
    DINGO_LOG(INFO) << "Get border vector id failed, error: " << status.error_str();
    return status;
  }

  return butil::Status();
}

butil::Status VectorReader::VectorScanQuery(std::shared_ptr<Engine::VectorReader::Context> ctx,
                                            std::vector<pb::common::VectorWithId>& vector_with_ids) {
  DINGO_LOG(INFO) << fmt::format("Scan vector id, region_id: {} start_id: {} is_reverse: {} limit: {}", ctx->region_id,
                                 ctx->start_id, ctx->is_reverse, ctx->limit);

  // scan for ids
  std::vector<uint64_t> vector_ids;
  auto status = ScanVectorId(ctx, vector_ids);
  if (!status.ok()) {
    DINGO_LOG(INFO) << "Scan vector id failed, error: " << status.error_str();
    return status;
  }

  DINGO_LOG(INFO) << "scan vector id count: " << vector_ids.size();

  if (vector_ids.empty()) {
    return butil::Status();
  }

  // query vector with id
  for (auto vector_id : vector_ids) {
    pb::common::VectorWithId vector_with_id;
    auto status = QueryVectorWithId(ctx->partition_id, vector_id, ctx->with_vector_data, vector_with_id);
    if (!status.ok()) {
      DINGO_LOG(WARNING) << fmt::format("Query vector data failed, vector_id {} error: {}", vector_id,
                                        status.error_str());
    }

    // if the id is not exist, the vector_with_id will be empty, sdk client will handle this
    vector_with_ids.push_back(vector_with_id);
  }

  if (ctx->with_scalar_data) {
    for (auto& vector_with_id : vector_with_ids) {
      if (vector_with_id.ByteSizeLong() == 0) {
        continue;
      }

      auto status = QueryVectorScalarData(ctx->partition_id, ctx->selected_scalar_keys, vector_with_id);
      if (!status.ok()) {
        DINGO_LOG(WARNING) << fmt::format("Query vector scalar data failed, vector_id {} error: {]",
                                          vector_with_id.id(), status.error_str());
      }
    }
  }

  if (ctx->with_table_data) {
    for (auto& vector_with_id : vector_with_ids) {
      if (vector_with_id.ByteSizeLong() == 0) {
        continue;
      }

      auto status = QueryVectorTableData(ctx->partition_id, vector_with_id);
      if (!status.ok()) {
        DINGO_LOG(WARNING) << fmt::format("Query vector table data failed, vector_id {} error: {]", vector_with_id.id(),
                                          status.error_str());
      }
    }
  }

  return butil::Status::OK();
}

butil::Status VectorReader::VectorGetRegionMetrics(uint64_t region_id, const pb::common::Range& region_range,
                                                   std::shared_ptr<VectorIndex> vector_index,
                                                   pb::common::VectorIndexMetrics& region_metrics) {
  auto vector_index_manager = Server::GetInstance()->GetVectorIndexManager();
  if (vector_index_manager == nullptr) {
    return butil::Status(pb::error::EVECTOR_INDEX_NOT_FOUND, fmt::format("Not found vector index mgr {}", region_id));
  }

  uint64_t total_vector_count = 0;
  uint64_t total_deleted_count = 0;
  uint64_t total_memory_usage = 0;
  uint64_t max_id = 0;
  uint64_t min_id = 0;

  vector_index->GetCount(total_vector_count);
  vector_index->GetDeletedCount(total_deleted_count);
  vector_index->GetMemorySize(total_memory_usage);

  GetBorderId(region_range, true, min_id);
  GetBorderId(region_range, false, max_id);

  region_metrics.set_current_count(total_vector_count);
  region_metrics.set_deleted_count(total_deleted_count);
  region_metrics.set_memory_bytes(total_memory_usage);
  region_metrics.set_max_id(max_id);
  region_metrics.set_min_id(min_id);

  return butil::Status();
}

// GetBorderId
butil::Status VectorReader::GetBorderId(const pb::common::Range& region_range, bool get_min, uint64_t& vector_id) {
  std::string start_key = VectorCodec::FillVectorDataPrefix(region_range.start_key());
  std::string end_key = VectorCodec::FillVectorDataPrefix(region_range.end_key());

  if (get_min) {
    IteratorOptions options;
    options.upper_bound = end_key;
    auto iter = reader_->NewIterator(options);
    if (iter == nullptr) {
      DINGO_LOG(ERROR) << fmt::format("New iterator failed, region range [{}-{})",
                                      Helper::StringToHex(region_range.start_key()),
                                      Helper::StringToHex(region_range.end_key()));
      return butil::Status(pb::error::Errno::EINTERNAL, "New iterator failed");
    }

    iter->Seek(start_key);
    if (!iter->Valid()) {
      vector_id = 0;
      return butil::Status();
    }

    std::string key(iter->Key());
    vector_id = VectorCodec::DecodeVectorId(key);
  } else {
    IteratorOptions options;
    options.lower_bound = start_key;
    auto iter = reader_->NewIterator(options);
    if (iter == nullptr) {
      DINGO_LOG(ERROR) << fmt::format("New iterator failed, region range [{}-{})",
                                      Helper::StringToHex(region_range.start_key()),
                                      Helper::StringToHex(region_range.end_key()));
      return butil::Status(pb::error::Errno::EINTERNAL, "New iterator failed");
    }
    iter->SeekForPrev(end_key);
    if (!iter->Valid()) {
      vector_id = 0;
      return butil::Status();
    }

    std::string key(iter->Key());
    vector_id = VectorCodec::DecodeVectorId(key);
  }

  return butil::Status::OK();
}

// ScanVectorId
butil::Status VectorReader::ScanVectorId(std::shared_ptr<Engine::VectorReader::Context> ctx,
                                         std::vector<uint64_t>& vector_ids) {
  std::string seek_key;
  VectorCodec::EncodeVectorData(ctx->partition_id, ctx->start_id, seek_key);

  IteratorOptions options;
  if (!ctx->is_reverse) {
    options.upper_bound = VectorCodec::FillVectorDataPrefix(ctx->region_range.end_key());
    auto iter = reader_->NewIterator(options);
    if (iter == nullptr) {
      DINGO_LOG(ERROR) << fmt::format("New iterator failed, region range [{}-{})",
                                      Helper::StringToHex(ctx->region_range.start_key()),
                                      Helper::StringToHex(ctx->region_range.end_key()));
      return butil::Status(pb::error::Errno::EINTERNAL, "New iterator failed");
    }
    for (iter->Seek(seek_key); iter->Valid(); iter->Next()) {
      pb::common::VectorWithId vector;

      std::string key(iter->Key());
      auto vector_id = VectorCodec::DecodeVectorId(key);
      if (vector_id == 0 || vector_id == UINT64_MAX) {
        continue;
      }

      if (vector_id < ctx->start_id) {
        break;
      }

      if (ctx->end_id != 0 && vector_id > ctx->end_id) {
        break;
      }

      if (ctx->use_scalar_filter) {
        bool compare_result = false;
        auto status =
            CompareVectorScalarData(ctx->partition_id, vector_id, ctx->scalar_data_for_filter, compare_result);
        if (!status.ok()) {
          DINGO_LOG(ERROR) << " CompareVectorScalarData failed, vector_id: " << vector_id
                           << " error: " << status.error_str();
          return status;
        }
        if (!compare_result) {
          continue;
        }
      }

      vector_ids.push_back(vector_id);
      if (vector_ids.size() >= ctx->limit) {
        break;
      }
    }
  } else {
    options.lower_bound = VectorCodec::FillVectorDataPrefix(ctx->region_range.start_key());
    auto iter = reader_->NewIterator(options);
    if (iter == nullptr) {
      DINGO_LOG(ERROR) << fmt::format("New iterator failed, region range [{}-{})",
                                      Helper::StringToHex(ctx->region_range.start_key()),
                                      Helper::StringToHex(ctx->region_range.end_key()));
      return butil::Status(pb::error::Errno::EINTERNAL, "New iterator failed");
    }
    for (iter->SeekForPrev(seek_key); iter->Valid(); iter->Prev()) {
      pb::common::VectorWithId vector;

      std::string key(iter->Key());
      auto vector_id = VectorCodec::DecodeVectorId(key);
      if (vector_id == 0 || vector_id == UINT64_MAX) {
        continue;
      }

      if (vector_id > ctx->start_id) {
        break;
      }

      if (ctx->end_id != 0 && vector_id < ctx->end_id) {
        break;
      }

      if (ctx->use_scalar_filter) {
        bool compare_result = false;
        auto status =
            CompareVectorScalarData(ctx->partition_id, vector_id, ctx->scalar_data_for_filter, compare_result);
        if (!status.ok()) {
          return status;
        }
        if (!compare_result) {
          continue;
        }
      }

      vector_ids.push_back(vector_id);
      if (vector_ids.size() >= ctx->limit) {
        break;
      }
    }
  }

  return butil::Status::OK();
}

butil::Status VectorReader::DoVectorSearchForVectorIdPreFilter(  // NOLINT
    std::shared_ptr<VectorIndex> vector_index, const std::vector<pb::common::VectorWithId>& vector_with_ids,
    const pb::common::VectorSearchParameter& parameter,
    std::vector<std::shared_ptr<VectorIndex::FilterFunctor>> filters,
    std::vector<pb::index::VectorWithDistanceResult>& vector_with_distance_results) {
  if (vector_index->VectorIndexType() == pb::common::VECTOR_INDEX_TYPE_HNSW) {
    filters.push_back(
        std::make_shared<VectorIndex::HnswListFilterFunctor>(Helper::PbRepeatedToVector(parameter.vector_ids())));
  } else if (vector_index->VectorIndexType() == pb::common::VECTOR_INDEX_TYPE_FLAT) {
    filters.push_back(
        std::make_shared<VectorIndex::FlatListFilterFunctor>(Helper::PbRepeatedToVector(parameter.vector_ids())));
  }

  butil::Status status = vector_index->Search(vector_with_ids, parameter.top_n(), filters, vector_with_distance_results,
                                              !(parameter.without_vector_data()));
  if (!status.ok()) {
    std::string s = fmt::format("DoVectorSearchForVectorIdPreFilter::VectorIndex::Search failed");
    DINGO_LOG(ERROR) << s;
    return status;
  }

  return butil::Status::OK();
}

butil::Status VectorReader::DoVectorSearchForScalarPreFilter(
    std::shared_ptr<VectorIndex> vector_index, pb::common::Range region_range,
    const std::vector<pb::common::VectorWithId>& vector_with_ids, const pb::common::VectorSearchParameter& parameter,
    std::vector<std::shared_ptr<VectorIndex::FilterFunctor>> filters,
    std::vector<pb::index::VectorWithDistanceResult>& vector_with_distance_results) {  // NOLINT

  // scalar pre filter search

  const auto& std_vector_scalar = vector_with_ids[0].scalar_data();
  auto lambda_scalar_compare_function =
      [&std_vector_scalar](const pb::common::VectorScalardata& internal_vector_scalar) {
        for (const auto& [key, value] : std_vector_scalar.scalar_data()) {
          auto it = internal_vector_scalar.scalar_data().find(key);
          if (it == internal_vector_scalar.scalar_data().end()) {
            return false;
          }

          bool compare_result = Helper::IsEqualVectorScalarValue(value, it->second);
          if (!compare_result) {
            return false;
          }
        }
        return true;
      };

  std::string start_key = VectorCodec::FillVectorScalarPrefix(region_range.start_key());
  std::string end_key = VectorCodec::FillVectorScalarPrefix(region_range.end_key());

  IteratorOptions options;
  options.upper_bound = end_key;

  auto iter = reader_->NewIterator(options);
  if (iter == nullptr) {
    DINGO_LOG(ERROR) << fmt::format("New iterator failed, region range [{}-{})",
                                    Helper::StringToHex(region_range.start_key()),
                                    Helper::StringToHex(region_range.end_key()));
    return butil::Status(pb::error::Errno::EINTERNAL, "New iterator failed");
  }

  std::vector<uint64_t> vector_ids;
  vector_ids.reserve(1024);
  for (iter->Seek(start_key); iter->Valid(); iter->Next()) {
    pb::common::VectorScalardata internal_vector_scalar;
    if (!internal_vector_scalar.ParseFromString(std::string(iter->Value()))) {
      return butil::Status(pb::error::EINTERNAL, "Internal error, decode VectorScalar failed");
    }

    bool compare_result = lambda_scalar_compare_function(internal_vector_scalar);
    if (compare_result) {
      std::string key(iter->Key());
      uint64_t internal_vector_id = VectorCodec::DecodeVectorId(key);
      if (0 == internal_vector_id) {
        std::string s = fmt::format("VectorCodec::DecodeVectorId failed key : {}", Helper::StringToHex(key));
        DINGO_LOG(ERROR) << s;
        return butil::Status(pb::error::Errno::EVECTOR_NOT_SUPPORT, s);
      }
      vector_ids.push_back(internal_vector_id);
    }
  }

  if (vector_index->VectorIndexType() == pb::common::VECTOR_INDEX_TYPE_HNSW) {
    filters.push_back(std::make_shared<VectorIndex::HnswListFilterFunctor>(vector_ids));
  } else if (vector_index->VectorIndexType() == pb::common::VECTOR_INDEX_TYPE_FLAT) {
    filters.push_back(std::make_shared<VectorIndex::FlatListFilterFunctor>(std::move(vector_ids)));
  }

  butil::Status status = vector_index->Search(vector_with_ids, parameter.top_n(), filters, vector_with_distance_results,
                                              !(parameter.without_vector_data()));
  if (!status.ok()) {
    std::string s = fmt::format("DoVectorSearchForScalarPreFilter::VectorIndex::Search failed");
    DINGO_LOG(ERROR) << s;
    return status;
  }

  return butil::Status::OK();
}

butil::Status VectorReader::DoVectorSearchForTableCoprocessor(  // NOLINT(*static)
    [[maybe_unused]] std::shared_ptr<VectorIndex> vector_index, [[maybe_unused]] uint64_t partition_id,
    [[maybe_unused]] const std::vector<pb::common::VectorWithId>& vector_with_ids,
    [[maybe_unused]] const pb::common::VectorSearchParameter& parameter,
    std::vector<pb::index::VectorWithDistanceResult>& vector_with_distance_results) {  // NOLINT
  std::string s = fmt::format("vector index search table filter for coprocessor not support now !!! ");
  DINGO_LOG(ERROR) << s;
  return butil::Status(pb::error::Errno::EVECTOR_NOT_SUPPORT, s);
}

butil::Status VectorReader::VectorBatchSearchDebug(std::shared_ptr<Engine::VectorReader::Context> ctx,
                                                   std::vector<pb::index::VectorWithDistanceResult>& results,
                                                   int64_t& deserialization_id_time_us, int64_t& scan_scalar_time_us,
                                                   int64_t& search_time_us) {  // NOLINT
  // Search vectors by vectors
  auto status =
      SearchVectorDebug(ctx->partition_id, ctx->vector_index, ctx->region_range, ctx->vector_with_ids, ctx->parameter,
                        results, deserialization_id_time_us, scan_scalar_time_us, search_time_us);
  if (!status.ok()) {
    return status;
  }

  if (ctx->parameter.with_scalar_data()) {
    // Get scalar data by parameter
    std::vector<std::string> selected_scalar_keys = Helper::PbRepeatedToVector(ctx->parameter.selected_keys());
    auto status = QueryVectorScalarData(ctx->partition_id, selected_scalar_keys, results);
    if (!status.ok()) {
      return status;
    }
  }

  if (ctx->parameter.with_table_data()) {
    // Get table data by parameter
    auto status = QueryVectorTableData(ctx->partition_id, results);
    if (!status.ok()) {
      return status;
    }
  }

  return butil::Status();
}

butil::Status VectorReader::SearchVectorDebug(
    uint64_t partition_id, std::shared_ptr<VectorIndex> vector_index, pb::common::Range region_range,
    const std::vector<pb::common::VectorWithId>& vector_with_ids, const pb::common::VectorSearchParameter& parameter,
    std::vector<pb::index::VectorWithDistanceResult>& vector_with_distance_results, int64_t& deserialization_id_time_us,
    int64_t& scan_scalar_time_us, int64_t& search_time_us) {
  if (vector_with_ids.empty()) {
    DINGO_LOG(WARNING) << "Empty vector with ids";
    return butil::Status();
  }

  auto vector_filter = parameter.vector_filter();
  auto vector_filter_type = parameter.vector_filter_type();

  bool with_vector_data = !(parameter.without_vector_data());
  std::vector<pb::index::VectorWithDistanceResult> tmp_results;

  uint64_t min_vector_id = VectorCodec::DecodeVectorId(region_range.start_key());
  uint64_t max_vector_id = VectorCodec::DecodeVectorId(region_range.end_key());
  DINGO_LOG(INFO) << fmt::format("vector id range [{}-{})", min_vector_id, max_vector_id);

  std::vector<std::shared_ptr<VectorIndex::FilterFunctor>> filters;
  if (vector_index->VectorIndexType() == pb::common::VECTOR_INDEX_TYPE_HNSW) {
    filters.push_back(std::make_shared<VectorIndex::RangeFilterFunctor>(min_vector_id, max_vector_id));
  } else if (vector_index->VectorIndexType() == pb::common::VECTOR_INDEX_TYPE_FLAT) {
    filters.push_back(std::make_shared<VectorIndex::FlatRangeFilterFunctor>(min_vector_id, max_vector_id));
  }

  auto lambda_time_now_function = []() { return std::chrono::steady_clock::now(); };
  auto lambda_time_diff_microseconds_function = [](auto start, auto end) {
    return std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
  };

  // scalar post filter
  if (dingodb::pb::common::VectorFilter::SCALAR_FILTER == vector_filter &&
      dingodb::pb::common::VectorFilterType::QUERY_POST == vector_filter_type) {
    uint32_t top_n = parameter.top_n();
    if (BAIDU_UNLIKELY(vector_with_ids[0].scalar_data().scalar_data_size() == 0)) {
      butil::Status status =
          vector_index->Search(vector_with_ids, top_n, filters, vector_with_distance_results, with_vector_data);
      if (!status.ok()) {
        DINGO_LOG(ERROR) << fmt::format("vector_index::Search failed ");
        return status;
      }
    } else {
      top_n *= 10;
      auto start = lambda_time_now_function();
      butil::Status status = vector_index->Search(vector_with_ids, top_n, filters, tmp_results, with_vector_data);
      auto end = lambda_time_now_function();
      search_time_us = lambda_time_diff_microseconds_function(start, end);
      if (!status.ok()) {
        DINGO_LOG(ERROR) << fmt::format("vector_index::Search failed ");
        return status;
      }
      auto start_kv_get = lambda_time_now_function();
      for (auto& vector_with_distance_result : tmp_results) {
        pb::index::VectorWithDistanceResult new_vector_with_distance_result;

        for (auto& temp_vector_with_distance : *vector_with_distance_result.mutable_vector_with_distances()) {
          uint64_t temp_id = temp_vector_with_distance.vector_with_id().id();
          bool compare_result = false;
          butil::Status status =
              CompareVectorScalarData(partition_id, temp_id, vector_with_ids[0].scalar_data(), compare_result);
          if (!status.ok()) {
            return status;
          }
          if (!compare_result) {
            continue;
          }

          new_vector_with_distance_result.add_vector_with_distances()->Swap(&temp_vector_with_distance);
          if (new_vector_with_distance_result.vector_with_distances_size() >= parameter.top_n()) {
            break;
          }
        }
        vector_with_distance_results.emplace_back(std::move(new_vector_with_distance_result));
      }
      auto end_kv_get = lambda_time_now_function();
      scan_scalar_time_us = lambda_time_diff_microseconds_function(start_kv_get, end_kv_get);
    }
  } else if (dingodb::pb::common::VectorFilter::VECTOR_ID_FILTER == vector_filter) {  // vector id array search
    butil::Status status = DoVectorSearchForVectorIdPreFilterDebug(vector_index, vector_with_ids, parameter, filters,
                                                                   vector_with_distance_results,
                                                                   deserialization_id_time_us, search_time_us);
    if (!status.ok()) {
      DINGO_LOG(ERROR) << fmt::format("DoVectorSearchForVectorIdPreFilterDebug failed");
      return status;
    }

  } else if (dingodb::pb::common::VectorFilter::SCALAR_FILTER == vector_filter &&
             dingodb::pb::common::VectorFilterType::QUERY_PRE == vector_filter_type) {  // scalar pre filter search

    butil::Status status =
        DoVectorSearchForScalarPreFilterDebug(vector_index, region_range, vector_with_ids, parameter, filters,
                                              vector_with_distance_results, scan_scalar_time_us, search_time_us);
    if (!status.ok()) {
      DINGO_LOG(ERROR) << fmt::format("DoVectorSearchForScalarPreFilterDebug failed ");
      return status;
    }

  } else if (dingodb::pb::common::VectorFilter::TABLE_FILTER ==
             vector_filter) {  //  table coprocessor pre filter search. not impl
    butil::Status status = DoVectorSearchForTableCoprocessor(vector_index, partition_id, vector_with_ids, parameter,
                                                             vector_with_distance_results);
    if (!status.ok()) {
      DINGO_LOG(ERROR) << fmt::format("DoVectorSearchForTableCoprocessor failed ");
      return status;
    }
  }

  // if vector index does not support restruct vector ,we restruct it using RocksDB
  if (with_vector_data) {
    for (auto& result : vector_with_distance_results) {
      for (auto& vector_with_distance : *result.mutable_vector_with_distances()) {
        if (vector_with_distance.vector_with_id().vector().float_values_size() > 0 ||
            vector_with_distance.vector_with_id().vector().binary_values_size() > 0) {
          continue;
        }

        pb::common::VectorWithId vector_with_id;
        auto status = QueryVectorWithId(partition_id, vector_with_distance.vector_with_id().id(), true, vector_with_id);
        if (!status.ok()) {
          return status;
        }
        vector_with_distance.mutable_vector_with_id()->Swap(&vector_with_id);
      }
    }
  }

  return butil::Status();
}

butil::Status VectorReader::DoVectorSearchForVectorIdPreFilterDebug(
    std::shared_ptr<VectorIndex> vector_index, const std::vector<pb::common::VectorWithId>& vector_with_ids,
    const pb::common::VectorSearchParameter& parameter,
    std::vector<std::shared_ptr<VectorIndex::FilterFunctor>> filters,
    std::vector<pb::index::VectorWithDistanceResult>& vector_with_distance_results, int64_t& deserialization_id_time_us,
    int64_t& search_time_us) {
  auto lambda_time_now_function = []() { return std::chrono::steady_clock::now(); };
  auto lambda_time_diff_microseconds_function = [](auto start, auto end) {
    return std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
  };

  auto start_ids = lambda_time_now_function();
  if (vector_index->VectorIndexType() == pb::common::VECTOR_INDEX_TYPE_HNSW) {
    filters.push_back(
        std::make_shared<VectorIndex::HnswListFilterFunctor>(Helper::PbRepeatedToVector(parameter.vector_ids())));
  } else if (vector_index->VectorIndexType() == pb::common::VECTOR_INDEX_TYPE_FLAT) {
    filters.push_back(
        std::make_shared<VectorIndex::FlatListFilterFunctor>(Helper::PbRepeatedToVector(parameter.vector_ids())));
  }
  auto end_ids = lambda_time_now_function();
  deserialization_id_time_us = lambda_time_diff_microseconds_function(start_ids, end_ids);

  auto start_search = lambda_time_now_function();
  butil::Status status = vector_index->Search(vector_with_ids, parameter.top_n(), filters, vector_with_distance_results,
                                              !(parameter.without_vector_data()));
  auto end_search = lambda_time_now_function();
  search_time_us = lambda_time_diff_microseconds_function(start_search, end_search);
  if (!status.ok()) {
    std::string s = fmt::format("DoVectorSearchForVectorIdPreFilter::VectorIndex::Search failed");
    DINGO_LOG(ERROR) << s;
    return status;
  }

  return butil::Status::OK();
}

butil::Status VectorReader::DoVectorSearchForScalarPreFilterDebug(
    std::shared_ptr<VectorIndex> vector_index, pb::common::Range region_range,
    const std::vector<pb::common::VectorWithId>& vector_with_ids, const pb::common::VectorSearchParameter& parameter,
    std::vector<std::shared_ptr<VectorIndex::FilterFunctor>> filters,
    std::vector<pb::index::VectorWithDistanceResult>& vector_with_distance_results, int64_t& scan_scalar_time_us,
    int64_t& search_time_us) {
  // scalar pre filter search

  const auto& std_vector_scalar = vector_with_ids[0].scalar_data();
  auto lambda_scalar_compare_function =
      [&std_vector_scalar](const pb::common::VectorScalardata& internal_vector_scalar) {
        for (const auto& [key, value] : std_vector_scalar.scalar_data()) {
          auto it = internal_vector_scalar.scalar_data().find(key);
          if (it == internal_vector_scalar.scalar_data().end()) {
            return false;
          }

          bool compare_result = Helper::IsEqualVectorScalarValue(value, it->second);
          if (!compare_result) {
            return false;
          }
        }
        return true;
      };

  std::string start_key = VectorCodec::FillVectorScalarPrefix(region_range.start_key());
  std::string end_key = VectorCodec::FillVectorScalarPrefix(region_range.end_key());

  IteratorOptions options;
  options.upper_bound = end_key;

  auto lambda_time_now_function = []() { return std::chrono::steady_clock::now(); };
  auto lambda_time_diff_microseconds_function = [](auto start, auto end) {
    return std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
  };

  auto start_iter = lambda_time_now_function();
  auto iter = reader_->NewIterator(options);
  if (iter == nullptr) {
    DINGO_LOG(ERROR) << fmt::format("New iterator failed, region range [{}-{})",
                                    Helper::StringToHex(region_range.start_key()),
                                    Helper::StringToHex(region_range.end_key()));
    return butil::Status(pb::error::Errno::EINTERNAL, "New iterator failed");
  }

  std::vector<uint64_t> vector_ids;
  vector_ids.reserve(1024);
  for (iter->Seek(start_key); iter->Valid(); iter->Next()) {
    pb::common::VectorScalardata internal_vector_scalar;
    if (!internal_vector_scalar.ParseFromString(std::string(iter->Value()))) {
      return butil::Status(pb::error::EINTERNAL, "Internal error, decode VectorScalar failed");
    }

    bool compare_result = lambda_scalar_compare_function(internal_vector_scalar);
    if (compare_result) {
      std::string key(iter->Key());
      uint64_t internal_vector_id = VectorCodec::DecodeVectorId(key);
      if (0 == internal_vector_id) {
        std::string s = fmt::format("VectorCodec::DecodeVectorId failed key : {}", Helper::StringToHex(key));
        DINGO_LOG(ERROR) << s;
        return butil::Status(pb::error::Errno::EVECTOR_NOT_SUPPORT, s);
      }
      vector_ids.push_back(internal_vector_id);
    }
  }
  auto end_iter = lambda_time_now_function();
  scan_scalar_time_us = lambda_time_diff_microseconds_function(start_iter, end_iter);

  if (vector_index->VectorIndexType() == pb::common::VECTOR_INDEX_TYPE_HNSW) {
    filters.push_back(std::make_shared<VectorIndex::HnswListFilterFunctor>(vector_ids));
  } else if (vector_index->VectorIndexType() == pb::common::VECTOR_INDEX_TYPE_FLAT) {
    filters.push_back(std::make_shared<VectorIndex::FlatListFilterFunctor>(std::move(vector_ids)));
  }

  auto start_search = lambda_time_now_function();
  butil::Status status = vector_index->Search(vector_with_ids, parameter.top_n(), filters, vector_with_distance_results,
                                              !(parameter.without_vector_data()));
  auto end_search = lambda_time_now_function();
  search_time_us = lambda_time_diff_microseconds_function(start_search, end_search);
  if (!status.ok()) {
    std::string s = fmt::format("DoVectorSearchForScalarPreFilter::VectorIndex::Search failed");
    DINGO_LOG(ERROR) << s;
    return status;
  }

  return butil::Status::OK();
}

}  // namespace dingodb