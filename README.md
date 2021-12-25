## httpserver
- 只支持get请求的多线程HTTP服务器。
## 环境
- gcc version 9.3.0 (Ubuntu 9.3.0-17ubuntu1~20.04)
## 运行
```
gcc http.c -o http -lpthread
```
```
sudo ./http
```
在游览器输入：http://127.0.0.1/index.html