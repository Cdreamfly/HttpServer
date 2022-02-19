#include <sys/socket.h>
#include <sys/types.h>
#include <sys/signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"
#include "tw_timer.h"

#define MAX_FD 65536
#define MAX_EVENT_NUMBER 10000
#define TIMESTOL 2

static int pipefd[2];
static time_wheel timer_lst;

void sig_handler(int sig)
{
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}
void timer_handler()
{
    /*定时处理任务，调用tick'函数*/
    timer_lst.tick();
    /*重新定时引起SIGALRM信号*/
    alarm(TIMESTOL);
}
void cb_func(http_conn *user_data)
{
    printf("close fd %d\n", user_data->get_sockfd());
    user_data->close_conn();
}
void addsig(int sig, void(handler)(int), bool restart = true)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart)
    {
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}
void show_error(int connfd, const char *info)
{
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}
int main(int argc, char *argv[])
{
    if (argc <= 2)
    {
        printf("usage:%s ip_address port_number\n", basename(argv[0]));
        return 1;
    }
    const char *ip = argv[1];
    int port = atoi(argv[2]);

    epoll_event events[MAX_EVENT_NUMBER];
    myEpoll my_epoll;
    assert(my_epoll.getEpollfd() != -1);
    http_conn::m_epollfd = &my_epoll;
    int ret = 0;
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]);
    my_epoll.addfd(pipefd[0], false);

    /*忽略SIGPIPE信号*/
    addsig(SIGPIPE, SIG_IGN);
    addsig(SIGALRM, sig_handler);
    alarm(TIMESTOL);
    /*创建线程池*/
    threadpool<http_conn> *pool = NULL;
    try
    {
        pool = new threadpool<http_conn>;
    }
    catch (...)
    {
        return 1;
    }
    MysqlDB mysqlDB;
    mysqlDB.connect("localhost", "root", "cmf.199991", "school");
    http_conn::mysqlDB = &mysqlDB;
    /*预先为每个可能的客户连接分配一个http_conn对象*/
    http_conn *users = new http_conn[MAX_FD];
    assert(users);
    int user_count = 0;
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);
    struct linger tmp = {1, 0}; //当调用close时，会立即返回，并且如果请求队列里若还有数据，也会被放弃
    setsockopt(listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &address.sin_addr);
    address.sin_port = htons(port);
    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(listenfd, 5);
    assert(ret >= 0);
    my_epoll.addfd(listenfd, false);
    bool timeout = false;

    while (true)
    {
        int number = epoll_wait(my_epoll.getEpollfd(), events, MAX_EVENT_NUMBER, -1);
        if ((number < 0) && (errno != EINTR))
        {
            printf("epoll failure\n");
            break;
        }
        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;
            if (sockfd == listenfd)
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                if (connfd < 0)
                {
                    printf("errno is:%d\n", errno);
                    continue;
                }
                if (http_conn::m_user_count >= MAX_FD)
                {
                    show_error(connfd, "Internal server busy");
                    continue;
                }
                /*初始化客户连接*/
                users[connfd].init(connfd, client_address);
                /*创建定时器，设置回调函数和超时时间*/
                tw_timer *timer = timer_lst.add_timer(TIMESTOL);
                timer->user_data = &users[connfd];
                timer->cb_func = cb_func;
                users[connfd].timer = timer;
            }
            /*处理信号*/
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1)
                {
                    printf("error signal\n");
                    continue;
                }
                else if (ret == 0)
                {
                    continue;
                }
                else
                {
                    for (int i = 0; i < ret; i++)
                    {
                        switch (signals[i])
                        {
                        case SIGALRM:
                        {
                            /*用timeout标记定时任务到期，但不立即处理定时任务，因为定时任务的优先级不高，放到最后处理*/
                            timeout = true;
                            break;
                        }
                        default:
                            break;
                        }
                    }
                }
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                /*如果有异常，直接关闭客户连接*/
                users[sockfd].close_conn();
            }
            else if (events[i].events & EPOLLIN)
            {
                tw_timer *timer = users[sockfd].timer;
                /*根据读的结果，决定是将任务添加到线程池，还是关闭连接*/
                if (users[sockfd].read())
                {
                    pool->append(users + sockfd);
                    if (timer)
                    {
                        printf("adjust timer once.\n");
                        timer = timer_lst.add_timer(TIMESTOL);
                    }
                }
                else
                {
                    timer_lst.del_timer(users[sockfd].timer);
                    cb_func(&users[sockfd]);
                }
            }
            else if (events[i].events & EPOLLOUT)
            {
                tw_timer *timer = users[sockfd].timer;
                /*根据写的结果，决定是否关闭连接*/
                if (!users[sockfd].write())
                {
                    timer_lst.del_timer(users[sockfd].timer);
                    cb_func(&users[sockfd]);
                }
                else
                {
                    if (timer)
                    {
                        printf("adjust timer once.\n");
                        timer = timer_lst.add_timer(TIMESTOL);
                    }
                }
            }
        }
        if (timeout)
        {
            timer_handler();
            timeout = false;
        }
    }
    close(listenfd);
    close(pipefd[0]);
    close(pipefd[1]);
    delete[] users;
    delete pool;
    return 0;
}