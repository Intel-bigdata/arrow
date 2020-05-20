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

#ifndef VMEMCACHE_STORE_H
#define VMEMCACHE_STORE_H

#include "plasma/eviction_policy.h"
#include "plasma/external_store.h"
#include "plasma/numaThreadPool.h"
#include "tools/PlasmaProperties.h"

#include <libvmemcache.h>

#include <vector>

namespace plasma {

#pragma pack(push, 1)
class putParam {
 public:
  VMEMcache* cache;
  void* key;
  size_t keySize;
  void* value;
  size_t valueSize;
  putParam(VMEMcache* cache_, void* key_, size_t keySize_, void* value_,
           size_t valueSize_) {
    cache = cache_;
    key = key_;
    keySize = keySize_;
    value = value_;
    valueSize = valueSize_;
  }
};

class getParam {
 public:
  VMEMcache* cache;
  void* key;
  size_t key_size;
  void* vbuf;
  size_t vbufsize;
  size_t offset;
  size_t* vsize;

  getParam(VMEMcache* cache_, void* key_, size_t key_size_, void* vbuf_, size_t vbufsize_,
           size_t offset_, size_t* vSize_) {
    cache = cache_;
    key = key_;
    key_size = key_size_;
    vbuf = vbuf_;
    vbufsize = vbufsize_;
    offset = offset_;
    vsize = vSize_;
  }
};
#pragma pack(pop)

class VmemcacheStore : public ExternalStore {
 public:
  VmemcacheStore() = default;

  Status Connect(const std::string& endpoint) override;

  Status Get(const std::vector<ObjectID>& ids,
             std::vector<std::shared_ptr<Buffer>> buffers) override;

  Status Get(const std::vector<ObjectID>& ids,
             std::vector<std::shared_ptr<Buffer>> buffers,
             ObjectTableEntry* entry) override;
  Status Get(const ObjectID id, ObjectTableEntry* entry) override;

  Status Put(const std::vector<ObjectID>& ids,
             const std::vector<std::shared_ptr<Buffer>>& data) override;
  Status Put(const std::vector<ObjectID>& ids,
             const std::vector<std::shared_ptr<Buffer>>& data, int numaId);

  Status Exist(ObjectID id) override;
  static std::string hex(char* id);
  Status RegisterEvictionPolicy(EvictionPolicy* eviction_policy) override;
  void Metrics(int64_t* memory_total, int64_t* memory_used) override;

  bool DetectInitialPath(std::vector<plasma::numaNodeInfo>& numaNodeVt,
                         std::string& path);

 private:
  void Evict(std::vector<ObjectID>& ids, std::vector<std::shared_ptr<Buffer>>& datas);
  std::vector<VMEMcache*> caches;
  std::vector<std::shared_ptr<numaThreadPool>> putThreadPools;
  std::vector<std::shared_ptr<numaThreadPool>> getThreadPools;
  int totalNumaNodes = 2;
  int64_t totalCacheSize = 0;
  EvictionPolicy* evictionPolicy_;
};

}  // namespace plasma

#endif