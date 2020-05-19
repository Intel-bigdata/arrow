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

#include "plasma/tools/PlasmaProperties.h"
#include <mntent.h>
#include <sys/vfs.h>
#include <fstream>
#include <string>
#include "stdio.h"
#include <sstream>
#include "unistd.h"
#include "arrow/util/logging.h"

namespace plasma {

PlasmaProperties::PlasmaProperties(std::string argStr, std::string propertyFilePath) {
  this->argsStr = argsStr;
  this->propertyFilePath = propertyFilePath;
  uint32_t poolSize = sysconf(_SC_NPROCESSORS_ONLN) / numanodeInfos.size() / 2 / 2;
  defaultNumaNodeInfo.readPoolSize = poolSize;
  defaultNumaNodeInfo.writePoolSize = poolSize;
  defaultNumaNodeInfo.requiredSize = 5000000;
}

void PlasmaProperties::parseArgStr(std::string argStr) {
  argStr = argStr.substr(12, argStr.length());
  std::vector<std::string> numaNodeStrVector;
  split(argsStr,numaNodeStrVector,'?');
  for(std::string numaNodeStr: numaNodeStrVector){
    std::vector<std::string> numaNodeElementStrVector;
    split(numaNodeStr, numaNodeElementStrVector, ',');
    for(std::string element:numaNodeElementStrVector){
      int index = element.find(":");
      this->argsMap[element.substr(0,index)] = element.substr(index+1,element.length());
    }
  } 
}

void PlasmaProperties::parsePropertyFilePath(std::string propertyFilePath) {
  std::ifstream infile(propertyFilePath.c_str());
  if (!infile) return;
  std::string line, key, value;
  while (std::getline(infile, line)) {
    if (analyseLine(line, key, value)) {
      this->propertyFileMap[key] = value;
    }
  }
}

std::vector<plasma::numaNodeInfo>& PlasmaProperties::getNumaNodeInfos() {
  parseArgStr(argsStr);
  parsePropertyFilePath(propertyFilePath);
  if(!buildNumaNodeInfos()) {
    ARROW_LOG(FATAL)<< "InitialPath is not correct, please check.";
  }
  return this->numanodeInfos;
}

std::string PlasmaProperties::getProperty(std::string key){
  if(key.find("initialPath")!=key.npos) return "";
  if(this->argsMap.find(key)!=this->argsMap.end()) return argsMap[key];
  if(this->propertyFileMap.find(key)!=this->propertyFileMap.end()) return propertyFileMap[key];
  if(key == "totalNumaNodeNum") return "";
  return getDefaultProperty(key);
}

bool PlasmaProperties::buildNumaNodeInfos() {
  this->totalNumaNodeNum = std::stoi(getProperty("totalNumaNodeNum"));
  for(int i=1;i<=this->totalNumaNodeNum;i++){
     plasma::numaNodeInfo info;
     info.numaNodeId = std::stoul(this->getProperty("numaNodeId"+i));
     info.initialPath = this->getProperty("initialPath"+i);
     if(info.initialPath.length()==0){
       this->numanodeInfos.clear();
       return false;
     }
     info.readPoolSize =  std::stoul(this->getProperty("readPoolSize"+i));
     info.writePoolSize = std::stoul(this->getProperty("writePoolSize"+i));
     info.requiredSize = std::stoull(this->getProperty("requiredSize"+i));
     this->numanodeInfos.push_back(info);
  }
  return totalNumaNodeNum == (int)numanodeInfos.size();
}

std::string PlasmaProperties::getDefaultProperty(std::string key){
  if(key.find("numaNodeId")!=key.npos) return key.substr(9, key.length());
  else if(key.find("readPoolSize")!=key.npos) return std::to_string(defaultNumaNodeInfo.readPoolSize);
  else if(key.find("writePoolSize")!=key.npos) return std::to_string(defaultNumaNodeInfo.writePoolSize);
  else if(key.find("requiredSize")!=key.npos) return std::to_string(defaultNumaNodeInfo.requiredSize);
  else return "";
}

bool PlasmaProperties::isSpace(char c) {
  if (' ' == c || '\t' == c) return true;
  return false;
}

bool PlasmaProperties::isCommentChar(char c) {
  if (c == COMMENT_CHAR) {
    return true;
  } else {
    return false;
  }
}

void PlasmaProperties::trim(std::string& str) {
  if (str.empty()) {
    return;
  }
  int i, start_pos, end_pos;
  for (i = 0; i < (int)str.size(); ++i) {
    if (!isSpace(str[i])) {
      break;
    }
  }
  if (i == (int)str.size()) {
    str = "";
    return;
  }
  start_pos = i;
  for (i = (int)str.size() - 1; i >= 0; --i) {
    if (!isSpace(str[i])) {
      break;
    }
  }
  end_pos = i;
  str = str.substr(start_pos, end_pos - start_pos + 1);
}

bool PlasmaProperties::analyseLine(const std::string& line, std::string& key,
                                   std::string& value) {
  if (line.empty()) return false;
  int start_pos = 0, end_pos = line.size() - 1, pos;
  if ((pos = line.find(COMMENT_CHAR)) != -1) {
    if (0 == pos) {
      return false;
    }
    end_pos = pos - 1;
  }
  std::string new_line = line.substr(start_pos, start_pos + 1 - end_pos);

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

void PlasmaProperties::split(const std::string& s,std::vector<std::string>& sv,const char flag){
    sv.clear();
    std::istringstream iss(s);
    std::string temp;

    while (getline(iss, temp, flag)) {
        sv.push_back(temp);
    }
    return;
}
}  // namespace plasma