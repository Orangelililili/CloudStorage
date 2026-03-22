/*
 * api_common.h / api_common.cc —— 各 API 模块共用的工具与初始化
 *
 * - ApiInit：服务启动时把 MySQL 中的统计类数据预热进 Redis（如共享文件总数）
 * - Cache*Count：对 Redis 中计数器做读写、自增自减（文件数、分享数等）
 * - 另见 api_common.cc：FastDFS 命令、字符串工具、DB 查询封装等
 */
#ifndef _API_COMMON_H_
#define _API_COMMON_H_
#include "cache_pool.h"
#include "db_pool.h"
#include "redis_keys.h"
#include "tc_common.h"
#include "dlog.h"
#include "json/json.h"
#include <string>

#define HTTP_RESPONSE_HTML_MAX 4096
#define HTTP_RESPONSE_HTML                                                     \
    "HTTP/1.1 200 OK\r\n"                                                      \
    "Connection:close\r\n"                                                     \
    "Content-Length:%d\r\n"                                                    \
    "Content-Type:application/json;charset=utf-8\r\n\r\n%s"
    
// 开启多线程
#define API_MYFILES_MUTIL_THREAD  1
#define API_LOGIN_MUTIL_THREAD  1

extern string s_dfs_path_client;
extern string s_storage_web_server_ip;
extern string s_storage_web_server_port;
extern string s_shorturl_server_address;
extern string s_shorturl_server_access_token;
using std::string;
int ApiInit();
//获取用户文件个数
int CacheSetCount(CacheConn *cache_conn, string key, int64_t count);
int CacheGetCount(CacheConn *cache_conn, string key, int64_t &count);
int CacheIncrCount(CacheConn *cache_conn, string key);
int CacheDecrCount(CacheConn *cache_conn, string key);
int DBGetUserFilesCountByUsername(CDBConn *db_conn, string user_name,
                                  int &count);
int DBGetShareFilesCount(CDBConn *db_conn, int &count);
int DBGetSharePictureCountByUsername(CDBConn *db_conn, string user_name,
                                     int &count);
int RemoveFileFromFastDfs(const char *fileid);
#endif