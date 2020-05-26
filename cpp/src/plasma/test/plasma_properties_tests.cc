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

#include <gtest/gtest.h>
#include "arrow/testing/gtest_util.h"
#include "arrow/util/io_util.h"

#include "plasma/plasma.h"
#include "plasma/protocol.h"
#include "plasma/test_util.h"
#include "plasma/tools/PlasmaProperties.h"

namespace plasma {

using arrow::internal::TemporaryDir;

std::string properties_test_executable;  // NOLINT

class TestPlasmaProperties : public ::testing::Test {
 public:
  void SetUp() override {}

 protected:
  std::unique_ptr<TemporaryDir> temp_dir;
  std::string store_socket_name_;
};

TEST_F(TestPlasmaProperties, ParseArgStrTest) {
  std::string propertyStr = "";
  std::string argStr =
      "totalNumaNodeNum:2,numaNodeId1:1,initialPath1:/mnt/"
      "pmem0,requiredSize1:15000000,readPoolSize1:12,writePoolSize1:12?numaNodeId2:2,"
      "initialPath2:/mnt/pmem1,requiredSize2:15000000,readPoolSize2:12,writePoolSize2:12";
  PlasmaProperties* p = new PlasmaProperties(argStr, propertyStr);
  std::vector<plasma::numaNodeInfo> vector = p->getNumaNodeInfos();
  ASSERT_EQ(vector[0].initialPath, "/mnt/pmem0");
  ASSERT_EQ(vector[0].numaNodeId, 1);
  ASSERT_EQ(vector[0].readPoolSize, 12);
  ASSERT_EQ(vector[0].writePoolSize, 12);
  ASSERT_EQ(vector[0].requiredSize, 15000000);

  ASSERT_EQ(vector[1].initialPath, "/mnt/pmem1");
  ASSERT_EQ(vector[1].numaNodeId, 2);
  ASSERT_EQ(vector[1].readPoolSize, 12);
  ASSERT_EQ(vector[1].writePoolSize, 12);
  ASSERT_EQ(vector[1].requiredSize, 15000000);
}

TEST_F(TestPlasmaProperties, ParsePropertyFilePathTest) {
  std::string currDir = get_current_dir_name();
  int index = currDir.find("cpp");
  currDir = currDir.substr(0, index + 3);
  std::string propertyStr =
      currDir + "/src/plasma/test/test-persistent-memory.properties";
  std::string argStr = "";
  PlasmaProperties* p = new PlasmaProperties(argStr, propertyStr);
  std::vector<plasma::numaNodeInfo> vector;
  vector = p->getNumaNodeInfos();
  ASSERT_EQ(vector[0].initialPath, "/mnt/pmem0");
  ASSERT_EQ(vector[0].numaNodeId, 1);
  ASSERT_EQ(vector[0].readPoolSize, 12);
  ASSERT_EQ(vector[0].writePoolSize, 12);
  ASSERT_EQ(vector[0].requiredSize, 15000000);

  ASSERT_EQ(vector[1].initialPath, "/mnt/pmem1");
  ASSERT_EQ(vector[1].numaNodeId, 2);
  ASSERT_EQ(vector[1].readPoolSize, 12);
  ASSERT_EQ(vector[1].writePoolSize, 12);
  ASSERT_EQ(vector[1].requiredSize, 15000000);
}

TEST_F(TestPlasmaProperties, ParsePropertyFilePathAndParseArgStrTest) {
  std::string currDir = get_current_dir_name();
  int index = currDir.find("cpp");
  currDir = currDir.substr(0, index + 3);
  std::string propertyStr =
      currDir + "/src/plasma/test/test-persistent-memory.properties";
  std::string argStr =
      "totalNumaNodeNum:2,numaNodeId1:1,initialPath1:/mnt/"
      "pmem0?numaNodeId2:2,initialPath2:/mnt/pmem1,requiredSize2:15000000";
  PlasmaProperties* p = new PlasmaProperties(argStr, propertyStr);
  std::vector<plasma::numaNodeInfo> vector = p->getNumaNodeInfos();
  ASSERT_EQ(vector[0].initialPath, "/mnt/pmem0");
  ASSERT_EQ(vector[0].numaNodeId, 1);
  ASSERT_EQ(vector[0].readPoolSize, 12);
  ASSERT_EQ(vector[0].writePoolSize, 12);
  ASSERT_EQ(vector[0].requiredSize, 15000000);

  ASSERT_EQ(vector[1].initialPath, "/mnt/pmem1");
  ASSERT_EQ(vector[1].numaNodeId, 2);
  ASSERT_EQ(vector[1].readPoolSize, 12);
  ASSERT_EQ(vector[1].writePoolSize, 12);
  ASSERT_EQ(vector[1].requiredSize, 15000000);
}

}  // namespace plasma

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  plasma::properties_test_executable = std::string(argv[0]);
  return RUN_ALL_TESTS();
}
