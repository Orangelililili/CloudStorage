/*
 * api_sharepicture —— /api/sharepic：生成带 token 的图片分享页链接、拉取分享详情等
 */
#ifndef _API_SHAREPICTURE_H_
#define _API_SHAREPICTURE_H_
#include <string>
using std::string;
;
int ApiSharepicture(string &url, string &post_data, string &str_json);

#endif