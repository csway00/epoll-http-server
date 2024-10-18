/*server.c*/
#include <sys/epoll.h>
#include <dirent.h>
#include <ctype.h>
#include <strings.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "wrap.h"

#define SERV_PORT 9999
#define BUF_SIZE 4096
#define OPEN_MAX 5000
#define MY_DIR "/home/b/dir"  //供浏览器访问的目录

int listenFd, clitFd;

void sendFile(int cfd, const char* file);
void sendRespond(int cfd, int no, char* disp, char* type, int len);

// 16进制数转化为10进制
int hexit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;

	return 0;
}

/*
 *  这里的内容是处理%20之类的东西！是"解码"过程。
 *  %20 URL编码中的‘ ’(space)
 *  %21 '!' %22 '"' %23 '#' %24 '$'
 *  %25 '%' %26 '&' %27 ''' %28 '('......
 *  相关知识html中的‘ ’(space)是&nbsp
 */
void encode_str(char* to, int tosize, const char* from)
{
	int tolen;

	for (tolen = 0; *from != '\0' && tolen + 4 < tosize; ++from) {    
		if (isalnum(*from) || strchr("/_.-~", *from) != (char*)0) {      
			*to = *from;
			++to;
			++tolen;
		} else {
			sprintf(to, "%%%02x", (int) *from & 0xff);
			to += 3;
			tolen += 3;
		}
	}
*to = '\0';
}

void decode_str(char *to, char *from)
{
	for ( ; *from != '\0'; ++to, ++from  ) {     
		if (from[0] == '%' && isxdigit(from[1]) && isxdigit(from[2])) {       
			*to = hexit(from[1])*16 + hexit(from[2]);
			from += 2;                      
		} else {
			*to = *from;
		}
	}
	*to = '\0';
}

// 获取一行 \r\n 结尾的数据
int getLine(int cfd, char *buf, int size)
{
	int i = 0;
	char c = '\0';
	int n;
	while ((i < size-1) && (c != '\n')) {
		n = recv(cfd, &c, 1, 0);
		if (n > 0) {
			if (c == '\r') {
				n = recv(cfd, &c, 1, MSG_PEEK);
				if ((n > 0) && (c == '\n')) {
					recv(cfd, &c, 1, 0);
				} else {
					c = '\n';
				}
			}
			buf[i] = c;
			i++;
		}
		/*
		   else {
		   if (errno == EAGAIN) {
		   printf("-------EAGAIN----\n");
		   continue;
		   }
		   else if (errno == EINTR) {
		   printf("----EINTR----\n");
		   continue;
		   }
		   else {
		   c = '\n';
		   }
		   }
		   */
		else {
			c = '\n';
		}
	}
	buf[i] = '\0';

	if (-1 == n)
		i = n;

	return i;
}


// 通过文件名获取文件的类型
const char *getFileType(const char *name)
{
	char* dot;

	// 自右向左查找‘.’字符, 如不存在返回NULL
	dot = strrchr(name, '.');   
	if (dot == NULL)
		return "text/plain; charset=utf-8";
	if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0)
		return "text/html; charset=utf-8";
	if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0)
		return "image/jpeg";
	if (strcmp(dot, ".gif") == 0)
		return "image/gif";
	if (strcmp(dot, ".png") == 0)
		return "image/png";
	if (strcmp(dot, ".css") == 0)
		return "text/css";
	if (strcmp(dot, ".au") == 0)
		return "audio/basic";
	if (strcmp( dot, ".wav" ) == 0)
		return "audio/wav";
	if (strcmp(dot, ".avi") == 0)
		return "video/x-msvideo";
	if (strcmp(dot, ".mov") == 0 || strcmp(dot, ".qt") == 0)
		return "video/quicktime";
	if (strcmp(dot, ".mpeg") == 0 || strcmp(dot, ".mpe") == 0)
		return "video/mpeg";
	if (strcmp(dot, ".vrml") == 0 || strcmp(dot, ".wrl") == 0)
		return "model/vrml";
	if (strcmp(dot, ".midi") == 0 || strcmp(dot, ".mid") == 0)
		return "audio/midi";
	if (strcmp(dot, ".mp3") == 0)
		return "audio/mpeg";
	if (strcmp(dot, ".ogg") == 0)
		return "application/ogg";
	if (strcmp(dot, ".pac") == 0)
		return "application/x-ns-proxy-autoconfig";

	if (strcmp(dot, ".mp4") == 0)
		return "video/mp4";
	if (strcmp(dot, ".pdf") == 0)
		return "application/pdf";
	if (strcmp(dot, ".doc") == 0 || strcmp(dot, ".docx"))
		return "application/msword" ;

	return "text/plain; charset=utf-8";
}

void sendError(int cfd, int status, char* title, char* text) {
	char buf[4096] = {0};
	printf(buf, "%s %d %s\r\n", "HTTP/1.1", status, title);
	sprintf(buf+strlen(buf), "Content-Type:%s\r\n", "text/html");
	sprintf(buf+strlen(buf), "Content-Length:%d\r\n", -1);
	sprintf(buf+strlen(buf), "Connection: close\r\n");
	send(cfd, buf, strlen(buf), 0);
	send(cfd, "\r\n", 2, 0);

	memset(buf, 0, sizeof(buf));

	sprintf(buf, "<html><head><title>%d %s </title></head>\n", status, title);
	sprintf(buf+strlen(buf), "<body bgcolor = \"#cc99cc\"><h2 align = \"center\"> %d %s </h4>\n", status, title);
	sprintf(buf+strlen(buf), "%s\n", text);
	sprintf(buf+strlen(buf), "<hr>\n</body>\n</html>\n");
	send(cfd, buf, strlen(buf), 0);
}

void sendError2(int cfd, int status, char* title, char* text) {
	//sendRespond(cfd, 200, "OK", "Content-Type: text/plain; charset=iso-8859-1", sbuf.st_size); //文本
	sendRespond(cfd, 404, title, "Content-Type: text/html", -1);
	sendFile(cfd, "404.html");
}


void disConnect(int fd, int epTree) {
	int ret;
	ret = epoll_ctl(epTree, EPOLL_CTL_DEL, fd, NULL); //将该文件描述符从红黑树移除
	if (ret == -1) {
		perr_exit("epoll_ctl error");
	}
	printf("clitArr[%d] is don't connect\n", fd);
	close(fd);
}

//客户端的fd,错误号，错误描述，回发文件类型，文件长度
void sendRespond(int cfd, int no, char* disp, char* type, int len) {
	char buf[1024];
	sprintf(buf, "HTTP/1.1 %d %s\r\n", no, disp);
	sprintf(buf + strlen(buf), "%s\r\n", type);
	sprintf(buf + strlen(buf), "Content-Length:%d\r\n", -1);
	sprintf(buf + strlen(buf), "%s\r\n", type);
	send(cfd, buf, strlen(buf), 0);
	send(cfd, "\r\n", 2, 0);
}

//以下注释为错误的代码
void sendDir(int cfd, char* dirname) {
	// 拼一个html页面<table></table>
	char buf[4096] = {0};
	sprintf(buf, "<html><head><title>目录名: %s</title></head>", dirname);
	sprintf(buf + strlen(buf), "<body><h1>当前目录: %s</h1><table>", dirname);

	char enstr[1024] = {0};
	char path[1024] = {0};

	//目录项二级指针
	struct dirent** ptr;
	int num = scandir(dirname, &ptr, NULL, alphasort);
	int dirLen = strlen(dirname);
	//遍历
	for (int i = 0; i < num; i++) {
		char* name = ptr[i]->d_name;

		//拼接文件的完整路径
		if (dirname[dirLen-1] != '/') {
			sprintf(path, "%s/%s",dirname, name);
		} else {
			sprintf(path, "%s%s",dirname, name);
		}

		printf("path = %s =============\n", path);

		struct stat st;
		stat(path, &st);

		//编码生成 %E5, %A7 之类的东西
		encode_str(enstr, sizeof(enstr), name);

		//如果是文件
		if(S_ISREG(st.st_mode)) {
			sprintf(buf+strlen(buf),
					"<tr><td><a href=\"%s\">%s</a></td><td>%ld</td></tr>",
					enstr, name, (long)st.st_size);
		} else if(S_ISDIR(st.st_mode)) {		// 如果是目录
			sprintf(buf+strlen(buf),
					"<tr><td><a href=\"%s/\">%s/</a></td><td>%ld</td></tr>",
					enstr, name, (long)st.st_size);
		}

		int ret = send(cfd, buf, strlen(buf), 0);
		if (ret == -1) {
			if (errno == EAGAIN) {
				continue;
			}
			else if (errno == EINTR) {
				continue;
			}
			else {
				perr_exit("send error");
			}
		}
		memset(buf, 0, sizeof(buf));

		//字符串拼接
	}
	sprintf(buf+strlen(buf), "</table></body></html>");
	send(cfd, buf, strlen(buf), 0);

	printf("dir message send OK!!!\n");

#if 0 //预编译指令，表示之后的代码不执行 改为非0值即可执行
	//打开目录
	DIR* dir = opendir(dirname);
	if (dir == NULL) {
		perr_exit("opendir error");
	}

	//读目录
	struct dirent* ptr = NULL;
	while ( (ptr = readdir(dir))) {
		char* name = ptr->d_name;
	}
	closedir(dir);

#endif	
}


void sendFile(int cfd, const char* file) {

	char buf[4096] = {0};
	int n;
	int ret;
	int fd = open(file, O_RDONLY);
	if (fd == -1) {
		sendError2(cfd, 404, "Not Found", "NO such file or direntry");
		perror("open error");
		exit(1);
	}

	while ((n = Read(fd, buf, sizeof(buf))) > 0) {
		ret = send(cfd, buf, n, 0);
		if (ret == -1) {
			if (errno == EAGAIN) {
				continue;
			}
			else if (errno == EINTR) {
				continue;
			}
			else {
				perr_exit("send error");
			}
		}
	}
	close(fd);
}


void httpRequest(int cfd, char* file) {
	struct stat sbuf;
	int ret = stat(file, &sbuf); //判断文件是否存在
	if (ret == -1) {
		//回发浏览器404错误页面
		sendError2(cfd, 404, "Not Found", "NO such file or direntry");
		perror("stat error");
		return;
	}
	if (S_ISREG(sbuf.st_mode)) { //如果是文件
		//回发http协议应答；
		const char *type = getFileType(file);
		char str[1024] = {0};
		sprintf(str, "Content-Type: %s", type);
		sendRespond(cfd, 200, "OK", str, sbuf.st_size); 

		//回发给客户端请求数据内容
		sendFile(cfd, file);
	}
	else if (S_ISDIR(sbuf.st_mode)) {
		//回发http协议应答；
		sendRespond(cfd, 200, "OK", "Content-Type: text/html; charset=utf-8", -1); //文本

		//回发给客户端请求数据内容
		sendDir(cfd, file);
	}

}

void doRead(int fd, int epTree) {
	char line[1024];
	int ret = getLine(fd, line, sizeof(line));
	if (ret == -1) {
		perr_exit("getLine error");
	}
	else if (ret == 0) {
		disConnect(fd, epTree);
		return;
	}

	else {
		char mathed[16], path[256], protocol[40];
		sscanf(line, "%[^ ] %[^ ] %[^ ]", mathed, path, protocol);
		printf("mathed = %s, path = %s, protocol = %s\n", mathed, path, protocol);

		while (1) {
			int ret = getLine(fd, line, sizeof(line));
			if (ret == -1) {
				//perr_exit("getLine error");
				break;
			}
			else if (ret == 0) {
				break;
			}
			if (ret == '\n') {
				break;
			}
		}

		// 转码 将不能识别的中文乱码 -> 中文
		// 解码 %23 %34 %5f
		decode_str(path, path);
		if (strncasecmp(mathed, "GET", 3) == 0) {
			char *file = path+1; //去掉path中的/获取访问文件名
			if (strcmp(path, "/") == 0) {
				file = "./";
			}
			httpRequest(fd, file);
		}
	}

}


void doSockBindListen() {

	int ret;
	listenFd = Socket(AF_INET, SOCK_STREAM, 0);

	//端口复用
	int opt = 1;
	setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	// 2. bind()  绑定服务器地址结构 int bind(int
	struct sockaddr_in servAddr;
	bzero(&servAddr, sizeof(servAddr));
	servAddr.sin_family = AF_INET;
	servAddr.sin_port =     htons(SERV_PORT); //true
	servAddr.sin_addr.s_addr = htonl(INADDR_ANY);  //本地-》网络(IP)

	ret = Bind(listenFd, (struct sockaddr *)&servAddr, sizeof(servAddr));

	//3. listen() 设置监听上限
	Listen(listenFd, 128);
}

void doAccept() {
	int ret;

	int treeFd = epoll_create(OPEN_MAX);

	struct epoll_event myEvent;
	myEvent.data.fd = listenFd;
	myEvent.events = EPOLLIN | EPOLLET;

	ret = epoll_ctl(treeFd, EPOLL_CTL_ADD, listenFd, &myEvent);
	if (ret == -1) {
		perr_exit("epoll_ctl");
	}

	struct epoll_event clitArr[OPEN_MAX];

	int n;
	struct sockaddr_in clitAddr;
	socklen_t clit_addr_len = sizeof(clitAddr);

	char ip[64] = {0};
	int nWait;
	while (1) {
		nWait = epoll_wait(treeFd, clitArr, OPEN_MAX, -1);  //阻塞监听;
		if (nWait == 0) {
			continue;
		}
		if (nWait == -1) {
			perr_exit("epoll_wait error");
		}
		for (int i = 0; i < nWait; i++) {
			printf("clitArr[i].data.fd = %d\n", clitArr[i].data.fd);
			if (!(clitArr[i].events & EPOLLIN)) { //不是读事件
				continue;
			}
			if (clitArr[i].data.fd == listenFd) {

				clitFd = Accept(listenFd, (struct sockaddr *)&clitAddr, &clit_addr_len); //与浏览器端建立连接
				myEvent.events = EPOLLIN;
				myEvent.data.fd = clitFd;

				ret = epoll_ctl(treeFd, EPOLL_CTL_ADD, clitFd, &myEvent);
				printf("New Client IP: %s, Port: %d, cfd = %d\n",
						inet_ntop(AF_INET, &clitAddr.sin_addr.s_addr, ip, sizeof(ip)),
						ntohs(clitAddr.sin_port), clitFd);
				//下面3行代码为设置阻塞
				int flg = fcntl(clitFd,F_GETFL);
				flg |= O_NONBLOCK;
				fcntl(clitFd,F_SETFL,flg);
			}
			else {
				//doReadTest(clitArr[i].data.fd, treeFd);
				doRead(clitArr[i].data.fd, treeFd);
			}
		}

	}

}

int main(int argc, char *argv[]) {
	/*
	   if (argc < 3) {
	   printf("Please enter the port number and dir\n");
	   exit(1);
	   }
	   servPort = atoi(argv[1]);

	// 改变进程工作目录
	int ret = chdir(argv[2]);
	if (ret != 0) {
	perror("chdir error");
	exit(1);
	}
	*/
	//int ret = chdir("/home/b/dir");  //改为自己给浏览器浏览文件的目录
	int ret = chdir(MY_DIR);  //改为自己给浏览器浏览文件的目录
	if (ret != 0) {
		perror("chdir error");
		exit(1);
	}

	// 启动 epoll监听
	doSockBindListen();

	doAccept();

	close(clitFd);
	close(listenFd);

	return 0;
}

