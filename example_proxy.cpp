#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/queue.h>
#include <stdlib.h>
#include <errno.h>
#include "co_routine.h"

//#define DEBUG(...) printf(__VA_ARGS__)
#define DEBUG(...)
#define INFO(...) printf(__VA_ARGS__)
#define ERROR(...) printf(__VA_ARGS__)
/* proxy for echosvr & echocli */

struct worker {
    TAILQ_ENTRY(worker) w_tqe;
    int                   cfd;
    int                   sfd;
    struct stCoRoutine_t  *co;
};

TAILQ_HEAD(w_tqh, worker) wq[1];

static int iSuccCnt = 0;
static int iFailCnt = 0;
static int iTime = 0;
static int lfd = -1;


void AddSuccCnt()
{
	int now = time(NULL);
    iSuccCnt++;
	if (now >iTime) {
		printf("time %d Succ Cnt %d Fail Cnt %d\n", iTime, iSuccCnt, iFailCnt);
		iTime = now;
		iSuccCnt = 0;
		iFailCnt = 0;
	}
}

void AddFailCnt()
{
	int now = time(NULL);
    iFailCnt++;
	if (now >iTime)
	{
		printf("time %d Succ Cnt %d Fail Cnt %d\n", iTime, iSuccCnt, iFailCnt);
		iTime = now;
		iSuccCnt = 0;
		iFailCnt = 0;
	}
}

int setnonblock(int fd)
{
    int flags;

    if (fd < 0 ) return -1;

    flags = fcntl(fd, F_GETFL, 0);
    flags |= O_NONBLOCK;
    if (fcntl(fd, F_SETFL, flags)) {
        perror("fcntl");
        return -1;
    }
    return 0;
}

int co_accept(int fd, struct sockaddr *addr, socklen_t *len );
static void *accept_routine(void *arg)
{
    int cfd;
    struct pollfd pfd;
    struct worker *w;

    co_enable_hook_sys();

    for (;;) {
        cfd = co_accept(lfd, NULL, NULL);
        if (cfd < 0) {
            pfd.fd = lfd;
            pfd.events = (POLLIN|POLLERR|POLLHUP);
            pfd.revents = 0;
            printf("accepting.\n");
            co_poll(co_get_epoll_ct(), &pfd, 1, -1);
            continue;
        }
        printf("accepted\n");
        //usleep(5000);
        if (setnonblock(cfd)) {
            ERROR("setnonblock\n");
        }

        w = wq->tqh_first;
        if (w) {
            w->cfd = cfd;
            TAILQ_REMOVE(wq, w, w_tqe);
            co_resume(w->co);
        } else {
            printf("no worker\n");
            AddFailCnt();
            close(cfd);
            cfd = -1;
        }
    }

    return NULL;
}

static int pre_connect_svr(struct sockaddr_in *sin)
{
    int sfd, ret;

    DEBUG("1.\n");
    sfd = socket(AF_INET, SOCK_STREAM, 0);
    DEBUG("2.\n");
    ret = connect(sfd, (struct sockaddr*)sin, sizeof(struct sockaddr_in));
    DEBUG("3.\n");
    if (ret != 0) {
        printf("pre_connect_svr fail \n");
        close(sfd);
    }
    return sfd;
}


/* sfd & cfd 都是user nonblock
 * user nonblock的fd 调用的网络API都是系统的，并没有hook
 * */
static void *proxy_routine(void *arg)
{
    int nr, nw;
    char buf[8];
    struct worker *w;

    w = (struct worker*)arg;

    co_enable_hook_sys();

    if (!(fcntl(w->sfd,F_GETFL, 0) & O_NONBLOCK)) {
        ERROR("sfd block\n");
    }

    if (!(fcntl(w->cfd,F_GETFL, 0) & O_NONBLOCK)) {
        ERROR("cfd block\n");
    }

    for (;;) {
restart:
        if (w->sfd < 0 || w->cfd < 0) {
            printf("proxy PANIC! \n");
            TAILQ_INSERT_HEAD(wq, w, w_tqe);
            co_yield_ct();
            continue;
        }

        for (;;) {
            nr = read(w->cfd, buf, sizeof(buf));
            if (nr < 0 && errno == EINTR) {
                continue;
            } else if (nr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                struct pollfd pf = {0};
                pf.fd = w->cfd;
                pf.events = (POLLIN|POLLERR|POLLHUP);
                co_poll(co_get_epoll_ct(), &pf, 1, -1);
                continue;
            } else if (nr <= 0) {
                printf("client closed\n");
                close(w->cfd), w->cfd = -1;
                TAILQ_INSERT_HEAD(wq, w, w_tqe);
                co_yield_ct();
                goto restart;
            } else {
                DEBUG("-->c\n");
                break;
            }
        }

        for (;;) {
            nw = write(w->sfd, buf, nr);
            if (nw < 0 && errno == EINTR) {
                continue;
            } else if (nw < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                struct pollfd pf;
                pf.fd = w->sfd;
                pf.events = (POLLOUT|POLLERR|POLLHUP);
                co_poll(co_get_epoll_ct(), &pf, 1, -1);
                continue;
            } else if (nw <= 0) {
                printf("svr closed, co exit\n");
                close(w->cfd), w->cfd = -1;
                close(w->sfd), w->sfd = -1;
                goto restart;
            } else {
                DEBUG("-->s\n");
                break;
            }
        }

        for (;;) {
            nr = read(w->sfd, buf, sizeof(buf));
            if (nr < 0 && errno == EINTR) {
                continue;
            } else if (nr < 0 && (errno == EWOULDBLOCK||errno == EAGAIN)) {
                struct pollfd pf;
                pf.fd = w->sfd;
                pf.events = (POLLIN|POLLHUP|POLLERR);
                co_poll(co_get_epoll_ct(), &pf, 1, -1);
                continue;
            } else if (nr <= 0) {
                printf("svr closed, co exit\n");
                close(w->cfd), w->cfd = -1;
                close(w->sfd), w->sfd = -1;
                co_yield_ct();
                goto restart;
            } else {
                DEBUG("s<--\n");
                break;
            }
        }

        for (;;) {
            nw = write(w->cfd, buf, nr);
            if (nw < 0 && errno == EINTR) {
                continue;
            } else if (nw < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                struct pollfd pf;
                pf.fd = w->cfd;
                pf.events = (POLLOUT|POLLERR|POLLHUP);
                co_poll(co_get_epoll_ct(), &pf, 1, -1);
                continue;
            } else if (nw <= 0) {
                printf("client closed\n");
                close(w->cfd), w->cfd = -1;
                TAILQ_INSERT_HEAD(wq, w, w_tqe);
                co_yield_ct();
                continue;
            } else {
                DEBUG("c<--\n");
                break;
            }
        }
    }

    (void)nw;
    return NULL;
}

static void *dummy_routine(void *arg)
{
    printf("dummy\n");
    return NULL;
}


/* 
 * 总体上来看，main函数中co_create，尝试enable_hook_sys是一个非常
 * 糟糕的尝试
 *
 * 从范例和实际体验下来看，几乎只能再co中enable_hook_sys，并且一般
 * fd都是user_nonblock的，所以能用到的hook真的很少！
 */
int main(int argc, char *argv[])
{
    int i, nco, lport, sport, nproc;
    pid_t pid;
    int flags;
    int reuse = 1;
    struct sockaddr_in sinl, sins;
    struct stCoRoutine_t *co;
    struct worker *ws;

    if (argc != 7) {
        fprintf(stderr, "Usage: %s <listen_ip> <listen_port> "
                "<svr_ip> <svr_port> <nco> <nproc>\n", argv[0]);
        return -1;
    }

    nco                  = atoi(argv[5]);
    nproc                = atoi(argv[6]);
    lport                = atoi(argv[2]);
    sinl.sin_family      = AF_INET;
    sinl.sin_port        = htons(lport);
    sinl.sin_addr.s_addr = inet_addr(argv[1]);

    sport                = atoi(argv[4]);
    sins.sin_family      = AF_INET;
    sins.sin_port        = htons(sport);
    sins.sin_addr.s_addr = inet_addr(argv[3]);

    TAILQ_INIT(wq);

    /* enable main co, otherwise enable_hook_sys would not work */
    co_create(&co, NULL, dummy_routine, NULL);
    co_enable_hook_sys();

    /* listen */
    lfd = socket(AF_INET, SOCK_STREAM, 0);
    flags = fcntl(lfd, F_GETFL);
    if (!(flags & O_NONBLOCK)) {
        printf("what the fuck! blocked lfd\n");
    }

    if (lfd < 0) {
        printf("socket create error \n");
        return -1;
    }
    setsockopt(lfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    if(bind(lfd, (struct sockaddr*)&sinl, sizeof(struct sockaddr_in))) perror("bind");
    if(listen(lfd, 500)) perror("listen");


    ws = (struct worker*)calloc(nco, sizeof(struct worker));

    for (i = 0; i < nproc; ++i) {
        pid = fork();
        if (pid > 0) {
            printf("forked %d\n", pid);
            continue;
        } else if (pid < 0) break;

        /* fork 之后再enable就会出现core */
        //co_create(&co, NULL, dummy_routine, NULL);
        //co_enable_hook_sys(); 
        /* pre connect svr */
        for (i = 0; i < nco; ++i) {
            ws[i].cfd = -1;
            ws[i].sfd = pre_connect_svr(&sins);
            if (setnonblock(ws[i].sfd)) {
                ERROR("setnonblock\n");
            }
        }

        printf("pre connect done\n");

        /* create (but not resume) proxy_routine */
        for (i = 0; i < nco; ++i) {
            if (ws[i].sfd < 0) {
                printf("co create error %d\n", i);
                continue;
            }
            co_create(&ws[i].co, NULL, proxy_routine, &ws[i]);
            TAILQ_INSERT_HEAD(wq, &ws[i], w_tqe);
        }

        /* create & resume accept routine, enable hook from now on */
        co_create(&co, NULL, accept_routine, &sinl);
        co_resume(co);

        co_eventloop(co_get_epoll_ct(), 0, 0);
    }

    wait(NULL);
}
