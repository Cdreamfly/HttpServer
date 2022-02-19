#include <sys/epoll.h>
#include <fcntl.h>
#include <unistd.h>

static int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

class myEpoll
{
public:
    myEpoll()
    {
        epollfd = epoll_create(5);
    }
    ~myEpoll()
    {
        close(epollfd);
    }

    int getEpollfd()
    {
        return epollfd;
    }
    void addfd(int fd, bool one_shot)
    {
        epoll_event event;
        event.data.fd = fd;
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
        if (one_shot)
        {
            event.events |= EPOLLONESHOT;
        }
        epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
        setnonblocking(fd);
    }
    void removefd(int fd)
    {
        epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
        close(fd);
    }
    void modfd(int fd, int ev)
    {
        epoll_event event;
        event.data.fd = fd;
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
        epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
    }

private:
    int epollfd;
};