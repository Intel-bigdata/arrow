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


#ifndef PLASMAPROPERTIES_H
#define PLASMAPROPERTIES_H
#define COMMENT_CHAR '#'
#define DEFALUT_CONFIG_MULTIPLIER 0.8

#include <map>
#include <string>
#include <vector>
#include "../plasma/vmemcache_store.h"

namespace plasma {

class PlasmaProperties {
 public:
  static bool parseConfig(const std::string& filename,
                         std::map<std::string, std::string>& configMap);
  static std::vector<plasma::numaNodeInfo> convertConfigMapToNumaNodeInfo(
      std::map<std::string, std::string>& configMap);
  static bool getDefaultConfig(std::vector<plasma::numaNodeInfo>& numanodeInfos);

 private:
  static bool analyseLine(const std::string& line, std::string& key, std::string& value);
  static void trim(std::string& str);
  static bool isCommentChar(char c);
  static bool isSpace(char c);
};

}  // namespace plasma

#endif