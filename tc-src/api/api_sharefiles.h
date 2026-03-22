/*
 * api_sharefiles —— /api/sharefiles：公共文件广场列表、下载排行等
 */
#ifndef _API_SHAREFILES_H_
#define _API_SHAREFILES_H_
#include <string>
using std::string;
;
int ApiSharefiles(string &url, string &post_data, string &str_json);

#endif