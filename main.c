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

void do_http_request(int);
void do_http_response(int);
void do_http_response(int, const char *);
int get_line(int,char*,int);
void not_found(int);
void inner_error(int);
void unimplemented(int);
void bad_request(int);
int handers(int,FILE*);
int cat(int,FILE*);

int main(int argc,char**argv)
{
    int server;
    server = socket(AF_INET,SOCK_STREAM,0);

    struct sockaddr_in server_addr;
    memset(&server_addr,0,sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(SERVER_PORT);

    if(bind(server,(struct sockaddr*)&server_addr,sizeof(server_addr)) < 0)
    {
        perror("bind() error");
    }

    if(listen(server,128) < 0)
    {
        perror("listen() error");
    }
    printf("等待连接请求。。。\n");
    while(1)
    {
        int client;
        socklen_t client_size;
        struct sockaddr_in client_addr;
        memset(&client_addr,0,sizeof(client_addr));
        client_size = sizeof(client_addr);
        if((client = accept(server,(struct sockaddr*)&client_addr,&client_size)) < 0)
        {
            perror("accept() error");
        }
        //打印客服端IP地址和端口号
        char client_ip[64];
        printf("client ip: %s\t port : %d\n",
                inet_ntop(AF_INET, &client_addr.sin_addr.s_addr,client_ip,sizeof(client_ip)),
                ntohs(client_addr.sin_port));
        //解析http请求
        do_http_request(client);
        close(client);
    }
    close(server);
    return 0;
}
void do_http_request(int client)
{
    int len = 0;
    char buf[SMALL_BUF];
    char method[256];
    char url[512];
    char path[1024];
    struct stat st;

    len = get_line(client,buf,SMALL_BUF);
    if(len > 0)
    {
        int i = 0,j = 0;
        while(!isspace(buf[j]) && (i<sizeof(method)-1)) //遇空格结束
        {
            method[i] = buf[j];
            ++i;
            ++j;
        }
        method[i] = '\0';
        printf("request method: %s\n", method);
        if(strncasecmp(method,"GET",i) == 0)
        {
            printf("method = GET\n");
            while(isspace(buf[j++]));//跳过白空格
            i = 0;
            while(!isspace(buf[j]) && (i<sizeof(url)-1))
            {
                url[i] = buf[j];
                ++i;
                ++j;
            }
            url[i] = '\0';
            printf("url: %s\n", url);
            //继续读取http 头部
		    do{
			    len = get_line(client, buf, SMALL_BUF);
			    printf("read: %s\n", buf);
		    }while(len > 0);
            /*定位服务器本地的html文件*/			
			//处理url 中的?
            {
				char *pos = strchr(url, '?');
				if(pos)
                {
					*pos = '\0';
					printf("real url: %s\n", url);
				}
			}
            sprintf(path,"./html_docs/%s",url);
            printf("path: %s\n", path);
            //执行http 响应
			//判断文件是否存在，如果存在就响应200 OK，同时发送相应的html 文件,如果不存在，就响应 404 NOT FOUND.
            if(stat(path,&st) == -1)
            {
                fprintf(stderr, "stat %s failed. reason: %s\n", path, strerror(errno));
			    not_found(client);

            }
            else
            {
                if(S_ISDIR(st.st_mode))
                {
                    strcat(path,"/index.html");
                }
                do_http_response(client,path);
            }
            
        }
        else    //非get请求, 读取http 头部，并响应客户端 501 	Method Not Implemented
        {
            fprintf(stderr, "warning! other request [%s]\n", method);
			do{
			    len = get_line(client, buf, SMALL_BUF);
			    printf("read: %s\n", buf);
			
		    }while(len>0);
            unimplemented(client);
        }
    }
    else
    {
        //请求格式有问题，出错处理
		bad_request(client);   //在响应时再实现
    }

}
////返回值： -1 表示读取出错， 等于0表示读到一个空行， 大于0 表示成功读取一行
int get_line(int client,char*buf,int size)
{
    int count = 0;
    char ch = '\0';
    int len = 0;

    while((count < size - 1) && ch != '\n') //读取一行到换行结束
    {
        len = read(client,&ch,1);   //每次读取一个字符
        if(len == 1)    //读取成功
        {
            if(ch == '\r')  //过滤回车
            {
                continue;
            }
            else if(ch == '\n') //读到换行了就结束
            {
                buf[count] = '\0';
                break;
            }
            buf[count] = ch;    //正常的存入buf
            ++count;
        }
        else if(len = -1)//读取出错
        {
            perror("read failed");
            count = -1;
            break;
        }
        else// read 返回0,客户端关闭sock 连接.
        {
            fprintf(stderr,"client close\n");
            count = -1;
            break;
        }
    }
    if(len >= 0)
    {
        buf[count] = '\0';
    }
    return count;
}
void do_http_response(int client)
{
    const char *main_header = "HTTP/1.0 200 OK\r\nServer: Cmf Server\r\nContent-Type: text/html\r\nConnection: Close\r\n";
    const char * welcome_content = "\
<html lang=\"zh-CN\">\n\
<head>\n\
<meta content=\"text/html; charset=utf-8\" http-equiv=\"Content-Type\">\n\
<title>This is a test</title>\n\
</head>\n\
<body>\n\
<div align=center height=\"500px\" >\n\
<br/><br/><br/>\n\
<h2>大家好，欢迎来到奇牛学院VIP 试听课！</h2><br/><br/>\n\
<form action=\"commit\" method=\"post\">\n\
尊姓大名: <input type=\"text\" name=\"name\" />\n\
<br/>芳龄几何: <input type=\"password\" name=\"age\" />\n\
<br/><br/><br/><input type=\"submit\" value=\"提交\" />\n\
<input type=\"reset\" value=\"重置\" />\n\
</form>\n\
</div>\n\
</body>\n\
</html>";

    char send_buf[64];
    int wc_len = strlen(welcome_content);
    int len = write(client, main_header, strlen(main_header));

    fprintf(stdout, "... do_http_response...\n");
    fprintf(stdout, "write[%d]: %s", len, main_header);

    len =snprintf(send_buf, 64,"Content-Length: %d\r\n\r\n", wc_len);
    len = write(client, send_buf, len);
    fprintf(stdout, "write[%d]: %s", len, send_buf);

    len = write(client, welcome_content, wc_len);
    fprintf(stdout, "write[%d]: %s", len, welcome_content);

}

void not_found(int client){
	const char * reply = "HTTP/1.0 404 NOT FOUND\r\n\
Content-Type: text/html\r\n\
\r\n\
<HTML lang=\"zh-CN\">\r\n\
<meta content=\"text/html; charset=utf-8\" http-equiv=\"Content-Type\">\r\n\
<HEAD>\r\n\
<TITLE>NOT FOUND</TITLE>\r\n\
</HEAD>\r\n\
<BODY>\r\n\
	<P>文件不存在！\r\n\
    <P>The server could not fulfill your request because the resource specified is unavailable or nonexistent.\r\n\
</BODY>\r\n\
</HTML>";

	int len = write(client, reply, strlen(reply));
	fprintf(stdout,"%s", reply);
	
	if(len <=0){
		fprintf(stderr, "send reply failed. reason: %s\n", strerror(errno));
	}
	
	
}
void inner_error(int client)
{
	const char * reply = "HTTP/1.0 500 Internal Sever Error\r\n\
Content-Type: text/html\r\n\
\r\n\
<HTML lang=\"zh-CN\">\r\n\
<meta content=\"text/html; charset=utf-8\" http-equiv=\"Content-Type\">\r\n\
<HEAD>\r\n\
<TITLE>Inner Error</TITLE>\r\n\
</HEAD>\r\n\
<BODY>\r\n\
    <P>服务器内部出错.\r\n\
</BODY>\r\n\
</HTML>";

	int len = write(client, reply, strlen(reply));
	fprintf(stdout, reply);
	
	if(len <=0){
		fprintf(stderr, "send reply failed. reason: %s\n", strerror(errno));
	}
}
void unimplemented(int client)
{
    const char * reply = "HTTP/1.0 501 Internal Sever Error\r\n\
Content-Type: text/html\r\n\
\r\n\
<HTML lang=\"zh-CN\">\r\n\
<meta content=\"text/html; charset=utf-8\" http-equiv=\"Content-Type\">\r\n\
<HEAD>\r\n\
<TITLE>Method Not Implemented</TITLE>\r\n\
</HEAD>\r\n\
<BODY>\r\n\
    <P>Method Not Implemented\r\n\
</BODY>\r\n\
</HTML>";

	int len = write(client, reply, strlen(reply));
	fprintf(stdout, reply);
	
	if(len <=0){
		fprintf(stderr, "send reply failed. reason: %s\n", strerror(errno));
	}    
}
void bad_request(int client)
{
    const char * reply = "HTTP/1.0 400 Internal Sever Error\r\n\
Content-Type: text/html\r\n\
\r\n\
<HTML lang=\"zh-CN\">\r\n\
<meta content=\"text/html; charset=utf-8\" http-equiv=\"Content-Type\">\r\n\
<HEAD>\r\n\
<TITLE>BAD REQUEST</TITLE>\r\n\
</HEAD>\r\n\
<BODY>\r\n\
    <P>Your browser sent a bad request！.\r\n\
</BODY>\r\n\
</HTML>";

	int len = write(client, reply, strlen(reply));
	fprintf(stdout, reply);
	
	if(len <=0){
		fprintf(stderr, "send reply failed. reason: %s\n", strerror(errno));
	}
}

void do_http_response(int client, const char *path)
{
    int ret = 0;
    FILE*resource = NULL;
    resource = fopen(path,"r");
    if(resource == NULL)
    {
        not_found(client);
        return ;
    }
    //1.发送http 头部
    ret = handers(client,resource);
    //2.发送http body .
    if(!ret)
    {
        cat(client,resource);
    }
    fclose(resource);
}
int handers(int client,FILE*resource)
{

}
int cat(int client,FILE*resource)
{

}