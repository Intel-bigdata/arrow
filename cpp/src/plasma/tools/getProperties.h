#ifndef _GET_PROPERTIES_H_
#define _GET_PROPERTIES_H_
#define COMMENT_CHAR '#'
#include <string>
#include <map>
 
using namespace std;
 
bool readConfig(const string & filename, map<string, string> & m);
 
#endif