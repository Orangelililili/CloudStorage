/*
 * api_dealfile —— /api/dealfile：个人文件的删除、下载记录等（具体 action 见实现与前端约定）
 */
#ifndef _API_DEALFILE_H_
#define _API_DEALFILE_H_
#include <string>
using std::string;
;
int ApiDealfile(string &url, string &post_data, string &str_json);
int ApiDealfileInit(char *dfs_path_client);

#endif