/*
 * ConfigFileReader.cpp
 *
 *  Created on: 2013-7-2
 *      Author: ziteng@mogujie.com
 */
//解析和读取配置文件（key=value 格式），提供读取和写入配置项的功能。

#include "config_file_reader.h"
CConfigFileReader::CConfigFileReader(const char *filename) {
    _LoadFile(filename);
}

CConfigFileReader::~CConfigFileReader() {}

char *CConfigFileReader::GetConfigName(const char *name) {
    if (!load_ok_)
        return NULL;

    char *value = NULL;
    map<string, string>::iterator it = config_map_.find(name);
    if (it != config_map_.end()) {
        value = (char *)it->second.c_str();
    }

    return value;
}

int CConfigFileReader::SetConfigValue(const char *name, const char *value) {
    if (!load_ok_)
        return -1;

    map<string, string>::iterator it = config_map_.find(name);
    if (it != config_map_.end()) {
        it->second = value;
    } else {
        config_map_.insert(make_pair(name, value));
    }
    return _WriteFIle();
}
void CConfigFileReader::_LoadFile(const char *filename) {
    config_file_.clear();
    config_file_.append(filename);
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        printf("can not open %s,errno = %d", filename, errno);
        return;
    }

    char buf[256];
    for (;;) {//读取文件内容
        char *p = fgets(buf, 256, fp);
        if (!p)
            break;

        size_t len = strlen(buf);
        if (buf[len - 1] == '\n')
            buf[len - 1] = 0; // remove \n at the end

        char *ch = strchr(buf, '#'); // remove string start with #
        if (ch)
            *ch = 0;

        if (strlen(buf) == 0)
            continue;

        _ParseLine(buf);//解析单行
    }

    fclose(fp);
    load_ok_ = true;
}
/* -------------------------------------------*/
/**
 * @brief  写入配置文件
 *
 * @param filename 文件名
 *
 * @returns
 *      0 succ, -1 fail
 */
/* -------------------------------------------*/
int CConfigFileReader::_WriteFIle(const char *filename) {
    FILE *fp = NULL;
    if (filename == NULL) {
        fp = fopen(config_file_.c_str(), "w");
    } else {
        fp = fopen(filename, "w");
    }
    if (fp == NULL) {
        return -1;
    }

    char szPaire[128];
    map<string, string>::iterator it = config_map_.begin();
    for (; it != config_map_.end(); it++) {
        memset(szPaire, 0, sizeof(szPaire));//清空szPaire
        snprintf(szPaire, sizeof(szPaire), "%s=%s\n", 
                    it->first.c_str(),//key（如 "HttpPort"）
                 it->second.c_str());//value（如 "8080"）
                //"HttpPort=8080\n"
        uint32_t ret = fwrite(szPaire, strlen(szPaire), 1, fp);//写入文件
        if (ret != 1) {//写入失败
            fclose(fp);//关闭文件
            return -1;
        }
    }
    fclose(fp);//关闭文件
    return 0;
}
/* -------------------------------------------*/
/**
 * @brief  解析单行
 *
 * @param line 单行内容
 *
 * @returns
 *      0 succ, -1 fail
 */
/* -------------------------------------------*/
void CConfigFileReader::_ParseLine(char *line) {
    char *p = strchr(line, '=');
    if (p == NULL)
        return;

    *p = 0;
    char *key = _TrimSpace(line);//去除空格和制表符
    char *value = _TrimSpace(p + 1);//去除空格和制表符
    if (key && value) {
        config_map_.insert(make_pair(key, value));//插入配置项
    }
}
/* -------------------------------------------*/
/**
 * @brief  去除空格和制表符
 *
 * @param name 字符串
 *
 * @returns
 *      0 succ, -1 fail
 */
/* -------------------------------------------*/
char *CConfigFileReader::_TrimSpace(char *name) {
    // remove starting space or tab
    char *start_pos = name;
    while ((*start_pos == ' ') || (*start_pos == '\t')) {
        start_pos++;
    }

    if (strlen(start_pos) == 0)
        return NULL;

    // remove ending space or tab
    char *end_pos = name + strlen(name) - 1;
    while ((*end_pos == ' ') || (*end_pos == '\t')) {
        *end_pos = 0;
        end_pos--;
    }

    int len = (int)(end_pos - start_pos) + 1;
    if (len <= 0)
        return NULL;

    return start_pos;
}
