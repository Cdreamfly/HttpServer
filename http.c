#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <ctype.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>

#define SERVER_PORT 80
#define SMALL_BUFF 1024

static int debug = 1;

int get_line(int sock, char *buf, int size);
void* do_http_request(void *client_sock);
void do_http_response(int client_sock, const char *path);
int  headers(int client_sock, FILE *resource);
void cat(int client_sock, FILE *resource);

void not_found(int client_sock);//404 
void unimplemented(int client_sock);//501
void bad_request(int client_sock); //400
void inner_error(int client_sock);//500

int main()
{

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;//选择协议族IPV4
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);//监听本地所有IP地址
    server_addr.sin_port = htons(SERVER_PORT);//绑定端口号

    if(bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
	{
		perror("bind error!");
	}

    if(listen(sock, 128) < 0)
	{
		perror("listen error!");
	}

    printf("等待客户端的连接...\n");

    while(1)
    {
        struct sockaddr_in client;
        int client_sock;
        char client_ip[64];
		pthread_t id;
		int* pclient_sock = NULL;
        socklen_t client_addr_len;

        client_addr_len = sizeof(client);
        client_sock = accept(sock, (struct sockaddr *)&client, &client_addr_len);

        //打印客服端IP地址和端口号
        printf("client ip: %s\t port : %d\n",
                 inet_ntop(AF_INET, &client.sin_addr.s_addr,client_ip,sizeof(client_ip)),
                 ntohs(client.sin_port));
				 
        /*处理http 请求,读取客户端发送的数据*/
        //do_http_request(client_sock);
		
		//启动线程处理http 请求
		pclient_sock = (int*)malloc(sizeof(int));
		*pclient_sock = client_sock;
		
		pthread_create(&id, NULL, do_http_request, (void *)pclient_sock);
    }
    close(sock);
    return 0;
}

void* do_http_request(void* pclient_sock)
{
	int len = 0;
	char buf[SMALL_BUFF];
	char method[64];
	char url[256];
	char path[512];
	int client_sock = *(int*)pclient_sock;
	struct stat  st;
	/*读取客户端发送的http 请求*/
	//1.读取请求行
	len = get_line(client_sock, buf, sizeof(buf));
	
	if(len > 0)
	{//读到了请求行
		int i=0, j=0;
		while(!isspace(buf[j]) && (i<sizeof(method)-1))
		{
			method[i] = buf[j];
			i++;
			j++;
		}
		
		method[i] = '\0';
		if(debug) printf("request method: %s\n", method);
		
		if(strncasecmp(method, "GET", i)==0)
		{ //只处理get请求
	        if(debug) printf("method = GET\n");
		    //获取url
		    while(isspace(buf[j++]));//跳过白空格
		    i = 0;
		    while(!isspace(buf[j]) && (i<sizeof(url)-1))
			{
			    url[i] = buf[j];
			    i++;
			    j++;
		    }
		    url[i] = '\0';
		    if(debug) printf("url: %s\n", url);
			//继续读取http 头部
		    do
			{
			    len = get_line(client_sock, buf, sizeof(buf));
			    if(debug) printf("read: %s\n", buf);
			
		    }while(len>0);
			//***定位服务器本地的html文件***
			//处理url 中的?
			{
				char *pos = strchr(url, '?');
				if(pos)
				{
					*pos = '\0';
					printf("real url: %s\n", url);
				}
			}
			
			sprintf(path, "./html_docs/%s", url);
			if(debug) printf("path: %s\n", path);
			//执行http 响应
			//判断文件是否存在，如果存在就响应200 OK，同时发送相应的html 文件,如果不存在，就响应 404 NOT FOUND.
			if(stat(path, &st)==-1)
			{//文件不存在或是出错
				fprintf(stderr, "stat %s failed. reason: %s\n", path, strerror(errno));
			    not_found(client_sock);
			}
			else //文件存在
			{
			    if(S_ISDIR(st.st_mode))
				{
					strcat(path, "/index.html");
				}
				do_http_response(client_sock, path);	
			}
	    }
		else//非get请求, 读取http 头部，并响应客户端 501 	Method Not Implemented
		{
			fprintf(stderr, "warning! other request [%s]\n", method);
			do
			{
			    len = get_line(client_sock, buf, sizeof(buf));
			    if(debug) printf("read: %s\n", buf);
			
		    }while(len > 0);
			unimplemented(client_sock);   //请求未实现
		}	
	}
	else //请求格式有问题，出错处理
	{
		bad_request(client_sock);   //在响应时再实现
	}
	close(client_sock);
	if(pclient_sock) free(pclient_sock);//释放动态分配的内存
	return NULL;
}

void do_http_response(int client_sock, const char *path)
{
	int ret = 0;
	FILE *resource = NULL;
	
	resource = fopen(path, "r");
	if(resource == NULL)
	{
		not_found(client_sock);
		return ;
	}
	//1.发送http 头部
	ret = headers(client_sock, resource);
	//2.发送http body .
	if(!ret)
	{
	    cat(client_sock, resource);
	}
	fclose(resource);
}

/****************************
 *返回关于响应文件信息的http 头部
 *输入： 
 *     client_sock - 客服端socket 句柄
 *     resource    - 文件的句柄 
 *返回值： 成功返回0 ，失败返回-1
******************************/
int headers(int client_sock, FILE *resource)
{
	struct stat st;
	int fileid = 0;
	char tmp[64];
	char buf[SMALL_BUFF] = {0};
	strcpy(buf, "HTTP/1.0 200 OK\r\n");
	strcat(buf, "Server: Martin Server\r\n");
	strcat(buf, "Content-Type: text/html\r\n");
	strcat(buf, "Connection: Close\r\n");
	
	fileid = fileno(resource);
	
	if(fstat(fileid, &st) == -1)
	{
		inner_error(client_sock);
		return -1;
	}
	
	snprintf(tmp, 64, "Content-Length: %ld\r\n\r\n", st.st_size);
	strcat(buf, tmp);
	
	if(debug) fprintf(stdout, "header: %s\n", buf);
	
	if(send(client_sock, buf, strlen(buf), 0) < 0)
	{
		fprintf(stderr, "send failed. data: %s, reason: %s\n", buf, strerror(errno));
		return -1;
	}
	return 0;
}

/****************************
 *说明：实现将html文件的内容按行
        读取并送给客户端
 ****************************/
void cat(int client_sock, FILE *resource)
{
	char buf[SMALL_BUFF];
	
    fgets(buf, sizeof(buf), resource);
	
	while(!feof(resource))
	{
		int len = write(client_sock, buf, strlen(buf));
		
		if(len<0){//发送body 的过程中出现问题,怎么办？1.重试？ 2.
		
			fprintf(stderr, "send body error. reason: %s\n", strerror(errno));
			break;
		}
		if(debug) fprintf(stdout,"%s", buf);
		fgets(buf, sizeof(buf), resource);
		
	}
}

//返回值： -1 表示读取出错， 等于0表示读到一个空行， 大于0 表示成功读取一行
int get_line(int sock, char *buf, int size)
{
	int count = 0;
	char ch = '\0';
	int len = 0;
	
	while((count < size - 1) && ch!='\n')
	{
		len = read(sock, &ch, 1);
		
		if(len == 1)
		{
			if(ch == '\r')
			{
				continue;
			}
			else if(ch == '\n')
			{
				break;
			}
			//这里处理一般的字符
			buf[count] = ch;
			count++;	
		}
		else if(len == -1)//读取出错
		{
			perror("read failed");
			count = -1;
			break;
		}
		else // read 返回0,客户端关闭sock 连接.
		{
			fprintf(stderr, "client close.\n");
			count = -1;
			break;
		}
	}
	if(count >= 0) buf[count] = '\0';
	return count;
}


void not_found(int client_sock)
{
	const char * reply = "HTTP/1.0 404 NOT FOUND\r\n\
Content-Type: text/html\r\n\
\r\n\
<HTML lang=\"zh-CN\">\r\n\
<meta content=\"text/html; charset=utf-8\" http-equiv=\"Content-Type\">\r\n\
<HEAD>\r\n\
<TITLE>NOT FOUND</TITLE>\r\n\
</HEAD>\r\n\
<BODY>\r\n\
	<P>文件不存在!\r\n\
    <P>The server could not fulfill your request because the resource specified is unavailable or nonexistent.\r\n\
</BODY>\r\n\
</HTML>";

	int len = write(client_sock, reply, strlen(reply));
	if(debug) fprintf(stdout,"%s", reply);
	
	if(len <=0)
	{
		fprintf(stderr, "send reply failed. reason: %s\n", strerror(errno));
	}
}

void unimplemented(int client_sock)
{
	const char * reply = "HTTP/1.0 501 Method Not Implemented\r\n\
Content-Type: text/html\r\n\
\r\n\
<HTML>\r\n\
<HEAD>\r\n\
<TITLE>Method Not Implemented</TITLE>\r\n\
</HEAD>\r\n\
<BODY>\r\n\
    <P>HTTP request method not supported.\r\n\
</BODY>\r\n\
</HTML>";

	int len = write(client_sock, reply, strlen(reply));
	if(debug) fprintf(stdout,"%s", reply);
	
	if(len <=0)
	{
		fprintf(stderr, "send reply failed. reason: %s\n", strerror(errno));
	}
}

void bad_request(int client_sock)
{
    const char * reply = "HTTP/1.0 400 BAD REQUEST\r\n\
Content-Type: text/html\r\n\
\r\n\
<HTML>\r\n\
<HEAD>\r\n\
<TITLE>BAD REQUEST</TITLE>\r\n\
</HEAD>\r\n\
<BODY>\r\n\
    <P>Your browser sent a bad request!\r\n\
</BODY>\r\n\
</HTML>";

    int len = write(client_sock, reply, strlen(reply));
    if(len <= 0)
	{
        fprintf(stderr, "send reply failed. reason: %s\n", strerror(errno));
    }	
}


void inner_error(int client_sock)
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

	int len = write(client_sock, reply, strlen(reply));
	if(debug) fprintf(stdout, "%s",reply);
	
	if(len <=0)
	{
		fprintf(stderr, "send reply failed. reason: %s\n", strerror(errno));
	}	
}

