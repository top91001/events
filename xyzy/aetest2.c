//linux编译指令: gcc -std=c1x -g -o ae src/ae.c aetest.c
/*假设这几个文件都放在xyzy文件夹下：
cd xyzy
cc -std=c1x -g -o ae2 src/ae.c aetest2.c

编译后执行以下指令
./ae2 5678

此外本例未提供客户端，可以另外打开一个终端使用telnet测试：
telnet  127.0.0.1 5678

服务端在没有客户端连接的情况下输出如下:
使用select作为IO复用层
成功添加了一个id=0的时间事件
阻塞前钩子函数提醒您:自1630916819秒零699毫秒开始阻塞等待事件发生。
阻塞后钩子函数提醒您:1630916824秒零704毫秒时有事件发生，阻塞结束。
执行id=0的时间事件。
阻塞前钩子函数提醒您:自1630916824秒零704毫秒开始阻塞等待事件发生。
阻塞后钩子函数提醒您:1630916834秒零706毫秒时有事件发生，阻塞结束。
执行id=0的时间事件。
阻塞前钩子函数提醒您:自1630916834秒零706毫秒开始阻塞等待事件发生。
阻塞后钩子函数提醒您:1630916844秒零711毫秒时有事件发生，阻塞结束。
执行id=0的时间事件。
阻塞前钩子函数提醒您:自1630916844秒零711毫秒开始阻塞等待事件发生。
阻塞后钩子函数提醒您:1630916844秒零711毫秒时有事件发生，阻塞结束。
在时间事件链表中删除过期时间事件节点。
阻塞前钩子函数提醒您:自1630916844秒零711毫秒开始阻塞等待事件发生。

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
//阻塞前钩子函数
void beforeSleepProc(aeEventLoop *eventLoop);
//阻塞后钩子函数
void afterSleepProc(aeEventLoop *eventLoop);

//时间事件函数
int timeProc(aeEventLoop *eventLoop, long long id, void *clientData);
//时间事件终止函数
void timeFinalizerProc(aeEventLoop *eventLoop, void *clientData);//时间终止事件函数

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
    //为事件循环器添加阻塞前的钩子函数:
    aeSetBeforeSleepProc(eventLoop, beforeSleepProc);
    //为事件循环器添加阻塞前的钩子函数:
    aeSetAfterSleepProc(eventLoop, afterSleepProc);
    //添加一个5秒后执行的定时任务,该定时任务执行后会每隔10秒再执行一次
    long long time_id= aeCreateTimeEvent(eventLoop, 5000,timeProc, NULL,timeFinalizerProc);
    if(time_id!=AE_DELETED_EVENT_ID)
    {
        printf("成功添加了一个id=%lld的时间事件\n", time_id);
    }

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

void beforeSleepProc(aeEventLoop *eventLoop)
{
	long seconds,  milliseconds;
	aeGetTime(&seconds,&milliseconds);
	printf("阻塞前钩子函数提醒您:自%ld秒零%ld毫秒开始阻塞等待事件发生。\n", seconds,  milliseconds);
}
void afterSleepProc(aeEventLoop *eventLoop)
{
	long seconds,  milliseconds;
	aeGetTime(&seconds,&milliseconds);
	printf("阻塞后钩子函数提醒您:%ld秒零%ld毫秒时有事件发生，阻塞结束。\n", seconds,  milliseconds);
}
int time_proc_num=0;
//时间事件函数,该函数的返回值为隔多久再此执行此方法,如果返回值为
int timeProc(aeEventLoop *eventLoop, long long id, void *clientData)
{
    time_proc_num++;
    printf("执行id=%lld的时间事件。\n", id);
    if(time_proc_num<3){//再连续两次执行相同的操作
        return 10000;//隔10000毫秒(10秒)再次执行此函数
    }
    //两次之后删除此时间事件
    return AE_NOMORE;//或者return 0;注意如果使用redis源码的话,返回0会使这个函数不断连续执行,所以请谨慎使用0值
}
//时间事件终止函数,这个函数会在这个节点的事件删除前执行
void timeFinalizerProc(aeEventLoop *eventLoop, void *clientData)
{
    printf("在时间事件链表中删除过期时间事件节点。\n");
}