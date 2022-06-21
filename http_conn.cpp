#include"http_conn.h"

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
    epev.events = EPOLLIN| EPOLLRDHUP;
    epev.data.fd = fd;
    if(one_shot){
        //防止同一个通信被不同的线程处理
        epev.events|= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &epev);
    //设置文件描述符为非阻塞
    setnonblocking(fd);

}

//从epoll中移除监听的文件描述符
void removefd(int epollfd,int fd){
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
    close(fd);
}
