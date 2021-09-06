//linux编译指令: gcc -std=c1x -g -o ae src/ae.c aetest.c
/*假设这几个文件都放在events文件夹下：
cd events
cc -std=c1x -g -o ae src/ae.c aetest.c

编译后执行以下指令
./ae 5678

此外本例未提供客户端，可以另外打开一个终端使用telnet测试：
telnet  127.0.0.1 5678
你好，世界
你好，世界
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <signal.h>
#include "src/ae.h"

#define BUF_SIZE 1024
#define E_SIZE 100

int serv_sock;//listen的套接字需要在事件处理函数中单独处理，所以定义为全局变量
void error_handling(char *buf);

//文件事件处理函数，本示例只为简单说明如何使用所以只处理可读事件
void acceptTcpHandler(aeEventLoop *el, int fd, void *privdata, int mask); 


int main(int argc, char *argv[])
{
	signal(SIGHUP, SIG_IGN);//忽略这两个信号
    signal(SIGPIPE, SIG_IGN);

	
	struct sockaddr_in serv_adr;
	
	if(argc!=2) {//
		printf("正确的调用格式：%s <监听的端口>\n", argv[0]);
		exit(1);
	}

	serv_sock=socket(PF_INET, SOCK_STREAM, 0);
	memset(&serv_adr, 0, sizeof(serv_adr));
	serv_adr.sin_family=AF_INET;
	serv_adr.sin_addr.s_addr=htonl(INADDR_ANY);
	serv_adr.sin_port=htons(atoi(argv[1]));
	
	if(bind(serv_sock, (struct sockaddr*) &serv_adr, sizeof(serv_adr))==-1)
		error_handling("bind() 错误");
	if(listen(serv_sock, 5)==-1)
		error_handling("listen() 错误");

	//创建一个事件循环器：其内部逻辑参考ae.c。
	aeEventLoop *eventLoop=aeCreateEventLoop(E_SIZE);
	char *ename=aeGetApiName();//查看底层IO复用的名称
	printf("使用%s作为IO复用层\n", ename);

	//为serv_sock添加读事件处理函数，即有客户端连接时启用accept
	if (aeCreateFileEvent(eventLoop, serv_sock, AE_READABLE,acceptTcpHandler,NULL) == AE_ERR)
	{
		error_handling("创建listen fd文件事件处理函数失败");
	}
       
	//死循环处理注册的所有事件
	aeMain(eventLoop);
	//清理资源
    aeDeleteEventLoop(eventLoop);
    close(serv_sock);
	return 0;
}

void error_handling(char *buf)
{
	fputs(buf, stderr);
	fputc('\n', stderr);
	exit(1);
}


//具体的事件处理函数，内部逻辑参考ae.c
void acceptTcpHandler(aeEventLoop *el, int fd, void *privdata, int mask){
	if(fd==serv_sock)//单独处理listen的套接字，添加accept
	{
		socklen_t adr_sz;
		struct sockaddr_in clnt_adr;
		adr_sz=sizeof(clnt_adr);
		int clnt_sock=accept(serv_sock, (struct sockaddr*)&clnt_adr, &adr_sz);

		//为每个accept的套接字注册可读事件处理函数
		if (aeCreateFileEvent(el,clnt_sock, AE_READABLE,acceptTcpHandler,NULL) == AE_ERR)
		{
			error_handling("创建accept fd文件事件处理函数失败");
		}
	}
	else //可读的是accept套接字
	{
		int str_len, i;
		char buf[BUF_SIZE];
    	str_len=read(fd, buf, BUF_SIZE);
		if(str_len==0)    // 客户端关闭
		{
			aeDeleteFileEvent(el, fd, mask);//清理此fd相关的事件注册及资源
			close(fd);
			printf("fd=%d的客户端关闭 \n", fd);
		}
		else
		{
			write(fd, buf, str_len);//回声
		}
	}
}