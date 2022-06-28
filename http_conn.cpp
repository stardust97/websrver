#include"http_conn.h"

/*静态成员变量的初始化*/
int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;
/*静态成员变量的初始化*/

// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

const char* doc_root = "/home/tiancheng/webserver/resources";


//设置文件描述符为非阻塞
int setnonblocking(int fd){
    int old_option = fcntl( fd, F_GETFL );//F_GETFL 获取文件状态标志
    int new_option = old_option | O_NONBLOCK;
    fcntl( fd, F_SETFL, new_option );// F_SETFL 设置文件状态标志
    return old_option;
}

//使用LT模式向epoll中添加需要监听的文件描述符
void addfd(int epollfd,int fd,bool one_shot){
    struct epoll_event epev;
    epev.events = EPOLLIN| EPOLLRDHUP;//EPOLLRDHUP表示对端异常断开
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

//使用ET模式epoll
void addfd(int epollfd, int fd )
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;
    epoll_ctl( epollfd, EPOLL_CTL_ADD, fd, &event );
    setnonblocking( fd );
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

    init();
}

//init()和init(int,sockaddr_in&)分开写是因为init()可能会被多次调用，
//为了不影响init(int,sockaddr_in&)中的参数，如m_user_count
void http_conn::init(){
    m_check_state=CHECK_STATE_REQUESTLINE;
    m_checked_index=0;
    m_start_line=0;
    m_read_idx=0;

    m_method = GET;         // 默认请求方式为GET
    m_url = 0;              
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_index = 0;
    m_read_idx = 0;
    m_write_idx = 0;

    bzero(m_read_buf,READ_BUFFER_SIZE);//将读缓冲区清空
    bzero(m_write_buf, READ_BUFFER_SIZE);
    bzero(m_real_file, FILENAME_LEN);
}

// 关闭连接
void http_conn::close_conn() {
    if(m_sockfd != -1) {
        removefd(m_epollfd, m_sockfd);//从epoll监听列表中删除
        m_sockfd = -1;//关闭文件描述符close(m_sockfd)在remove函数中
        m_user_count--; // 关闭一个连接，将客户总数量-1
    }
}


void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))//获取数据库用户名和密码
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索后，保存到结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入哈希表中
    while (MYSQL_ROW row = mysql_fetch_row(result))//检索结果集的下一行
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
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
        // m_sockfd已经设置为非阻塞了 m_read_idx用来继续上次的位置读
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0 );
        //printf("读到数据：\n%s\n",m_read_buf);
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

// 写HTTP响应
bool http_conn::write() {
    int temp = 0;
    if ( bytes_to_send == 0 ) {
        // 将要发送的字节为0，这一次响应结束。
        modfd( m_epollfd, m_sockfd, EPOLLIN ); 
        init();
        return true;
    }

    while(1) {
        // 分散写
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if ( temp <= -1 ) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if( errno == EAGAIN ) {
                modfd( m_epollfd, m_sockfd, EPOLLOUT );
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;

        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - temp;
        }

        if (bytes_to_send <= 0)
        {
            // 没有数据要发送了
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }

    }

}


// 往写缓冲中写入待发送的数据
bool http_conn::add_response( const char* format, ... ) {
    if( m_write_idx >= WRITE_BUFFER_SIZE ) {//写缓冲区已满
        return false;
    }
    va_list arg_list;
    va_start( arg_list, format );
    int len = vsnprintf( m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list );
    if( len >= ( WRITE_BUFFER_SIZE - 1 - m_write_idx ) ) {
        return false;
    }
    m_write_idx += len;
    va_end( arg_list );
    return true;
}

//响应状态行
bool http_conn::add_status_line( int status, const char* title ) {
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

//响应头部
bool http_conn::add_headers(int content_len) {
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
    return true;
}

//响应体
bool http_conn::add_content_length(int content_len) {
    return add_response( "Content-Length: %d\r\n", content_len );
}

bool http_conn::add_linger()
{
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}

bool http_conn::add_blank_line()
{
    return add_response( "%s", "\r\n" );
}

bool http_conn::add_content( const char* content )
{
    return add_response( "%s", content );
}

bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}

// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret)
    {
        case INTERNAL_ERROR:{
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) ) {
                return false;
            }
            break;
        }
        case BAD_REQUEST:{
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) ) {
                return false;
            }
            break;
        }
        case NO_RESOURCE:{
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) ) {
                return false;
            }
            break;
        }
        case FORBIDDEN_REQUEST:{
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if ( ! add_content( error_403_form ) ) {
                return false;
            }
            break;
        }
        case FILE_REQUEST:{
            add_status_line(200, ok_200_title );
            add_headers(m_file_stat.st_size);
            m_iv[ 0 ].iov_base = m_write_buf;
            m_iv[ 0 ].iov_len = m_write_idx;
            m_iv[ 1 ].iov_base = m_file_address;
            m_iv[ 1 ].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        default:
            return false;
    }

    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

//主状态机，解析请求，返回解析结果
http_conn::HTTP_CODE http_conn:: process_read(){
    HTTP_CODE ret=NO_REQUEST;
    LINE_STATUS line_status = LINE_OK;
    char* text=0;//要解析的内容的指针

    //parse_line()根据\r\n判断每一行是不是完整的行(LINE_OK),接着根据主状态机解析不同的内容
    //解析请求头和请求体时有\r\n，要一行一行解析
    //解析请求体时，报文段并没有\r\n的格式,（实际上没有实现解析请求体的功能）
    while( ( (m_check_state == CHECK_STATE_CONTENT)&& (line_status == LINE_OK) )
            || ( (line_status = parse_line()) == LINE_OK) ){
       // 获取text的地址
        text = get_line();         
        m_start_line = m_checked_index;//到了下一行，更新本行的起始位置
        printf( "got 1 http line: %s\n", text );

        //根据主状态机的状态
        switch ( m_check_state ) {
            //主状态机为解析请求行
            case CHECK_STATE_REQUESTLINE: {
                ret =parse_request_line(text);
                if(ret==BAD_REQUEST){
                    return BAD_REQUEST;
                }
                break;
            }

            //主状态机为解析请求头部
            case CHECK_STATE_HEADER: {
                ret=parse_headers(text);
                if(ret==BAD_REQUEST){
                    return BAD_REQUEST;
                }else if(ret==GET_REQUEST){//GET_REQUEST表示得到了完整请求（即无请求体）
                    return  do_request();//执行请求头部的具体请求
                }
                break;
            }

            //主状态机为解析请求体
            case CHECK_STATE_CONTENT: {
                ret = parse_content( text );
                if ( ret == GET_REQUEST ) {
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;
            }

            default: {
                return INTERNAL_ERROR;
            }

        }

    }

    return NO_REQUEST;
}

//解析HTTP请求行，包括请求方法，目标URL,HTTP版本
http_conn::HTTP_CODE http_conn::parse_request_line( char* text ){
    //请求行的格式： GET /index.html HTTP/1.1
    //char* strpbrk（str1,str2）依次检验字符串 str1 中的字符，
    //当被检验字符在字符串 str2 中也包含时，则停止检验，并返回该字符位置,未找到返回NULL
    m_url = strpbrk(text, " \t"); // 判断第二个参数中的字符哪个在text中最先出现
    if (! m_url) { 
        return BAD_REQUEST;
    }
    // GET\0/index.html HTTP/1.1
    *m_url++ = '\0';    // 置位空字符，字符串结束符，即隔断分割字符串
    char* method = text; //此时method应该为GET
    // strcasecmp()忽略大小写比较
    if ( strcasecmp(method, "GET") == 0 ) { 
        m_method = GET;
    } else if(strcasecmp(method, "POST") == 0){
        m_method = POST;
    }else {
        return BAD_REQUEST;
    }

    if(method==GET){
        // /index.html HTTP/1.1
        m_version = strpbrk( m_url, " \t" );
        if (!m_version) {
            return BAD_REQUEST;
        }
        *m_version++ = '\0';    // /index.html\0HTTP/1.1
        // \r\n已经被parse_line置为\0\0了，所以可以从m_version直接比较版本"HTTP/1.1"
        if (strcasecmp( m_version, "HTTP/1.1") != 0 ) {
            return BAD_REQUEST;
        }

        /** url请求格式
         * http://192.168.110.129:10000/index.html
        */
    //strncasecmp和strcasecmp不同之处在于只比较n个字符
        if (strncasecmp(m_url, "http://", 7) == 0 ) {   
            m_url += 7;
            // char *strchr(const char *str, int c) 在str中搜索第一次出现字符 c的位置
            m_url = strchr( m_url, '/' );//  index.html
        }

        if ( !m_url || m_url[0] != '/' ) {
            return BAD_REQUEST;
        }

    }
    else if(method==POST){
        printf("暂时没有实现post请求的解析\n");
    }
    
    m_check_state = CHECK_STATE_HEADER; // 主状态机状态变化
    return NO_REQUEST;//还需要继续解析请求头部请求体等，所以返回NO_REQUEST，只有全部解析完成才返回GET_REQUEST

    
}

// 解析HTTP请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers( char* text ){
    // 遇到空行，表示头部字段解析完毕
    if( text[0] == '\0' ) {
        // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，
        // 状态机转移到CHECK_STATE_CONTENT状态
        if ( m_content_length != 0 ) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;//还需要继续解析请求体，所以返回NO_REQUEST，只有全部解析完成才返回GET_REQUEST
        }
        // 没有请求体说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    }else if ( strncasecmp( text, "Connection:", 11 ) == 0 ) {
        // 处理Connection 头部字段  Connection: keep-alive
        text += 11;
        //strspn(str1,str2)函数返回 str1 中第一个不在字符串 str2 中出现的字符下标。
        text += strspn( text, " \t" );
        if ( strcasecmp( text, "keep-alive" ) == 0 ) {
            m_linger = true;//启用长连接
        }
    }else if ( strncasecmp( text, "Content-Length:", 15 ) == 0 ) {
        // 处理Content-Length头部字段，请求体的长度
        text += 15;
        text += strspn( text, " \t" );
        m_content_length = atol(text);
    }else if ( strncasecmp( text, "Host:", 5 ) == 0 ) {
        // 处理Host头部字段
        text += 5;
        text += strspn( text, " \t" );
        m_host = text;
    }else {
        printf( "oop! unknow header %s\n", text );
    }
    return NO_REQUEST;//还需要继续解析请求体，所以返回NO_REQUEST，只有全部解析完成才返回GET_REQUEST
}

// 我们没有真正解析HTTP请求的消息体，只是判断它是否被完整的读入了
http_conn::HTTP_CODE http_conn::parse_content( char* text ){
    if ( m_read_idx >= ( m_content_length + m_checked_index ) )
    {
        text[ m_content_length ] = '\0';
        return GET_REQUEST;
    }
    /*post请求需要解析请求体*/

    return NO_REQUEST;//报文不完整
}

//对http请求进行响应
//分析目标文件的属性，如果目标文件存在、对所有用户可读，且不是目录，
//则使用mmap将其映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request(){
    // "/home/tiancheng/webserver/resources"
    strcpy( m_real_file, doc_root );
    int len = strlen( doc_root );
    //url为空进入首页 if(m_url=="/")
    
    strncpy( m_real_file + len, m_url, FILENAME_LEN - len - 1 );
    // 获取m_real_file文件的相关的状态信息，-1失败，0成功
    if ( stat( m_real_file, &m_file_stat ) < 0 ) {//m_file_stat是传出参数，保存了文件的状态信息
        return NO_RESOURCE;
    }

    // 判断访问权限
    if ( ! ( m_file_stat.st_mode & S_IROTH ) ) {
        return FORBIDDEN_REQUEST;
    }

    // 判断是否是目录
    if ( S_ISDIR( m_file_stat.st_mode ) ) {
        return BAD_REQUEST;
    }

    // 以只读方式打开文件
    int fd = open( m_real_file, O_RDONLY );

    // 创建内存映射
    m_file_address = ( char* )mmap( 0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
    close( fd );
    return FILE_REQUEST;

}


// 对内存映射区执行munmap操作
void http_conn::unmap() {
    if( m_file_address )
    {
        munmap( m_file_address, m_file_stat.st_size );
        m_file_address = 0;
    }
}


//根据\r\n，将每一行的\r\n改为\0\0,完成分割请求报文的每一行的任务
//如果请求的某一行有问题（格式不对），直接返回NO_REQUEST(请求不完整)
http_conn::LINE_STATUS http_conn::parse_line(){
    char temp;
    for ( ; m_checked_index < m_read_idx; ++m_checked_index ) {
        temp = m_read_buf[ m_checked_index ];
        if ( temp == '\r' ) {//\r下一个寻找\n
            if ( ( m_checked_index + 1 ) == m_read_idx ) {//如果\r下一个不是\n,直接到末尾了
                return LINE_OPEN;
            }else if ( m_read_buf[ m_checked_index + 1 ] == '\n' ) {
                //把\r\n都改成\0（字符串结束符）,即切割字符串
                m_read_buf[ m_checked_index++ ] = '\0';
                m_read_buf[ m_checked_index++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;   
        }
        //防止上一次检查到\r时后面到达末尾，这次检查时需要看上一个字符是不是之前检查的\r
        else if( temp == '\n' ){
            if( ( m_checked_index > 1) && ( m_read_buf[ m_checked_index - 1 ] == '\r' ) ) {
                //同样需要切割字符串
                m_read_buf[ m_checked_index-1 ] = '\0';
                m_read_buf[ m_checked_index++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }  
    }
    return LINE_OPEN;
}


//线程池的工作线程代码，用于处理HTTP请求
void http_conn::process() {
    //解析HTTP请求,此时请求已经由主线程读到了读缓冲区
    HTTP_CODE read_ret= process_read();
    if ( read_ret == NO_REQUEST ) {
        modfd( m_epollfd, m_sockfd, EPOLLIN );
        return;
    }

    // 生成响应，将数据写入写缓冲区，之后交个主线程发送给客户端
    bool write_ret = process_write( read_ret );
    if ( !write_ret ) {
        close_conn();
    }
    modfd( m_epollfd, m_sockfd, EPOLLOUT);
}