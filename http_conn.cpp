#include"http_conn.h"

/*静态成员变量的初始化*/
int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;
/*静态成员变量的初始化*/

//设置文件描述符为非阻塞
int setnonblocking(int fd){
    int old_option = fcntl( fd, F_GETFL );//F_GETFL 获取文件状态标志
    int new_option = old_option | O_NONBLOCK;
    fcntl( fd, F_SETFL, new_option );// F_SETFL 设置文件状态标志
    return old_option;
}

//像epoll中添加需要监听的文件描述符
void addfd(int epollfd,int fd,bool one_shot){
    struct epoll_event epev;
    //epev.events = EPOLLIN| EPOLLRDHUP;//EPOLLRDHUP表示对端异常断开
    epev.events = EPOLLIN| EPOLLET|EPOLLRDHUP;
    epev.data.fd = fd;
    if(one_shot){
        //防止同一个通信被不同的线程处理(即是是ET模式)
        epev.events|=EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &epev);
    //设置文件描述符为非阻塞,因为ET模式只支持非阻塞，原因如下：
    //ET模式只在socket描述符状态发生变化时才触发事件，如果不一次把socket内核缓冲区的数据读完，
    //会导致socket内核缓冲区中即使还有一部分数据，该socket的可读事件也不会被触发
    //若ET模式下使用阻塞IO，则程序一定会阻塞在最后一次write或read操作，因此说ET模式下一定要使用非阻塞IO

    setnonblocking(fd);

}

//从epoll中移除监听的文件描述符
void removefd(int epollfd,int fd){
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
    close(fd);
}

//从epoll中修改文件描述符，重置socket上的EPOLLONESHOT事件，确保以下一次可读时，EPOLLIN事件可以被触发
void modfd(int epollfd,int fd,int ev){
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl( epollfd, EPOLL_CTL_MOD, fd, &event );
}
 
// 初始化新接受的客户端连接
void http_conn::init(int sockfd, const sockaddr_in& addr){
    m_sockfd=sockfd;
    m_address=addr;

    //设置该连接的socket端口复用
    int reuse = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );
    //将此连接添加到epoll中，用于后续监听读写事件
    addfd(m_epollfd,sockfd,true);

    m_user_count++;

}

// 关闭连接
void http_conn::close_conn() {
    if(m_sockfd != -1) {
        removefd(m_epollfd, m_sockfd);//从epoll监听列表中删除
        m_sockfd = -1;//关闭文件描述符close(m_sockfd)在remove函数中
        m_user_count--; // 关闭一个连接，将客户总数量-1
    }
}

//循环读取客户数据，知道没有数据可读或者对方断开连接
bool http_conn::read() {
   if( m_read_idx >= READ_BUFFER_SIZE ) {
        return false;
    }
    int bytes_read = 0;
    while(true) {
        // 从m_read_buf + m_read_idx索引出开始保存数据，大小是READ_BUFFER_SIZE - m_read_idx
        // m_sockfd已经设置为非阻塞了
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0 );
        printf("读到数据：\n%s\n",m_read_buf);
        if (bytes_read == -1) {
            if( errno == EAGAIN || errno == EWOULDBLOCK ) {
                // 没有数据
                break;
            }
            return false;   
        } else if (bytes_read == 0) {   // 对方关闭连接
            return false;
        }
        m_read_idx += bytes_read;
    }
    //printf("读到数据：\n%s\n",m_read_buf);
    return true;
}

bool http_conn::write() {
    printf("一次性写完数据\n");
    return true;
}

//线程池的工作线程代码，用于处理HTTP请求
void http_conn::process() {
    //解析HTTP请求

    printf("解析http请求\n");
    //生成响应（之后就可以写入数据了）
}
