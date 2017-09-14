#include <stdio.h>
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
};

TAILQ_HEAD(w_tqh, worker) wq[1];

static void *accept_routine(void *arg)
{
    int lfd, cfd;
    struct sockaddr *sa = (struct sockaddr*)arg;
    struct worker *w;

    lfd = socket(AF_INET, SOCK_STREAM, 0);
    bind(lfd, sa, sizeof(struct sockaddr_in));
    listen(lfd, 500);

    for (;;) {
        cfd = accept(lfd, NULL, NULL);

        w = wq->tqh_first;
        if (w) {
            w->cfd = cfd;
            TAILQ_REMOVE(wq, w, w_tqe);
            co_resume(w->co);
        }
    }
}

static void *proxy_routine(void *arg)
{
    struct worker *w;

    w = (struct worker*)arg;
    if (w->sfd < 0) {
        fprintf(stderr, "connect to svr fail\n");
        return NULL;
    }

    for (;;) {
    }

}

static int conn_svr(struct sockaddr_in *sin)
{
    int sfd;
    
    sfd = socket(AF_INET, SOCK_STREAM, 0);
    connect(sfd, (struct sockaddr*)sin, sizeof(*sin));

    return sfd;
}


int main(int argc, char *argv[])
{

    int i, nco, lport, sport;
    struct sockaddr_in sinl, sins;
    struct stCoRoutine_t *co;
    struct worker *ws;

    if (argc != 4) {
        fprintf(stderr, "Usage: %s <listen_ip> <listen_port>\
                <svr_ip> <svr_port> <nco>\n", argv[0]);
    }

    nco                 = atoi(argv[5]);
    lport                = atoi(argv[2]);
    sinl.sin_family      = AF_INET;
    sinl.sin_port        = htons(lport);
    sinl.sin_addr.s_addr = inet_addr(argv[1]);

    sport                = atoi(argv[4]);
    sins.sin_family      = AF_INET;
    sins.sin_port        = htons(sport);
    sins.sin_addr.s_addr = inet_addr(argv[3]);

    co_enable_hook_sys();

    TAILQ_INIT(wq);

    ws = calloc(nco, sizeof(struct worker));

    co_create(&co, NULL, accept_routine, &sinl);
    co_resume(co);

    for (i = 0; i < nco; ++i) {
        ws[i].cfd = -1;
        ws[i].sfd = conn_svr(&sins);
        co_create(&ws[i].co, NULL, proxy_routine, &ws[i]);
        co_resume(ws[i].co);
    }

    co_eventloop(co_get_epoll_ct(), 0, 0);
}

