cmake_minimum_required(VERSION 3.0)
PROJECT(tc_http_server)


# AUX_SOURCE_DIRECTORY(./common COMMON_LIST)
AUX_SOURCE_DIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/base BASE_LIST)
AUX_SOURCE_DIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/jsoncpp JSON_LIST)
AUX_SOURCE_DIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/mysql MYSQL_LIST)
AUX_SOURCE_DIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/redis REDIS_LIST)
AUX_SOURCE_DIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/api API_LIST)
AUX_SOURCE_DIRECTORY(${CMAKE_CURRENT_SOURCE_DIR} SRC_LIST)

#判断编译器类型,如果是gcc编译器,则在编译选项中加入c++14支持
if(CMAKE_COMPILER_IS_GNUCXX)
    set(CMAKE_CXX_FLAGS "-std=c++14 ${CMAKE_CXX_FLAGS}")
    message(STATUS "optional:-std=c++14")   
endif(CMAKE_COMPILER_IS_GNUCXX)

# 如果要开启警告使用 -Wall
# -Wno-deprecated选项用于关闭编译器产生的关于使用已经被废弃（deprecated）的函数、变量或头文件时产生的警告信息
# -Wno-unused-parameter选项则用于关闭编译器产生的有关未使用函数参数的警告信息。在定义函数时，可能会为了保持一致性而添加某些无用参数。
# 此时编译器会提示这些参数未被使用，但是你又不希望在编译时得到这个警告信息。这种情况下可以使用该选项。
ADD_DEFINITIONS( -g -Wno-deprecated -Wno-unused-parameter -Bsymbolic -D_REENTRANT -D_FILE_OFFSET_BITS=64 -DAC_HAS_INFO
-DAC_HAS_WARNING -DAC_HAS_ERROR -DAC_HAS_CRITICAL -DTIXML_USE_STL
-DAC_HAS_DEBUG -DLINUX_DAEMON)

# find_package的作用就是去寻找该库的头文件位置、库文件位置以及库文件名称，并将其设为变量，返回提供给CMakeLists.txt其他部分使用
# grpc相关
find_package(Threads REQUIRED)
# Ubuntu 等发行版常只提供 FindProtobuf.cmake，无 ProtobufConfig.cmake
find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)

message(STATUS "Using protobuf-${Protobuf_VERSION}")
message(STATUS "Using gRPC-${gRPC_VERSION}")

# 无系统 libmysqlclient-dev 时，可将 MariaDB Connector/C 安装到仓库根目录 .deps（见 scripts/bootstrap-deps.sh）
set(TUCHUANG_DEPS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.deps")
if(EXISTS "${TUCHUANG_DEPS_ROOT}/include/mariadb/mysql.h")
    message(STATUS "MySQL C API: MariaDB connector at ${TUCHUANG_DEPS_ROOT}")
    INCLUDE_DIRECTORIES(${TUCHUANG_DEPS_ROOT}/include/mariadb)
    LINK_DIRECTORIES(${TUCHUANG_DEPS_ROOT}/lib/mariadb)
    set(TUCHUANG_MYSQL_LIB mariadb)
else()
    set(TUCHUANG_MYSQL_LIB mysqlclient)
endif()

INCLUDE_DIRECTORIES(/usr/include/fastdfs /usr/include/fastcommon /usr/local/include/hiredis /usr/include/mysql)
INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR}/base)
INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR}/jsoncpp)
INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR}/mysql)
INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR}/redis)
INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR}/api)
INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR})

# 添加库文件路径，部分Linux版本不自动加载/usr/local/lib
LINK_DIRECTORIES(/usr/local/lib)

#ADD_XXXX必须在TARGET_LINK_LIBRARIES前面，否则会报错
#ADD_LIBRARY(${PROJECTNAME} SHARED/STATIC ${SRC_LIST})
ADD_EXECUTABLE(tc_http_server ${SRC_LIST} ${BASE_LIST} ${API_LIST} ${LOG_LIST}
        ${MYSQL_LIST} ${REDIS_LIST} ${JSON_LIST})

if(TUCHUANG_MYSQL_LIB STREQUAL "mariadb")
    set_target_properties(tc_http_server PROPERTIES
            BUILD_RPATH "${TUCHUANG_DEPS_ROOT}/lib/mariadb")
endif()

# 注意添加grpc相关的库gRPC::grpc++ gRPC::grpc++_reflection protobuf::libprotobuf
TARGET_LINK_LIBRARIES(tc_http_server pthread fdfsclient fastcommon m ${TUCHUANG_MYSQL_LIB} spdlogd
        gRPC::grpc++ gRPC::grpc++_reflection protobuf::libprotobuf)
