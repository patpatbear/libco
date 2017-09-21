#include <fcntl.h>
#include <time.h>
#include <event.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

//#define DEBUG(...) printf(__VA_ARGS__)
#define DEBUG(...) 
#define ERROR(...) do {fprintf(stderr, __VA_ARGS__); pause();}while(0)

static int iSuccCnt = 0;
static int iFailCnt = 0;
static int iTime = 0;

static void AddSuccCnt()
{
	int now = time(NULL);
	if (now >iTime) {
		//DEBUG("time %d Succ Cnt %d Fail Cnt %d\n", iTime, iSuccCnt, iFailCnt);
		iTime = now;
		iSuccCnt = 0;
		iFailCnt = 0;
	} else {
		iSuccCnt++;
	}
}


static int setnonblock(int fd) 
{
    int flags;
    if (fd < 0) {
        ERROR("fd %d < 0", fd);
        return -1;
    }

    flags = fcntl(fd, F_GETFL, 0);
    flags |= O_NONBLOCK;
    flags |= O_NDELAY;
    if (fcntl(fd, F_SETFL, flags)) {
        perror("fcntl");
        return -1;
    }

    return 0;
}

struct accept_arg {
    struct sockaddr_in *saddr;   /* svr socket addr */
    struct event *eva;           /* accept event */
};

static int gid;

struct rw_arg {
    struct accept_arg *aa;
    struct event *evrc;     /* client read event */
    struct event *evrs;     /* server read event */
    struct event *evwc;     /* client write event */
    struct event *evws;     /* server write event */
    int id;
    int sfd;
    int cfd;
    int reqcnt;
    char reqbuf[1024];
    int rspcnt;
    char rspbuf[1024];
    int svr_connected;
};

void client_write_cb(int fd, short event, void *arg)
{
    int ret, i;
    struct rw_arg *rw = arg;

    DEBUG("evwc triggered: ");

    if (!(event & EV_WRITE)) {
        ERROR("PANIC! no write event\n");
        return;
    }

    for (i = 0; i < rw->rspcnt; ++i) {
        for (;;) {
            ret = write(fd, rw->rspbuf, 8);

            if (ret < 0) {
                if (errno == EINTR) {
                    continue;
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    goto end;
                } else {
                    ERROR("client write\n");
                    goto err;
                }
            } else if (ret == 0) {
                ERROR("write return 0 \n");
                goto err;
            } else {
                DEBUG("%d c<--: %s", i+1, rw->rspbuf);
                break;
            }
        }
    }

end:
    rw->rspcnt -= i;

    return;

err:
    ERROR("client_write_cb");
    close(fd);
    event_del(rw->evrc);
    event_del(rw->evwc);
}

void client_read_cb(int fd, short event, void *arg)
{
    int ret = 0, cnt = 0;
    struct rw_arg *rw = arg;

    DEBUG("---------------\n");
    DEBUG("evrc triggered: ");

    if (event & EV_READ) {
        for (;;) {
            ret = read(fd, rw->reqbuf, sizeof(rw->reqbuf));

            if (ret < 0) {
                if (errno == EINTR) {
                    continue;
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                } else {
                    perror("read");
                    goto err;
                }
            } else if (ret == 0) {
                DEBUG("connection closed\n");
                goto err;
            } else {
                AddSuccCnt();
                cnt++;
                DEBUG("%d -->c: %s", cnt, rw->reqbuf);
            }
        }

        if (cnt > 0) {
            rw->reqcnt += cnt;
            event_add(rw->evws, NULL);
            DEBUG("evws added\n");
        }
    }

    return ;

err:
    DEBUG("client_read_cb");
    event_del(rw->evrc);
    event_del(rw->evwc);
    close(fd);
    return;
}

void svr_write_cb(int fd, short event, void *arg)
{
    int ret, i;
    struct rw_arg *rw = arg;

    DEBUG("evws triggered: ");

    if (!(event & EV_WRITE)) {
        ERROR("PANIC! no write event\n");
        return;
    }

    for (i = 0; i < rw->reqcnt; ++i) {
        for (;;) {
            ret = write(fd, rw->reqbuf, 8);

            if (ret < 0) {
                if (errno == EINTR) {
                    continue;
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    goto end;
                } else {
                    ERROR("svr write\n");
                    goto err;
                }
            } else if (ret == 0) {
                ERROR("write return 0 \n");
                goto err;
            } else {
                DEBUG("%d -->s: %s", i+1, rw->reqbuf);
                break;
            }
        }
    }

end:

    rw->reqcnt -= i;

    return;

err:
    ERROR("svr_write_cb\n");
    close(fd);
    event_del(rw->evws);
    event_del(rw->evrs);
}

void svr_read_cb(int fd, short event, void *arg)
{
    int ret = 0, cnt = 0;
    struct rw_arg *rw = arg;

    DEBUG("evrs triggered: ");

    if (event & EV_READ) {
        for (;;) {
            ret = read(fd, rw->rspbuf, sizeof(rw->rspbuf));

            if (ret < 0) {
                if (errno == EINTR) {
                    continue;
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                } else {
                    perror("read");
                    goto err;
                }
            } else if (ret == 0) {
                DEBUG("connection closed: %d\n", fd);
                goto err;
            } else {
                AddSuccCnt();
                cnt++;
                DEBUG("%d s<--: %s", cnt, rw->rspbuf);
            }
        }

        if (cnt > 0) {
            rw->rspcnt += cnt;
            event_add(rw->evwc, NULL);
            DEBUG("evwc added\n");
        }
    }

    return ;

err:
    DEBUG("svr_read_cb\n");
    event_del(rw->evrs);
    event_del(rw->evws);
    close(fd);
    return ;

}

void svr_connected_cb(int fd, short event, void *arg)
{
    int err = 0;
    socklen_t len = sizeof(err);
    struct rw_arg *rw = arg;

    DEBUG("connect triggered: %d\n", fd);

    if (event & EV_WRITE) {
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR,&err, &len)) perror("getsockopt");
        if (err) {
            ERROR("connectx!\n");
            return;
        }

        event_set(rw->evws, fd, EV_WRITE, svr_write_cb, rw);
        event_add(rw->evrs, NULL);
    }
}

void accept_cb(int fd, short event, void *arg)
{
    int cfd = -1, sfd = -1, ret;
    struct event *evrc, *evwc, *evws, *evrs;
    struct accept_arg *aa = arg;
    struct rw_arg *rw;

    if (event & EV_READ) {
        if (-1 == (cfd = accept(fd, NULL, NULL))) {
            ERROR("accept error\n");
            goto err;
        }

        if (-1 == (sfd = socket(AF_INET, SOCK_STREAM, 0)))
            perror("socket");


        if (setnonblock(cfd)) perror("setnonblock"); 
        if (setnonblock(sfd)) perror("setnonblock");

        rw = calloc(1, sizeof(struct rw_arg));
        evwc = calloc(1, sizeof(struct event));
        evrc = calloc(1, sizeof(struct event));
        evws = calloc(1, sizeof(struct event));
        evrs = calloc(1, sizeof(struct event));


        /* evwc can be write event */
        event_set(evrs, sfd, EV_READ|EV_PERSIST, svr_read_cb, rw);
        event_set(evrc, cfd, EV_READ|EV_PERSIST, client_read_cb, rw);
        event_set(evwc, cfd, EV_WRITE, client_write_cb, rw);

        rw->aa = aa;
        rw->evrc = evrc;
        rw->evwc = evwc;
        rw->evrs = evrs;
        rw->evws = evws;
        rw->reqcnt = 0;
        rw->rspcnt = 0;
        rw->svr_connected = 0;
        rw->cfd = cfd;
        rw->sfd = sfd;
        rw->id = gid++;

        ret = connect(sfd, (struct sockaddr*)aa->saddr, sizeof(struct sockaddr_in));
        if (ret == 0) {
            rw->svr_connected = 1;
            DEBUG("fd pair: %d->%d suc\n", cfd, sfd);
            
            event_set(evws, sfd, EV_WRITE, svr_write_cb, rw);
            event_add(evrs, NULL);
        }  else if (ret == -1 && errno == EINPROGRESS) {
            rw->svr_connected = 0; /* connecting */
            DEBUG("inprogress\n");

            event_set(evws, sfd, EV_WRITE, svr_connected_cb, rw);
            event_add(evws, NULL);
        } else {
            perror("connect");
            goto err;
        }

        event_add(evrc, NULL);
    }

    if (event & (~EV_READ)) {
        ERROR("PANIC!\n");
        goto err;
    }

    return ;

err:
    if (fd >= 0) close(fd);
    if (sfd >= 0) close(sfd);
    if (cfd >= 0) close(cfd);
    event_del(aa->eva);
}

int main(int argc, char *argv[])
{
    int i, fd, nproc, port;
    struct event *eva;
    pid_t pid;
    struct accept_arg *aa;
    int reuse = 1;
    struct sockaddr_in laddr, saddr;

    if (argc != 6) {
        printf("Usage: %s <listenip> <listenport> <svrip> <svrport> <nproc>\n", argv[0]);
        return -1;
    }

    nproc = atoi(argv[5]);

    laddr.sin_family = AF_INET;
    port = atoi(argv[2]);
    laddr.sin_port = htons(port);
    laddr.sin_addr.s_addr = inet_addr(argv[1]);

    saddr.sin_family = AF_INET;
    port = atoi(argv[4]);
    saddr.sin_port = htons(port);
    saddr.sin_addr.s_addr = inet_addr(argv[3]);

    aa = calloc(1, sizeof(struct accept_arg));
    aa->saddr = &saddr;

    event_init();

    if (-1 == (fd = socket(AF_INET, SOCK_STREAM, 0))) perror("socket");
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse))) perror("reuse");
    if (bind(fd, (struct sockaddr*)&laddr, sizeof(laddr))) perror("bind");
    if (listen(fd, 1024)) perror("listen");
    if (setnonblock(fd)) ERROR("setnonblock error\n");
    DEBUG("1\n");

    eva = calloc(1, sizeof(struct event));

    event_set(eva, fd, EV_READ|EV_PERSIST, accept_cb, aa);
    event_add(eva, NULL);
    aa->eva = eva;

    for (i = 0; i < nproc; ++i) {
        pid = fork();
        if (pid > 0) {
            DEBUG("forked %d\n", pid);
            continue;
        }
        if (pid < 0) {
            perror("fork");
            return -1;
        }

        event_dispatch();
    }
    wait(NULL);
    return 0;
}
