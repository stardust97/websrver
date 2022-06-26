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
#include"lst_timer.h"

class http_conn{
public:
    http_conn()=default;
    ~http_conn()=default;

    void init(int sockfd, const sockaddr_in& addr); // 初始化新接受的客户端连接
    void close_conn();  // 关闭连接
    void process(); // 处理客户端请求
    bool read();// 非阻塞读
    bool write();// 非阻塞写


public:
    static int m_epollfd;//所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的
    static int m_user_count;//统计用户的数量
    static const int READ_BUFFER_SIZE = 2048;   // 读缓冲区的大小
    static const int WRITE_BUFFER_SIZE = 1024;  // 写缓冲区的大小
    static const int FILENAME_LEN = 200;        // 文件名的最大长度


    // HTTP请求方法，这里只支持GET
    enum METHOD {GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT};
    
    /*
        解析客户端请求时，主状态机的状态
        CHECK_STATE_REQUESTLINE:当前正在分析请求行
        CHECK_STATE_HEADER:当前正在分析头部字段
        CHECK_STATE_CONTENT:当前正在解析请求体
    */
    enum CHECK_STATE { CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT };
    
    /*
        服务器处理HTTP请求的可能结果，报文解析的结果
        NO_REQUEST          :   请求不完整，需要继续读取客户数据
        GET_REQUEST         :   表示获得了一个完成的客户请求
        BAD_REQUEST         :   表示客户请求语法错误
        NO_RESOURCE         :   表示服务器没有资源
        FORBIDDEN_REQUEST   :   表示客户对资源没有足够的访问权限
        FILE_REQUEST        :   文件请求,获取文件成功
        INTERNAL_ERROR      :   表示服务器内部错误
        CLOSED_CONNECTION   :   表示客户端已经关闭连接了
    */
    enum HTTP_CODE { NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION };
    
    // 从状态机的三种可能状态，即行的读取状态，分别表示
    // 1.读取到一个完整的行 2.行出错 3.行数据尚且不完整
    enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };

private:
    int m_sockfd;//本机中用于和该HTTP连接的socket文件描述符
    sockaddr_in m_address;//该HTTP连接对方的socket地址
    char m_read_buf[ READ_BUFFER_SIZE ];    // 读缓冲区

    //标识读缓冲区中已经读入的客户端数据的最后一个字节的下一个位置，也就是缓冲区有效数据的长度
    int m_read_idx;   
    
    int m_checked_index;//当前正在分析的字符在读缓冲区中的位置
    int m_start_line;  // 当前正在解析的行的起始位置

    CHECK_STATE m_check_state;              // 主状态机当前所处的状态

    /*以下变量和解析请求报文有关*/
    char* m_version;                        // HTTP协议版本号，我们仅支持HTTP1.1
    METHOD m_method;                        // 请求方法
    char* m_url;                            // 客户请求的目标文件的文件名
    char* m_host;                            //主机名
    bool m_linger;                          //是否启用长连接keep alive
    int m_content_length;                   // HTTP请求体的消息总长度

    /*以下变量和响应请求有关*/
    char m_real_file[ FILENAME_LEN ];       // 客户请求的目标文件的完整路径，其内容等于 doc_root + m_url, doc_root是网站根目录
    struct stat m_file_stat;                // 目标文件的状态。通过它我们可以判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息
    char* m_file_address;                   // 客户请求的目标文件被mmap到内存中的起始位置
    
    //m_iv[0]用于发送响应报文的响应行与响应头部，m_iv[1]用于发送响应报文需求的资源文件（响应体）
    struct iovec m_iv[2];                   // 我们将采用writev来执行写操作，所以定义下面两个成员，其中m_iv_count表示被写内存块的数量。
    int m_iv_count;

    char m_write_buf[ WRITE_BUFFER_SIZE ];  // 写缓冲区
    int m_write_idx;                        // 写缓冲区中待发送的字节数
    int bytes_to_send;              // 将要发送的数据的字节数
    int bytes_have_send;            // 已经发送的字节数
    
private:
    //初始化其他的连接信息
    void init();

    /*以下函数用于解析http请求*/
    // 下面这一组函数被process_read调用以分析HTTP请求
    HTTP_CODE parse_request_line( char* text );//解析请求行
    HTTP_CODE parse_headers( char* text );//解析请求头部
    HTTP_CODE parse_content( char* text );//解析请求体
    HTTP_CODE do_request();//根据不同的请求具体相应请求

    //因为缓冲区一次性读完请求报文，没有行的概念（要通过\r\n手动分行，每分一次行，m_start_line位置都会变）
    //获取当前解析的行的地址，即缓冲区起始地址加m_start_line位置
    char* get_line() { return m_read_buf + m_start_line; }

    //获取一行，交给前三个解析函数处理
    LINE_STATUS parse_line();
    HTTP_CODE process_read();
    

    /*以下函数用于相应http请求*/
    bool add_response( const char* format, ... );
    bool add_status_line( int status, const char* title );
    bool add_headers(int content_len);
    bool add_content_length(int content_len);
    bool add_linger();
    bool add_blank_line();
    bool add_content( const char* content );
    bool add_content_type();

    // 往写缓冲中写入待发送的数据
    bool process_write(HTTP_CODE ret);
    void unmap();



};


#endif