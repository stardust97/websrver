#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
/*
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
*/
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include "lock.h"
#include <sys/uio.h>
#include <unistd.h>

class http_conn{
public:
    http_conn()=default;
    ~http_conn()=default;

    void init(int sockfd, const sockaddr_in& addr); // 初始化新接受的客户端连接
    void close_conn();  // 关闭连接
    void process(); // 处理客户端请求
    bool read();// 非阻塞读
    bool write();// 非阻塞写
private:
   

public:
    static int m_epollfd;//所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的
    static int m_user_count;//统计用户的数量

private:
    int m_sockfd;//本机中用于和该HTTP连接的socket文件描述符
    sockaddr_in m_address;//该HTTP连接对方的socket地址

};


#endif