/*
 * http_conn.h —— 单条 TCP 连接上的 HTTP 会话（Reactor 线程里读请求，必要时丢给线程池）
 *
 * 职责概要：
 * - OnConnect / OnRead / OnWrite：与 netlib(epoll) 配合，收齐一次 HTTP 请求后解析 URL+Body
 * - 按 URL 前缀分发到 _Handle*Request，再调用各 api_*.cc 中的业务函数
 * - 多线程接口：工作线程通过 AddResponseData 把 JSON 放进队列，主线程在 http_loop_callback
 *   里调用 SendResponseDataList，按 conn_uuid 找回 CHttpConn 再 Send（避免在子线程里直接写 socket）
 *
 * 连接生命周期：一般「请求-响应-关闭」，写完后 OnWriteComlete → Close；超时由 OnTimer 关闭。
 *
 *  Created on: 2013-9-29
 *      Author: ziteng
 */

#ifndef __HTTP_CONN_H__
#define __HTTP_CONN_H__

#include "http_parser_wrapper.h"
#include "netlib.h"
#include "tc_thread_pool.h"
#include "util.h"
#include <list>
#include <mutex>
#define HTTP_CONN_TIMEOUT 60000

#define READ_BUF_SIZE 2048
/** 单次 HTTP 请求在 in_buf 中累计的最大字节数（含头与体），与 nginx client_max_body_size 量级对齐 */
#define MAX_HTTP_REQUEST_BYTES (32 * 1024 * 1024)

#define HTTP_RESPONSE_HTML                                                     \
    "HTTP/1.1 200 OK\r\n"                                                      \
    "Connection:close\r\n"                                                     \
    "Content-Length:%d\r\n"                                                    \
    "Content-Type:application/json;charset=utf-8\r\n\r\n%s"

#define HTTP_RESPONSE_HTML_MAX 4096
extern ThreadPool g_thread_pool;
enum {
    CONN_STATE_IDLE,
    CONN_STATE_CONNECTED,
    CONN_STATE_OPEN,
    CONN_STATE_CLOSED,
};

typedef struct {
    uint32_t conn_uuid; // 用于查找connection
    string resp_data;   // 要回发的数据
} ResponsePdu_t;

class CHttpConn : public CRefObject {
  public:
    CHttpConn();
    virtual ~CHttpConn();

    uint32_t GetConnHandle() { return conn_handle_; }
    char *GetPeerIP() { return (char *)peer_ip_.c_str(); }

    int Send(void *data, int len);

    void Close();
    void OnConnect(net_handle_t handle);
    void OnRead();
    void OnWrite();
    void OnClose();
    void OnTimer(uint64_t curr_tick);
    void OnWriteComlete();

    static void AddResponseData(uint32_t conn_uuid,
                                string &resp_data); // 工作线程调用
    static void SendResponseDataList();             // 主线程调用

  private:
    // 文件上传处理
    int _HandleUploadRequest(string &url, string &post_data);
    // 账号注册处理
    int _HandleRegisterRequest(string &url, string &post_data);
    // 账号登陆处理
    int _HandleLoginRequest(string &url, string &post_data);
    //
    int _HandleDealfileRequest(string &url, string &post_data);
    //
    int _HandleDealsharefileRequest(string &url, string &post_data);
    //
    int _HandleMd5Request(string &url, string &post_data);
    //
    int _HandleMyfilesRequest(string &url, string &post_data);
    //
    int _HandleSharefilesRequest(string &url, string &post_data);
    //
    int _HandleSharepictureRequest(string &url, string &post_data);

  protected:
    net_handle_t m_sock_handle;
    uint32_t conn_handle_;
    bool busy_;

    uint32_t state_;
    std::string peer_ip_;
    uint16_t peer_port_;
    CSimpleBuffer in_buf_;
    CSimpleBuffer out_buf_;

    uint64_t last_send_tick_;
    uint64_t last_recv_tick_;

    CHttpParserWrapper http_parser_;

    static uint32_t s_uuid_alloctor; // uuid分配
    uint32_t uuid_;                  // 自己的uuid

    static std::mutex s_resp_mutex;
    static std::list<ResponsePdu_t *> s_response_pdu_list; // 主线程发送回复消息
};

typedef hash_map<uint32_t, CHttpConn *> HttpConnMap_t;

CHttpConn *FindHttpConnByHandle(uint32_t handle);
void InitHttpConn();
CHttpConn *GetHttpConnByUuid(uint32_t uuid);

#endif /* IMCONN_H_ */
