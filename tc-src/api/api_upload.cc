// api_upload.cc —— /api/upload：读 nginx 落盘路径 → FastDFS storage_upload → 写 MySQL → 可选短链（见头文件说明）
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <string>

// 必须在 fdfs_client.h（fastcommon）之前：shared_func.h 的 #define byte 会破坏 protobuf 头
#include <grpcpp/grpcpp.h>
#include "shorturl.pb.h"
#include "shorturl.grpc.pb.h"

#include "fdfs_client.h"

#include "api_common.h"
#include "api_upload.h"

// grpc 远程调用
using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using shorturl_voice::ShortUrl;     //服务
using shorturl_voice::Url;
using shorturl_voice::ShortKey;

/**
 * 开发环境经 Python 反代时，浏览器直接 POST multipart（字段 file、user、md5、size），
 * 无 Nginx upload 模块。本函数解析后写入临时文件并加后缀，后续仍走 uploadFileToFastDfs。
 */
static bool mp_disp_name(const std::string &hdr, std::string *name_out) {
    size_t i = hdr.find("name=\"");
    if (i == std::string::npos) {
        return false;
    }
    i += 6;
    size_t j = hdr.find('"', i);
    if (j == std::string::npos) {
        return false;
    }
    *name_out = hdr.substr(i, j - i);
    return true;
}

static bool mp_disp_filename(const std::string &hdr, std::string *fn_out) {
    size_t i = hdr.find("filename=\"");
    if (i != std::string::npos) {
        i += 10;
        size_t j = hdr.find('"', i);
        if (j == std::string::npos) {
            return false;
        }
        *fn_out = hdr.substr(i, j - i);
        return true;
    }
    i = hdr.find("filename=");
    if (i == std::string::npos) {
        return false;
    }
    i += 9;
    while (i < hdr.size() && hdr[i] == ' ') {
        i++;
    }
    if (i < hdr.size() && hdr[i] == '"') {
        i++;
        size_t j = hdr.find('"', i);
        *fn_out = hdr.substr(i, j - i);
    } else {
        size_t j = hdr.find_first_of(";\r\n", i);
        *fn_out = hdr.substr(i, j - i);
    }
    return true;
}

static void mp_trim_crlf(std::string *s) {
    while (s->size() >= 2 && s->compare(s->size() - 2, 2, "\r\n") == 0) {
        s->resize(s->size() - 2);
    }
    while (!s->empty() &&
           ((*s)[s->size() - 1] == '\n' || (*s)[s->size() - 1] == '\r')) {
        s->pop_back();
    }
}

/** 成功返回 0，并写入 new_file_path（带后缀的临时文件，调用方负责 unlink） */
static int parse_browser_multipart_to_temp(const std::string &post_data,
                                           char *file_name, size_t file_name_len,
                                           char *file_md5, size_t file_md5_len,
                                           long *long_file_size, char *user,
                                           size_t user_len, char *new_file_path,
                                           size_t new_file_path_len) {
    size_t le = post_data.find("\r\n");
    if (le == std::string::npos) {
        LogError("browser upload: no first line");
        return -1;
    }
    const std::string bline = post_data.substr(0, le);
    if (bline.size() < 2 || bline.compare(0, 2, "--") != 0) {
        LogError("browser upload: bad boundary");
        return -1;
    }
    const std::string sep = "\r\n" + bline;
    const std::string sep_end = "\r\n" + bline + "--";

    std::string f_user, f_md5, f_size, orig_fn, f_bytes;
    size_t cur = le + 2;
    while (cur < post_data.size()) {
        size_t hdr_end = post_data.find("\r\n\r\n", cur);
        if (hdr_end == std::string::npos) {
            break;
        }
        const std::string headers = post_data.substr(cur, hdr_end - cur);
        size_t body_start = hdr_end + 4;
        size_t next_sep = post_data.find(sep, body_start);
        size_t part_end;
        bool has_next;
        if (next_sep != std::string::npos) {
            part_end = next_sep;
            has_next = true;
        } else {
            size_t em = post_data.find(sep_end, body_start);
            part_end = (em == std::string::npos) ? post_data.size() : em;
            has_next = false;
        }
        std::string body = post_data.substr(body_start, part_end - body_start);
        std::string pname;
        if (mp_disp_name(headers, &pname)) {
            if (pname == "file") {
                if (!mp_disp_filename(headers, &orig_fn)) {
                    orig_fn = "upload.bin";
                }
                f_bytes = std::move(body);
            } else if (pname == "user") {
                mp_trim_crlf(&body);
                f_user = std::move(body);
            } else if (pname == "md5") {
                mp_trim_crlf(&body);
                f_md5 = std::move(body);
            } else if (pname == "size") {
                mp_trim_crlf(&body);
                f_size = std::move(body);
            }
        }
        if (!has_next) {
            break;
        }
        cur = next_sep + sep.size();
    }
    if (f_user.empty() || f_md5.empty() || orig_fn.empty() || f_bytes.empty()) {
        LogError("browser upload: need file, user, md5 and filename");
        return -1;
    }

    strncpy(file_name, orig_fn.c_str(), file_name_len - 1);
    file_name[file_name_len - 1] = '\0';
    strncpy(file_md5, f_md5.c_str(), file_md5_len - 1);
    file_md5[file_md5_len - 1] = '\0';
    strncpy(user, f_user.c_str(), user_len - 1);
    user[user_len - 1] = '\0';

    *long_file_size = static_cast<long>(f_bytes.size());
    if (!f_size.empty()) {
        *long_file_size = strtol(f_size.c_str(), nullptr, 10);
    }

    char suffix[SUFFIX_LEN] = {0};
    GetFileSuffix(file_name, suffix);

    char tmpl[] = "/tmp/tuchuang_upXXXXXX";
    int tfd = mkstemp(tmpl);
    if (tfd < 0) {
        LogError("mkstemp failed: {}", strerror(errno));
        return -1;
    }
    ssize_t w = write(tfd, f_bytes.data(), f_bytes.size());
    close(tfd);
    if (w != static_cast<ssize_t>(f_bytes.size())) {
        LogError("write temp upload incomplete");
        unlink(tmpl);
        return -1;
    }
    snprintf(new_file_path, new_file_path_len, "%s.%s", tmpl, suffix);
    if (rename(tmpl, new_file_path) != 0) {
        LogError("rename temp upload: {}", strerror(errno));
        unlink(tmpl);
        return -1;
    }
    return 0;
}

//短链服务客户端
class ShortUrlClient {
 public:
  ShortUrlClient(std::shared_ptr<Channel> channel)
      : stub_(ShortUrl::NewStub(channel)) {}

  
  int GetShortUrl(const std::string& url, const bool is_public, std::string &short_key) {
    // Data we are sending to the server.
    Url request;
    request.set_url(url);
    request.set_ispublic(is_public);
 
    // Container for the data we expect from the server.
    Url reply;

    // Context for the client. It could be used to convey extra information to
    // the server and/or tweak certain RPC behaviors.
    ClientContext context;
    string meta_key = "authorization";  // 自定义，目前和shorurl-server是写的固定key
    context.AddMetadata(meta_key, s_shorturl_server_access_token);
    // The actual RPC.
    Status status = stub_->GetShortUrl(&context, request, &reply);

    // Act upon its status.
    if (status.ok()) {
        short_key = reply.url();   
        return 0;
    } else {
        std::cout << status.error_code() << ": " << status.error_message() << std::endl;
        return -1;
    }
    return 0;
  }

 private:
  std::unique_ptr<ShortUrl::Stub> stub_;
};

/// @brief 将长链转成短链
/// @param origin_url 
/// @param short_url 
/// @return 
int originUrl2ShortUrl(const string &origin_url, string &short_url)
{
    // sksSdkFdDngKie8n05nr9jey84prEhw5u43th0yi294780yjr3h7
    ShortUrlClient client(grpc::CreateChannel(s_shorturl_server_address, grpc::InsecureChannelCredentials()));

    int ret = client.GetShortUrl(origin_url, true, short_url);
    LogInfo("origin_url = {}", origin_url);
    if(ret == 0) {
        LogInfo("short_url = {}", short_url);
    }
    else {
        LogError("get short_url failed");
    }
    return ret;
}


/* -------------------------------------------*/
/**
 * @brief  将一个本地文件上传到 后台分布式文件系统中
 * 对应 fdfs_upload_file /etc/fdfs/client.conf  完整文件路径
 *
 * @param file_path  (in) 本地文件的路径
 * @param fileid    (out)得到上传之后的文件ID路径
 *
 * @returns
 *      0 succ, -1 fail
 */
/* -------------------------------------------*/
int uploadFileToFastDfs(char *file_path, char *fileid) {
    int ret = 0;

    pid_t pid;
    int fd[2];

    //无名管道的创建
    if (pipe(fd) < 0) // fd[0] → r； fd[1] → w  获取上传后返回的信息 fileid
    {
        LogError("pipe error");
        ret = -1;
        goto END;
    }

    //创建进程
    pid = fork(); // 
    if (pid < 0)  //进程创建失败
    {
        LogError("fork error");
        ret = -1;
        goto END;
    }

    if (pid == 0) //子进程
    {
        //关闭读端
        close(fd[0]);
        //将标准输出 重定向 写管道
        dup2(fd[1],
             STDOUT_FILENO); // 往标准输出写的东西都会重定向到fd所指向的文件,
                             // 当fileid产生时输出到管道fd[1]
        // fdfs_upload_file /etc/fdfs/client.conf 123.txt
        // printf("fdfs_upload_file %s %s %s\n", fdfs_cli_conf_path, filename,
        // file_path);
        //通过execlp执行fdfs_upload_file
        //如果函数调用成功,进程自己的执行代码就会变成加载程序的代码,execlp()后边的代码也就不会执行了.
        execlp("fdfs_upload_file", "fdfs_upload_file",
               s_dfs_path_client.c_str(), file_path, NULL); //
        // 执行正常不会跑下面的代码
        //执行失败
        LogError("execlp fdfs_upload_file error");

        close(fd[1]);
    } else //父进程
    {
        //关闭写端
        close(fd[1]);

        //从管道中去读数据
        read(fd[0], fileid, TEMP_BUF_MAX_LEN); // 等待管道写入然后读取

        LogInfo("fileid1: {}", fileid);
        //去掉一个字符串两边的空白字符
        TrimSpace(fileid);

        if (strlen(fileid) == 0) {
            LogError("upload failed");
            ret = -1;
            goto END;
        }
        LogInfo("fileid2: {}", fileid);

        wait(NULL); //等待子进程结束，回收其资源
        close(fd[0]);
    }

END:
    return ret;
}

/* -------------------------------------------*/
/**
 * @brief  封装文件存储在分布式系统中的 完整 url
 *
 * @param fileid        (in)    文件分布式id路径
 * @param fdfs_file_url (out)   文件的完整url地址
 *
 * @returns
 *      0 succ, -1 fail
 */
/* -------------------------------------------*/
int getFullurlByFileid(char *fileid, char *fdfs_file_url) {
    int ret = 0;

    char *p = NULL;
    char *q = NULL;
    char *k = NULL;

    char fdfs_file_stat_buf[TEMP_BUF_MAX_LEN] = {0};
    char fdfs_file_host_name[HOST_NAME_LEN] = {0}; // storage所在服务器ip地址

    pid_t pid;
    int fd[2];

    //无名管道的创建
    if (pipe(fd) < 0) {
        LogError("pipe error");
        ret = -1;
        goto END;
    }

    //创建进程
    pid = fork();
    if (pid < 0) //进程创建失败
    {
        LogError("fork error");
        ret = -1;
        goto END;
    }

    if (pid == 0) //子进程
    {
        //关闭读端
        close(fd[0]);

        //将标准输出 重定向 写管道
        dup2(fd[1], STDOUT_FILENO); // dup2(fd[1], 1);

        execlp("fdfs_file_info", "fdfs_file_info", s_dfs_path_client.c_str(),
               fileid, NULL);

        //执行失败
        LogError("execlp fdfs_file_info error");

        close(fd[1]);
    } else //父进程
    {
        //关闭写端
        close(fd[1]);

        //从管道中去读数据
        read(fd[0], fdfs_file_stat_buf, TEMP_BUF_MAX_LEN);
        ;

        wait(NULL); //等待子进程结束，回收其资源
        close(fd[0]);
        // LogInfo("fdfs_file_stat_buf: {}", fdfs_file_stat_buf);
        //拼接上传文件的完整url地址--->http://host_name/group1/M00/00/00/D12313123232312.png
        p = strstr(fdfs_file_stat_buf, "source ip address: ");

        q = p + strlen("source ip address: ");
        k = strstr(q, "\n");

        strncpy(fdfs_file_host_name, q, k - q);
        fdfs_file_host_name[k - q] =
            '\0'; // 这里这个获取回来只是局域网的ip地址，在讲fastdfs原理的时候再继续讲这个问题

        LogInfo("host_name:{}, fdfs_file_host_name: {}", s_storage_web_server_ip, fdfs_file_host_name);

        // storage_web_server服务器的端口
        strcat(fdfs_file_url, "http://");
        strcat(fdfs_file_url, s_storage_web_server_ip.c_str());
        strcat(fdfs_file_url, ":");
        strcat(fdfs_file_url, s_storage_web_server_port.c_str());
        strcat(fdfs_file_url, "/");
        strcat(fdfs_file_url, fileid);

        LogInfo("fdfs_file_url:{}", fdfs_file_url);
    }

END:

    return ret;
}
/* -------------------------------------------*/
/**
 * @brief  将文件信息存入mysql中
 *
 * @param db_conn 数据库连接
 * @param cache_conn redis连接
 * @param user 用户名
 * @param filename 文件名
 * @param md5 文件md5
 * @param size 文件大小
 * @param fileid 文件id
 * @param fdfs_file_url 文件url
 *
 * @returns
 *      0 succ, -1 fail
 */
/* -------------------------------------------*/
int storeFileinfo(CDBConn *db_conn, CacheConn *cache_conn, char *user,
                  char *filename, char *md5, long size, char *fileid,
                  const char *fdfs_file_url) {
    int ret = 0;
    time_t now;
    char create_time[TIME_STRING_LEN];
    char suffix[SUFFIX_LEN];
    char sql_cmd[SQL_MAX_LEN] = {0};

    //得到文件后缀字符串 如果非法文件后缀,返回"null"
    GetFileSuffix(filename, suffix); // mp4, jpg, png

    // sql 语句
    /*
       -- =============================================== 文件信息表
       -- md5 文件md5
       -- file_id 文件id
       -- url 文件url
       -- size 文件大小, 以字节为单位
       -- type 文件类型： png, zip, mp4……
       -- count 文件引用计数， 默认为1， 每增加一个用户拥有此文件，此计数器+1
       */
    sprintf(sql_cmd,
            "insert into file_info (md5, file_id, url, size, type, count) "
            "values ('%s', '%s', '%s', '%ld', '%s', %d)",
            md5, fileid, fdfs_file_url, size, suffix, 1);
    // LogInfo("执行: {}", sql_cmd);
    if (!db_conn->ExecuteCreate(sql_cmd)) //执行sql语句
    {
        LogError("{} 操作失败", sql_cmd);
        ret = -1;
        goto END;
    }

    //获取当前时间
    now = time(NULL);
    strftime(create_time, TIME_STRING_LEN - 1, "%Y-%m-%d %H:%M:%S",
             localtime(&now));

    /*
       -- =============================================== 用户文件列表
       -- user 文件所属用户
       -- md5 文件md5
       -- create_time 文件创建时间
       -- file_name 文件名字
       -- shared_status 共享状态, 0为没有共享， 1为共享
       -- pv 文件下载量，默认值为0，下载一次加1
       */
    // sql语句
    sprintf(sql_cmd,
            "insert into user_file_list(user, md5, create_time, file_name, "
            "shared_status, pv) values ('%s', '%s', '%s', '%s', %d, %d)",
            user, md5, create_time, filename, 0, 0);
    // LogInfo("执行: {}", sql_cmd);
    if (!db_conn->ExecuteCreate(sql_cmd)) {
        LogError("{} 操作失败", sql_cmd);
        ret = -1;
        goto END;
    }

    // 询用户文件数量+1      web热点 大明星  微博存在缓存里面。
    if (CacheIncrCount(cache_conn, string(user)) < 0) {
        LogError(" CacheIncrCount 操作失败");
    }

END:
    return ret;
}
/* -------------------------------------------*/
/**
 * @brief  初始化上传文件相关配置
 *
 * @param dfs_path_client 分布式文件系统路径
 * @param storage_web_server_ip storage服务器ip
 * @param storage_web_server_port storage服务器端口
 * @param shorturl_server_address 短链服务器地址
 * @param access_token 短链服务器访问token
 *
 * @returns
 *      0 succ, -1 fail
 */
/* -------------------------------------------*/
int ApiUploadInit(const char *dfs_path_client, 
                    const char *storage_web_server_ip, const char *storage_web_server_port, 
                  const char *shorturl_server_address, const char *access_token) {
    s_dfs_path_client = dfs_path_client;
    s_storage_web_server_ip = storage_web_server_ip;
    s_storage_web_server_port = storage_web_server_port;
    s_shorturl_server_address = shorturl_server_address;
    s_shorturl_server_access_token = access_token;
    return 0;
}

/* -------------------------------------------*/
/**
 * @brief  上传文件
 *
 * @param url 请求url
 * @param post_data 请求数据
 * @param str_json 返回json
 *
 * @returns
 *      0 succ, -1 fail
 */
/* -------------------------------------------*/
int ApiUpload(string &url, string &post_data, string &str_json) {
    UNUSED(url);

    char suffix[SUFFIX_LEN] = {0};
    char fileid[TEMP_BUF_MAX_LEN] = {0}; //文件上传到fastDFS后的文件id
    char fdfs_file_url[FILE_URL_LEN] = {0}; //文件所存放storage的host_name
    int ret = 0;
    char file_name[128] = {0};
    char new_file_path[512] = {0};
    char file_md5[128] = {0};
    char file_size[32] = {0};
    long long_file_size = 0;
    char user[32] = {0};
    string short_url;   // 保存短链
    string origin_url;    //原始链接
    Json::Value value;

    // 获取数据库连接
    CDBManager *db_manager = CDBManager::getInstance();
    CDBConn *db_conn = db_manager->GetDBConn("tuchuang_slave"); // 连接池可以配置多个 分库
    AUTO_REL_DBCONN(db_manager, db_conn);
    CacheManager *cache_manager = CacheManager::getInstance();
    CacheConn *cache_conn = cache_manager->GetCacheConn("token");
    AUTO_REL_CACHECONN(cache_manager, cache_conn); //自动规划连接 -》连接池

    LogInfo("upload post_data len={}", post_data.size());

    if (strstr(post_data.c_str(), "name=\"file_path\"") != nullptr) {
        // Nginx ngx_http_upload_module 改写后的字段（生产典型路径）
        char boundary[TEMP_BUF_MAX_LEN] = {0};
        char file_content_type[128] = {0};
        char file_path[128] = {0};
        char *begin = (char *)post_data.c_str();
        char *p1, *p2;

        p1 = strstr(begin, "\r\n");
        if (p1 == NULL) {
            LogError("wrong no boundary!");
            ret = -1;
            goto END;
        }
        strncpy(boundary, begin, p1 - begin);
        boundary[p1 - begin] = '\0';
        LogInfo("boundary: {}", boundary);

        begin = p1 + 2;
        p2 = strstr(begin, "name=\"file_name\"");
        if (!p2) {
            LogError("wrong no file_name!");
            ret = -1;
            goto END;
        }
        p2 = strstr(begin, "\r\n");
        p2 += 4;
        begin = p2;
        p2 = strstr(begin, "\r\n");
        strncpy(file_name, begin, p2 - begin);
        file_name[p2 - begin] = '\0';
        LogInfo("file_name: {}", file_name);

        begin = p2 + 2;
        p2 = strstr(begin, "name=\"file_content_type\"");
        if (!p2) {
            LogError("wrong no file_content_type!");
            ret = -1;
            goto END;
        }
        p2 = strstr(p2, "\r\n");
        p2 += 4;
        begin = p2;
        p2 = strstr(begin, "\r\n");
        strncpy(file_content_type, begin, p2 - begin);
        file_content_type[p2 - begin] = '\0';
        LogInfo("file_content_type: {}", file_content_type);

        begin = p2 + 2;
        p2 = strstr(begin, "name=\"file_path\"");
        if (!p2) {
            LogError("wrong no file_path!");
            ret = -1;
            goto END;
        }
        p2 = strstr(p2, "\r\n");
        p2 += 4;
        begin = p2;
        p2 = strstr(begin, "\r\n");
        strncpy(file_path, begin, p2 - begin);
        file_path[p2 - begin] = '\0';
        LogInfo("file_path: {}", file_path);

        begin = p2 + 2;
        p2 = strstr(begin, "name=\"file_md5\"");
        if (!p2) {
            LogError("wrong no file_md5!");
            ret = -1;
            goto END;
        }
        p2 = strstr(p2, "\r\n");
        p2 += 4;
        begin = p2;
        p2 = strstr(begin, "\r\n");
        strncpy(file_md5, begin, p2 - begin);
        file_md5[p2 - begin] = '\0';
        LogInfo("file_md5: {}", file_md5);

        begin = p2 + 2;
        p2 = strstr(begin, "name=\"file_size\"");
        if (!p2) {
            LogError("wrong no file_size!");
            ret = -1;
            goto END;
        }
        p2 = strstr(p2, "\r\n");
        p2 += 4;
        begin = p2;
        p2 = strstr(begin, "\r\n");
        strncpy(file_size, begin, p2 - begin);
        file_size[p2 - begin] = '\0';
        LogInfo("file_size: {}", file_size);
        long_file_size = strtol(file_size, NULL, 10);

        begin = p2 + 2;
        p2 = strstr(begin, "name=\"user\"");
        if (!p2) {
            LogError("wrong no user!");
            ret = -1;
            goto END;
        }
        p2 = strstr(p2, "\r\n");
        p2 += 4;
        begin = p2;
        p2 = strstr(begin, "\r\n");
        strncpy(user, begin, p2 - begin);
        user[p2 - begin] = '\0';
        LogInfo("user: {}", user);

        GetFileSuffix(file_name, suffix);
        new_file_path[0] = '\0';
        strcat(new_file_path, file_path);
        strcat(new_file_path, ".");
        strcat(new_file_path, suffix);
        ret = rename(file_path, new_file_path);
        if (ret < 0) {
            LogError("rename {} to {} failed", file_path, new_file_path);
            ret = -1;
            goto END;
        }
    } else if (strstr(post_data.c_str(), "name=\"file\"") != nullptr) {
        // 浏览器 / run-dev-8080 直传：multipart 内含 file 实体
        if (parse_browser_multipart_to_temp(
                post_data, file_name, sizeof(file_name), file_md5,
                sizeof(file_md5), &long_file_size, user, sizeof(user),
                new_file_path, sizeof(new_file_path)) != 0) {
            ret = -1;
            goto END;
        }
    } else {
        LogError("upload: need Nginx upload fields (file_path) or browser field (file)");
        ret = -1;
        goto END;
    }

    //===============> 将该文件存入fastDFS中,并得到文件的file_id <============
    LogInfo("uploadFileToFastDfs, file_name:{}, new_file_path:{}", file_name, new_file_path);
    if (uploadFileToFastDfs(new_file_path, fileid) < 0) {
        LogError("uploadFileToFastDfs failed, unlink: {}", new_file_path);
        ret = unlink(new_file_path);
        if (ret != 0) {
            LogError("unlink: {} failed", new_file_path); // 删除失败则需要有个监控重新清除过期的临时文件，比如过期两天的都删除
        }
        ret = -1;
        goto END;
    }
    //================> 删除本地临时存放的上传文件 <===============
    LogInfo("unlink: {}", new_file_path);
    ret = unlink(new_file_path);//删除本地临时存放的上传文件
    if (ret != 0) {
        LogWarn("unlink: {} failed", new_file_path); // 删除失败则需要有个监控重新清除过期的临时文件，比如过期两天的都删除
    }
    //================> 得到文件所存放storage的host_name <=================
    // 拼接出完整的http地址
    LogInfo("getFullurlByFileid, fileid: {}", fileid);
    if (getFullurlByFileid(fileid, fdfs_file_url) < 0) {
        LogError("getFullurlByFileid failed ");
        ret = -1;
        goto END;
    }

    origin_url = fdfs_file_url;
     // 如果需要使用短链服务，当短链不开启时程序自动将s_shorturl_server_address设置为empty
    if(!s_shorturl_server_address.empty()) {
        ret = originUrl2ShortUrl(origin_url, short_url);
        if(ret != 0) {
            short_url = origin_url; // 如果调用失败则保持原来的url
            LogWarn("originUrl2ShortUrl failed");
        }
    } else {
        short_url = origin_url;
    }
    
    //===============> 将该文件的FastDFS相关信息存入mysql中 <======
    LogInfo("storeFileinfo, origin url: {} -> short url: {}", origin_url, short_url);
    // 把文件写入file_info
    if (storeFileinfo(db_conn, cache_conn, user, file_name, file_md5,
                      long_file_size, fileid, short_url.c_str()) < 0) {
        LogError("storeFileinfo failed ");
        ret = -1;
        // 严谨而言，这里需要删除 已经上传的文件
        goto END;
    }
    ret = 0;
    value["code"] = 0;
    str_json = value.toStyledString(); // json序列化,  直接用writer是紧凑方式，这里toStyledString是格式化更可读方式

    return 0;
END:
    value["code"] = 1;
    str_json = value.toStyledString(); // json序列化

    return -1;
}