#ifndef HTTP_CONN_H
#define HTTP_CONN_H

#include <netinet/in.h>
#include <sys/stat.h>
#include <iostream>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include "database.h"
#include "epoll.h"
#include "tw_timer.h"

class http_conn
{
public:
    static const int FILENAME_LEN = 200;       //文件名的最大长度
    static const int READ_BUFFER_SIZE = 2048;  //读缓冲区大小
    static const int WRITE_BUFFER_SIZE = 1024; //写缓冲区大小
    tw_timer *timer;
    /*http请求方法*/
    enum METHOD
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TREACE,
        OPTIONS,
        CONNCET,
        PTAH
    };
    /*主状态机的两种可能状态，分别表示:当前正在分析请求行，当前正在分析头部字段,分析中*/
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };
    /*服务器处理HTTP请求的结果: NO_REQUEST表示请求不完整，霄要继续读取客户数据;
                              GET_REQUEST表示获得了一个完整的客户请求;
                              BAD_REQUEST表示客户请求有语法错误;
                              NO_RESOURCE没有资源
                              FORBIDDEN_REQUEST 表示客户对资源没有足够的访问权限;
                              FILE_REQUEST文件请求
                              INTERNAL_ERROR表示服务器内部错误;
                              CLOSED_CONNECTION表示客户端已经关闭连接了*/
    enum HTTP_CODE
    {
        NO_REQUEST,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };
    /*从状态机的三种可能状态，即行的读取状态，分别表示:读取到一个完整的行、行出错和行数据尚且不完整*/
    enum LINE_STATUS
    {
        LINE_OK = 0,
        LINE_BAD,
        LINE_OPEN
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    /*初始化新接受的连接*/
    void init(int sockfd, const sockaddr_in &addr);
    /*关闭连接*/
    void close_conn(bool real_close = true);
    /*处理客户请求*/
    void process();
    /*非阻塞读操作*/
    bool read();
    /*非阻塞写操作*/
    bool write();

    int get_sockfd() { return m_sockfd; }

private:
    /*初始化连接*/
    void init();
    /*解析HTTP请求*/
    HTTP_CODE process_read();
    /*填充HTTP应答*/
    bool process_write(HTTP_CODE ret);
    /*下一组函数被process_read调用以分析HTTP请求*/
    HTTP_CODE parse_request_line(char *text);
    HTTP_CODE parse_headers(char *text);
    HTTP_CODE parse_content(char *text);
    HTTP_CODE do_request_get();
    HTTP_CODE do_request_post();
    char *get_line() { return m_read_buf + m_start_line; }
    LINE_STATUS parse_line();
    /*下一组函数被process_write调用以分析HTTP请求*/
    void unmap();
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();
    void init_student();

public:
    /*所有socket上的事件都被注册到同一个epoll内核事件表中所以将epoll文件描述符设置成静态的*/
    static myEpoll *m_epollfd;
    /*统计用户数量*/
    static int m_user_count;

    static MysqlDB *mysqlDB;

private:
    /*该HTTP连接的socket和对方的socket地址*/
    int m_sockfd;
    sockaddr_in m_address;
    /*读缓冲区*/
    char m_read_buf[READ_BUFFER_SIZE];
    /*标识读缓冲中已经读入的客户数据的最后一个字符的下一个位置*/
    int m_read_idx;
    /*当前正在分析的字符在读缓冲区中的位置*/
    int m_checked_idx;
    /*当前正在解析的行的起始位置*/
    int m_start_line;
    /*写缓冲区*/
    char m_write_buf[WRITE_BUFFER_SIZE];
    /*写缓冲区中待发送的字符数*/
    int m_write_idx;
    /*主状态机当前所处状态*/
    CHECK_STATE m_check_state;
    /*请求方法*/
    METHOD m_method;
    /*客户请求的目标文件的完整路径，其内容等于doc_root + m_url, doc_root是网站根目录*/
    char m_real_file[FILENAME_LEN];
    /*客户请求的目标文件的文件名*/
    char *m_url;
    /*HTTP协议版本号*/
    char *m_version;
    /*主机名*/
    char *m_host;
    /*HTTP请求的消息体的长度*/
    int m_content_length;
    /*HTTP请求是否要保持连接*/
    bool m_linger;
    /*客户请求的目标文件被mmap到内存中的起始位置*/
    char *m_file_address;
    /*目标文件的状态。通过它我们可以判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息*/
    struct stat m_file_stat;
    /*我们将采用writev来执行写操作，所以定义下面两个成员，其中m_iv_count表示被写内存块的数量*/
    struct iovec m_iv[2];
    int m_iv_count;
    student stu;
};

#endif