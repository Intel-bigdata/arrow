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

#include <arrow/util/logging.h>

#include "plasma/malloc.h"
#include "plasma/plasma_allocator.h"

namespace plasma {

extern "C" {
void* dlmemalign(size_t alignment, size_t bytes);
void dlfree(void* mem);
}

#ifdef PLASMA_MEMKIND
struct memkind* PlasmaAllocator::pmem_kind_ = nullptr;
#endif

int64_t PlasmaAllocator::footprint_limit_ = 0;
int64_t PlasmaAllocator::allocated_ = 0;
std::mutex PlasmaAllocator::mtx;
void* PlasmaAllocator::Memalign(size_t alignment, size_t bytes) {
  void* mem = NULL;
  {
    std::lock_guard<std::mutex> lck(PlasmaAllocator::mtx);
    if (allocated_ + static_cast<int64_t>(bytes) > footprint_limit_) {
      return nullptr;
    }
#ifdef PLASMA_MEMKIND
    memkind_posix_memalign(pmem_kind_, (void **)&mem, alignment, bytes);
#else
    mem = dlmemalign(alignment, bytes);
#endif
    allocated_ += bytes;
  }
  ARROW_CHECK(mem);
  return mem;
}

void PlasmaAllocator::Free(void* mem, size_t bytes) {
  std::lock_guard<std::mutex> lck(PlasmaAllocator::mtx);
#ifdef PLASMA_MEMKIND
   memkind_free(pmem_kind_, mem);
#else
  dlfree(mem);
#endif
  allocated_ -= bytes;
}

void PlasmaAllocator::SetFootprintLimit(size_t bytes) {
  footprint_limit_ = static_cast<int64_t>(bytes);
}

int64_t PlasmaAllocator::GetFootprintLimit() { return footprint_limit_; }

int64_t PlasmaAllocator::Allocated() { return allocated_; }

#ifdef PLASMA_MEMKIND
static void print_err_message(int err)
{
  char error_message[MEMKIND_ERROR_MESSAGE_SIZE];
  memkind_error_message(err, error_message, MEMKIND_ERROR_MESSAGE_SIZE);
  ARROW_LOG(INFO) << error_message;
}

int PlasmaAllocator::CreateMemkind(std::string &path) {
  ARROW_LOG(INFO) << "Creat pmem at " << path
                  << " size "
                  <<  footprint_limit_;
  int result = memkind_create_pmem(path.c_str(), footprint_limit_, &pmem_kind_);
  if (result) {
    print_err_message(result);
  }
  ARROW_CHECK(result == 0) << "Failed to create pmem";
  return result;
}

int PlasmaAllocator::DestroyMemkind() {
  int result = memkind_destroy_kind(pmem_kind_);
  ARROW_CHECK(result == 0) << "Failed to destroy pmem";
  return result;
}

void PlasmaAllocator::GetMemkindMapinfo(void* addr, int* fd,
                                        int64_t* map_size, ptrdiff_t* offset) {
  memkind_pmem_get_mmap_record(pmem_kind_, addr, fd,
                               reinterpret_cast<off_t *>(offset),
                               reinterpret_cast<size_t *>(map_size));
}

int64_t PlasmaAllocator::GetMemkindMmapSize(int fd) {
  return memkind_pmem_get_mmap_size(pmem_kind_, fd);
}
#endif

}  // namespace plasma
