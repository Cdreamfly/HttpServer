#include "http_conn.h"
#include <iostream>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>

const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file from this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the requested file.\n";
const char *doc_root = "/var/www/html";

myEpoll *http_conn::m_epollfd = nullptr;
int http_conn::m_user_count = 0;

MysqlDB *http_conn::mysqlDB = nullptr;

/*初始化新接受的连接*/
void http_conn::init(int sockfd, const sockaddr_in &addr)
{
    m_sockfd = sockfd;
    m_address = addr;
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    m_epollfd->addfd(sockfd, true);
    ++m_user_count;
    init();
}
/*初始化连接*/
void http_conn::init()
{
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;

    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}
/*关闭连接*/
void http_conn::close_conn(bool read_close)
{
    if (read_close && m_sockfd != -1)
    {
        m_epollfd->removefd(m_sockfd);
        m_sockfd = -1;
        --m_user_count;
    }
}
/*由线程池中的工作线程调用，这是处理HTTP请求的入口函数*/
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        m_epollfd->modfd(m_sockfd, EPOLLIN);
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    m_epollfd->modfd(m_sockfd, EPOLLOUT);
}
/*循环读取客户数据直到无数据可读，或关闭连接*/
bool http_conn::read()
{
    if (m_read_idx >= READ_BUFFER_SIZE) //读缓冲区已满
    {
        return false;
    }
    int bytes_read = 0; //记录接受的字节数
    while (true)        //循环读取的原因是EPOLLONESHOT一个事件只触发一次所以需要一次性读取完全否则数据丢
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0); //读缓冲区一次填不满会偏移接受字节数为下次读取起始位置，然后可读入大小要减去已读取大小。
        if (bytes_read == -1)                                                                   //如果读取失败
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK) //非阻塞描述符这两个errno不是网络出错而是设备当前不可得，在这里就是一次事件的数据读取完毕
            {
                break;
            }
            return false; //否则返回失败
        }
        if (bytes_read == 0) //客户端关闭连接
        {
            break;
        }
        m_read_idx += bytes_read; //更新缓冲区已读取字节数
    }
    return true;
}
/*非阻塞写操作*/
bool http_conn::write()
{
    int bytes_have_send = 0;
    int bytes_to_send = m_write_idx;
    if (bytes_to_send == 0)
    {
        m_epollfd->modfd(m_sockfd, EPOLLIN);
        init();
        return true;
    }
    int temp = 0;
    while (true)
    {
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if (temp <= -1)
        {
            if (errno == EAGAIN)
            {
                m_epollfd->modfd(m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }
        bytes_to_send -= temp;
        bytes_have_send += temp;
        if (bytes_to_send <= bytes_have_send)
        {
            unmap();
            if (m_linger)
            {
                init();
                m_epollfd->modfd(m_sockfd, EPOLLIN);
                return true;
            }
            else
            {
                m_epollfd->modfd(m_sockfd, EPOLLIN);
                return true;
            }
        }
    }
}
/*主状态机*/
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;
    // 满足条件：正在进行HTTP解析、读取一个完整行
    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK)) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();
        m_start_line = m_checked_idx; //记录下一行的起始位置
        std::cout << "got 1 http line: " << text << std::endl;
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
            {
                return BAD_REQUEST;
            }
            break;
        case CHECK_STATE_HEADER:
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
            {
                return BAD_REQUEST;
            }
            else if (ret == GET_REQUEST)
            {
                if (m_method == GET)
                {
                    return do_request_get(); //当获得一个完整的连接请求则调用do_request分析处理资源页文件
                }
            }
            break;
        case CHECK_STATE_CONTENT: // HTTP解析状态仍为正在解析...没有办法只好继续解析呗....解析消息体
            ret = parse_content(text);
            if (ret == GET_REQUEST)
            {
                if (m_method == POST)
                {
                    return do_request_post();
                }
            }
            line_status = LINE_OPEN;
            break;
        default:
            return INTERNAL_ERROR; //内部错误
        }
    }
}
/*填充HTTP应答*/
bool http_conn::process_write(http_conn::HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
        {
            return false;
        }
        break;
    case BAD_REQUEST:
        add_status_line(400, error_400_title);
        add_headers(strlen(error_400_form));
        if (!add_content(error_400_form))
        {
            return false;
        }
        break;
    case NO_RESOURCE:
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
        {
            return false;
        }
        break;
    case FORBIDDEN_REQUEST:
        add_status_line(403, error_403_form);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
        {
            return false;
        }
        break;
    case FILE_REQUEST:
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
            {
                return false;
            }
        }
    case GET_REQUEST:
        add_status_line(200, ok_200_title);
        add_headers(strlen(ok_200_title));
        if (!add_content(ok_200_title))
        {
            return false;
        }
        break;
    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;
}

/*下一组函数被process_read调用以分析HTTP请求*/
http_conn::HTTP_CODE http_conn::parse_request_line(char *text) //解析请求行:GET http://www.baidu.com/index.html HTTP/1.1
{
    /*如果s1、s2含有相同的字符，那么返回指向s1中第一个相同字符的指针，否则返回NULL。*/
    m_url = strpbrk(text, " \t");
    if (m_url == nullptr)
    {
        return BAD_REQUEST; //表示客户请求有语法错误
    }
    *m_url++ = '\0'; //分割请求方法和url
    char *method = text;
    if (strcasecmp(method, "GET") == 0) ////忽略大小写比较mehtod和GET
    {
        m_method = GET;
    }
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
    }
    else
    {
        return BAD_REQUEST;
    }
    m_url += strspn(m_url, " \t"); //跳过空白
    m_version = strpbrk(m_url, " \t");
    if (m_version == nullptr)
    {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "http/1.1") != 0)
    {
        return BAD_REQUEST;
    }
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;                 //跳过http://指针指向开头www.baidu.com/index.html
        m_url = strchr(m_url, '/'); //找到第一个/的位置，指针指向开头/index.html
    }
    if (!m_url || m_url[0] != '/') //判断url是否正确
    {
        return BAD_REQUEST;
    }
    m_check_state = CHECK_STATE_HEADER; //将HTTP解析状态更新为解析头部，那么HTTP解析进入解析HTTP头部。这是有限状态机
    return NO_REQUEST;
}
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    if (text[0] == '\0')
    {
        if (m_method == HEAD)
        {
            return GET_REQUEST; //已经获取了一个完整的HTTP请求
        }
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT; //则解析头部后还要解析消息体，所以HTTP解析状态仍为正在解析中...GET请求不会出现这个...
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, "\t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-Length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, "\t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, "\t");
        m_host = text;
    }
    else
    {
        std::cout << "oop! Content-Length" << std::endl;
    }
    return NO_REQUEST;
}
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx)) //若解析到缓冲区的最后位置则获得一个一个完整的连接请求
    {
        init_student();
        text[m_content_length] = '\0';
        text += strspn(text, " \t");
        char *next, *end = text + m_content_length;
        while (text < end)
        {
            next = strpbrk(text, "&"); // next: name=1111&sex=111
            if (next != nullptr)
            {
                *next = '\0'; // next:name=1111\0sex=111
            }
            if (strncasecmp(text, "name=", 5) == 0)
            {
                text += 5;
                stu.name = text; // stu.name:1111
            }
            else if (strncasecmp(text, "pwd=", 4) == 0)
            {
                text += 4;
                stu.pwd = text;
            }
            else
            {
                return BAD_REQUEST;
            }
            if (next == nullptr)
            {
                return GET_REQUEST;
            }
            text = next + 1; // next: \0sex=111&birthday=111
        }
        return GET_REQUEST;
    }
    return NO_REQUEST;
}
http_conn::HTTP_CODE http_conn::do_request_get() //用于获取资源页文件的状态
{
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    if (stat(m_real_file, &m_file_stat) < 0)
    {
        return NO_RESOURCE; //若资源页不存在则
    }
    if (!(m_file_stat.st_mode & S_IROTH))
    {
        return FORBIDDEN_REQUEST; //没有权限
    }
    if (S_ISDIR(m_file_stat.st_mode))
    {
        return BAD_REQUEST; //请求错误
    }
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}
http_conn::HTTP_CODE http_conn::do_request_post()
{
    std::cout << "do_request_post" << std::endl;
    if (strcasecmp(m_url, "/add") == 0)
    {
        mysqlDB->add(stu.name, stu.pwd);
    }
    return GET_REQUEST;
}

http_conn::LINE_STATUS http_conn::parse_line() //解析HTTP数据：将HTTP数据的每行数据提取出来，每行以回车\r和换行符\n结束
{
    /*m_checked_idx是当前正在解析的字节，m_read_idx是读缓冲区中已有的数据(客户端发送了多少HTTP请求数据到来),解析到m_read_idx号字节*/
    for (char temp = 0; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            //若为回车符：若此回车符是已读取数据的最后一个则仍需要解析改行(即该行数据还没有接收完整)
            if ((m_checked_idx + 1) == m_read_idx)
            {
                return LINE_OPEN;
            }
            else if (m_read_buf[m_checked_idx + 1] == '\n') //如果下一个字符是\n，则说明成功读取了一个完整行
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
            }
            return LINE_OK;
        }
        else if (temp == '\n') //如果读取的是换行符，则也可能读取了一个完整行
        {
            if ((m_checked_idx > 1) && (m_read_buf[m_checked_idx - 1] == '\r'))
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN; //如果所有内容都读取完也没遇到回车换行，则返回line_open表示还要继续读取客户端数据才能进一步分析
}

/*下一组函数被process_write调用以分析HTTP请求*/
void http_conn::unmap()
{
    if (m_file_address != 0)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}
bool http_conn::add_response(const char *format, ...) // HTTP应答主要是将应答数据添加到写缓冲区m_write_buf
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
    {
        return false;
    }
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - m_write_idx - 1, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);
    return true;
}
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool http_conn::add_headers(int content_length)
{
    add_content_length(content_length);
    add_linger();
    add_blank_line();
}
bool http_conn::add_content_length(int content_length)
{
    return add_response("Content-Length: %d\r\n", content_length);
}
bool http_conn::add_linger()
{
    return add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
void http_conn::init_student()
{
    stu.name = nullptr;
    stu.pwd = nullptr;
}