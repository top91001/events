/*事件循环器，基本完全照搬自redis的源码，做了少量修改（不是优化，redis的源码写的很好）修改了其内存管理的部分，为了少依赖其他文件
主要处理两类事件，文件事件和时间事件
文件事件的底层（IO复用层）使用的是系统的epoll或kqueue或evport等
时间事件即定时事件，负责定时任务的处理
*/
#ifndef __TSAE_H__
#define __TSAE_H__

#ifdef __linux__
#include <linux/version.h>
#include <features.h>
#endif

//根据操作系统为IO复用层选择使用epoll或者kqueue或者evport或者select。这部分宏判断本来在config.h中。为便于使用，我摘抄到此----------------------------------------------------
#ifdef __linux__
#define TOPS_AE_EPOLL 1
#endif //epoll可用,如果用linux系统编译，则ae.c中会include “ae_epoll.c”

#if (defined(__APPLE__) && defined(MAC_OS_X_VERSION_10_6)) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined (__NetBSD__)
#define TOPS_AE__KQUEUE 1
#endif//kqueue可用

#ifdef __sun
#include <sys/feature_tests.h>
#ifdef _DTRACE_VERSION
#define TOPS_AE__EVPORT 1
#endif
#endif //evport可用
//-----------------------------选择使用epoll或者kqueue或者evport或者select-------------------------------------------------------


#include <time.h>
//定义一部分标志
#define AE_OK 0  //一般操作成功返回0
#define AE_ERR -1   //一般情况下操作失败时返回-1

#define AE_NONE 0       /* 没有注册事件*/
#define AE_READABLE 1  //可读
#define AE_WRITABLE 2  //可写
/* 
一般情况下，Redis会先处理读事件(AE_READABLE)，再处理写事件(AE_WRITABLE)。 
这个顺序安排其实也算是一点小优化，先读后写可以让一个请求的处理和回包都是在同一次循环里面，使得请求可以尽快地回包，
如果某个fd的mask包含了AE_BARRIER，那它的处理顺序会是先写后读。 
*/
#define AE_BARRIER 4    //规定先写后读的处理顺序
/*
下面标志位主要用于eventLoop.flags
*/
#define AE_FILE_EVENTS 1   //文件事件。(1<<0)就是1
#define AE_TIME_EVENTS 2   //时间事件。源码中写的是(1<<1)这样写为了易读，一看就知道是第二位置1
#define AE_ALL_EVENTS  3    //全部事件：（文件|时间）。源码中写的是(AE_FILE_EVENTS|AE_TIME_EVENTS)
#define AE_DONT_WAIT   4    //是否阻塞进程，设置了此标志位的话就表示不阻塞而立即返回(1<<2)
#define AE_CALL_BEFORE_SLEEP 8//阻塞前是否调用eventLoop.beforesleep钩子函数(1<<3)
#define AE_CALL_AFTER_SLEEP 16//阻塞后是否调用eventLoop.aftersleep钩子函数(1<<4)

#define AE_NOMORE -1
#define AE_DELETED_EVENT_ID -1

/* Macros */
#define AE_NOTUSED(V) ((void) V)

struct aeEventLoop;

/* 下面结构体中要用到的函数指针类型*/
typedef void aeFileProc(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask);//读写文件函数
typedef int aeTimeProc(struct aeEventLoop *eventLoop, long long id, void *clientData);//时间事件函数
typedef void aeEventFinalizerProc(struct aeEventLoop *eventLoop, void *clientData);//时间终止事件函数
typedef void aeBeforeSleepProc(struct aeEventLoop *eventLoop);//阻塞前后钩子函数
/* 
一个文件描述符上已注册的文件事件。
aeFileEvent中没有记录fd，这是因为POSIX对文件描述符有以下约束：
1、fd=0、1、2的描述符用于标准输入、输出和错误处理
2、每次新打开的描述符必须使用当前进程中最小可用的文件描述符
Redis源码中充分利用这些特点，定义了一个数组，即：下面aeEventLoop结构体中的*events
用来存储已注册的文件事件，数组的索引就是文件描述符。
*/
typedef struct aeFileEvent {
    int mask;/*已注册的文件事件类型： AE_(READABLE|WRITABLE|BARRIER) 
             通过类似下面的语句转换成系统事件：  if (mask & AE_READABLE) ee.events |= EPOLLIN;*/
    aeFileProc *rfileProc;//AE_READABLE事件处理函数
    aeFileProc *wfileProc;//AE_WRITABLE事件处理函数
    void *clientData;//附加数据
} aeFileEvent;

/* 
一个时间事件的信息
 */
typedef struct aeTimeEvent {
    long long id; //时间事件的id
    long when_sec; //时间事件下一次执行的秒数
    long when_ms; //毫秒数
    aeTimeProc *timeProc;//时间事件处理函数
    aeEventFinalizerProc *finalizerProc;//时间事件终结函数,发生在该节点被删除时
    void *clientData;//客户端传入的附加数据
    struct aeTimeEvent *prev;//前一个时间事件
    struct aeTimeEvent *next;//后一个时间事件
    int refcount; /* 引用计数，防止在递归时间事件调用中释放计时器事件 */
} aeTimeEvent;

/*
一个已就绪的事件
*/
typedef struct aeFiredEvent {
    int fd;//产生事件的文件描述符
    int mask;//产生的事件类型
} aeFiredEvent;

/* 
事件循环器，负责管理事件
*/
typedef struct aeEventLoop {
    int maxfd;   //当前已注册的最大文件描述符
    int setsize; //该事件循环器允许监听的最大文件描述符
    long long timeEventNextId; //下一个时间事件id
    time_t lastTime;//上一次执行时间事件的时间，用于判断是否发生系统时钟偏移
    aeFileEvent *events; //已注册的文件事件表。数组的索引就是文件描述符
    aeFiredEvent *fired; //已就绪的事件表，等待事件循环器处理
    aeTimeEvent *timeEventHead;//时间事件表的头节点指针
    int stop;//事件循环器是否停止
    void *apidata; //存放用于IO复用层的附加数据
    aeBeforeSleepProc *beforesleep;//进程阻塞前调用的钩子函数
    aeBeforeSleepProc *aftersleep;//进程阻塞后调用的钩子函数
    /*
    标志位：AE_FILE_EVENTS、AE_TIME_EVENTS、AE_ALL_EVENTS、AE_DONT_WAIT、AE_CALL_BEFORE_SLEEP、AE_CALL_AFTER_SLEEP
    */
    int flags; //
} aeEventLoop;

/* 循环器函数集 */
aeEventLoop *aeCreateEventLoop(int setsize);//创建并初始化事件循环器，返回新创建的事件循环器指针
void aeDeleteEventLoop(aeEventLoop *eventLoop);//程序结束前删除事件循环器
void aeStop(aeEventLoop *eventLoop);//设置事件循环器停止运行标志

//为指定的fd添加mask对应的文件事件处理函数
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,aeFileProc *proc, void *clientData);
//为指定的fd删除mask对应的事件处理函数
void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask);
//获取指定的fd已经注册的事件mask标志位
int aeGetFileEvents(aeEventLoop *eventLoop, int fd);

//向循环器中时间事件链表添加新节点
long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,aeTimeProc *proc, void *clientData,aeEventFinalizerProc *finalizerProc);
//标记删除时间事件，并不真正执行删除操作
int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id);

//处理时间循环器中已经就绪的事件
int aeProcessEvents(aeEventLoop *eventLoop, int flags);

int aeWait(int fd, int mask, long long milliseconds);

void aeMain(aeEventLoop *eventLoop);//循环调用aeProcessEvents

char *aeGetApiName(void);//获取循环器所使用的IO复用层名称

//为循环器指定阻塞前钩子函数
void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep);
//为循环器指定阻塞后钩子函数
void aeSetAfterSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *aftersleep);
//
int aeGetSetSize(aeEventLoop *eventLoop);//获取为该事件循环器设置的允许监听的最大文件描述符
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize);//为循环器重置允许监听的最大文件描述符

void aeSetDontWait(aeEventLoop *eventLoop, int noWait);//为循环器设置是否阻塞标志位

void aeGetTime(long *seconds, long *milliseconds);//获取当前精确时间（距1970年1月1日）的秒数+毫秒数
#endif
