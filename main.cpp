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
#include"lst_timer.h"

#define MAX_FD 65535 //最大的文件描述符个数（http最大连接数）
#define MAX_EVENT_NUMBER 10000 // epoll监听的最大事件的数量
#define TIMESLOT 5


extern void addfd( int epollfd, int fd, bool one_shot );
void addfd(int epollfd, int fd );
extern void removefd(int epollfd,int fd);
extern void modfd(int epollfd,int fd,int ev);
extern int setnonblocking(int fd);


static int pipefd[2];//管道的读写两端
static int epollfd;
static sort_timer_lst timer_lst;


void sig_handler( int sig )
{
    int save_errno = errno;
    int msg = sig;
    send( pipefd[1], ( char* )&msg, 1, 0 );
    errno = save_errno;
}

void addsig(int sig, void( handler )(int)){//是否需要加* ？

    struct sigaction sa;
    memset(&sa,'\0',sizeof(sa));
    sa.sa_handler=handler;
    sa.sa_flags=0;//sa_flags为0表示使用handler作为信号处理函数
    
    sigfillset(&sa.sa_mask);//sa_samask在调用信号处理程序时,临时阻塞某些信号,处理完成后才可以收到这些信号

    //assert 宏的原型定义在 assert.h 中，其作用是如果它的条件返回错误，则终止程序执行。
    assert(sigaction(sig,&sa,NULL)!=-1);//信号捕捉

}

void addsig( int sig )
{
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = sig_handler;
    sa.sa_flags |= SA_RESTART;
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

// 定时器回调函数，它删除非活动连接socket上的注册事件，并关闭之。
void cb_func( client_data* user_data)
{
    epoll_ctl( epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0 );
    assert( user_data );
    close( user_data->sockfd );
    printf( "close fd %d\n", user_data->sockfd );
}

void timer_handler()
{
    // 定时处理任务，实际上就是调用tick()函数
    timer_lst.tick();
    // 因为一次 alarm 调用只会引起一次SIGALARM 信号，所以我们要重新定时，以不断触发 SIGALARM信号。
    alarm(TIMESLOT);
}

int main(int argc,char* argv[]){

    if(argc<=1){
        printf("usage: %s port_number\n",basename(argv[0]));//basename函数用于获取文件名的函数
        return 1;
    }

    int port = atoi( argv[1]);
    int ret=0;

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
    epollfd=epoll_create(5);//5这个数字没有意义，大于0就行
    assert( epollfd != -1 );
    //将监听的文件描述符添加到epoll实例中
    addfd(epollfd,listenfd,false);//监听的文件描述符不需要设置EPOLLONESHOT
    http_conn::m_epollfd=epollfd;

    // 创建管道
    int socketpair_ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert( socketpair_ret != -1 );
    setnonblocking( pipefd[1] );
    addfd( epollfd, pipefd[0]);

    // 设置信号处理函数
    addsig(SIGPIPE,SIG_IGN);
    addsig( SIGALRM);
    addsig( SIGTERM);
    bool stop_server = false;

    client_data* usersinfo = new client_data[MAX_FD]; 
    bool timeout = false;
    alarm(TIMESLOT);  // 定时,5秒后产生SIGALARM信号

    //循环检测是否有读事件发生
    while(!stop_server){
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );//events是传出参数，保存有事件的文件描述符（个数为返回值）
        if(number<0&&(errno!=EINTR)){//errno==EINTR是阻塞过程中被信号中断，需要再下一轮重新调用epoll_wait()。
        //epoll_pwait()可以避免上述现象，epoll_pwait()可以让程序安全的等到事件的发生
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
                struct sockaddr_in client_address;
                socklen_t client_addrlength=sizeof(client_address);
                int connectfd=accept(listenfd,(struct sockaddr*)&client_address,&client_addrlength);

                if(connectfd<0){
                    perror("connect");
                    continue;
                }

                //目前连接数满了
                if(http_conn::m_user_count>=MAX_FD){
                    //可以给客户端写一个信息：服务器内部正忙.
                    close(connectfd);
                    continue;
                }
                //将此http连接放到http用户数组users中
                //以connectfd作为下标索引，可以比较方便的记录用户连接
                users[connectfd].init(connectfd,client_address);
                usersinfo[connectfd].address = client_address;
                usersinfo[connectfd].sockfd = connectfd;

                // 创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表timer_lst中
                util_timer* timer = new util_timer;
                timer->user_data = &usersinfo[connectfd];
                timer->cb_func = cb_func;
                time_t cur = time( NULL );
                timer->expire = cur + 3 * TIMESLOT;
                usersinfo[connectfd].timer = timer;
                timer_lst.add_timer( timer );
            }

            //对方异常断开或者错误等事件
            else if(events[i].events&( EPOLLRDHUP | EPOLLHUP | EPOLLERR )){
                users[sockfd].close_conn();//关闭此http连接
            }

            //管道有读事件
            else if( ( sockfd == pipefd[0] ) && ( events[i].events & EPOLLIN ) ) {
                // 处理信号
                int sig;
                char signals[1024];
                ret = recv( pipefd[0], signals, sizeof( signals ), 0 );
                if( ret == -1 ) {
                    continue;
                } else if( ret == 0 ) {
                    continue;
                } else  {
                    for( int i = 0; i < ret; ++i ) {
                        switch( signals[i] )  {
                            case SIGALRM:
                            {
                                // 用timeout变量标记有定时任务需要处理，但不立即处理定时任务
                                // 这是因为定时任务的优先级不是很高，我们优先处理其他更重要的任务。
                                timeout = true;
                                break;
                            }
                            case SIGTERM:
                            {
                                stop_server = true;
                            }
                        }
                    }
                }
            }

            //已连接的客户端有新的读事件
            else if(events[i].events & EPOLLIN){
                memset( usersinfo[sockfd].buf, '\0', BUFFER_SIZE );
                //ret = recv( sockfd, usersinfo[sockfd].buf, BUFFER_SIZE-1, 0 );
                //printf( "get %d bytes of client data %s from %d\n", ret, usersinfo[sockfd].buf, sockfd );
                util_timer* timer = usersinfo[sockfd].timer;

                if(users[sockfd].read()){//read是一次性读完所有数据
                    printf("检测到读事件\n");
                    if( timer ) {
                        time_t cur = time( NULL );
                        timer->expire = cur + 3 * TIMESLOT;
                        printf( "adjust timer once\n" );
                        timer_lst.adjust_timer( timer );
                    }
                    pool->append(users+sockfd);//将此http连接任务添加到线程池处理

                }else{// 如果发生读错误或者对方关闭连接，则read()返回false，
                    //则我们关闭连接，并移除其对应的定时器
                    //cb_func( &usersinfo[sockfd] );
                    if( timer )
                    {
                        timer_lst.del_timer( timer );
                    }
                    users[sockfd].close_conn();
                }
            }

            //已连接的客户端有新的写事件
            else if( events[i].events & EPOLLOUT ) {
                if( !users[sockfd].write() ) {//wirte是一次性写完所有数据
                    users[sockfd].close_conn();
                }

            }

        }

        // 最后处理定时事件，因为I/O事件有更高的优先级。当然，这样做将导致定时任务不能精准的按照预定的时间执行。
        if( timeout ) {
            timer_handler();
            timeout = false;
        }

    }


    close(epollfd);
    close(listenfd);
    close( pipefd[1] );
    close( pipefd[0] );

    delete [] users;
    delete [] usersinfo;
    delete  pool;
    return 0;
}