#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/queue.h>
#include <stdlib.h>
#include "co_routine.h"

/* proxy for echosvr & echocli */

struct worker {
    TAILQ_ENTRY(worker) w_tqe;
    int                   cfd;
    int                   sfd;
    struct stCoRoutine_t  *co;
    struct sockaddr_in    *sin;
};

TAILQ_HEAD(w_tqh, worker) wq[1];

static int iSuccCnt = 0;
static int iFailCnt = 0;
static int iTime = 0;

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

int co_accept(int fd, struct sockaddr *addr, socklen_t *len );
static void *accept_routine(void *arg)
{
    int lfd, cfd;
    int reuse;
    struct pollfd pfd;
    struct sockaddr *sa = (struct sockaddr*)arg;
    struct worker *w;

    lfd = socket(AF_INET, SOCK_STREAM, 0);
    reuse = 1;
    setsockopt(lfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    bind(lfd, sa, sizeof(struct sockaddr_in));
    listen(lfd, 500);

    for (;;) {
        cfd = co_accept(lfd, NULL, NULL); //为什么要co_accept? 为啥不直接accept？
        if (cfd < 0) {
            pfd.fd = lfd;
            pfd.events = (POLLIN|POLLERR|POLLHUP);
            pfd.revents = 0;
            co_poll(co_get_epoll_ct(), &pfd, 1, 1000);
            printf("accepting.\n");
            continue;
        }
        printf("accepted\n");

        w = wq->tqh_first;
        if (w) {
            w->cfd = cfd;
            TAILQ_REMOVE(wq, w, w_tqe);
            co_resume(w->co);
        } else {
            AddFailCnt();
            close(cfd);
        }
    }

    return NULL;
}

static void *proxy_routine(void *arg)
{
    int nr, nw, sfd, ret;
    char buf[1024];
    struct worker *w;

    w = (struct worker*)arg;

    for (;;) {
        if (w->sfd < 0) {
            sfd = socket(AF_INET, SOCK_STREAM, 0);
            ret = connect(sfd, (struct sockaddr*)w->sin, sizeof(struct sockaddr_in));
            if (ret != 0) {
                printf("connect svr fail\n");
                close(sfd);
                if (w->cfd >= 0) {
                    printf("client conn closed\n");
                    close(w->cfd);
                }
                w->cfd = w->sfd = -1;
                TAILQ_INSERT_HEAD(wq, w, w_tqe);

                co_yield_ct();
                continue;
            }

            printf("connect svr suc\n");
            w->sfd = sfd;
        }

        nr = read(w->cfd, buf, sizeof(buf));
        printf("read client\n");
        nw = write(w->sfd, buf, nr);
        printf("write svr\n");
        nr = read(w->sfd, buf, sizeof(buf));
        printf("read svr \n");
        nw = write(w->cfd, buf, nr);
        printf("write client\n");
    }

    (void)nw;
    return NULL;
}


int main(int argc, char *argv[])
{
    int i, nco, lport, sport;
    struct sockaddr_in sinl, sins;
    struct stCoRoutine_t *co;
    struct worker *ws;

    if (argc != 6) {
        fprintf(stderr, "Usage: %s <listen_ip> <listen_port> "
                "<svr_ip> <svr_port> <nco>\n", argv[0]);
        return -1;
    }

    nco                  = atoi(argv[5]);
    lport                = atoi(argv[2]);
    sinl.sin_family      = AF_INET;
    sinl.sin_port        = htons(lport);
    sinl.sin_addr.s_addr = inet_addr(argv[1]);

    sport                = atoi(argv[4]);
    sins.sin_family      = AF_INET;
    sins.sin_port        = htons(sport);
    sins.sin_addr.s_addr = inet_addr(argv[3]);

    //accept_routine(&sinl);

    co_enable_hook_sys();

    TAILQ_INIT(wq);

    ws = (struct worker*)calloc(nco, sizeof(struct worker));

    co_create(&co, NULL, accept_routine, &sinl);
    co_resume(co);

    for (i = 0; i < nco; ++i) {
        ws[i].cfd = -1;
        ws[i].sfd = -1;
        ws[i].sin = &sins;
        co_create(&ws[i].co, NULL, proxy_routine, &ws[i]);
        //co_resume(ws[i].co);
        TAILQ_INSERT_HEAD(wq, &ws[i], w_tqe);
    }


    co_eventloop(co_get_epoll_ct(), 0, 0);
}

