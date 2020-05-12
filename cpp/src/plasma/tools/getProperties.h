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

#define _GET_PROPERTIES_H_
#define COMMENT_CHAR '#'
#define DEFALUT_CONFIG_MULTIPLIER 0.8
#include <mntent.h>
#include <sys/vfs.h>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include "../plasma/vmemcache_store.h"
#include "arrow/util/logging.h"

using namespace std;

class getProperties {
 public:
  static bool readConfig(const string& filename, map<string, string>& configMap);
  static std::vector<plasma::numaNodeInfo> convertConfigMapToNumaNodeInfo(
      map<string, string>& configMap);
  static bool getDefaultConfig(std::vector<plasma::numaNodeInfo>& numanodeInfos);

 private:
  static bool analyseLine(const string& line, string& key, string& value);
  static void trim(string& str);
  static bool isCommentChar(char c);
  static bool isSpace(char c);
};

bool getProperties::getDefaultConfig(std::vector<plasma::numaNodeInfo>& numanodeInfos) {
  FILE* mount_table = NULL;
  struct mntent* mount_entry;
  mount_table = setmntent("/etc/mtab", "r");
  if (!mount_table) {
    ARROW_LOG(FATAL) << "set mount entry error";
    return false;
  }
  int numaNodeId = 0;
  while (1) {
    if (mount_table) {
      mount_entry = getmntent(mount_table);
      if (!mount_entry) {
        endmntent(mount_table);
        break;
      }
    }
    std::string mount_point = mount_entry->mnt_dir;
    struct statfs pathInfo;
    if (mount_point.find("/mnt/pmem") != -1) {
      if (statfs(mount_point.c_str(), &pathInfo) < 0) {
        return false;
      }
      plasma::numaNodeInfo info;
      info.numaNodeId = numaNodeId;
      info.initialPath = mount_point;
      info.readPoolSize = 12;
      info.writePoolSize = 12;
      info.requiredSize = pathInfo.f_bsize * pathInfo.f_bfree * DEFALUT_CONFIG_MULTIPLIER;
      numanodeInfos.push_back(info);
      numaNodeId += 1;
    }
  }
  return numanodeInfos.size() > 0;
}

std::vector<plasma::numaNodeInfo> getProperties::convertConfigMapToNumaNodeInfo(
    map<string, string>& configMap) {
  std::vector<plasma::numaNodeInfo> res;
  int numanodeNum = configMap.size() / 5;
  for (int i = 1; i <= numanodeNum; i++) {
    plasma::numaNodeInfo info;
    info.requiredSize = std::stoull(configMap["requiredSize" + std::to_string(i)]);
    info.initialPath = configMap["initialPath" + std::to_string(i)];
    info.numaNodeId = std::stoul(configMap["numanodeId" + std::to_string(i)]);
    info.readPoolSize = std::stoul(configMap["readPoolSize" + std::to_string(i)]);
    info.writePoolSize = std::stoul(configMap["writePoolSize" + std::to_string(i)]);
    res.push_back(info);
  }
  return res;
}

bool getProperties::readConfig(const string& filename, map<string, string>& configMap) {
  configMap.clear();
  ifstream infile(filename.c_str());
  if (!infile) {
    return false;
  }
  string line, key, value;
  while (getline(infile, line)) {
    if (analyseLine(line, key, value)) {
      configMap[key] = value;
    }
  }
  infile.close();
  return true;
}

bool getProperties::isSpace(char c) {
  if (' ' == c || '\t' == c) return true;
  return false;
}

bool getProperties::isCommentChar(char c) {
  if (c == COMMENT_CHAR) {
    return true;
  } else {
    return false;
  }
}

void getProperties::trim(string& str) {
  if (str.empty()) {
    return;
  }
  int i, start_pos, end_pos;
  for (i = 0; i < str.size(); ++i) {
    if (!isSpace(str[i])) {
      break;
    }
  }
  if (i == str.size()) {
    str = "";
    return;
  }
  start_pos = i;
  for (i = str.size() - 1; i >= 0; --i) {
    if (!isSpace(str[i])) {
      break;
    }
  }
  end_pos = i;
  str = str.substr(start_pos, end_pos - start_pos + 1);
}

bool getProperties::analyseLine(const string& line, string& key, string& value) {
  if (line.empty()) return false;
  int start_pos = 0, end_pos = line.size() - 1, pos;
  if ((pos = line.find(COMMENT_CHAR)) != -1) {
    if (0 == pos) {
      return false;
    }
    end_pos = pos - 1;
  }
  string new_line = line.substr(start_pos, start_pos + 1 - end_pos);

  if ((pos = new_line.find('=')) == -1) return false;

  key = new_line.substr(0, pos);
  value = new_line.substr(pos + 1, end_pos + 1 - (pos + 1));

  trim(key);
  if (key.empty()) {
    return false;
  }
  trim(value);
  return true;
}
