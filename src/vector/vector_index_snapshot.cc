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

#include "vector/vector_index_snapshot.h"

#include <sys/wait.h>  // Add this include

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "braft/file_system_adaptor.h"
#include "butil/endpoint.h"
#include "butil/iobuf.h"
#include "butil/status.h"
#include "common/failpoint.h"
#include "common/file_reader.h"
#include "common/helper.h"
#include "common/logging.h"
#include "common/service_access.h"
#include "fmt/core.h"
#include "proto/error.pb.h"
#include "proto/file_service.pb.h"
#include "proto/node.pb.h"
#include "server/file_service.h"
#include "server/server.h"
#include "vector/vector_index_factory.h"

namespace dingodb {

namespace vector_index {

SnapshotMeta::SnapshotMeta(uint64_t vector_index_id, const std::string& path)
    : vector_index_id_(vector_index_id), path_(path) {}

SnapshotMeta::~SnapshotMeta() {
  // Delete directory
  DINGO_LOG(INFO) << "Delete vector index snapshot directory" << path_;
  Helper::RemoveAllFileOrDirectory(path_);
}

bool SnapshotMeta::Init() {
  std::filesystem::path path(path_);
  path.filename();

  uint64_t snapshot_index_id = 0;
  int match = sscanf(path.filename().c_str(), "snapshot_%020" PRId64, &snapshot_index_id);
  if (match != 1) {
    DINGO_LOG(ERROR) << fmt::format("Parse snapshot index id failed from snapshot name, {}", path_);
    return false;
  }

  snapshot_log_id_ = snapshot_index_id;

  return true;
}

std::string SnapshotMeta::MetaPath() { return fmt::format("{}/meta", path_); }

std::string SnapshotMeta::IndexDataPath() {
  return fmt::format("{}/index_{}_{}.idx", path_, vector_index_id_, snapshot_log_id_);
}

std::vector<std::string> SnapshotMeta::ListFileNames() { return Helper::TraverseDirectory(path_); }

}  // namespace vector_index

// Get all snapshot path, except tmp dir.
static std::vector<std::string> GetSnapshotPaths(std::string path) {
  auto filenames = Helper::TraverseDirectory(path);
  if (filenames.empty()) {
    return {};
  }

  std::sort(filenames.begin(), filenames.end(), std::greater<>());

  std::vector<std::string> result;
  for (const auto& filename : filenames) {
    if (filename.find("tmp") == std::string::npos) {
      result.push_back(fmt::format("{}/{}", path, filename));
    }
  }

  return result;
}

// Parse host
static butil::EndPoint ParseHost(const std::string& uri) {
  std::vector<std::string> strs;
  butil::SplitString(uri, '/', &strs);

  if (strs.size() < 4) {
    return {};
  }
  std::string host_and_port = strs[2];

  butil::EndPoint endpoint;
  butil::str2endpoint(host_and_port.c_str(), &endpoint);

  return endpoint;
}

// Parse reader id
static uint64_t ParseReaderId(const std::string& uri) {
  std::vector<std::string> strs;
  butil::SplitString(uri, '/', &strs);

  if (strs.size() < 4) {
    return 0;
  }

  std::string& reader_id_str = strs[3];

  char* end = nullptr;
  uint64_t result = std::strtoull(reader_id_str.c_str(), &end, 10);
  if ((end - reader_id_str.c_str()) + 1 <= reader_id_str.size()) {
    return 0;
  }

  return result;
}

static uint64_t ParseMetaLogId(const std::string& path) {
  std::ifstream file;
  file.open(path, std::ifstream::in);

  std::string str;
  std::getline(file, str);

  try {
    return std::strtoull(str.c_str(), nullptr, 10);
  } catch (const std::invalid_argument& e) {
    DINGO_LOG(ERROR) << " path: " << path << ", Invalid argument: " << e.what();
  } catch (const std::out_of_range& e) {
    DINGO_LOG(ERROR) << " path: " << path << ", Out of range: " << e.what();
  } catch (...) {
    DINGO_LOG(ERROR) << " path: " << path << ", Unknown error";
  }

  return 0;
}

bool VectorIndexSnapshotManager::IsExistVectorIndexSnapshot(uint64_t vector_index_id) {
  auto last_snapshot = GetLastSnapshot(vector_index_id);
  return last_snapshot != nullptr;
}

bool VectorIndexSnapshotManager::Init(std::vector<store::RegionPtr> regions) {
  for (auto& region : regions) {
    uint64_t vector_index_id = region->Id();
    auto snapshot_paths = GetSnapshotPaths(GetSnapshotParentPath(vector_index_id));
    for (auto snapshot_path : snapshot_paths) {
      auto snapshot = std::make_shared<vector_index::SnapshotMeta>(vector_index_id, snapshot_path);
      if (!snapshot->Init()) {
        return false;
      }

      AddSnapshot(snapshot);
    }
  }

  return true;
}

std::string VectorIndexSnapshotManager::GetSnapshotParentPath(uint64_t vector_index_id) {
  return fmt::format("{}/{}", Server::GetInstance()->GetIndexPath(), vector_index_id);
}

std::string VectorIndexSnapshotManager::GetSnapshotTmpPath(uint64_t vector_index_id) {
  return fmt::format("{}/tmp_{}", GetSnapshotParentPath(vector_index_id), Helper::TimestampNs());
}

std::string VectorIndexSnapshotManager::GetSnapshotNewPath(uint64_t vector_index_id, uint64_t snapshot_log_id) {
  return fmt::format("{}/snapshot_{:020}", GetSnapshotParentPath(vector_index_id), snapshot_log_id);
}

butil::Status VectorIndexSnapshotManager::LaunchInstallSnapshot(const butil::EndPoint& endpoint,
                                                                uint64_t vector_index_id) {
  uint64_t start_time = Helper::TimestampMs();
  auto snapshot_manager = Server::GetInstance()->GetVectorIndexManager()->GetVectorIndexSnapshotManager();

  // Get last snapshot
  auto last_snapshot = snapshot_manager->GetLastSnapshot(vector_index_id);
  if (last_snapshot == nullptr) {
    return butil::Status(pb::error::EVECTOR_SNAPSHOT_NOT_FOUND, "Not found vector index snapshot %lu", vector_index_id);
  }
  DINGO_LOG(INFO) << fmt::format("last vector index snapshot: {}", last_snapshot->Path());

  // Get uri
  auto reader = std::make_shared<FileReaderWrapper>(last_snapshot);
  uint64_t reader_id = FileServiceReaderManager::GetInstance().AddReader(reader);
  auto config = Server::GetInstance()->GetConfig();
  auto host = config->GetString("server.host");
  int port = config->GetInt("server.port");
  if (host.empty() || port == 0) {
    return butil::Status(pb::error::EILLEGAL_PARAMTETERS, "Parse server host or port error.");
  }
  std::string uri = fmt::format("remote://{}:{}/{}", host, port, reader_id);

  // Build request
  pb::node::InstallVectorIndexSnapshotRequest request;
  request.set_uri(uri);
  auto* meta = request.mutable_meta();
  meta->set_snapshot_log_index(last_snapshot->SnapshotLogId());
  for (const auto& filename : last_snapshot->ListFileNames()) {
    meta->add_filenames(filename);
  }
  meta->set_vector_index_id(vector_index_id);

  pb::node::InstallVectorIndexSnapshotResponse response;
  auto status = ServiceAccess::InstallVectorIndexSnapshot(request, endpoint, response);
  FileServiceReaderManager::GetInstance().DeleteReader(reader_id);

  DINGO_LOG(INFO) << fmt::format("Install vector index snapshot {} to {} finish elapsed time {}ms request: {}",
                                 vector_index_id, Helper::EndPointToStr(endpoint), Helper::TimestampMs() - start_time,
                                 request.ShortDebugString());

  return status;
}

butil::Status VectorIndexSnapshotManager::HandleInstallSnapshot(std::shared_ptr<Context>, const std::string& uri,
                                                                const pb::node::VectorIndexSnapshotMeta& meta) {
  auto vector_index = Server::GetInstance()->GetVectorIndexManager()->GetVectorIndex(meta.vector_index_id());
  if (vector_index != nullptr) {
    return butil::Status(pb::error::EVECTOR_NOT_NEED_SNAPSHOT, "Not need snapshot, follower own vector index.");
  }

  return DownloadSnapshotFile(uri, meta);
}

butil::Status VectorIndexSnapshotManager::LaunchPullSnapshot(const butil::EndPoint& endpoint,
                                                             uint64_t vector_index_id) {
  pb::node::GetVectorIndexSnapshotRequest request;
  request.set_vector_index_id(vector_index_id);

  pb::node::GetVectorIndexSnapshotResponse response;
  auto status = ServiceAccess::GetVectorIndexSnapshot(request, endpoint, response);
  if (!status.ok()) {
    return status;
  }

  status = DownloadSnapshotFile(response.uri(), response.meta());
  if (!status.ok()) {
    return status;
  }

  // Clean corresponding reader id.
  uint64_t reader_id = ParseReaderId(response.uri());
  if (reader_id > 0) {
    pb::fileservice::CleanFileReaderRequest request;
    request.set_reader_id(reader_id);
    ServiceAccess::CleanFileReader(request, endpoint);
  }

  return butil::Status();
}

butil::Status VectorIndexSnapshotManager::InstallSnapshotToFollowers(std::shared_ptr<VectorIndex> vector_index) {
  uint64_t start_time = Helper::TimestampMs();
  auto engine = Server::GetInstance()->GetEngine();
  if (engine->GetID() != pb::common::ENG_RAFT_STORE) {
    return butil::Status(pb::error::EINTERNAL, "Not raft store engine.");
  }

  auto raft_kv_engine = std::dynamic_pointer_cast<RaftStoreEngine>(engine);
  auto raft_node = raft_kv_engine->GetNode(vector_index->Id());
  if (raft_node == nullptr) {
    return butil::Status(pb::error::ERAFT_NOT_FOUND, "Not found raft node.");
  }

  auto self_peer = raft_node->GetPeerId();
  std::vector<braft::PeerId> peers;
  raft_node->ListPeers(&peers);
  for (const auto& peer : peers) {
    if (peer != self_peer) {
      auto status = LaunchInstallSnapshot(peer.addr, vector_index->Id());
      if (!status.ok()) {
        if (status.error_code() == pb::error::EVECTOR_NOT_NEED_SNAPSHOT ||
            status.error_code() == pb::error::EVECTOR_SNAPSHOT_EXIST) {
          DINGO_LOG(INFO) << fmt::format("vetor index {} peer {} {}", vector_index->Id(),
                                         Helper::EndPointToStr(peer.addr), status.error_str());
        } else {
          DINGO_LOG(ERROR) << fmt::format("Install vector index snapshot {} to {} failed, error: {}",
                                          vector_index->Id(), Helper::EndPointToStr(peer.addr), status.error_str());
        }
      }
    }
  }

  DINGO_LOG(INFO) << fmt::format("Install vector index snapshot {} to all followers finish elapsed time {}ms",
                                 vector_index->Id(), Helper::TimestampMs() - start_time);

  return butil::Status();
}

butil::Status VectorIndexSnapshotManager::HandlePullSnapshot(std::shared_ptr<Context> ctx, uint64_t vector_index_id) {
  // Check last snapshot is exist.
  auto snapshot_manager = Server::GetInstance()->GetVectorIndexManager()->GetVectorIndexSnapshotManager();

  // Get last snapshot
  auto last_snapshot = snapshot_manager->GetLastSnapshot(vector_index_id);
  if (last_snapshot == nullptr) {
    return butil::Status(pb::error::EVECTOR_SNAPSHOT_NOT_FOUND, "Not found vector index snapshot %lu", vector_index_id);
  }
  DINGO_LOG(INFO) << fmt::format("last vector index snapshot: {}", last_snapshot->Path());

  auto* response = dynamic_cast<pb::node::GetVectorIndexSnapshotResponse*>(ctx->Response());
  // Build response meta
  auto* meta = response->mutable_meta();
  meta->set_vector_index_id(vector_index_id);
  meta->set_snapshot_log_index(last_snapshot->SnapshotLogId());
  for (const auto& filename : last_snapshot->ListFileNames()) {
    meta->add_filenames(filename);
  }

  // Build response uri
  auto config = Server::GetInstance()->GetConfig();
  auto host = config->GetString("server.host");
  int port = config->GetInt("server.port");
  if (host.empty() || port == 0) {
    return butil::Status(pb::error::EILLEGAL_PARAMTETERS, "Parse server host or port error.");
  }

  auto reader = std::make_shared<FileReaderWrapper>(last_snapshot);
  uint64_t reader_id = FileServiceReaderManager::GetInstance().AddReader(reader);
  response->set_uri(fmt::format("remote://{}:{}/{}", host, port, reader_id));

  DINGO_LOG(INFO) << fmt::format("====response: {}", response->ShortDebugString());

  return butil::Status();
}

butil::Status VectorIndexSnapshotManager::PullLastSnapshotFromPeers(uint64_t vector_index_id) {
  uint64_t start_time = Helper::TimestampMs();
  auto engine = Server::GetInstance()->GetEngine();
  if (engine->GetID() != pb::common::ENG_RAFT_STORE) {
    return butil::Status(pb::error::EINTERNAL, "Not raft store engine.");
  }

  auto raft_kv_engine = std::dynamic_pointer_cast<RaftStoreEngine>(engine);
  auto raft_node = raft_kv_engine->GetNode(vector_index_id);
  if (raft_node == nullptr) {
    return butil::Status(pb::error::ERAFT_NOT_FOUND, "Not found raft node.");
  }

  // Find max vector index snapshot peer.
  pb::node::GetVectorIndexSnapshotRequest request;
  request.set_vector_index_id(vector_index_id);

  uint64_t max_snapshot_log_index = 0;
  butil::EndPoint endpoint;

  auto self_peer = raft_node->GetPeerId();
  std::vector<braft::PeerId> peers;
  raft_node->ListPeers(&peers);
  for (const auto& peer : peers) {
    if (peer == self_peer) {
      continue;
    }

    pb::node::GetVectorIndexSnapshotResponse response;
    auto status = ServiceAccess::GetVectorIndexSnapshot(request, peer.addr, response);
    if (!status.ok()) {
      continue;
    }

    if (max_snapshot_log_index < response.meta().snapshot_log_index()) {
      max_snapshot_log_index = response.meta().snapshot_log_index();
      endpoint = peer.addr;
    }
  }

  // Has vector index snapshot, pull it.
  if (max_snapshot_log_index == 0) {
    DINGO_LOG(INFO) << fmt::format("Other peers not exist vector index snapshot {}", vector_index_id);
    return butil::Status();
  }

  auto status = LaunchPullSnapshot(endpoint, vector_index_id);
  if (!status.ok()) {
    DINGO_LOG(ERROR) << fmt::format("Pull vector index snapshot {} from {} failed, error: {}", vector_index_id,
                                    Helper::EndPointToStr(endpoint), status.error_str());
    return status;
  }

  DINGO_LOG(INFO) << fmt::format("Pull vector index snapshot {} finish elapsed time {}ms", vector_index_id,
                                 Helper::TimestampMs() - start_time);

  return butil::Status();
}

butil::Status VectorIndexSnapshotManager::DownloadSnapshotFile(const std::string& uri,
                                                               const pb::node::VectorIndexSnapshotMeta& meta) {
  // Parse reader_id and endpoint
  uint64_t reader_id = ParseReaderId(uri);
  butil::EndPoint endpoint = ParseHost(uri);
  if (reader_id == 0 || endpoint.port == 0) {
    return butil::Status(pb::error::EINTERNAL, "Parse uri to reader_id and endpoint error");
  }

  auto snapshot_manager = Server::GetInstance()->GetVectorIndexManager()->GetVectorIndexSnapshotManager();

  if (snapshot_manager->IsExistSnapshot(meta.vector_index_id(), meta.snapshot_log_index())) {
    std::string msg = fmt::format("Already exist vector index snapshot vector_index_id: {} snapshot_log_index: {}",
                                  meta.vector_index_id(), meta.snapshot_log_index());
    DINGO_LOG(INFO) << msg;
    return butil::Status(pb::error::EVECTOR_SNAPSHOT_EXIST, msg);
  }

  // temp snapshot path for save vector index.
  std::string tmp_snapshot_path = GetSnapshotTmpPath(meta.vector_index_id());
  if (std::filesystem::exists(tmp_snapshot_path)) {
    Helper::RemoveAllFileOrDirectory(tmp_snapshot_path);
  } else {
    Helper::CreateDirectory(tmp_snapshot_path);
  }

  auto remote_file_copier = RemoteFileCopier::New(endpoint);
  if (!remote_file_copier->Init()) {
    return butil::Status(pb::error::EINTERNAL,
                         fmt::format("Init remote file copier failed, endpoint {}", Helper::EndPointToStr(endpoint)));
  }
  for (const auto& filename : meta.filenames()) {
    uint64_t offset = 0;
    std::ofstream ofile;

    std::string filepath = fmt::format("{}/{}", tmp_snapshot_path, filename);
    ofile.open(filepath, std::ofstream::out | std::ofstream::binary);
    DINGO_LOG(INFO) << "Get vector index snapshot file: " << filepath;

    for (;;) {
      pb::fileservice::GetFileRequest request;
      request.set_reader_id(reader_id);
      request.set_filename(filename);
      request.set_offset(offset);
      request.set_size(Constant::kFileTransportChunkSize);

      DINGO_LOG(DEBUG) << "GetFileRequest: " << request.ShortDebugString();

      butil::IOBuf buf;
      auto response = remote_file_copier->GetFile(request, &buf);
      if (response == nullptr) {
        return butil::Status(pb::error::EINTERNAL, "Get file failed");
      }

      DINGO_LOG(DEBUG) << "GetFileResponse: " << response->ShortDebugString();

      // Write local file.
      ofile << buf;

      if (response->eof()) {
        break;
      }

      offset += response->read_size();
    }

    ofile.close();
  }

  if (snapshot_manager->IsExistSnapshot(meta.vector_index_id(), meta.snapshot_log_index())) {
    std::string msg = fmt::format("Already exist vector index snapshot vector_index_id: {} snapshot_log_index: {}",
                                  meta.vector_index_id(), meta.snapshot_log_index());
    DINGO_LOG(INFO) << msg;
    return butil::Status(pb::error::EVECTOR_SNAPSHOT_EXIST, msg);
  }

  // Todo: lock rename
  // Rename
  std::string new_snapshot_path = GetSnapshotNewPath(meta.vector_index_id(), meta.snapshot_log_index());
  auto status = Helper::Rename(tmp_snapshot_path, new_snapshot_path);
  if (!status.ok()) {
    DINGO_LOG(ERROR) << fmt::format("Rename vector index snapshot failed, {} -> {} error: {}", tmp_snapshot_path,
                                    new_snapshot_path, status.error_str());
    return status;
  }

  // Get stale snapshot
  auto stale_snapshots = snapshot_manager->GetSnapshots(meta.vector_index_id());

  auto new_snapshot = vector_index::SnapshotMeta::New(meta.vector_index_id(), new_snapshot_path);
  if (!new_snapshot->Init()) {
    return butil::Status(pb::error::EINTERNAL, "Init snapshot failed, path: %s", new_snapshot_path.c_str());
  }

  if (!snapshot_manager->AddSnapshot(new_snapshot)) {
    return butil::Status(pb::error::EVECTOR_SNAPSHOT_EXIST, "Already exist vector index snapshot, path: %s",
                         new_snapshot_path.c_str());
  }

  // Remove stale snapshot
  for (auto& snapshot : stale_snapshots) {
    snapshot_manager->DeleteSnapshot(snapshot);
  }

  return butil::Status();
}

// Save vector index snapshot, just one concurrence.
butil::Status VectorIndexSnapshotManager::SaveVectorIndexSnapshot(std::shared_ptr<VectorIndex> vector_index,
                                                                  uint64_t& snapshot_log_index) {
  // Check if vector_index is null
  if (vector_index == nullptr) {
    DINGO_LOG(WARNING) << fmt::format(
        "[vector_index.save_snapshot][index_id()] Save vector index failed, vector_index is null");
    return butil::Status(pb::error::Errno::EINTERNAL, "Save vector index failed, vector_index is null");
  }

  // Control concurrence.
  if (vector_index->SnapshotDoing()) {
    return butil::Status(pb::error::EINTERNAL, "Save vector index is busy.");
  }
  vector_index->SetSnapshotLogIndex(true);
  ON_SCOPE_EXIT([&]() { vector_index->SetSnapshotLogIndex(false); });

  uint64_t start_time = Helper::TimestampMs();

  // lock write for atomic ops
  // this lock will be unlocked after fork()
  vector_index->LockWrite();

  uint64_t apply_log_index = vector_index->ApplyLogIndex();

  auto snapshot_manager = Server::GetInstance()->GetVectorIndexManager()->GetVectorIndexSnapshotManager();

  // If already exist snapshot then give up.
  if (snapshot_manager->IsExistSnapshot(vector_index->Id(), apply_log_index)) {
    snapshot_log_index = apply_log_index;
    // unlock write
    vector_index->UnlockWrite();

    DINGO_LOG(INFO) << fmt::format(
        "[vector_index.save_snapshot][index_id({})] VectorIndex Snapshot already exist, cannot do save, log_id: {}",
        vector_index->Id(), apply_log_index);
    return butil::Status();
  }

  // Temp snapshot path for save vector index.
  std::string tmp_snapshot_path = GetSnapshotTmpPath(vector_index->Id());
  if (std::filesystem::exists(tmp_snapshot_path)) {
    Helper::RemoveAllFileOrDirectory(tmp_snapshot_path);
  } else {
    Helper::CreateDirectory(tmp_snapshot_path);
  }

  // Get vector index file path
  std::string index_filepath =
      fmt::format("{}/index_{}_{}.idx", tmp_snapshot_path, vector_index->Id(), apply_log_index);

  DINGO_LOG(INFO) << fmt::format("[vector_index.save_snapshot][index_id({})] Save vector index to file {}",
                                 vector_index->Id(), index_filepath);

  // Save vector index to tmp file
  // fork() a child process to save vector index to tmp file
  int pipefd[2];  // Pipe file descriptors
  if (pipe(pipefd) == -1) {
    // unlock write
    vector_index->UnlockWrite();

    DINGO_LOG(ERROR) << fmt::format(
        "[vector_index.save_snapshot][index_id({})] Save vector index snapshot failed, create pipe failed, error: {}",
        vector_index->Id(), strerror(errno));
    Helper::RemoveAllFileOrDirectory(tmp_snapshot_path);
    return butil::Status(pb::error::Errno::EINTERNAL, "Save vector index failed, create pipe failed");
  }

  pid_t pid = fork();
  if (pid < 0) {
    // unlock write
    vector_index->UnlockWrite();

    DINGO_LOG(ERROR) << fmt::format(
        "[vector_index.save_snapshot][index_id({})] Save vector index snapshot failed, fork failed, error: {}",
        vector_index->Id(), strerror(errno));
    Helper::RemoveAllFileOrDirectory(tmp_snapshot_path);
    return butil::Status(pb::error::Errno::EINTERNAL, "Save vector index failed, fork failed");
  } else if (pid == 0) {
    // Caution: child process can't do any DINGO_LOG, because DINGO_LOG will overwrite the whole log file
    //          but there is DINGO_LOG call in RemoveAllFileOrDirectory if error ocurred, careful to use it.

    // Child process
    close(pipefd[0]);  // Close unused read end

    auto ret = vector_index->Save(index_filepath);
    if (ret.error_code() == pb::error::Errno::EVECTOR_NOT_SUPPORT) {
      ret = butil::Status();
    } else if (!ret.ok()) {
      Helper::RemoveAllFileOrDirectory(tmp_snapshot_path);
    }

    // Write result to pipe
    pb::error::Error error;
    error.set_errcode(static_cast<pb::error::Errno>(ret.error_code()));
    error.set_errmsg(ret.error_str());

    std::string buf;
    if (!error.SerializeToString(&buf)) {
      DINGO_LOG(ERROR) << fmt::format(
          "[vector_index.save_snapshot][index_id({})] Save vector index snapshot failed, serialize error failed",
          vector_index->Id());
      Helper::RemoveAllFileOrDirectory(tmp_snapshot_path);
    }
    write(pipefd[1], buf.c_str(), buf.size());

    close(pipefd[1]);  // Close write end

    _exit(0);
  } else {
    // unlock write
    vector_index->UnlockWrite();

    // Parent process
    close(pipefd[1]);  // Close unused write end

    // Wait for the child process to complete
    int status;

    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
      // Child process exited successfully
      char result[4096];  // we need to make sure the child process will not write more than 4096 bytes to pipe
      read(pipefd[0], result, sizeof(result));

      pb::error::Error error;
      error.ParseFromString(result);
      if (error.errcode() != pb::error::Errno::OK) {
        DINGO_LOG(ERROR) << fmt::format(
            "[vector_index.save_snapshot][index_id({})] Save vector index snapshot  failed, {}", vector_index->Id(),
            error.errmsg());
        Helper::RemoveAllFileOrDirectory(tmp_snapshot_path);
        return butil::Status(error.errcode(), error.errmsg());
      }

      close(pipefd[0]);  // Close read end

      DINGO_LOG(INFO) << fmt::format("[vector_index.save_snapshot][index_id({})] Save vector index snapshot success",
                                     vector_index->Id());

    } else {
      DINGO_LOG(ERROR) << fmt::format(
          "[vector_index.save_snapshot][index_id({})] Save vector index snapshot failed, child process encountered an "
          "error",
          vector_index->Id());
      Helper::RemoveAllFileOrDirectory(tmp_snapshot_path);
      return butil::Status(pb::error::Errno::EINTERNAL, "Save vector index failed, child process encountered an error");
    }
  }

  // Write vector index meta
  std::string meta_filepath = fmt::format("{}/meta", tmp_snapshot_path, vector_index->Id());
  std::ofstream meta_file(meta_filepath);
  if (!meta_file.is_open()) {
    DINGO_LOG(ERROR) << fmt::format(
        "[vector_index.save_snapshot][index_id({})] Open vector index file log_id file {} failed", vector_index->Id(),
        meta_filepath);
    return butil::Status(pb::error::Errno::EINTERNAL, "Open vector index file log_id file failed");
  }

  meta_file << apply_log_index;
  meta_file.close();

  // Rename
  std::string new_snapshot_path = GetSnapshotNewPath(vector_index->Id(), apply_log_index);
  auto status = Helper::Rename(tmp_snapshot_path, new_snapshot_path);
  if (!status.ok()) {
    DINGO_LOG(ERROR) << fmt::format(
        "[vector_index.save_snapshot][index_id({})] Rename vector index snapshot failed, {} -> {} error: {}",
        vector_index->Id(), tmp_snapshot_path, new_snapshot_path, status.error_str());
    return status;
  }

  // Get stale snapshot
  auto stale_snapshots = snapshot_manager->GetSnapshots(vector_index->Id());

  auto new_snapshot = vector_index::SnapshotMeta::New(vector_index->Id(), new_snapshot_path);
  if (!new_snapshot->Init()) {
    return butil::Status(pb::error::EINTERNAL, "Init snapshot failed, path: %s", new_snapshot_path.c_str());
  }

  if (!snapshot_manager->AddSnapshot(new_snapshot)) {
    return butil::Status(pb::error::EVECTOR_SNAPSHOT_EXIST, "Already exist vector index snapshot, path: %s",
                         new_snapshot_path.c_str());
  }

  // Remove stale snapshot
  for (auto& snapshot : stale_snapshots) {
    snapshot_manager->DeleteSnapshot(snapshot);
  }

  // Set truncate wal log index.
  auto log_storage = Server::GetInstance()->GetLogStorageManager()->GetLogStorage(vector_index->Id());
  if (log_storage != nullptr) {
    log_storage->SetVectorIndexTruncateLogIndex(apply_log_index);
  }

  snapshot_log_index = apply_log_index;

  DINGO_LOG(INFO) << fmt::format(
      "[vector_index.save_snapshot][index_id({})] Save vector index snapshot snapshot_{:020} elapsed time {}ms",
      vector_index->Id(), apply_log_index, Helper::TimestampMs() - start_time);

  return butil::Status::OK();
}

// Load vector index for already exist vector index at bootstrap.
std::shared_ptr<VectorIndex> VectorIndexSnapshotManager::LoadVectorIndexSnapshot(store::RegionPtr region) {
  assert(region != nullptr);
  uint64_t vector_index_id = region->Id();

  auto snapshot_manager = Server::GetInstance()->GetVectorIndexManager()->GetVectorIndexSnapshotManager();

  // Read vector index snapshot log id form snapshot meta file.
  auto last_snapshot = snapshot_manager->GetLastSnapshot(vector_index_id);
  if (last_snapshot == nullptr) {
    DINGO_LOG(WARNING) << fmt::format(
        "[vector_index.load_snapshot][index_id({})] Get last vector index snapshot log id failed.", vector_index_id);
    return nullptr;
  }

  DINGO_LOG(INFO) << fmt::format("[vector_index.load_snapshot][index_id({})] snapshot log id is {}",
                                 last_snapshot->VectorIndexId(), last_snapshot->SnapshotLogId());

  // check if can load from file
  std::string index_data_path = last_snapshot->IndexDataPath();

  // check if file vector_index_file_path exists
  if (!std::filesystem::exists(index_data_path)) {
    DINGO_LOG(ERROR) << fmt::format(
        "[vector_index.load_snapshot][index_id({})] file {} not exist, can't load, need to build vector_index",
        last_snapshot->VectorIndexId(), index_data_path);
    return nullptr;
  }

  // create a new vector_index
  auto vector_index = VectorIndexFactory::New(vector_index_id, region->InnerRegion().definition().index_parameter());
  if (!vector_index) {
    DINGO_LOG(WARNING) << fmt::format("[vector_index.load_snapshot][index_id({})] New vector index failed.",
                                      vector_index_id);
    return nullptr;
  }

  // load index from file
  auto ret = vector_index->Load(index_data_path);
  if (!ret.ok()) {
    DINGO_LOG(WARNING) << fmt::format("[vector_index.load_snapshot][index_id({})] Load vector index failed.",
                                      vector_index_id);
    return nullptr;
  }

  // set vector_index apply log id
  vector_index->SetSnapshotLogIndex(last_snapshot->SnapshotLogId());
  vector_index->SetApplyLogIndex(last_snapshot->SnapshotLogId());

  return vector_index;
}

bool VectorIndexSnapshotManager::AddSnapshot(vector_index::SnapshotMetaPtr snapshot) {
  BAIDU_SCOPED_LOCK(mutex_);
  auto it = snapshot_maps_.find(snapshot->VectorIndexId());
  if (it == snapshot_maps_.end()) {
    std::map<uint64_t, vector_index::SnapshotMetaPtr> inner_snapshots;
    inner_snapshots[snapshot->SnapshotLogId()] = snapshot;
    snapshot_maps_.insert(std::make_pair(snapshot->VectorIndexId(), inner_snapshots));
  } else {
    auto& inner_snapshots = it->second;
    if (inner_snapshots.find(snapshot->SnapshotLogId()) == inner_snapshots.end()) {
      inner_snapshots[snapshot->SnapshotLogId()] = snapshot;
    } else {
      DINGO_LOG(WARNING) << fmt::format("Already exist vector index snapshot {} {}", snapshot->VectorIndexId(),
                                        snapshot->SnapshotLogId());
      return false;
    }
  }

  return true;
}

void VectorIndexSnapshotManager::DeleteSnapshot(vector_index::SnapshotMetaPtr snapshot) {
  BAIDU_SCOPED_LOCK(mutex_);

  auto it = snapshot_maps_.find(snapshot->VectorIndexId());
  if (it != snapshot_maps_.end()) {
    auto& inner_snapshots = it->second;
    auto inner_it = inner_snapshots.find(snapshot->SnapshotLogId());
    if (inner_it != inner_snapshots.end()) {
      inner_snapshots.erase(inner_it);
      DINGO_LOG(INFO) << "delete snapshot...";
    }
  }
}

void VectorIndexSnapshotManager::DeleteSnapshots(uint64_t vector_index_id) {
  BAIDU_SCOPED_LOCK(mutex_);

  auto it = snapshot_maps_.find(vector_index_id);
  if (it != snapshot_maps_.end()) {
    auto& inner_snapshots = it->second;
    inner_snapshots.clear();
    snapshot_maps_.erase(it);
  }
}

vector_index::SnapshotMetaPtr VectorIndexSnapshotManager::GetLastSnapshot(uint64_t vector_index_id) {
  BAIDU_SCOPED_LOCK(mutex_);

  auto it = snapshot_maps_.find(vector_index_id);
  if (it != snapshot_maps_.end()) {
    auto& inner_snapshots = it->second;
    if (!inner_snapshots.empty()) {
      return inner_snapshots.rbegin()->second;
    }
  }

  return nullptr;
}

std::vector<vector_index::SnapshotMetaPtr> VectorIndexSnapshotManager::GetSnapshots(uint64_t vector_index_id) {
  BAIDU_SCOPED_LOCK(mutex_);

  std::vector<vector_index::SnapshotMetaPtr> result;
  auto it = snapshot_maps_.find(vector_index_id);
  if (it != snapshot_maps_.end()) {
    auto& inner_snapshots = it->second;
    for (auto& [_, snapshot] : inner_snapshots) {
      result.push_back(snapshot);
    }
  }

  return result;
}

bool VectorIndexSnapshotManager::IsExistSnapshot(uint64_t vector_index_id, uint64_t snapshot_log_id) {
  auto snapshot = GetLastSnapshot(vector_index_id);
  if (snapshot == nullptr) {
    return false;
  }

  return snapshot_log_id <= snapshot->SnapshotLogId();
}

}  // namespace dingodb
