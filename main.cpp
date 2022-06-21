#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<arpa/inet.h>
#include<unistd.h>
#include<errno.h>
#include<fcntl.h>
#include<sys/epoll.h>
#include"lock.h"
#include"threadpool.h"
#include<signal.h>
#include"http_conn.h"
#include<assert.h>

#define MAX_FD 65535 //最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000 // 监听的最大事件的数量

extern void addfd( int epollfd, int fd, bool one_shot );



void addsig(int sig, void( handler )(int)){//是否需要加* ？

    struct sigaction sa;
    memset(&sa,'\0',sizeof(sa));
    sa.sa_handler=handler;
    sa.sa_flags=0;//sa_flags为0表示使用handler作为信号处理函数
    sigfillset(&sa.sa_mask);//sa_samask表示在信号捕捉函数执行过程中，临时阻塞某些信号

    //assert 宏的原型定义在 assert.h 中，其作用是如果它的条件返回错误，则终止程序执行。
    assert(sigaction(sig,&sa,NULL)!=-1);//信号捕捉

}

int main(int argc,char* argv[]){

    if(argc<=1){
        printf("usage: %s port_number\n",basename(argv[0]));//basename函数用于获取文件名的函数
        return 1;
    }

    int port = atoi( argv[1]);
    addsig(SIGPIPE,SIG_IGN);

    //创建并初始化线程池
    threadpool<http_conn> *pool=NULL;
    try{
        pool =new threadpool<http_conn>;
    } catch(...){
        return 1;
    }

    //创建http客户端连接用户的数组
    http_conn* users=new http_conn[MAX_FD];

    /*配置客户端socket*/
    // 创建监听socket
    int listenfd=socket(AF_INET,SOCK_STREAM,0);
    if(listenfd==-1){
        perror("listen");
        exit(-1);
    }
    //配置端口复用
    int reuse = 1;
    setsockopt( listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );
    
    //绑定
    struct sockaddr_in address;
    address.sin_family=AF_INET;
    address.sin_addr.s_addr=INADDR_ANY;
    address.sin_port=htons(port);

    int bindret=bind(listenfd,(struct sockaddr*) &address,sizeof(address));
    if(bindret==-1){
        perror("bind");
        exit(-1);
    }

    //监听
    int listenret=listen(listenfd,5);
    if(listenret==-1){
        perror("listen");
        return -1;
    }

    /*创建eopll实例，和事件数组，添加*/
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd=epoll_create(5);//5这个数字没有意义，大于0就行
    //添加到epoll实例中
    addfd(epollfd,listenfd,false);
    http_conn::m_epollfd=epollfd;

    while(true){
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
        if(number<0&&(errno!=EINTR)){
            printf( "epoll failure\n" );
            break;
        }
        //number>0说明有数据 分两种情况，
        //一种是lisetenfd有数据，说明有新客户端连接进来
        //一种是其他fd有数据，说明已建立连接的客户端有数据发送过来
        
        /*读取number中有数据的文件描述符*/
        for(int i=0;i<number;++i){
            int sockfd=events[i].data.fd;
            if(sockfd==listenfd){
                struct sockaddr_in clietn_address;
                socklen_t clietn_addrlength=sizeof(clietn_address);
                int connectfd=accept(listenfd,(struct sockaddr*)&clietn_address,&clietn_addrlength);
                
                if(connectfd<0){
                    perror("connect");
                    continue;
                }

                if(http_conn::m_user_count>=MAX_FD){
                    close(connectfd);
                    continue;
                }

                users[connectfd].init(connectfd,clietn_address);

            }
        }

    }
    close(epollfd);
    close(listenfd);
    delete [] users;
    delete  pool;
    return 0;
}