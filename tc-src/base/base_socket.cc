#include "base_socket.h"
#include "event_dispatch.h"
//创建一个全局哈希表，管理所有活跃的socket连接
//          key: socket句柄
//          value: socket对象
typedef hash_map<net_handle_t, CBaseSocket *> SocketMap;
SocketMap g_socket_map;

void AddBaseSocket(CBaseSocket *pSocket) {
    g_socket_map.insert(make_pair((net_handle_t)pSocket->GetSocket(), pSocket));
}

void RemoveBaseSocket(CBaseSocket *pSocket) {
    g_socket_map.erase((net_handle_t)pSocket->GetSocket());
}

CBaseSocket *FindBaseSocket(net_handle_t fd) {
    CBaseSocket *pSocket = NULL;
    SocketMap::iterator iter = g_socket_map.find(fd);
    if (iter != g_socket_map.end()) {
        pSocket = iter->second;
        pSocket->AddRef();
    }

    return pSocket;
}

//////////////////////////////

CBaseSocket::CBaseSocket() {
    // printf("CBaseSocket::CBaseSocket\n");
    socket_ = INVALID_SOCKET;
    state_ = SOCKET_STATE_IDLE;
}

CBaseSocket::~CBaseSocket() {
    // printf("CBaseSocket::~CBaseSocket, socket=%d\n", m_socket);
}

int CBaseSocket::Listen(const char *server_ip, uint16_t port,
                        callback_t callback, void *callback_data) {
    local_ip_ = server_ip;
    local_port_ = port;
    callback_ = callback;
    callback_data_ = callback_data;
    //创建socket
    socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_ == INVALID_SOCKET) {
        printf("socket failed, err_code=%d, server_ip=%s, port=%u",
               _GetErrorCode(), server_ip, port);
        return NETLIB_ERROR;
    }

    //设置socket选项
    _SetReuseAddr(socket_);//允许重用端口
    _SetNonblock(socket_);//设置非阻塞

    //绑定地址
    sockaddr_in serv_addr;
    _SetAddr(server_ip, port, &serv_addr);//设置地址
    int ret = ::bind(socket_, (sockaddr *)&serv_addr, sizeof(serv_addr));
    if (ret == SOCKET_ERROR) {
        printf("bind failed, err_code=%d, server_ip=%s, port=%u",
               _GetErrorCode(), server_ip, port);
        closesocket(socket_);
        return NETLIB_ERROR;
    }

    ret = listen(socket_, 64);//监听端口
    if (ret == SOCKET_ERROR) {
        printf("listen failed, err_code=%d, server_ip=%s, port=%u",
               _GetErrorCode(), server_ip, port);
        closesocket(socket_);
        return NETLIB_ERROR;
    }

    state_ = SOCKET_STATE_LISTENING;

    printf("CBaseSocket::Listen on %s:%d", server_ip, port);

    AddBaseSocket(this);
    CEventDispatch::Instance()->AddEvent(socket_, SOCKET_READ | SOCKET_EXCEP);
    return NETLIB_OK;
}

net_handle_t CBaseSocket::Connect(const char *server_ip, uint16_t port,
                                  callback_t callback, void *callback_data) {
    printf("CBaseSocket::Connect, server_ip=%s, port=%d", server_ip, port);

    remote_ip_ = server_ip;
    remote_port_ = port;
    callback_ = callback;
    callback_data_ = callback_data;

    socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_ == INVALID_SOCKET) {
        printf("socket failed, err_code=%d, server_ip=%s, port=%u",
               _GetErrorCode(), server_ip, port);
        return NETLIB_INVALID_HANDLE;
    }

    _SetNonblock(socket_);
    _SetNoDelay(socket_);
    sockaddr_in serv_addr;
    _SetAddr(server_ip, port, &serv_addr);
    int ret = connect(socket_, (sockaddr *)&serv_addr, sizeof(serv_addr));
    if ((ret == SOCKET_ERROR) && (!_IsBlock(_GetErrorCode()))) {
        printf("connect failed, err_code=%d, server_ip=%s, port=%u",
               _GetErrorCode(), server_ip, port);
        closesocket(socket_);
        return NETLIB_INVALID_HANDLE;
    }
    state_ = SOCKET_STATE_CONNECTING;
    AddBaseSocket(this);
    CEventDispatch::Instance()->AddEvent(socket_, SOCKET_ALL);

    return (net_handle_t)socket_;
}

int CBaseSocket::Send(void *buf, int len) {
    if (state_ != SOCKET_STATE_CONNECTED)
        return NETLIB_ERROR;

    int ret = send(socket_, (char *)buf, len, 0);
    if (ret == SOCKET_ERROR) {
        int err_code = _GetErrorCode();
        if (_IsBlock(err_code)) {
#if ((defined _WIN32) || (defined __APPLE__))
            CEventDispatch::Instance()->AddEvent(m_socket, SOCKET_WRITE);
#endif
            ret = 0;
            // printf("socket send block fd=%d", m_socket);
        } else {
            printf("send failed, err_code=%d, len=%d", err_code, len);
        }
    }

    return ret;
}

int CBaseSocket::Recv(void *buf, int len) {
    return recv(socket_, (char *)buf, len, 0);
}

int CBaseSocket::Close() {
    CEventDispatch::Instance()->RemoveEvent(socket_, SOCKET_ALL);
    RemoveBaseSocket(this);
    closesocket(socket_);
    ReleaseRef();

    return 0;
}
/* -------------------------------------------*/
/**
 * @brief  读事件处理
 *
 * @param 
 *
 * @returns
 *      0 succ, -1 fail
 */
/* -------------------------------------------*/
void CBaseSocket::OnRead() {
    if (state_ == SOCKET_STATE_LISTENING) {//监听状态，有新连接
        _AcceptNewSocket();
    } else {
        u_long avail = 0;//可读数据长度
        int ret = ioctlsocket(socket_, FIONREAD, &avail);
        if ((SOCKET_ERROR == ret) || (avail == 0)) {//读取失败或没有数据
            callback_(callback_data_, NETLIB_MSG_CLOSE, (net_handle_t)socket_,
                      NULL);
        } else {
            callback_(callback_data_, NETLIB_MSG_READ, (net_handle_t)socket_,//读取数据
                      NULL);
        }
    }
}
/* -------------------------------------------*/
/**
 * @brief  写事件处理
 *
 * @param 
 *
 * @returns
 *      0 succ, -1 fail
 */
/* -------------------------------------------*/
void CBaseSocket::OnWrite() {
#if ((defined _WIN32) || (defined __APPLE__))
    CEventDispatch::Instance()->RemoveEvent(m_socket, SOCKET_WRITE);//移除写事件
#endif

    if (state_ == SOCKET_STATE_CONNECTING) {//连接状态，有数据可写
        int error = 0;//错误码
        socklen_t len = sizeof(error);//错误码长度
#ifdef _WIN32

        getsockopt(m_socket, SOL_SOCKET, SO_ERROR, (char *)&error, &len);//获取错误码
#else
        getsockopt(socket_, SOL_SOCKET, SO_ERROR, (void *)&error, &len);//获取错误码
#endif
        if (error) {
            callback_(callback_data_, NETLIB_MSG_CLOSE, (net_handle_t)socket_,//关闭连接
                      NULL);
        } else {
            state_ = SOCKET_STATE_CONNECTED;
            callback_(callback_data_, NETLIB_MSG_CONFIRM, (net_handle_t)socket_,
                      NULL);
        }
    } else {
        callback_(callback_data_, NETLIB_MSG_WRITE, (net_handle_t)socket_,
                  NULL);
    }
}
/* -------------------------------------------*/
/**
 * @brief  关闭事件处理
 *
 * @param 
 *
 * @returns
 *      0 succ, -1 fail
 */
/* -------------------------------------------*/
void CBaseSocket::OnClose() {
    state_ = SOCKET_STATE_CLOSING;
    callback_(callback_data_, NETLIB_MSG_CLOSE, (net_handle_t)socket_, NULL);
}
/* -------------------------------------------*/
/**
 * @brief  设置发送缓冲区大小
 *
 * @param send_size 发送缓冲区大小
 *
 * @returns
 *      0 succ, -1 fail
 */
/* -------------------------------------------*/
void CBaseSocket::SetSendBufSize(uint32_t send_size) {
    int ret = setsockopt(socket_, SOL_SOCKET, SO_SNDBUF, &send_size, 4);//设置发送缓冲区大小
    if (ret == SOCKET_ERROR) {
        printf("set SO_SNDBUF failed for fd=%d", socket_);//设置失败
    }

    socklen_t len = 4;
    int size = 0;
    //设置之后立即验证是否成功
    getsockopt(socket_, SOL_SOCKET, SO_SNDBUF, &size, &len);//获取发送缓冲区大小
    printf("socket=%d send_buf_size=%d", socket_, size);
}

void CBaseSocket::SetRecvBufSize(uint32_t recv_size) {
    int ret = setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, &recv_size, 4);
    if (ret == SOCKET_ERROR) {
        printf("set SO_RCVBUF failed for fd=%d", socket_);
    }

    socklen_t len = 4;
    int size = 0;
    getsockopt(socket_, SOL_SOCKET, SO_RCVBUF, &size, &len);
    printf("socket=%d recv_buf_size=%d", socket_, size);
}

int CBaseSocket::_GetErrorCode() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool CBaseSocket::_IsBlock(int error_code) {
#ifdef _WIN32
    return ((error_code == WSAEINPROGRESS) || (error_code == WSAEWOULDBLOCK));
#else
    return ((error_code == EINPROGRESS) || (error_code == EWOULDBLOCK));
#endif
}

void CBaseSocket::_SetNonblock(SOCKET fd) {//设置非阻塞
#ifdef _WIN32
    u_long nonblock = 1;
    int ret = ioctlsocket(fd, FIONBIO, &nonblock);
#else
    int ret = fcntl(fd, F_SETFL, O_NONBLOCK | fcntl(fd, F_GETFL));//设置非阻塞
#endif
    if (ret == SOCKET_ERROR) {
        printf("_SetNonblock failed, err_code=%d, fd=%d", _GetErrorCode(), fd);
    }
}

void CBaseSocket::_SetReuseAddr(SOCKET fd) {
    int reuse = 1;
    int ret =
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse));
    if (ret == SOCKET_ERROR) {
        printf("_SetReuseAddr failed, err_code=%d, fd=%d", _GetErrorCode(), fd);
    }
}

void CBaseSocket::_SetNoDelay(SOCKET fd) {//设置无延迟
    int nodelay = 1;
    int ret = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&nodelay,
                         sizeof(nodelay));
    if (ret == SOCKET_ERROR) {
        printf("_SetNoDelay failed, err_code=%d, fd=%d", _GetErrorCode(), fd);
    }
}

void CBaseSocket::_SetAddr(const char *ip, const uint16_t port,
                           sockaddr_in *addr) {
    memset(addr, 0, sizeof(sockaddr_in));
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);//将端口转换为网络字节序
    addr->sin_addr.s_addr = inet_addr(ip);//将ip地址转换为网络字节序
    if (addr->sin_addr.s_addr == INADDR_NONE) {
        hostent *host = gethostbyname(ip);//将ip地址转换为hostent结构体
        if (host == NULL) {
            printf("gethostbyname failed, ip=%s, port=%u", ip, port);
            return;
        }

        addr->sin_addr.s_addr = *(uint32_t *)host->h_addr;
    }
}
//一次性处理所有排队连接
void CBaseSocket::_AcceptNewSocket() {
    SOCKET fd = 0;
    sockaddr_in peer_addr;
    socklen_t addr_len = sizeof(sockaddr_in);
    char ip_str[64];
    while ((fd = accept(socket_, (sockaddr *)&peer_addr, &addr_len)) !=
           INVALID_SOCKET) {
        CBaseSocket *pSocket = new CBaseSocket();
        uint32_t ip = ntohl(peer_addr.sin_addr.s_addr);
        uint16_t port = ntohs(peer_addr.sin_port);

        snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d", ip >> 24,
                 (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);

        // printf("AcceptNewSocket, socket=%d from %s:%d\n", fd, ip_str, port);

        pSocket->SetSocket(fd);
        pSocket->SetCallback(callback_);
        pSocket->SetCallbackData(callback_data_);
        pSocket->SetState(SOCKET_STATE_CONNECTED);
        pSocket->SetRemoteIP(ip_str);
        pSocket->SetRemotePort(port);

        _SetNoDelay(fd);
        _SetNonblock(fd);
        AddBaseSocket(pSocket);
        CEventDispatch::Instance()->AddEvent(fd, SOCKET_READ | SOCKET_EXCEP);
        callback_(callback_data_, NETLIB_MSG_CONNECT, (net_handle_t)fd, NULL);
    }
}
