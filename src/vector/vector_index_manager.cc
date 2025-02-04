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

#include "vector/vector_index_manager.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "bthread/bthread.h"
#include "butil/binary_printer.h"
#include "butil/status.h"
#include "common/helper.h"
#include "common/logging.h"
#include "common/synchronization.h"
#include "config/config_manager.h"
#include "fmt/core.h"
#include "log/segment_log_storage.h"
#include "meta/store_meta_manager.h"
#include "proto/common.pb.h"
#include "proto/error.pb.h"
#include "proto/file_service.pb.h"
#include "proto/node.pb.h"
#include "proto/raft.pb.h"
#include "server/file_service.h"
#include "server/server.h"
#include "vector/codec.h"
#include "vector/vector_index.h"
#include "vector/vector_index_factory.h"
#include "vector/vector_index_snapshot.h"

namespace dingodb {

static std::string GenApplyLogIdKey(uint64_t vector_index_id) {
  return fmt::format("{}_{}", Constant::kVectorIndexApplyLogIdPrefix, vector_index_id);
}

static std::string GenSnapshotLogIdKey(uint64_t vector_index_id) {
  return fmt::format("{}_{}", Constant::kVectorIndexSnapshotLogIdPrefix, vector_index_id);
}

bool VectorIndexManager::Init(std::vector<store::RegionPtr> regions) {
  // Init vector index snapshot
  if (!vector_index_snapshot_manager_->Init(regions)) {
    return false;
  }

  // Load vector index.
  auto status = ParallelLoadOrBuildVectorIndex(regions, Constant::kLoadOrBuildVectorIndexConcurrency);
  if (!status.ok()) {
    DINGO_LOG(ERROR) << "Parallel load or build vector index failed, error: " << status.error_str();
    return false;
  }

  return true;
}

bool VectorIndexManager::AddVectorIndex(std::shared_ptr<VectorIndex> vector_index, bool force) {
  if (force) {
    return vector_indexs_.Put(vector_index->Id(), vector_index) > 0;
  }
  return vector_indexs_.PutIfExists(vector_index->Id(), vector_index) > 0;
}

bool VectorIndexManager::AddVectorIndex(uint64_t vector_index_id, const pb::common::IndexParameter& index_parameter) {
  auto vector_index = VectorIndexFactory::New(vector_index_id, index_parameter);
  if (vector_index == nullptr) {
    DINGO_LOG(ERROR) << fmt::format("New vector index failed, vector index id: {} parameter: {}", vector_index_id,
                                    index_parameter.ShortDebugString());
    return false;
  }

  auto ret = AddVectorIndex(vector_index);
  if (!ret) {
    DINGO_LOG(ERROR) << fmt::format("Add region {} vector index failed", vector_index_id);
    return false;
  }

  // Update vector index status NORMAL
  vector_index->SetStatus(pb::common::VECTOR_INDEX_STATUS_NORMAL);

  DINGO_LOG(INFO) << fmt::format("Add region {} vector index success", vector_index_id);

  return true;
}

// Deletes the vector index for the specified region ID.
// @param vector_index_id The ID of the region whose vector index is to be deleted.
void VectorIndexManager::DeleteVectorIndex(uint64_t vector_index_id) {
  // Log the deletion of the vector index.
  DINGO_LOG(INFO) << fmt::format("Delete region's vector index {}", vector_index_id);

  auto vector_index = GetVectorIndex(vector_index_id);
  if (vector_index != nullptr) {
    // Remove the vector index from the vector index map.
    vector_indexs_.Erase(vector_index_id);
    // Set vector index state to delete
    vector_index->SetStatus(pb::common::VECTOR_INDEX_STATUS_DELETE);

    // Delete the vector index metadata from the metadata store.
    meta_writer_->Delete(GenApplyLogIdKey(vector_index_id));
    meta_writer_->Delete(GenSnapshotLogIdKey(vector_index_id));
  }
}

std::shared_ptr<VectorIndex> VectorIndexManager::GetVectorIndex(uint64_t vector_index_id) {
  return vector_indexs_.Get(vector_index_id);
}

std::shared_ptr<VectorIndex> VectorIndexManager::GetVectorIndex(store::RegionPtr region) {
  auto vector_index = region->ShareVectorIndex();
  if (vector_index != nullptr) {
    DINGO_LOG(INFO) << "get share vector index: " << region->Id();
    return vector_index;
  }

  return vector_indexs_.Get(region->Id());
}

std::vector<std::shared_ptr<VectorIndex>> VectorIndexManager::GetAllVectorIndex() {
  std::vector<std::shared_ptr<VectorIndex>> vector_indexs;
  if (vector_indexs_.GetAllValues(vector_indexs) < 0) {
    DINGO_LOG(ERROR) << "Get all vector index failed";
  }
  return vector_indexs;
}

butil::Status VectorIndexManager::LoadOrBuildVectorIndex(uint64_t region_id) {
  auto store_region_meta = Server::GetInstance()->GetStoreMetaManager()->GetStoreRegionMeta();
  auto region = store_region_meta->GetRegion(region_id);
  if (region == nullptr) {
    return butil::Status(pb::error::EREGION_NOT_FOUND, "Not found region %lu", region_id);
  }

  return LoadOrBuildVectorIndex(region);
}

// Load vector index for already exist vector index at bootstrap.
// Priority load from snapshot, if snapshot not exist then load from rocksdb.
butil::Status VectorIndexManager::LoadOrBuildVectorIndex(store::RegionPtr region) {
  assert(region != nullptr);
  uint64_t vector_index_id = region->Id();

  auto online_vector_index = GetVectorIndex(vector_index_id);
  auto update_online_vector_index_status = [online_vector_index](pb::common::RegionVectorIndexStatus status) {
    if (online_vector_index != nullptr) {
      online_vector_index->SetStatus(status);
    }
  };

  // Update vector index status LOADING
  update_online_vector_index_status(pb::common::VECTOR_INDEX_STATUS_LOADING);

  // try to LoadVectorIndexFromSnapshot
  auto new_vector_index = VectorIndexSnapshotManager::LoadVectorIndexSnapshot(region);
  if (new_vector_index != nullptr) {
    // replay wal
    DINGO_LOG(INFO) << fmt::format(
        "[vector_index.load][index_id({})] Load vector index from snapshot success, will ReplayWal", vector_index_id);
    auto status = ReplayWalToVectorIndex(new_vector_index, new_vector_index->ApplyLogIndex() + 1, UINT64_MAX);
    if (status.ok()) {
      DINGO_LOG(INFO) << fmt::format("[vector_index.load][index_id({})] ReplayWal success, log_id {}", vector_index_id,
                                     new_vector_index->ApplyLogIndex());
      new_vector_index->SetStatus(pb::common::VECTOR_INDEX_STATUS_NORMAL);
      // set vector index to vector index map
      AddVectorIndex(new_vector_index);

      // Update vector index status NORMAL
      update_online_vector_index_status(pb::common::VECTOR_INDEX_STATUS_NORMAL);

      return status;
    }
  }

  DINGO_LOG(INFO) << fmt::format(
      "[vector_index.load][index_id({})] Load vector index from snapshot failed, will build vector_index",
      vector_index_id);

  // build a new vector_index from rocksdb
  new_vector_index = BuildVectorIndex(region);
  if (new_vector_index == nullptr) {
    DINGO_LOG(WARNING) << fmt::format("[vector_index.build][index_id({})] Build vector index failed", vector_index_id);
    // Update vector index status NORMAL
    update_online_vector_index_status(pb::common::VECTOR_INDEX_STATUS_NORMAL);

    return butil::Status(pb::error::Errno::EINTERNAL, "Build vector index failed, vector index id %lu",
                         vector_index_id);
  }

  // add vector index to vector index map
  new_vector_index->SetStatus(pb::common::VECTOR_INDEX_STATUS_NORMAL);
  AddVectorIndex(new_vector_index);

  // Update vector index status NORMAL
  update_online_vector_index_status(pb::common::VECTOR_INDEX_STATUS_NORMAL);

  DINGO_LOG(INFO) << fmt::format("[vector_index.load][index_id({})] Build vector index success.", vector_index_id);

  return butil::Status();
}

butil::Status VectorIndexManager::ParallelLoadOrBuildVectorIndex(std::vector<store::RegionPtr> regions,
                                                                 int concurrency) {
  struct Parameter {
    VectorIndexManager* vector_index_manager;
    std::vector<store::RegionPtr> regions;
    std::atomic<int> offset;
    std::vector<int> results;
  };

  Parameter* param = new Parameter();
  param->vector_index_manager = this;
  param->regions = regions;
  param->offset = 0;
  param->results.resize(regions.size(), 0);

  auto task = [](void* arg) -> void* {
    if (arg == nullptr) {
      return nullptr;
    }
    auto* param = static_cast<Parameter*>(arg);

    for (;;) {
      int offset = param->offset.fetch_add(0, std::memory_order_relaxed);
      if (offset >= param->regions.size()) {
        break;
      }

      auto region = param->regions[offset];

      uint64_t vector_index_id = region->Id();
      LOG(INFO) << fmt::format("Init load region {} vector index", vector_index_id);

      // When raft leader start, may load vector index,
      // so check vector index wherther exist, if exist then don't load vector index.
      auto vector_index = param->vector_index_manager->GetVectorIndex(vector_index_id);
      if (vector_index == nullptr) {
        continue;
      }
      auto status = param->vector_index_manager->LoadOrBuildVectorIndex(region);
      if (!status.ok()) {
        LOG(ERROR) << fmt::format("Load region {} vector index failed, ", vector_index_id);
        param->results[offset] = -1;
        break;
      }
    }

    return nullptr;
  };

  if (!Helper::ParallelRunTask(task, param, concurrency)) {
    return butil::Status(pb::error::EINTERNAL, "Create bthread failed.");
  }

  for (auto result : param->results) {
    if (result == -1) {
      return butil::Status(pb::error::EINTERNAL, "Load or build vector index failed.");
    }
  }

  return butil::Status();
}

// Replay vector index from wal
butil::Status VectorIndexManager::ReplayWalToVectorIndex(std::shared_ptr<VectorIndex> vector_index,
                                                         uint64_t start_log_id, uint64_t end_log_id) {
  assert(vector_index != nullptr);
  DINGO_LOG(INFO) << fmt::format("Replay vector index {} from log id {} to log id {}", vector_index->Id(), start_log_id,
                                 end_log_id);

  uint64_t start_time = Helper::TimestampMs();
  auto engine = Server::GetInstance()->GetEngine();
  if (engine->GetID() != pb::common::ENG_RAFT_STORE) {
    return butil::Status(pb::error::Errno::EINTERNAL, "Engine is not raft store.");
  }
  auto raft_kv_engine = std::dynamic_pointer_cast<RaftStoreEngine>(engine);
  auto node = raft_kv_engine->GetNode(vector_index->Id());
  if (node == nullptr) {
    return butil::Status(pb::error::Errno::ERAFT_NOT_FOUND, fmt::format("Not found node {}", vector_index->Id()));
  }

  auto log_stroage = Server::GetInstance()->GetLogStorageManager()->GetLogStorage(vector_index->Id());
  if (log_stroage == nullptr) {
    return butil::Status(pb::error::Errno::EINTERNAL, fmt::format("Not found log stroage {}", vector_index->Id()));
  }

  std::vector<pb::common::VectorWithId> vectors;
  vectors.reserve(10000);
  uint64_t last_log_id = vector_index->ApplyLogIndex();
  auto log_entrys = log_stroage->GetEntrys(start_log_id, end_log_id);
  for (const auto& log_entry : log_entrys) {
    auto raft_cmd = std::make_shared<pb::raft::RaftCmdRequest>();
    butil::IOBufAsZeroCopyInputStream wrapper(log_entry->data);
    CHECK(raft_cmd->ParseFromZeroCopyStream(&wrapper));
    for (auto& request : *raft_cmd->mutable_requests()) {
      switch (request.cmd_type()) {
        case pb::raft::VECTOR_ADD: {
          for (auto& vector : *request.mutable_vector_add()->mutable_vectors()) {
            vectors.push_back(vector);
          }

          if (vectors.size() == 10000) {
            vector_index->Upsert(vectors);
            vectors.resize(0);
          }
          break;
        }
        case pb::raft::VECTOR_DELETE: {
          if (!vectors.empty()) {
            vector_index->Upsert(vectors);
            vectors.resize(0);
          }
          std::vector<uint64_t> ids;
          for (auto vector_id : request.vector_delete().ids()) {
            ids.push_back(vector_id);
          }
          vector_index->Delete(ids);
          break;
        }
        default:
          break;
      }
    }

    last_log_id = log_entry->index;
  }
  if (!vectors.empty()) {
    vector_index->Upsert(vectors);
  }

  vector_index->SetApplyLogIndex(last_log_id);

  DINGO_LOG(INFO) << fmt::format(
      "Replay vector index {} from log id {} to log id {} finish, last_log_id {} elapsed time {}ms", vector_index->Id(),
      start_log_id, end_log_id, last_log_id, Helper::TimestampMs() - start_time);

  return butil::Status();
}

// Build vector index with original all data(store rocksdb).
std::shared_ptr<VectorIndex> VectorIndexManager::BuildVectorIndex(store::RegionPtr region) {
  assert(region != nullptr);
  uint64_t vector_index_id = region->Id();

  auto vector_index = VectorIndexFactory::New(vector_index_id, region->InnerRegion().definition().index_parameter());
  if (!vector_index) {
    DINGO_LOG(WARNING) << fmt::format("[vector_index.build][index_id({})] New vector index failed.", vector_index_id);
    return nullptr;
  }

  uint64_t apply_log_id = 0;
  auto status = LoadApplyLogId(vector_index_id, apply_log_id);
  if (!status.ok()) {
    return nullptr;
  }
  vector_index->SetApplyLogIndex(apply_log_id);

  uint64_t snapshot_log_id = 0;
  status = LoadSnapshotLogId(vector_index_id, snapshot_log_id);
  if (!status.ok()) {
    return nullptr;
  }

  vector_index->SetSnapshotLogIndex(snapshot_log_id);

  std::string start_key = VectorCodec::FillVectorDataPrefix(region->RawRange().start_key());
  std::string end_key = VectorCodec::FillVectorDataPrefix(region->RawRange().end_key());
  DINGO_LOG(INFO) << fmt::format(
      "[vector_index.build][index_id({})] Build vector index, snapshot_log_id({}) apply_log_id({}) range: [{}-{})",
      vector_index_id, snapshot_log_id, apply_log_id, Helper::StringToHex(start_key), Helper::StringToHex(end_key));

  uint64_t start_time = Helper::TimestampMs();
  // load vector data to vector index
  IteratorOptions options;
  options.upper_bound = end_key;

  auto iter = raw_engine_->NewIterator(Constant::kStoreDataCF, options);
  uint64_t count = 0;
  std::vector<pb::common::VectorWithId> vectors;
  vectors.reserve(Constant::kBuildVectorIndexBatchSize);
  for (iter->Seek(start_key); iter->Valid(); iter->Next()) {
    pb::common::VectorWithId vector;

    std::string key(iter->Key());
    vector.set_id(VectorCodec::DecodeVectorId(key));

    std::string value(iter->Value());
    if (!vector.mutable_vector()->ParseFromString(value)) {
      DINGO_LOG(WARNING) << fmt::format("[vector_index.build][index_id({})] vector with id ParseFromString failed.");
      continue;
    }

    if (vector.vector().float_values_size() <= 0) {
      DINGO_LOG(WARNING) << fmt::format("[vector_index.build][index_id({})] vector values_size error.", vector.id());
      continue;
    }

    ++count;

    vectors.push_back(vector);
    if (count + 1 % Constant::kBuildVectorIndexBatchSize == 0) {
      vector_index->Upsert(vectors);
      vectors.clear();
    }
  }

  if (!vectors.empty()) {
    vector_index->Upsert(vectors);
  }

  DINGO_LOG(INFO) << fmt::format(
      "[vector_index.build][index_id({})] Build vector index finish, snapshot_log_index({}) apply_log_index({}) "
      "count({}) elapsed time({}ms)",
      vector_index_id, snapshot_log_id, apply_log_id, count, Helper::TimestampMs() - start_time);

  return vector_index;
}

butil::Status VectorIndexManager::AsyncRebuildVectorIndex(store::RegionPtr region, bool need_save) {
  struct Parameter {
    VectorIndexManager* vector_index_manager;
    store::RegionPtr region;
    bool need_save;
  };

  DINGO_LOG(INFO) << fmt::format("[vector_index.rebuild][index_id({})] Async rebuild vector index.", region->Id());

  Parameter* param = new Parameter();
  param->vector_index_manager = this;
  param->region = region;
  param->need_save = need_save;

  bthread_t tid;
  int ret = bthread_start_background(
      &tid, nullptr,
      [](void* arg) -> void* {
        if (arg == nullptr) {
          return nullptr;
        }

        auto* param = static_cast<Parameter*>(arg);

        // Wait vector index state ready.
        for (;;) {
          auto vector_index = param->vector_index_manager->GetVectorIndex(param->region->Id());
          if (vector_index == nullptr) {
            break;
          }
          if (vector_index->Status() == pb::common::VECTOR_INDEX_STATUS_REBUILDING ||
              vector_index->Status() == pb::common::VECTOR_INDEX_STATUS_SNAPSHOTTING ||
              vector_index->Status() == pb::common::VECTOR_INDEX_STATUS_BUILDING ||
              vector_index->Status() == pb::common::VECTOR_INDEX_STATUS_REPLAYING) {
            LOG(INFO) << fmt::format("[vector_index.rebuild][index_id({})] Waiting rebuild vector index.",
                                     param->region->Id());
            bthread_usleep(2 * 1000 * 1000);
          } else {
            LOG(INFO) << fmt::format("[vector_index.rebuild][index_id({})] Vector index status is ok, start rebuild.",
                                     param->region->Id());
            break;
          }
        }

        auto status = param->vector_index_manager->RebuildVectorIndex(param->region, param->need_save);
        if (!status.ok()) {
          LOG(ERROR) << fmt::format("[vector_index.rebuild][index_id({})] Rebuild vector index failed, error: {}",
                                    param->region->Id(), status.error_str());
        }

        auto config = Server::GetInstance()->GetConfig();
        if (config == nullptr) {
          return nullptr;
        }

        if (!config->GetBool("vector.enable_follower_hold_index")) {
          // If follower, delete vector index.
          auto engine = Server::GetInstance()->GetEngine();
          if (engine->GetID() == pb::common::ENG_RAFT_STORE) {
            auto raft_kv_engine = std::dynamic_pointer_cast<RaftStoreEngine>(engine);
            auto node = raft_kv_engine->GetNode(param->region->Id());
            if (node == nullptr) {
              LOG(ERROR) << fmt::format("No found raft node {}.", param->region->Id());
            }

            if (!node->IsLeader()) {
              param->vector_index_manager->DeleteVectorIndex(param->region->Id());
            }
          }
        }

        return nullptr;
      },
      param);
  if (ret != 0) {
    DINGO_LOG(ERROR) << fmt::format("[vector_index.rebuild][index_id({})] Create bthread failed, ret: {}", region->Id(),
                                    ret);
  }

  return butil::Status();
}

static butil::Status CheckRebuildStatus(std::shared_ptr<VectorIndex> vector_index) {
  if (vector_index == nullptr) {
    return butil::Status::OK();
  }

  if (vector_index->Status() != pb::common::VECTOR_INDEX_STATUS_NORMAL &&
      vector_index->Status() != pb::common::VECTOR_INDEX_STATUS_ERROR &&
      vector_index->Status() != pb::common::VECTOR_INDEX_STATUS_NONE) {
    std::string msg = fmt::format(
        "online_vector_index status is not normal/error/none, cannot do rebuild, vector index id {}, status {}",
        vector_index->Id(), pb::common::RegionVectorIndexStatus_Name(vector_index->Status()));
    DINGO_LOG(WARNING) << msg;
    return butil::Status(pb::error::Errno::EINTERNAL, msg);
  }

  return butil::Status::OK();
}

// Rebuild vector index
butil::Status VectorIndexManager::RebuildVectorIndex(store::RegionPtr region, bool need_save) {
  assert(region != nullptr);
  uint64_t vector_index_id = region->Id();

  DINGO_LOG(INFO) << fmt::format("[vector_index.rebuild][index_id({})] Start rebuild vector index.", vector_index_id);

  // check rebuild status
  auto online_vector_index = GetVectorIndex(vector_index_id);
  auto status = CheckRebuildStatus(online_vector_index);
  if (!status.ok()) {
    return status;
  }

  // update vector index status rebuilding
  if (online_vector_index != nullptr) {
    online_vector_index->SetStatus(pb::common::VECTOR_INDEX_STATUS_REBUILDING);
  }

  uint64_t start_time = Helper::TimestampMs();
  // Build vector index with all original data.
  auto vector_index = BuildVectorIndex(region);
  if (vector_index == nullptr) {
    DINGO_LOG(WARNING) << fmt::format("[vector_index.rebuild][index_id({})] Build vector index failed.",
                                      vector_index_id);
    return butil::Status(pb::error::Errno::EINTERNAL, "Build vector index failed");
  }
  if (online_vector_index != nullptr) {
    vector_index->SetVersion(online_vector_index->Version() + 1);
  }

  DINGO_LOG(INFO) << fmt::format(
      "[vector_index.rebuild][index_id({})] Build vector index success, log_id {} elapsed time: {}ms", vector_index_id,
      vector_index->ApplyLogIndex(), Helper::TimestampMs() - start_time);

  // we want to eliminate the impact of the blocking during replay wal in catch-up round
  // so save is done before replay wal first-round
  if (need_save) {
    start_time = Helper::TimestampMs();
    auto status = SaveVectorIndex(vector_index);
    if (!status.ok()) {
      DINGO_LOG(WARNING) << fmt::format("[vector_index.rebuild][index_id({})] Save vector index failed, message: {}",
                                        vector_index_id, status.error_str());
      return butil::Status(pb::error::Errno::EINTERNAL, "Save vector index failed");
    }

    DINGO_LOG(INFO) << fmt::format(
        "[vector_index.rebuild][index_id({})] Save vector index snapshot success, snapshot_log_id {} elapsed time: "
        "{}ms",
        vector_index_id, vector_index->SnapshotLogIndex(), Helper::TimestampMs() - start_time);
  }

  start_time = Helper::TimestampMs();
  // first ground replay wal
  status = ReplayWalToVectorIndex(vector_index, vector_index->ApplyLogIndex() + 1, UINT64_MAX);
  if (!status.ok()) {
    DINGO_LOG(ERROR) << fmt::format("[vector_index.rebuild][index_id({})] ReplayWal failed first-round, log_id {}",
                                    vector_index_id, vector_index->ApplyLogIndex());
    return butil::Status(pb::error::Errno::EINTERNAL, "ReplayWal failed first-round");
  }

  DINGO_LOG(INFO) << fmt::format(
      "[vector_index.rebuild][index_id({})] ReplayWal success first-round, log_id {} elapsed time: {}ms",
      vector_index_id, vector_index->ApplyLogIndex(), Helper::TimestampMs() - start_time);

  // set online_vector_index to offline, so it will reject all vector add/del, raft handler will usleep and try to
  // switch to new vector_index to add/del
  region->SetIsSwitchingVectorIndex(true);

  {
    ON_SCOPE_EXIT([&]() { region->SetIsSwitchingVectorIndex(false); });

    start_time = Helper::TimestampMs();
    // second ground replay wal
    status = ReplayWalToVectorIndex(vector_index, vector_index->ApplyLogIndex() + 1, UINT64_MAX);
    if (!status.ok()) {
      DINGO_LOG(ERROR) << fmt::format("[vector_index.rebuild][index_id({})] ReplayWal failed catch-up round, log_id {}",
                                      vector_index_id, vector_index->ApplyLogIndex());
      return status;
    }
    // set the new vector_index's status to NORMAL
    vector_index->SetStatus(pb::common::VECTOR_INDEX_STATUS_NORMAL);

    DINGO_LOG(INFO) << fmt::format(
        "[vector_index.rebuild][index_id({})] ReplayWal success catch-up round, log_id {} elapsed time: {}ms",
        vector_index_id, vector_index->ApplyLogIndex(), Helper::TimestampMs() - start_time);

    // set vector index to vector index map
    bool ret = AddVectorIndex(vector_index, true);
    if (!ret) {
      DINGO_LOG(ERROR) << fmt::format(
          "[vector_index.rebuild][index_id({})] ReplayWal catch-up round finish, but online_vector_index maybe delete "
          "by others, so stop to update "
          "vector_indexes map, log_id {}",
          vector_index_id, vector_index->ApplyLogIndex());
      return butil::Status(pb::error::Errno::EINTERNAL,
                           "ReplayWal catch-up round finish, but online_vector_index "
                           "maybe delete by others, so stop to update vector_indexes map");
    }
  }

  DINGO_LOG(INFO) << fmt::format("[vector_index.rebuild][index_id({})] Rebuild vector index success", vector_index_id);

  // Reset region share vector index id.
  region->SetShareVectorIndex(nullptr);

  return butil::Status();
}

butil::Status VectorIndexManager::SaveVectorIndex(std::shared_ptr<VectorIndex> vector_index) {
  assert(vector_index != nullptr);
  DINGO_LOG(INFO) << fmt::format("[vector_index.save][index_id({})] Save vector index.", vector_index->Id());

  // Update vector index status SNAPSHOTTING
  vector_index->SetStatus(pb::common::VECTOR_INDEX_STATUS_SNAPSHOTTING);

  uint64_t snapshot_log_index = 0;
  auto status = VectorIndexSnapshotManager::SaveVectorIndexSnapshot(vector_index, snapshot_log_index);
  if (!status.ok()) {
    DINGO_LOG(ERROR) << fmt::format(
        "[vector_index.save][index_id({})] Save vector index snapshot failed, errno: {}, errstr: {}",
        vector_index->Id(), status.error_code(), status.error_str());
    vector_index->SetStatus(pb::common::VECTOR_INDEX_STATUS_NORMAL);
    return status;
  } else {
    UpdateSnapshotLogId(vector_index, snapshot_log_index);
  }

  // update vector index status NORMAL
  vector_index->SetStatus(pb::common::VECTOR_INDEX_STATUS_NORMAL);
  DINGO_LOG(INFO) << fmt::format("[vector_index.save][index_id({})] Save vector index success.", vector_index->Id());

  // Install vector index snapshot to followers.
  status = VectorIndexSnapshotManager::InstallSnapshotToFollowers(vector_index);
  if (!status.ok()) {
    DINGO_LOG(ERROR) << fmt::format("[vector_index.save][index_id({})] Install snapshot to followers failed, error {}",
                                    vector_index->Id(), status.error_str());
  }

  return butil::Status();
}

void VectorIndexManager::SaveApplyLogId(uint64_t vector_index_id, uint64_t apply_log_id) {
  auto kv = std::make_shared<pb::common::KeyValue>();
  kv->set_key(GenApplyLogIdKey(vector_index_id));
  kv->set_value(VectorCodec::EncodeApplyLogId(apply_log_id));

  meta_writer_->Put(kv);
}

butil::Status VectorIndexManager::LoadApplyLogId(uint64_t vector_index_id, uint64_t& apply_log_id) {
  auto kv = meta_reader_->Get(GenApplyLogIdKey(vector_index_id));
  if (kv == nullptr) {
    DINGO_LOG(ERROR) << fmt::format("Get vector index apply log id failed, vector_index_id {}", vector_index_id);
    return butil::Status(pb::error::EINTERNAL, "Get vector index log id failed, vector_index_id %lu", vector_index_id);
  }

  if (kv->value().empty()) {
    return butil::Status();
  }

  auto ret = VectorCodec::DecodeApplyLogId(kv->value(), apply_log_id);
  if (ret < 0) {
    DINGO_LOG(ERROR) << fmt::format("Decode vector index apply log id failed, vector_index_id {}", vector_index_id);
    return butil::Status(pb::error::EINTERNAL, "Decode vector index log id failed, vector_index_id %lu",
                         vector_index_id);
  }

  return butil::Status();
}

void VectorIndexManager::SaveSnapshotLogId(uint64_t vector_index_id, uint64_t snapshot_log_id) {
  auto kv = std::make_shared<pb::common::KeyValue>();
  kv->set_key(GenSnapshotLogIdKey(vector_index_id));
  kv->set_value(VectorCodec::EncodeSnapshotLogId(snapshot_log_id));

  meta_writer_->Put(kv);
}

butil::Status VectorIndexManager::LoadSnapshotLogId(uint64_t vector_index_id, uint64_t& snapshot_log_id) {
  auto kv = meta_reader_->Get(GenSnapshotLogIdKey(vector_index_id));
  if (kv == nullptr) {
    DINGO_LOG(ERROR) << fmt::format("Get vector index snapshot log id failed, vector_index_id {}", vector_index_id);
    return butil::Status(pb::error::EINTERNAL, "Get vector index snapshot log id failed, vector_index_id %lu",
                         vector_index_id);
  }

  if (kv->value().empty()) {
    return butil::Status();
  }

  auto ret = VectorCodec::DecodeSnapshotLogId(kv->value(), snapshot_log_id);
  if (ret < 0) {
    DINGO_LOG(ERROR) << fmt::format("Decode vector index snapshot log id failed, vector_index_id {}", vector_index_id);
    return butil::Status(pb::error::EINTERNAL, "Decode vector index snapshot log id failed, vector_index_id %lu",
                         vector_index_id);
  }

  return butil::Status();
}

void VectorIndexManager::UpdateApplyLogId(std::shared_ptr<VectorIndex> vector_index, uint64_t log_index) {
  assert(vector_index != nullptr);

  vector_index->SetApplyLogIndex(log_index);
  SaveApplyLogId(vector_index->Id(), log_index);
}

void VectorIndexManager::UpdateApplyLogId(uint64_t vector_index_id, uint64_t log_index) {
  auto vector_index = GetVectorIndex(vector_index_id);
  if (vector_index != nullptr) {
    UpdateApplyLogId(vector_index, log_index);
  }
}

void VectorIndexManager::UpdateSnapshotLogId(std::shared_ptr<VectorIndex> vector_index, uint64_t log_index) {
  assert(vector_index != nullptr);

  vector_index->SetSnapshotLogIndex(log_index);
  SaveSnapshotLogId(vector_index->Id(), log_index);
}

void VectorIndexManager::UpdateSnapshotLogId(uint64_t vector_index_id, uint64_t log_index) {
  auto vector_index = GetVectorIndex(vector_index_id);
  if (vector_index != nullptr) {
    UpdateSnapshotLogId(vector_index, log_index);
  }
}

butil::Status VectorIndexManager::ScrubVectorIndex() {
  auto store_meta_manager = Server::GetInstance()->GetStoreMetaManager();
  if (store_meta_manager == nullptr) {
    return butil::Status(pb::error::Errno::EINTERNAL, "Get store meta manager failed");
  }

  auto regions = store_meta_manager->GetStoreRegionMeta()->GetAllAliveRegion();
  if (regions.empty()) {
    DINGO_LOG(INFO) << "No alive region, skip scrub vector index";
    return butil::Status::OK();
  }

  DINGO_LOG(INFO) << "Scrub vector index start, alive region_count is " << regions.size();

  for (const auto& region : regions) {
    uint64_t vector_index_id = region->Id();
    auto vector_index = GetVectorIndex(vector_index_id);
    if (vector_index == nullptr) {
      continue;
    }

    auto last_snapshot = vector_index_snapshot_manager_->GetLastSnapshot(vector_index->Id());
    uint64_t last_snaphsot_log_id = (last_snapshot == nullptr) ? 0 : last_snapshot->SnapshotLogId();

    auto last_save_log_behind = vector_index->ApplyLogIndex() - last_snaphsot_log_id;

    bool need_rebuild = false;
    vector_index->NeedToRebuild(need_rebuild, last_save_log_behind);

    bool need_save = false;
    vector_index->NeedToSave(need_save, last_save_log_behind);

    if (need_rebuild || need_save) {
      DINGO_LOG(INFO) << fmt::format("vector index {} need rebuild({}) and need save({})", vector_index_id,
                                     need_rebuild, need_save);
      auto status = ScrubVectorIndex(region, need_rebuild, need_save);
      if (!status.ok()) {
        DINGO_LOG(ERROR) << fmt::format("Scrub vector index failed, id {} error: {}", vector_index_id,
                                        status.error_str());
        continue;
      }
    }
  }

  return butil::Status::OK();
}

butil::Status VectorIndexManager::ScrubVectorIndex(store::RegionPtr region, bool need_rebuild, bool need_save) {
  uint64_t vector_index_id = region->Id();
  // check vector index status
  auto vector_index = GetVectorIndex(vector_index_id);
  if (vector_index == nullptr) {
    DINGO_LOG(ERROR) << fmt::format("Get vector index failed, vector index id {}", vector_index_id);
    return butil::Status(pb::error::Errno::EINTERNAL, "Get vector index failed");
  }
  auto status = vector_index->Status();
  if (status != pb::common::RegionVectorIndexStatus::VECTOR_INDEX_STATUS_NORMAL) {
    DINGO_LOG(INFO) << fmt::format("vector index status is not normal, skip to ScrubVectorIndex, vector_index_id {}",
                                   vector_index_id);
    return butil::Status::OK();
  }

  if (need_rebuild) {
    DINGO_LOG(INFO) << fmt::format("need rebuild, do rebuild vector index, vector_index_id {}", vector_index_id);
    auto status = RebuildVectorIndex(region);
    if (!status.ok()) {
      DINGO_LOG(ERROR) << fmt::format("Rebuild vector index failed, vector_index_id {} error {}", vector_index_id,
                                      status.error_str());
      return status;
    }
  } else if (need_save) {
    DINGO_LOG(INFO) << fmt::format("need save, do save vector index, vector_index_id {}", vector_index_id);
    auto status = SaveVectorIndex(vector_index);
    if (!status.ok()) {
      DINGO_LOG(ERROR) << fmt::format("Save vector index failed, vector_index_id {} error {}", vector_index_id,
                                      status.error_str());
      return status;
    }
  }

  DINGO_LOG(INFO) << fmt::format("ScrubVectorIndex success, vector_index_id {}", vector_index_id);

  return butil::Status::OK();
}

}  // namespace dingodb