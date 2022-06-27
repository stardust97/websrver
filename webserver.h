#pragma once

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "threadpool.h"
#include "http_conn.h"

#include<string>
using namespace std;

#define MAX_FD 65535 //最大的文件描述符个数（http最大连接数）
#define MAX_EVENT_NUMBER 10000 // epoll监听的最大事件的数量
#define TIMESLOT 5


class WebServer{
public:
    WebServer();
    ~WebServer();
public:
 
};
