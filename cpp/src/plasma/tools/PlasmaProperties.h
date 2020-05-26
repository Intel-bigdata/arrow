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

#ifndef PLASMA_PROPERTIES_H
#define PLASMA_PROPERTIES_H

#define COMMENT_CHAR '#'
#define DEFALUT_CONFIG_MULTIPLIER 0.8

#include <map>
#include <string>
#include <vector>

namespace plasma {

struct numaNodeInfo {
  std::string initialPath;
  uint32_t numaNodeId;
  uint32_t readPoolSize;
  uint32_t writePoolSize;
  uint64_t requiredSize;
};

class PlasmaProperties {
 public:
  PlasmaProperties(std::string& argStr_, std::string& propertyFilePath_);
  PlasmaProperties() = default;
  std::vector<plasma::numaNodeInfo>& getNumaNodeInfos();

 private:
  int totalNumaNodeNum = -1;
  std::string argsStr;
  std::string propertyFilePath;
  std::map<std::string, std::string> argsMap;
  std::map<std::string, std::string> propertyFileMap;
  std::vector<plasma::numaNodeInfo> numanodeInfos;
  plasma::numaNodeInfo defaultNumaNodeInfo;

  void split(const std::string& s, std::vector<std::string>& sv, const char flag);
  void parseArgStr(std::string argStr);
  void parsePropertyFilePath(std::string propertyFilePath);
  bool buildNumaNodeInfos();
  std::string getProperty(std::string key);

  bool analyseLine(std::string& line, std::string& key, std::string& value);
};

}  // namespace plasma

#endif
