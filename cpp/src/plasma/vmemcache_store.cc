// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <memory>
#include <string>

#include <stdlib.h>

#include "arrow/util/logging.h"

#include "plasma/plasma_allocator.h"
#include "plasma/store.h"
#include "plasma/vmemcache_store.h"

#include <libvmemcache.h>

#include <sys/statfs.h>

#include <map>

#include "plasma/tools/PlasmaProperties.h"

#define CACHE_MAX_SIZE (1024 * 1024 * 1024L)
#define CACHE_EXTENT_SIZE 512

namespace plasma {
bool VmemcacheStore::DetectInitialPath(std::vector<numaNodeInfo>& numaNodeInfos,
                                       std::string& argStr) {
  std::string propertyFilePath = "";
  int index = argStr.find("propertyFilePath");
  if (index != -1) {
    const int commaIndex = argStr.find_first_of(",");
    const int propertyFilePathEndIndex = std::strlen("propertyFilePath:");
    propertyFilePath = argStr.substr(propertyFilePathEndIndex, commaIndex);
    argStr = argStr.substr(commaIndex + 1, argStr.length());
  }
  PlasmaProperties* plasmaProperties = new PlasmaProperties(argStr, propertyFilePath);
  numaNodeInfos = plasmaProperties->getNumaNodeInfos();
  if (numaNodeInfos.size() == 0) return false;

  for (numaNodeInfo info : numaNodeInfos) {
    struct statfs pathInfo;
    int retVal = statfs(info.initialPath.c_str(), &pathInfo);
    if (retVal < 0) {
      ARROW_LOG(FATAL) << "Directory: " << info.initialPath << " not exist.";
      return false;
    }
    uint64_t blockSize = pathInfo.f_bsize;
    uint64_t freeSize = pathInfo.f_bfree * blockSize;
    if (info.requiredSize > freeSize) {
      ARROW_LOG(WARNING) << "Failed to provide enough size for allocation"
                         << "Directory: " << info.initialPath
                         << "will use max free_size * " << DEFALUT_CONFIG_MULTIPLIER
                         << " " << freeSize << "B";
      info.requiredSize = freeSize * DEFALUT_CONFIG_MULTIPLIER;
    }
  }
  return true;
}

// Connect here is like something initial
Status VmemcacheStore::Connect(const std::string& endpoint) {
  const int vmemcacheStrEndIndex = 12;
  std::string argStr = endpoint.substr(vmemcacheStrEndIndex, endpoint.length());
  std::vector<numaNodeInfo> numaNodeInfos;
  if (!DetectInitialPath(numaNodeInfos, argStr)) {
    return Status::UnknownError("Initial vmemcache failed!");
  }
  totalNumaNodes = numaNodeInfos.size();
  for (int i = 0; i < totalNumaNodes; i++) {
    // initial vmemcache on numa node i
    numaNodeInfo nninfo = numaNodeInfos[i];
    totalCacheSize += nninfo.requiredSize;

    VMEMcache* cache = vmemcache_new();
    if (!cache) {
      ARROW_LOG(FATAL) << "Initial vmemcache failed!";
      return Status::UnknownError("Initial vmemcache failed!");
    }
    std::string path = nninfo.initialPath;
    u_int64_t size = nninfo.requiredSize;

    ARROW_LOG(DEBUG) << "initial vmemcache on " << path << ", size" << size
                     << ", extent size" << CACHE_EXTENT_SIZE
                     << ", numa mode id: " << nninfo.numaNodeId;

    if (vmemcache_set_size(cache, size)) {
      ARROW_LOG(DEBUG) << "vmemcache_set_size error:" << vmemcache_errormsg();
      ARROW_LOG(FATAL) << "vmemcache_set_size failed!";
    }

    if (vmemcache_set_extent_size(cache, CACHE_EXTENT_SIZE)) {
      ARROW_LOG(DEBUG) << "vmemcache_set_extent_size error:" << vmemcache_errormsg();
      ARROW_LOG(FATAL) << "vmemcache_set_extent_size failed!";
    }

    if (vmemcache_add(cache, path.c_str())) {
      ARROW_LOG(FATAL) << "Initial vmemcache failed!" << vmemcache_errormsg();
    }

    caches.push_back(cache);

    std::vector<int> cpus_in_node;
    numaThreadPool::getNumaNodeCpu(i, cpus_in_node);
    std::vector<int> cpus_for_put(nninfo.writePoolSize);
    for (uint32_t j = 0; j < nninfo.writePoolSize; j++) {
      cpus_for_put[j] = cpus_in_node[j % cpus_in_node.size()];
    }
    std::shared_ptr<numaThreadPool> poolPut(
        new numaThreadPool(i, nninfo.writePoolSize, cpus_for_put));
    putThreadPools.push_back(poolPut);

    std::vector<int> cpus_for_get(nninfo.readPoolSize);
    for (uint32_t j = 0; j < nninfo.readPoolSize; j++) {
      cpus_for_get[j] = cpus_in_node[(j + nninfo.readPoolSize) % cpus_in_node.size()];
    }
    std::shared_ptr<numaThreadPool> poolGet(
        new numaThreadPool(i, nninfo.readPoolSize, cpus_for_get));
    getThreadPools.push_back(poolGet);

    ARROW_LOG(DEBUG) << "initial vmemcache success!";
  }
  srand((unsigned int)time(NULL));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // try not use lambda function
  putThreadPools[0]->enqueue([&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    while (true) {
      if (evictionPolicy_->RemainingCapacity() <= evictionPolicy_->Capacity() / 3 * 2) {
        auto tic = std::chrono::steady_clock::now();
        std::vector<ObjectID> objIds;
        evictionPolicy_->ChooseObjectsToEvict(evictionPolicy_->Capacity() / 3, &objIds);
        ARROW_LOG(DEBUG) << "will evict " << objIds.size()
                         << " objects. Plasma Allocator allocated size is "
                         << PlasmaAllocator::Allocated();
        std::vector<std::future<int>> ret;
        for (auto objId : objIds) {
          if (Exist(objId).ok()) {
            // change states
            auto entry = GetObjectTableEntry(evictionPolicy_->getStoreInfo(), objId);
            entry->state = ObjectState::PLASMA_EVICTED;
            PlasmaAllocator::Free(entry->pointer,
                                  entry->data_size + entry->metadata_size);
            entry->pointer = nullptr;
            evictionPolicy_->RemoveObject(objId);
            continue;
          }
          int node = rand() % totalNumaNodes;
          ret.push_back(putThreadPools[node]->enqueue([&, objId, node]() {
            auto entry = GetObjectTableEntry(evictionPolicy_->getStoreInfo(), objId);
            if (entry == nullptr) {
              ARROW_LOG(WARNING) << "try to evict an object not exist in object table!!! "
                                 << objId.hex();
              return -1;
            }
            ARROW_LOG(DEBUG) << "evict " << objId.hex() << " ref count is "
                             << entry->ref_count;
            if (entry->ref_count != 0) {
              ARROW_LOG(WARNING) << "this object can't evict now due to unreleased";
              return -1;
            }
            entry->mtx.lock();
            ARROW_CHECK(entry != nullptr)
                << "To evict an object it must be in the object table.";
            ARROW_CHECK(entry->state == ObjectState::PLASMA_SEALED)
                << "To evict an object it must have been sealed.";
            ARROW_CHECK(entry->ref_count == 0)
                << "To evict an object, there must be no clients currently using it.";
            entry->numaNodePostion = node;
            entry->state = ObjectState::PLASMA_EVICTED;
            auto status =
                Put({objId},
                    {std::make_shared<arrow::Buffer>(
                        entry->pointer, entry->data_size + entry->metadata_size)},
                    node);
            PlasmaAllocator::Free(entry->pointer,
                                  entry->data_size + entry->metadata_size);
            entry->pointer = nullptr;
            evictionPolicy_->RemoveObject(objId);
            entry->mtx.unlock();
            return 0;
          }));
        }
        for (unsigned long i = 0; i < ret.size(); i++) ret[i].get();
        auto toc = std::chrono::steady_clock::now();
        std::chrono::duration<double> time_ = toc - tic;
        ARROW_LOG(DEBUG) << "Eviction done, takes " << time_.count() * 1000 << " ms";
      }
    }
  });

  return Status::OK();
}

Status VmemcacheStore::Put(const std::vector<ObjectID>& ids,
                           const std::vector<std::shared_ptr<Buffer>>& data, int numaId) {
  auto tic = std::chrono::steady_clock::now();
  int total = ids.size();
  for (int i = 0; i < total; i++) {
    if (Exist(ids[i]).ok()) continue;
    auto cache = caches[numaId];
    int ret = vmemcache_put(cache, ids[i].data(), ids[i].size(), (char*)data[i]->data(),
                            data[i]->size());
    if (ret != 0) ARROW_LOG(WARNING) << "vmemcache_put error:" << vmemcache_errormsg();
  }
  auto toc = std::chrono::steady_clock::now();
  std::chrono::duration<double> time_ = toc - tic;
  ARROW_LOG(DEBUG) << "Put " << total << " objects takes " << time_.count() * 1000
                   << " ms";
  return Status::OK();
}

// maintain a thread-pool conatins all numa node threads
Status VmemcacheStore::Put(const std::vector<ObjectID>& ids,
                           const std::vector<std::shared_ptr<Buffer>>& data) {
  auto tic = std::chrono::steady_clock::now();
  int total = ids.size();
  std::vector<std::future<int>> results;
  for (int i = 0; i < total; i++) {
    if (Exist(ids[i]).ok()) continue;
    // find a random instansce to put
    int numaId = rand() % totalNumaNodes;
    auto pool = putThreadPools[numaId];
    auto cache = caches[numaId];
    size_t keySize = ids[i].size();
    char* key = new char[keySize];
    memcpy(key, ids[i].data(), keySize);

    putParam* param =
        new putParam(cache, key, keySize, (char*)data[i]->data(), data[i]->size());
    results.emplace_back(pool->enqueue([&, param]() {
      if (param != nullptr) {
        int ret = vmemcache_put(param->cache, (char*)(param->key), param->keySize,
                                (char*)(param->value), param->valueSize);
        if (ret != 0) ARROW_LOG(DEBUG) << "vmemcache_put error:" << vmemcache_errormsg();
        delete[](char*) param->key;
        delete param;

        return ret;
      } else {
        ARROW_LOG(FATAL) << "ptr is null !!!";
        return -1;
      }
    }));
  }
  for (int i = 0; i < (int)results.size(); i++) {
    if (results[i].get() != 0) ARROW_LOG(WARNING) << "Put " << i << " failed";
  }
  auto toc = std::chrono::steady_clock::now();
  std::chrono::duration<double> time_ = toc - tic;
  ARROW_LOG(DEBUG) << "Put " << total << " objects takes " << time_.count() * 1000
                   << " ms";
  return Status::OK();
}

std::string VmemcacheStore::hex(char* id) {
  char hex[] = "0123456789abcdef";
  std::string result;
  for (int i = 0; i < 20; i++) {
    unsigned char val = *(id + i);
    result.push_back(hex[val >> 4]);
    result.push_back(hex[val & 0xf]);
  }
  return result;
}

// not used
Status VmemcacheStore::Get(const std::vector<ObjectID>& ids,
                           std::vector<std::shared_ptr<Buffer>> buffers,
                           ObjectTableEntry* entry) {
  int total = ids.size();

  for (int i = 0; i < total; i++) {
    auto id = ids[i];
    auto buffer = buffers[i];
    getThreadPools[entry->numaNodePostion]->enqueue([&, entry, id, buffer]() {
      auto cache = caches[entry->numaNodePostion];
      size_t vSize = size_t(0);
      int ret = 0;
      ret = vmemcache_get(cache, id.data(), id.size(), (void*)buffer->mutable_data(),
                          buffer->size(), 0, &vSize);
      if (ret <= 0) {
        ARROW_LOG(WARNING) << "vmemcache get fails! err msg " << vmemcache_errormsg();
      }
      entry->state = ObjectState::PLASMA_SEALED;
    });
  }

  return Status::OK();
}

// not used
Status VmemcacheStore::Get(const std::vector<ObjectID>& ids,
                           std::vector<std::shared_ptr<Buffer>> buffers) {
  auto tic = std::chrono::steady_clock::now();
  int total = ids.size();
  std::vector<std::future<int>> results;
  for (int i = 0; i < total; i++) {
    auto id = ids[i];
    auto buffer = buffers[i];
    size_t valueSize = 0;
    for (int j = 0; j < totalNumaNodes; j++) {
      size_t keySize = id.size();
      char* key = new char[keySize];
      memcpy(key, id.data(), keySize);
      auto cache = caches[j];
      if (vmemcache_exists(cache, key, keySize, &valueSize) == 1) {
        auto pool = getThreadPools[j];

        size_t* vSize = new size_t(0);
        getParam* param = new getParam(cache, key, keySize, (void*)buffer->mutable_data(),
                                       buffer->size(), 0, vSize);
        results.emplace_back(pool->enqueue([&, param]() {
          if (param != nullptr) {
            int ret =
                vmemcache_get(param->cache, param->key, param->key_size, param->vbuf,
                              param->vbufsize, param->offset, param->vsize);
            delete[](char*) param->key;
            delete (getParam*)param;
            return ret;
          } else {
            return -1;
          }
        }));
        break;
      } else {
        ARROW_LOG(DEBUG) << id.hex() << " not exist in Vmemcache instance" << j;
      }
    }
  }
  auto toc = std::chrono::steady_clock::now();
  std::chrono::duration<double> time_ = toc - tic;
  ARROW_LOG(DEBUG) << "Get " << total << " objects takes " << time_.count() * 1000
                   << " ms";
  return Status::OK();
}

Status VmemcacheStore::Get(const ObjectID id, ObjectTableEntry* entry) {
  getThreadPools[entry->numaNodePostion]->enqueue([&, id, entry]() {
    ARROW_LOG(DEBUG) << "pre fetch object " << id.hex();
    uint8_t* pointer = PlasmaStore::AllocateMemory(
        entry->data_size + entry->metadata_size, false, &entry->fd, &entry->map_size,
        &entry->offset, nullptr, true);
    if (!pointer) {
      ARROW_LOG(ERROR) << "Not enough memory to create the object " << id.hex()
                       << ", data_size=" << entry->data_size
                       << ", metadata_size=" << entry->metadata_size
                       << ", will send a reply of PlasmaError::OutOfMemory";
    } else {
      entry->pointer = pointer;
      std::shared_ptr<Buffer> buffer(
          new arrow::MutableBuffer(entry->pointer, entry->data_size));
      auto cache = caches[entry->numaNodePostion];
      size_t vSize = size_t(0);
      int ret = 0;
      ret = vmemcache_get(cache, id.data(), id.size(), (void*)buffer->mutable_data(),
                          buffer->size(), 0, &vSize);
      if (ret <= 0)
        ARROW_LOG(WARNING) << "vmemcache get fails! err msg " << vmemcache_errormsg();
      else
        entry->state = ObjectState::PLASMA_SEALED;
    }
  });

  return Status::OK();
}

Status VmemcacheStore::Exist(ObjectID id) {
  for (auto cache : caches) {
    size_t valueSize = 0;
    int ret = vmemcache_exists(cache, id.data(), id.size(), &valueSize);
    if (ret == 1) {
      return Status::OK();
    }
  }
  return Status::Invalid("Not Exist");
}

Status VmemcacheStore::RegisterEvictionPolicy(EvictionPolicy* eviction_policy) {
  evictionPolicy_ = eviction_policy;
  return Status::OK();
}

void VmemcacheStore::Metrics(int64_t* memory_total, int64_t* memory_used) {
  *memory_total = totalCacheSize;
  int64_t memory_used_ = 0;
  for (int i = 0; i < totalNumaNodes; i++) {
    int64_t tmp;
    vmemcache_get_stat(caches[i], VMEMCACHE_STAT_POOL_SIZE_USED, &tmp, sizeof(tmp));
    memory_used_ += tmp;
  }
  *memory_used = memory_used_;
}

REGISTER_EXTERNAL_STORE("vmemcache", VmemcacheStore);

}  // namespace plasma
