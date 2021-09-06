#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <assert.h>

#include "ae.h"
//-------------------------判断可用的io复用方法 
#ifdef TOPS_AE_EVPORT
//--------evport可用,包含相关头文件,并定义必须的结构体、变量--------
#include <port.h>
static int evport_debug = 0;
#define MAX_EVENT_BATCHSZ 512
typedef struct aeApiState {
    int     portfd;                             /* event port */
    int     npending;                           /* # of pending fds */
    int     pending_fds[MAX_EVENT_BATCHSZ];     /* pending fds */
    int     pending_masks[MAX_EVENT_BATCHSZ];   /* pending fds' masks */
} aeApiState;
//--------evport包含相关头文件,并定义必须的结构体、变量 结束--------
#else
    #ifdef TOPS_AE_EPOLL
//--------epoll可用,包含相关头文件,并定义必须的结构体、变量--------
    #include <sys/epoll.h>
    typedef struct aeApiState {
        int epfd;
        struct epoll_event *events;
    } aeApiState;
//--------epoll包含相关头文件,并定义必须的结构体、变量 结束--------
    #else
        #ifdef TOPS_AE_KQUEUE
//--------kqueue可用,包含相关头文件,并定义必须的结构体、变量--------
        #include <sys/event.h>
        typedef struct aeApiState {
            int kqfd;
            struct kevent *events;
        } aeApiState;
//--------kqueue包含相关头文件,并定义必须的结构体、变量  结束--------
        #else
//--------以上方法均不可用,则使用select,包含相关头文件,并定义必须的结构体、变量--------
        #include <sys/select.h>
        #include <string.h>
        typedef struct aeApiState {
            fd_set rfds, wfds;
            /* We need to have a copy of the fd sets as it's not safe to reuse
            * FD sets after select(). */
            fd_set _rfds, _wfds;
        } aeApiState;
//------select包含相关头文件,并定义必须的结构体、变量  结束--------
        #endif
    #endif
#endif
//IO复用层具体实现在本文件底部
//先声明IO复用层统一的函数格式:
static int aeApiCreate(aeEventLoop *eventLoop);
static int aeApiResize(aeEventLoop *eventLoop, int setsize);
static void aeApiFree(aeEventLoop *eventLoop);
static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask);
static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int delmask);
static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp);
static char *aeApiName(void);
//IO复用层函数声明结束

//初始化一个事件循环器，一般用于main函数中
aeEventLoop *aeCreateEventLoop(int setsize) {
    aeEventLoop *eventLoop;
    int i;

    if ((eventLoop = malloc(sizeof(*eventLoop))) == NULL) goto err;//redis中有很多goto语句，用于统一资源清理
    eventLoop->events = malloc(sizeof(aeFileEvent)*setsize);
    eventLoop->fired = malloc(sizeof(aeFiredEvent)*setsize);
    if (eventLoop->events == NULL || eventLoop->fired == NULL) goto err;
    eventLoop->setsize = setsize;
    eventLoop->lastTime = time(NULL);
    eventLoop->timeEventHead = NULL;
    eventLoop->timeEventNextId = 0;
    eventLoop->stop = 0;
    eventLoop->maxfd = -1;
    eventLoop->beforesleep = NULL;
    eventLoop->aftersleep = NULL;
    eventLoop->flags = 0;
    /*
    使用epoll或者kqueue等初始化，并填充eventLoop->apidata
    比如使用epoll则这个apidata包括epfd、epoll_event
    */
    if (aeApiCreate(eventLoop) == -1) goto err;
    /* 初始化每个event的mask为0 */
    for (i = 0; i < setsize; i++)
        eventLoop->events[i].mask = AE_NONE;
    return eventLoop;

err: //出错时goto到此清理资源
    if (eventLoop) {
        free(eventLoop->events);
        free(eventLoop->fired);
        free(eventLoop);
    }
    return NULL;
}

/*获取该事件循环器允许监听的的最大文件描述符 */
int aeGetSetSize(aeEventLoop *eventLoop) {
    return eventLoop->setsize;
}

/* 设置修改flags标志位，是否 AE_DONT_WAIT (非阻塞模式)*/
void aeSetDontWait(aeEventLoop *eventLoop, int noWait) {
    if (noWait)
        eventLoop->flags |= AE_DONT_WAIT;
    else
        eventLoop->flags &= ~AE_DONT_WAIT;
}

/* 调整事件循环器允许监听的最大文件描述符*/
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize) {
    if (setsize == eventLoop->setsize) return AE_OK;
    if (eventLoop->maxfd >= setsize) return AE_ERR;
    //调用epoll、kqueue等IO复用层的调整方法
    if (aeApiResize(eventLoop,setsize) == -1) return AE_ERR;

    eventLoop->events = realloc(eventLoop->events,sizeof(aeFileEvent)*setsize);
    eventLoop->fired = realloc(eventLoop->fired,sizeof(aeFiredEvent)*setsize);
    eventLoop->setsize = setsize;

    /* 初始化未使用的空间 */
    int i;
    for (i = eventLoop->maxfd+1; i < setsize; i++)
        eventLoop->events[i].mask = AE_NONE;
    return AE_OK;
}
/*
清理循环器
*/
void aeDeleteEventLoop(aeEventLoop *eventLoop) {
    aeApiFree(eventLoop);//先清理IO复用层
    free(eventLoop->events);
    free(eventLoop->fired);

    /*逐个清理时间事件 */
    aeTimeEvent *next_te, *te = eventLoop->timeEventHead;
    while (te) {
        next_te = te->next;
        free(te);
        te = next_te;
    }
    free(eventLoop);
}

//设置循环器停止标志
void aeStop(aeEventLoop *eventLoop) {
    eventLoop->stop = 1;
}
//为事件循环器添加文件事件处理函数
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,aeFileProc *proc, void *clientData)
{
    if (fd >= eventLoop->setsize) {
        errno = ERANGE;//设置系统的错误码
        return AE_ERR;
    }
    //eventLoop->events的索引就是文件描述符fd，参考ae.h第72行。
    aeFileEvent *fe = &eventLoop->events[fd];

    /*
    于io复用层添加事件
    比如AE_READABLE对应epoll的EPOLLIN，AE_WRITABLE对应EPOLLOUT
    */
    if (aeApiAddEvent(eventLoop, fd, mask) == -1)
        return AE_ERR;
    fe->mask |= mask;
    if (mask & AE_READABLE) fe->rfileProc = proc;//设置了可读标志位，那就添加读文件事件处理函数
    if (mask & AE_WRITABLE) fe->wfileProc = proc;//设置了可写标志位，那就添加写文件事件处理函数
    fe->clientData = clientData;
    //可能需要修改循环器当前最大文件描述符
    if (fd > eventLoop->maxfd)
        eventLoop->maxfd = fd;
    return AE_OK;
}
/*
移除mask中包含的文件事件
*/
void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask)
{
    if (fd >= eventLoop->setsize) return;
    aeFileEvent *fe = &eventLoop->events[fd];
    if (fe->mask == AE_NONE) return;

    /* 如果移除的是写事件函数，则总是清理掉 AE_BARRIER标志（先写后读顺序）*/
    if (mask & AE_WRITABLE) mask |= AE_BARRIER;
    //于io复用层去掉相关事件的注册
    aeApiDelEvent(eventLoop, fd, mask);
    fe->mask = fe->mask & (~mask);//循环器events数组中取消相关标志位
    /*
    如果当前文件描述符恰好是循环器中的最大值，则重新计算这个最大值
    */
    if (fd == eventLoop->maxfd && fe->mask == AE_NONE) {
        /* Update the max fd */
        int j;

        for (j = eventLoop->maxfd-1; j >= 0; j--)
            if (eventLoop->events[j].mask != AE_NONE) break;
        eventLoop->maxfd = j;
    }
}
//获取某文件描述符注册的事件mask标志位
int aeGetFileEvents(aeEventLoop *eventLoop, int fd) {
    if (fd >= eventLoop->setsize) return 0;
    aeFileEvent *fe = &eventLoop->events[fd];

    return fe->mask;
}
//获取当前精确时间（距1970年1月1日）的秒数+毫秒数
void aeGetTime(long *seconds, long *milliseconds)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    *seconds = tv.tv_sec;
    *milliseconds = tv.tv_usec/1000;
}
//在当前时间的基础上增加指定的秒数+毫秒数
static void aeAddMillisecondsToNow(long long milliseconds, long *sec, long *ms) {
    long cur_sec, cur_ms, when_sec, when_ms;

    aeGetTime(&cur_sec, &cur_ms);
    when_sec = cur_sec + milliseconds/1000;
    when_ms = cur_ms + milliseconds%1000;
    if (when_ms >= 1000) {
        when_sec ++;
        when_ms -= 1000;
    }
    *sec = when_sec;
    *ms = when_ms;
}
//添加时间事件
long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,aeTimeProc *proc, void *clientData,aeEventFinalizerProc *finalizerProc)
{
    long long id = eventLoop->timeEventNextId++;//计算事件id并记录下来
    aeTimeEvent *te;

    te = malloc(sizeof(*te));
    if (te == NULL) return AE_ERR;
    te->id = id;
    aeAddMillisecondsToNow(milliseconds,&te->when_sec,&te->when_ms);//计算到期时间
    te->timeProc = proc;//时间事件处理函数
    te->finalizerProc = finalizerProc;//事件终结函数
    te->clientData = clientData;//客户端数据
    te->refcount = 0;//引用计数初始化为0
    //当前事件设为链表头
    te->prev = NULL;
    te->next = eventLoop->timeEventHead;
    if (te->next)
        te->next->prev = te;
    eventLoop->timeEventHead = te;
    return id;
}
/*
标记删除时间事件，只是将事件id设为-1
*/
int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id)
{
    aeTimeEvent *te = eventLoop->timeEventHead;
    while(te) {
        if (te->id == id) {
            te->id = AE_DELETED_EVENT_ID;
            return AE_OK;
        }
        te = te->next;
    }
    return AE_ERR; /* NO event with the specified ID found */
}

/* 查找最先到期的时间事件
 */
static aeTimeEvent *aeSearchNearestTimer(aeEventLoop *eventLoop)
{
    aeTimeEvent *te = eventLoop->timeEventHead;
    aeTimeEvent *nearest = NULL;

    while(te) {
        if (!nearest || te->when_sec < nearest->when_sec ||
                (te->when_sec == nearest->when_sec &&
                 te->when_ms < nearest->when_ms))
            nearest = te;
        te = te->next;
    }
    return nearest;
}

/*处理时间事件 */
static int processTimeEvents(aeEventLoop *eventLoop) {
    int processed = 0;
    aeTimeEvent *te;
    long long maxId;
    time_t now = time(NULL);

    /* 上一次执行时间事件的时间比当前时间还大，说明系统时间由于系统时钟偏移等原因混乱了 
    这里将所有时间事件的秒数置0，这样会导致时间事件提前执行。之所以这样做，是因为提前执行比延后执行危害小*/
    if (now < eventLoop->lastTime) {
        te = eventLoop->timeEventHead;
        while(te) {
            te->when_sec = 0;
            te = te->next;
        }
    }

    eventLoop->lastTime = now;//记录本次执行时间

    te = eventLoop->timeEventHead;
    maxId = eventLoop->timeEventNextId-1;//当前最大的时间事件id
    while(te) {
        long now_sec, now_ms;
        long long id;

        /* 在链表中移除已经作了删除标记的事件，参考上面的aeDeleteTimeEvent方法 */
        if (te->id == AE_DELETED_EVENT_ID) {
            aeTimeEvent *next = te->next;
            /* 引用计数不为0的不能移除 */
            if (te->refcount) {
                te = next;
                continue;
            }
            //下面先在链表中摘除
            if (te->prev)
                te->prev->next = te->next;
            else
                eventLoop->timeEventHead = te->next;
            if (te->next)
                te->next->prev = te->prev;
            //执行终结函数
            if (te->finalizerProc)
                te->finalizerProc(eventLoop, te->clientData);
            //清理资源
            free(te);

            te = next;
            continue;//继续处理下一个时间事件
        }

        /* 我的理解是可能在处理事件期间有正在添加的事件，暂时不处理正在添加的事件 */
        if (te->id > maxId) {
            te = te->next;
            continue;
        }
        //判断时间有没有到期
        aeGetTime(&now_sec, &now_ms);
        if (now_sec > te->when_sec ||(now_sec == te->when_sec && now_ms >= te->when_ms))
        {
            int retval;

            id = te->id;
            te->refcount++;//引用计数加1，这里考虑要不要改成原子操作
            /*
            执行时间事件的逻辑，该逻辑返回值为再次执行该操作的间隔时间毫秒数。
            如果间隔时间为AE_NOMORE（-1）表示没有下次了，可以清理该事件了。
            */
            retval = te->timeProc(eventLoop, id, te->clientData);//执行事件处理函数
            te->refcount--;//处理结束引用计数减1
            processed++;//处理事件数，这里考虑要不要改成原子操作
            /*
            //除了-1和0之外的整数值表示再间隔tetval毫秒还要执行此逻辑，不会在链表中清理此节点
            注意:这个地方redis本来的写法是:if (retval != AE_NOMORE),这样的话,如果te->timeProc的返回值为0,会无限循环执行这个时间事件处理函数
            如果有这个需求,可以改回它原来的逻辑
            */
            if (retval&&(retval != AE_NOMORE)) {
                aeAddMillisecondsToNow(retval,&te->when_sec,&te->when_ms);
            } else {//retval == AE_NOMORE(-1)或者0，标记删除，等引用计数为0时清理该事件
                te->id = AE_DELETED_EVENT_ID;
            }
        }
        te = te->next;
    }
    return processed;
}

/*
 程序运行期间要循环调用下面的事件处理函数
flags参数的含义参考tsea.h第47行
*/
int aeProcessEvents(aeEventLoop *eventLoop, int flags)
{
    int processed = 0, numevents;

    /*不处理时间事件也不处理文件事件，就直接返回0 */
    if (!(flags & AE_TIME_EVENTS) && !(flags & AE_FILE_EVENTS)) return 0;

    /*有待处理的文件描述符，或者设置了阻塞执行的时间事件*/
    if (eventLoop->maxfd != -1 || ((flags & AE_TIME_EVENTS) && !(flags & AE_DONT_WAIT))) //AE_DONT_WAIT表示非阻塞
    {
        int j;
        aeTimeEvent *shortest = NULL;
        struct timeval tv, *tvp;

        if (flags & AE_TIME_EVENTS && !(flags & AE_DONT_WAIT))
            shortest = aeSearchNearestTimer(eventLoop);//查找最先到期的时间事件
        if (shortest) {
            //找到了最近的时间事件，就用它的到期时间计算该进程需要阻塞的时间
            long now_sec, now_ms;
            aeGetTime(&now_sec, &now_ms);
            tvp = &tv;
            //还有多少毫秒到期
            long long ms =(shortest->when_sec - now_sec)*1000 +shortest->when_ms - now_ms;

            if (ms > 0) {
                tvp->tv_sec = ms/1000;
                tvp->tv_usec = (ms % 1000)*1000;
            } else {
                tvp->tv_sec = 0;
                tvp->tv_usec = 0;
            }
        } else {//没有找到即将到期的时间事件
            /* 如果设置了AE_DONT_WAIT（非阻塞）就把阻塞时间设为0*/
            if (flags & AE_DONT_WAIT) {
                tv.tv_sec = tv.tv_usec = 0;
                tvp = &tv;
            } else {
                /* 没设置AE_DONT_WAIT（非阻塞）就把阻塞时间设为NULL表示一直阻塞，等待有事件发生 */
                tvp = NULL; /* wait forever */
            }
        }

        /*
        上面处理的flags是外部调用传入的flags。下面检查事件循环器自身设置的flags。
        如果事件循环器自身的flags设置了非阻塞，则阻塞时间设为0，不管传入的flags是啥
        */
        if (eventLoop->flags & AE_DONT_WAIT) {//
            tv.tv_sec = tv.tv_usec = 0;
            tvp = &tv;
        }
        /*
        如果循环器设置了阻塞前钩子函数。并且标志位中设置了AE_CALL_BEFORE_SLEEP标志位
        就执行阻塞前钩子函数
        */
        if (eventLoop->beforesleep != NULL && flags & AE_CALL_BEFORE_SLEEP)
            eventLoop->beforesleep(eventLoop);//阻塞前钩子函数

        /* 
        调用io复用层的设置函数，设置阻塞 （比如epoll_wait函数）
        如果有文件事件就绪，该方法返回就绪文件事件数量。
        并且会把相关文件描述符和就绪的事件mask记入eventLoop.fired数组中
        如果只是时间到期，则返回0
        */
        numevents = aeApiPoll(eventLoop, tvp);

        //阻塞时间到了，或者epoll等IO复用层被事件唤醒了

        /* 
        如果循环器设置了阻塞后钩子函数。并且标志位中设置了AE_CALL_AFTER_SLEEP标志位
        就执行阻塞后钩子函数
         */
        if (eventLoop->aftersleep != NULL && flags & AE_CALL_AFTER_SLEEP)
            eventLoop->aftersleep(eventLoop);//阻塞后钩子函数

        /*有就绪的文件事件，逐个处理*/
        for (j = 0; j < numevents; j++) {
            int fd = eventLoop->fired[j].fd;//就绪的文件描述符
            //先读取该文件描述符上注册的文件事件
            aeFileEvent *fe = &eventLoop->events[fd];
            int mask = eventLoop->fired[j].mask;//就绪的事件mask
            
            int fired = 0; /* 是否处理过读写事件了 */

            /* 该描述符是否设置了先写后读，参考tsae.h第41行*/
            int invert = fe->mask & AE_BARRIER;

            /* 就绪的事件包含可读事件，并且未设置先写后读，就先调用读文件方法*/
            if (!invert && fe->mask & mask & AE_READABLE) {
                fe->rfileProc(eventLoop,fd,fe->clientData,mask);
                fired++;
                fe = &eventLoop->events[fd]; /* Refresh in case of resize. */
            }

            /* 就绪的事件包含可写事件. */
            if (fe->mask & mask & AE_WRITABLE) {
                if (!fired || fe->wfileProc != fe->rfileProc) {//如果已经处理过读事件，则需要保证读写事件不重复，才继续执行写事件
                    fe->wfileProc(eventLoop,fd,fe->clientData,mask);
                    fired++;
                }
            }

            /* 设置了先写后读，则读事件的处理放到写事件后面来处理*/
            if (invert) {
                fe = &eventLoop->events[fd]; /* Refresh in case of resize. */
                if ((fe->mask & mask & AE_READABLE) && (!fired || fe->wfileProc != fe->rfileProc))
                {
                    fe->rfileProc(eventLoop,fd,fe->clientData,mask);
                    fired++;
                }
            }

            processed++;
        }
    }//文件事件处理结束

    /* 检查并执行时间事件 */
    if (flags & AE_TIME_EVENTS)
        processed += processTimeEvents(eventLoop);

    return processed; /* return the number of processed file/time events */
}

/* Wait for milliseconds until the given file descriptor becomes
 * writable/readable/exception */
int aeWait(int fd, int mask, long long milliseconds) {
    struct pollfd pfd;
    int retmask = 0, retval;

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    if (mask & AE_READABLE) pfd.events |= POLLIN;
    if (mask & AE_WRITABLE) pfd.events |= POLLOUT;

    if ((retval = poll(&pfd, 1, milliseconds))== 1) {
        if (pfd.revents & POLLIN) retmask |= AE_READABLE;
        if (pfd.revents & POLLOUT) retmask |= AE_WRITABLE;
        if (pfd.revents & POLLERR) retmask |= AE_WRITABLE;
        if (pfd.revents & POLLHUP) retmask |= AE_WRITABLE;
        return retmask;
    } else {
        return retval;
    }
}
/*
循环调用处理函数
*/
void aeMain(aeEventLoop *eventLoop) {
    eventLoop->stop = 0;
    while (!eventLoop->stop) {
        aeProcessEvents(eventLoop, AE_ALL_EVENTS| AE_CALL_BEFORE_SLEEP| AE_CALL_AFTER_SLEEP);
    }
}

char *aeGetApiName(void) {
    return aeApiName();
}
//设置阻塞前钩子函数
void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep) {
    eventLoop->beforesleep = beforesleep;
}
//设置阻塞后钩子函数
void aeSetAfterSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *aftersleep) {
    eventLoop->aftersleep = aftersleep;
}

/*------------------------------------------下面是根据操作系统选择io复用层代码-------------------------------------*/
//-----------------evport可用的实现-------------------------------------------- 
#ifdef TOPS_AE_EVPORT
static int aeApiCreate(aeEventLoop *eventLoop) {
    int i;
    aeApiState *state = malloc(sizeof(aeApiState));
    if (!state) return -1;

    state->portfd = port_create();
    if (state->portfd == -1) {
        free(state);
        return -1;
    }

    state->npending = 0;

    for (i = 0; i < MAX_EVENT_BATCHSZ; i++) {
        state->pending_fds[i] = -1;
        state->pending_masks[i] = AE_NONE;
    }

    eventLoop->apidata = state;
    return 0;
}

static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    /* Nothing to resize here. */
    return 0;
}

static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;

    close(state->portfd);
    free(state);
}

static int aeApiLookupPending(aeApiState *state, int fd) {
    int i;

    for (i = 0; i < state->npending; i++) {
        if (state->pending_fds[i] == fd)
            return (i);
    }

    return (-1);
}

/*
 * Helper function to invoke port_associate for the given fd and mask.
 */
static int aeApiAssociate(const char *where, int portfd, int fd, int mask) {
    int events = 0;
    int rv, err;

    if (mask & AE_READABLE)
        events |= POLLIN;
    if (mask & AE_WRITABLE)
        events |= POLLOUT;

    if (evport_debug)
        fprintf(stderr, "%s: port_associate(%d, 0x%x) = ", where, fd, events);

    rv = port_associate(portfd, PORT_SOURCE_FD, fd, events,
        (void *)(uintptr_t)mask);
    err = errno;

    if (evport_debug)
        fprintf(stderr, "%d (%s)\n", rv, rv == 0 ? "no error" : strerror(err));

    if (rv == -1) {
        fprintf(stderr, "%s: port_associate: %s\n", where, strerror(err));

        if (err == EAGAIN)
            fprintf(stderr, "aeApiAssociate: event port limit exceeded.");
    }

    return rv;
}

static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    int fullmask, pfd;

    if (evport_debug)
        fprintf(stderr, "aeApiAddEvent: fd %d mask 0x%x\n", fd, mask);

    /*
     * Since port_associate's "events" argument replaces any existing events, we
     * must be sure to include whatever events are already associated when
     * we call port_associate() again.
     */
    fullmask = mask | eventLoop->events[fd].mask;
    pfd = aeApiLookupPending(state, fd);

    if (pfd != -1) {
        /*
         * This fd was recently returned from aeApiPoll.  It should be safe to
         * assume that the consumer has processed that poll event, but we play
         * it safer by simply updating pending_mask.  The fd will be
         * re-associated as usual when aeApiPoll is called again.
         */
        if (evport_debug)
            fprintf(stderr, "aeApiAddEvent: adding to pending fd %d\n", fd);
        state->pending_masks[pfd] |= fullmask;
        return 0;
    }

    return (aeApiAssociate("aeApiAddEvent", state->portfd, fd, fullmask));
}

static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    int fullmask, pfd;

    if (evport_debug)
        fprintf(stderr, "del fd %d mask 0x%x\n", fd, mask);

    pfd = aeApiLookupPending(state, fd);

    if (pfd != -1) {
        if (evport_debug)
            fprintf(stderr, "deleting event from pending fd %d\n", fd);

        /*
         * This fd was just returned from aeApiPoll, so it's not currently
         * associated with the port.  All we need to do is update
         * pending_mask appropriately.
         */
        state->pending_masks[pfd] &= ~mask;

        if (state->pending_masks[pfd] == AE_NONE)
            state->pending_fds[pfd] = -1;

        return;
    }

    /*
     * The fd is currently associated with the port.  Like with the add case
     * above, we must look at the full mask for the file descriptor before
     * updating that association.  We don't have a good way of knowing what the
     * events are without looking into the eventLoop state directly.  We rely on
     * the fact that our caller has already updated the mask in the eventLoop.
     */

    fullmask = eventLoop->events[fd].mask;
    if (fullmask == AE_NONE) {
        /*
         * We're removing *all* events, so use port_dissociate to remove the
         * association completely.  Failure here indicates a bug.
         */
        if (evport_debug)
            fprintf(stderr, "aeApiDelEvent: port_dissociate(%d)\n", fd);

        if (port_dissociate(state->portfd, PORT_SOURCE_FD, fd) != 0) {
            perror("aeApiDelEvent: port_dissociate");
            abort(); /* will not return */
        }
    } else if (aeApiAssociate("aeApiDelEvent", state->portfd, fd,
        fullmask) != 0) {
        /*
         * ENOMEM is a potentially transient condition, but the kernel won't
         * generally return it unless things are really bad.  EAGAIN indicates
         * we've reached a resource limit, for which it doesn't make sense to
         * retry (counter-intuitively).  All other errors indicate a bug.  In any
         * of these cases, the best we can do is to abort.
         */
        abort(); /* will not return */
    }
}

static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    struct timespec timeout, *tsp;
    int mask, i;
    uint_t nevents;
    port_event_t event[MAX_EVENT_BATCHSZ];

    /*
     * If we've returned fd events before, we must re-associate them with the
     * port now, before calling port_get().  See the block comment at the top of
     * this file for an explanation of why.
     */
    for (i = 0; i < state->npending; i++) {
        if (state->pending_fds[i] == -1)
            /* This fd has since been deleted. */
            continue;

        if (aeApiAssociate("aeApiPoll", state->portfd,
            state->pending_fds[i], state->pending_masks[i]) != 0) {
            /* See aeApiDelEvent for why this case is fatal. */
            abort();
        }

        state->pending_masks[i] = AE_NONE;
        state->pending_fds[i] = -1;
    }

    state->npending = 0;

    if (tvp != NULL) {
        timeout.tv_sec = tvp->tv_sec;
        timeout.tv_nsec = tvp->tv_usec * 1000;
        tsp = &timeout;
    } else {
        tsp = NULL;
    }

    /*
     * port_getn can return with errno == ETIME having returned some events (!).
     * So if we get ETIME, we check nevents, too.
     */
    nevents = 1;
    if (port_getn(state->portfd, event, MAX_EVENT_BATCHSZ, &nevents,
        tsp) == -1 && (errno != ETIME || nevents == 0)) {
        if (errno == ETIME || errno == EINTR)
            return 0;

        /* Any other error indicates a bug. */
        perror("aeApiPoll: port_get");
        abort();
    }

    state->npending = nevents;

    for (i = 0; i < nevents; i++) {
            mask = 0;
            if (event[i].portev_events & POLLIN)
                mask |= AE_READABLE;
            if (event[i].portev_events & POLLOUT)
                mask |= AE_WRITABLE;

            eventLoop->fired[i].fd = event[i].portev_object;
            eventLoop->fired[i].mask = mask;

            if (evport_debug)
                fprintf(stderr, "aeApiPoll: fd %d mask 0x%x\n",
                    (int)event[i].portev_object, mask);

            state->pending_fds[i] = event[i].portev_object;
            state->pending_masks[i] = (uintptr_t)event[i].portev_user;
    }

    return nevents;
}

static char *aeApiName(void) {
    return "evport";
}
//------------evport实现end---------------------------------
#else
    #ifdef TOPS_AE_EPOLL
//-----------------epoll可用--------------------------------
    static int aeApiCreate(aeEventLoop *eventLoop) {
    aeApiState *state = malloc(sizeof(aeApiState));

    if (!state) return -1;
    state->events = malloc(sizeof(struct epoll_event)*eventLoop->setsize);
    if (!state->events) {
        free(state);
        return -1;
    }
    state->epfd = epoll_create(1024); /* 1024 is just a hint for the kernel */
    if (state->epfd == -1) {
        free(state->events);
        free(state);
        return -1;
    }
    eventLoop->apidata = state;
    return 0;
}

static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    aeApiState *state = eventLoop->apidata;

    state->events = realloc(state->events, sizeof(struct epoll_event)*setsize);
    return 0;
}

static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;

    close(state->epfd);
    free(state->events);
    free(state);
}

static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    struct epoll_event ee = {0}; /* avoid valgrind warning */
    /* If the fd was already monitored for some event, we need a MOD
     * operation. Otherwise we need an ADD operation. */
    int op = eventLoop->events[fd].mask == AE_NONE ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;

    ee.events = 0;
    mask |= eventLoop->events[fd].mask; /* Merge old events */
    if (mask & AE_READABLE) ee.events |= EPOLLIN;
    if (mask & AE_WRITABLE) ee.events |= EPOLLOUT;
    ee.data.fd = fd;
    if (epoll_ctl(state->epfd,op,fd,&ee) == -1) return -1;
    return 0;
}

static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int delmask) {
    aeApiState *state = eventLoop->apidata;
    struct epoll_event ee = {0}; /* avoid valgrind warning */
    int mask = eventLoop->events[fd].mask & (~delmask);

    ee.events = 0;
    if (mask & AE_READABLE) ee.events |= EPOLLIN;
    if (mask & AE_WRITABLE) ee.events |= EPOLLOUT;
    ee.data.fd = fd;
    if (mask != AE_NONE) {
        epoll_ctl(state->epfd,EPOLL_CTL_MOD,fd,&ee);
    } else {
        /* Note, Kernel < 2.6.9 requires a non null event pointer even for
         * EPOLL_CTL_DEL. */
        epoll_ctl(state->epfd,EPOLL_CTL_DEL,fd,&ee);
    }
}

static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int retval, numevents = 0;

    retval = epoll_wait(state->epfd,state->events,eventLoop->setsize,
            tvp ? (tvp->tv_sec*1000 + tvp->tv_usec/1000) : -1);
    if (retval > 0) {
        int j;

        numevents = retval;
        for (j = 0; j < numevents; j++) {
            int mask = 0;
            struct epoll_event *e = state->events+j;

            if (e->events & EPOLLIN) mask |= AE_READABLE;
            if (e->events & EPOLLOUT) mask |= AE_WRITABLE;
            if (e->events & EPOLLERR) mask |= AE_WRITABLE|AE_READABLE;
            if (e->events & EPOLLHUP) mask |= AE_WRITABLE|AE_READABLE;
            eventLoop->fired[j].fd = e->data.fd;
            eventLoop->fired[j].mask = mask;
        }
    }
    return numevents;
}

static char *aeApiName(void) {
    return "epoll";
}
//-----------epoll的实现结束-------------------------------
    #else
        #ifdef TOPS_AE_KQUEUE
//-----------kqueue可用----------------------------------
static int aeApiCreate(aeEventLoop *eventLoop) {
    aeApiState *state = malloc(sizeof(aeApiState));

    if (!state) return -1;
    state->events = malloc(sizeof(struct kevent)*eventLoop->setsize);
    if (!state->events) {
        free(state);
        return -1;
    }
    state->kqfd = kqueue();
    if (state->kqfd == -1) {
        free(state->events);
        free(state);
        return -1;
    }
    eventLoop->apidata = state;
    return 0;
}

static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    aeApiState *state = eventLoop->apidata;

    state->events = realloc(state->events, sizeof(struct kevent)*setsize);
    return 0;
}

static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;

    close(state->kqfd);
    free(state->events);
    free(state);
}

static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    struct kevent ke;

    if (mask & AE_READABLE) {
        EV_SET(&ke, fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
        if (kevent(state->kqfd, &ke, 1, NULL, 0, NULL) == -1) return -1;
    }
    if (mask & AE_WRITABLE) {
        EV_SET(&ke, fd, EVFILT_WRITE, EV_ADD, 0, 0, NULL);
        if (kevent(state->kqfd, &ke, 1, NULL, 0, NULL) == -1) return -1;
    }
    return 0;
}

static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    struct kevent ke;

    if (mask & AE_READABLE) {
        EV_SET(&ke, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
        kevent(state->kqfd, &ke, 1, NULL, 0, NULL);
    }
    if (mask & AE_WRITABLE) {
        EV_SET(&ke, fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
        kevent(state->kqfd, &ke, 1, NULL, 0, NULL);
    }
}

static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int retval, numevents = 0;

    if (tvp != NULL) {
        struct timespec timeout;
        timeout.tv_sec = tvp->tv_sec;
        timeout.tv_nsec = tvp->tv_usec * 1000;
        retval = kevent(state->kqfd, NULL, 0, state->events, eventLoop->setsize,
                        &timeout);
    } else {
        retval = kevent(state->kqfd, NULL, 0, state->events, eventLoop->setsize,
                        NULL);
    }

    if (retval > 0) {
        int j;

        numevents = retval;
        for(j = 0; j < numevents; j++) {
            int mask = 0;
            struct kevent *e = state->events+j;

            if (e->filter == EVFILT_READ) mask |= AE_READABLE;
            if (e->filter == EVFILT_WRITE) mask |= AE_WRITABLE;
            eventLoop->fired[j].fd = e->ident;
            eventLoop->fired[j].mask = mask;
        }
    }
    return numevents;
}

static char *aeApiName(void) {
    return "kqueue";
}
//-----------kqueue实现结束------------------------------
        #else
//--------------------以上方法都不可用则使用select实现-------
static int aeApiCreate(aeEventLoop *eventLoop) {
    aeApiState *state = malloc(sizeof(aeApiState));

    if (!state) return -1;
    FD_ZERO(&state->rfds);
    FD_ZERO(&state->wfds);
    eventLoop->apidata = state;
    return 0;
}

static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    /* Just ensure we have enough room in the fd_set type. */
    if (setsize >= FD_SETSIZE) return -1;
    return 0;
}

static void aeApiFree(aeEventLoop *eventLoop) {
    free(eventLoop->apidata);
}

static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;

    if (mask & AE_READABLE) FD_SET(fd,&state->rfds);
    if (mask & AE_WRITABLE) FD_SET(fd,&state->wfds);
    return 0;
}

static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;

    if (mask & AE_READABLE) FD_CLR(fd,&state->rfds);
    if (mask & AE_WRITABLE) FD_CLR(fd,&state->wfds);
}

static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int retval, j, numevents = 0;

    memcpy(&state->_rfds,&state->rfds,sizeof(fd_set));
    memcpy(&state->_wfds,&state->wfds,sizeof(fd_set));

    retval = select(eventLoop->maxfd+1,
                &state->_rfds,&state->_wfds,NULL,tvp);
    if (retval > 0) {
        for (j = 0; j <= eventLoop->maxfd; j++) {
            int mask = 0;
            aeFileEvent *fe = &eventLoop->events[j];

            if (fe->mask == AE_NONE) continue;
            if (fe->mask & AE_READABLE && FD_ISSET(j,&state->_rfds))
                mask |= AE_READABLE;
            if (fe->mask & AE_WRITABLE && FD_ISSET(j,&state->_wfds))
                mask |= AE_WRITABLE;
            eventLoop->fired[numevents].fd = j;
            eventLoop->fired[numevents].mask = mask;
            numevents++;
        }
    }
    return numevents;
}
static char *aeApiName(void) {
    return "select";
}
//--------------------select实现结束----------------------
        #endif
    #endif
#endif