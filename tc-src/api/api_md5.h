/*
 * api_md5 —— /api/md5：按用户+文件 MD5 查是否已存在，用于秒传判断
 */
#ifndef _API_MD5_H_
#define _API_MD5_H_
#include <string>
using std::string;
;
int ApiMd5(string &url, string &post_data, string &str_json);
#endif // ! _API_MD5_H_
