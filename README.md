# Orange 分布式图床

基于 **C++ HTTP 服务 + React 前端 + MySQL / Redis + FastDFS** 的文件与图床系统。开发时通过 **8080** 访问页面，**8081** 为后端 API；生产环境通常由 Nginx 终结上传并反代 `/api`。

---

## 技术栈概览

| 层级 | 说明 |
|------|------|
| 网络与并发 | Reactor（epoll）、线程池处理耗时 API |
| 数据 | MySQL 持久化元数据；Redis 缓存 token、文件计数、分享集合等 |
| 存储 | FastDFS 存对象；`tc_http_server.conf` 中配置 `client.conf` 与 Web 访问地址 |
| 日志 | spdlog |
| 前端 | React 打包产物位于 `tc-front/` |

---

## 仓库目录结构

```
my_tuchuang/
├── README.md                 # 本说明
├── docker-compose.yml        # 本地 MySQL(3306) + Redis(6379)，密码与配置一致
├── tuchuang.sql              # 默认库「tuchuang」表结构（Docker 首次初始化会执行）
├── tuchuang_noindex.sql      # 无部分索引的库（性能对比实验用）
├── tuchuang_index.sql        # 带索引的对比库
├── tuchuang_clear.sql        # 清空业务表（慎用）
├── nginx.conf                # 生产部署可参考的 Nginx 示例
├── display/                  # 运行界面截图（自行放入，见下文「运行界面展示」）
├── scripts/
│   ├── run-dev-8080.sh       # 推荐：一键起后端 8081 + 8080 静态与 /api 反代
│   ├── docker-stack.sh       # 统一调用 docker compose 或 docker-compose
│   ├── ensure-mysql-tuchuang.sh  # 旧数据卷无「tuchuang」库时手动导入 SQL
│   ├── dev_proxy_8080.py     # 8080 开发入口（由 run-dev-8080.sh 调用）
│   ├── bootstrap-deps.sh     # 拉取 MariaDB 客户端等到 .deps（供 CMake 使用）
│   └── run-backend.sh        # 仅启动 tc_http_server（需已配置好环境）
├── tc-src/                   # C++ 服务源码
│   ├── main.cc               # 入口：初始化 DB/Redis 池、监听 HTTP
│   ├── http_conn.cc          # HTTP 解析与 /api 路由
│   ├── api/                  # 各接口实现（注册、登录、上传、文件列表等）
│   ├── mysql/                # 连接池与 SQL
│   ├── redis/                # hiredis 封装、连接池、键名定义 redis_keys.h
│   ├── base/                 # 网络、事件循环、socket 等
│   ├── tc_http_server.conf   # 运行时配置（需复制到 build 目录或工作目录）
│   └── CMakeLists.txt
├── tc-front/                 # 前端构建产物（index.html、static/js 等）
├── shorturl/                 # 短链相关 Go 服务（可选，默认配置可关闭短链）
└── .deps/                    # bootstrap 依赖输出目录（已 .gitignore）
```

---

## 快速运行（开发）

### 1. 依赖

- Docker：用于 MySQL / Redis（无 `docker compose` 子命令时，可用 **`docker-compose`** 或本仓库 **`./scripts/docker-stack.sh`**）。
- 编译：`cmake`、`g++`、项目依赖见 `tc-src/README.md` 与 `scripts/bootstrap-deps.sh`。

### 2. 启动数据库

在仓库根目录：

```bash
./scripts/docker-stack.sh up -d
```

若 MySQL 数据卷是旧的、没有库 **`tuchuang`**（后端日志报 `Unknown database 'tuchuang'`），执行：

```bash
./scripts/ensure-mysql-tuchuang.sh
```

或清空卷后重建（**会删除库内数据**）：

```bash
./scripts/docker-stack.sh down -v
./scripts/docker-stack.sh up -d
```

### 3. 编译后端

```bash
cd tc-src/build && cmake .. && make
cp -f ../tc_http_server.conf .
```

（若尚未建 `build` 目录，先 `mkdir -p tc-src/build`。）

### 4. 一键启动（前端 + API）

在仓库根目录：

```bash
./scripts/run-dev-8080.sh
```

浏览器打开：**http://127.0.0.1:8080/**

---

## 查看 MySQL 中的数据

默认连接信息见 `tc-src/tc_http_server.conf`：**主机 127.0.0.1:3306**，库名 **`tuchuang`**，用户 **`root`**，密码 **`123456`**（若已修改以本地为准）。

### 使用 Docker 容器内的客户端（无需本机安装 mysql）

```bash
# 列出表
docker exec -it tuchuang-mysql mysql -uroot -p123456 tuchuang -e "SHOW TABLES;"

# 查看用户
docker exec -it tuchuang-mysql mysql -uroot -p123456 tuchuang --default-character-set=utf8mb4 \
  -e "SELECT id, user_name, nick_name, create_time FROM user_info;"

# 用户文件列表（最近 10 条）
docker exec -it tuchuang-mysql mysql -uroot -p123456 tuchuang \
  -e "SELECT id, user, file_name, md5, create_time FROM user_file_list ORDER BY id DESC LIMIT 10;"

# 全局文件元数据（FastDFS file_id / url 等）
docker exec -it tuchuang-mysql mysql -uroot -p123456 tuchuang \
  -e "SELECT id, md5, LEFT(url,64) AS url_prefix, size FROM file_info ORDER BY id DESC LIMIT 10;"
```

### 本机安装客户端（可选）

```bash
sudo apt install -y mariadb-client
mysql -h127.0.0.1 -P3306 -uroot -p123456 tuchuang -e "SHOW TABLES;"
```

### 主要表含义

| 表名 | 作用 |
|------|------|
| `user_info` | 注册用户 |
| `user_file_list` | 用户与文件的关联（文件名、md5、分享状态等） |
| `file_info` | 文件全局记录（FastDFS `file_id`、URL、引用计数等） |
| `share_file_list` | 共享文件列表 |
| `share_picture_list` | 图床分享记录 |

---

## 查看 Redis 中的数据

默认 **127.0.0.1:6379**。配置中 **token** 使用 **db 0**，**ranking_list** 使用 **db 1**。

### 使用 Docker

```bash
# 连通性
docker exec -it tuchuang-redis redis-cli PING

# db0：列出 key（中文用户名在终端里可能显示为 \x 序列，属正常现象）
docker exec -it tuchuang-redis redis-cli -n 0 KEYS '*'

# 更易读地查看含中文的 key
docker exec -it tuchuang-redis redis-cli -n 0 --raw KEYS '*'
```

### 本机 redis-cli（可选）

```bash
sudo apt install -y redis-tools
redis-cli -h 127.0.0.1 -p 6379 -n 0 PING
```

### 与业务相关的键（定义见 `tc-src/redis/redis_keys.h`）

| 键 / 前缀 | 含义 |
|-----------|------|
| 用户名（字符串） | 登录后写入的 **token**（`SETEX`，TTL 约 24 小时） |
| `FILE_USER_COUNT` + 用户名 | 用户文件数量缓存 |
| `SHARE_PIC_COUNT` + 用户名 | 分享图片数量缓存 |
| `FILE_PUBLIC_ZSET` | 已分享文件的集合（ZSET） |
| `FILE_NAME_HASH` | 文件 id → 展示文件名（HASH） |
| `FILE_PUBLIC_COUNT` | 公共分享数量等计数 |

示例：

```bash
docker exec -it tuchuang-redis redis-cli -n 0 GET orange
docker exec -it tuchuang-redis redis-cli -n 0 GET FILE_USER_COUNTorange
docker exec -it tuchuang-redis redis-cli -n 0 ZCARD FILE_PUBLIC_ZSET
```

---

## 运行界面展示

请将运行效果截图放在仓库根目录下的 **`display/`** 文件夹中（可随仓库一并提交，便于报告或文档展示）。

建议在 `display/` 中使用下列文件名，与本节引用一致（也可改用自己喜欢的名字，并同步改下方 Markdown 路径）：

| 建议文件名 | 内容说明 |
|------------|----------|
| `display/01-login.png` | 登录页 |
| `display/02-register.png` | 注册页 |
| `display/03-myfiles.png` | 我的文件 / 文件列表 |
| `display/04-upload.png` | 上传流程或上传成功提示 |
| `display/05-share.png` | 分享或图床相关页面（若有） |
| `display/06-dev-proxy.png` | 终端中 `run-dev-8080.sh` 或 Docker 运行状态（可选） |

在图片放入对应路径后，以下引用即可正常显示：

![登录页](display/01-login.png)

![注册页](display/02-register.png)

![我的文件](display/03-myfiles.png)

![上传](display/04-upload.png)

![分享/图床](display/05-share.png)

![开发环境终端](display/06-dev-proxy.png)

> **说明**：若尚未添加某张图片，Markdown 预览可能显示裂图或仅 alt 文字，属正常现象；放入同名文件后即会显示。

---

## 配置与端口小结

| 服务 | 端口 | 说明 |
|------|------|------|
| 开发入口（静态 + `/api` 反代） | 8080 | `scripts/dev_proxy_8080.py` |
| `tc_http_server` | 8081 | `tc_http_server.conf` 中 `HttpPort` |
| MySQL | 3306 | `docker-compose.yml` 映射 |
| Redis | 6379 | `docker-compose.yml` 映射 |

FastDFS、`storage_web_server_*` 等见 `tc_http_server.conf`；上传成功依赖本机 **FastDFS 客户端配置** 与 **tracker/storage** 可用。

---

## 常见问题

1. **`docker compose up -d` 报 `unknown shorthand flag: 'd'`**  
   未安装 Compose 插件时，请使用 **`docker-compose up -d`** 或 **`./scripts/docker-stack.sh up -d`**。

2. **8081 `bind` 失败（err 98）**  
   已有旧的 `tc_http_server` 占用端口；`run-dev-8080.sh` 会尝试释放，亦可手动结束占用 8081 的进程。

3. **编译阶段 protobuf 与 `byte` 宏冲突**  
   若报错指向 `parse_context.h` 中的 `byte`，按 `tc-src/README.md` 将相关局部变量改名为 `byte2` 等（与部分第三方头文件宏冲突）。

4. **更详细的编译与短链 gRPC 说明**  
   见 **`tc-src/README.md`**。

---

## 许可证与致谢

本项目在原有教学用图床代码基础上演进；若使用第三方组件，请遵循各自许可证。
