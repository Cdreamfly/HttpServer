#include<stdio.h>
#include<sys/stat.h>
#include<sys/errno.h>
#include<sys/socket.h>
#include<arpa/inet.h>
#include<string.h>
#include<unistd.h>
#include<ctype.h>

#define SERVER_PORT 80  //端口
#define SMALL_BUF 1024   //读取一行buffer大小

int main(int argc,char**argv)
{
    printf("hello world!\n");
    return 0;
}