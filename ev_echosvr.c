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



static int iSuccCnt = 0;
static int iFailCnt = 0;
static int iTime = 0;

void AddSuccCnt()
{
	int now = time(NULL);
	if (now >iTime) {
		printf("time %d Succ Cnt %d Fail Cnt %d\n", iTime, iSuccCnt, iFailCnt);
		iTime = now;
		iSuccCnt = 0;
		iFailCnt = 0;
	} else {
		iSuccCnt++;
	}
}

void AddFailCnt()
{
	int now = time(NULL);
	if (now >iTime) {
		printf("time %d Succ Cnt %d Fail Cnt %d\n", iTime, iSuccCnt, iFailCnt);
		iTime = now;
		iSuccCnt = 0;
		iFailCnt = 0;
	} else {
        iFailCnt++;
	}
}

static int setnonblock(int fd) 
{
    int flags;
    if (fd < 0) {
        printf("fd %d < 0", fd);
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

static int gid = 0;

struct rwarg {
    struct event *evr;
    struct event *evw;
    int cnt;
    char buf[1024];
    int id;
};

void read_cb(int fd, short event, void *arg)
{
    int ret = 0, cnt = 0;
    struct rwarg *rw = arg;

    if (event & EV_READ) {
        for (;;) {
            ret = read(fd, rw->buf, sizeof(rw->buf));

            printf("%d: read %s", rw->id, rw->buf);

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
                printf("connection closed\n");
                goto err;
            } else {
                AddSuccCnt();
                cnt++;
            }
        }

        if (cnt > 0) {
            rw->cnt += cnt;
            event_add(rw->evw, NULL);
        }
    }

    return ;

err:
    printf("%d: closed\n", rw->id);
    close(fd);
    event_del(rw->evr);
    return ;
}

void write_cb(int fd, short event, void *arg)
{
    int ret, i;
    struct rwarg *rw = arg;

    if (!(event & EV_WRITE)) {
        printf("PANIC! no write event\n");
        return;
    }

    for (i = 0; i < rw->cnt; ++i) {
        for (;;) {
            ret = write(fd, rw->buf, 8);

            printf("%d: write %s", rw->id, rw->buf);

            if (ret < 0) {
                if (errno == EINTR) {
                    continue;
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                } else {
                    goto err;
                }
            } else if (ret == 0) {
                printf("write return 0 \n");
                goto err;
            } else {
                break;
            }
        }

        if (ret < 0) {
            break;
        }
    }

    rw->cnt -= i;

    return;

err:
    close(fd);
    event_del(rw->evw);
}

void accept_cb(int fd, short event, void *arg)
{
    int afd = -1;
    struct event *eva, *evr, *evw;
    struct rwarg *rw;
    eva = arg;

    if (event & EV_READ) {
        if (-1 == (afd = accept(fd, NULL, NULL))) {
            printf("accept error\n");
        }

        if (setnonblock(afd)) printf("setnonblock\n"); 

        evw = calloc(1, sizeof(struct event));
        evr = calloc(1, sizeof(struct event));
        
        rw = calloc(1, sizeof(struct rwarg));
        rw->evr = evr;
        rw->evw = evw;
        rw->cnt = 0;
        rw->id = gid++;

        event_set(evw, afd, EV_WRITE, write_cb, rw);
        event_set(evr, afd, EV_READ|EV_PERSIST, read_cb, rw);

        event_add(rw->evr, NULL);

        printf("accept fd %d suc\n", afd);
    }

    if (event & (~EV_READ)) {
        printf("PANIC!\n");
        close(fd);
        event_del(eva);
    }
}

int main(int argc, char *argv[])
{
    int i, fd, nproc, port;
    struct event *ev;
    pid_t pid;
    int reuse = 1;
    struct sockaddr_in sin;

    if (argc != 4) {
        printf("Usage: %s <ip> <port> <nproc>\n", argv[0]);
        return -1;
    }

    nproc = atoi(argv[3]);
    sin.sin_family = AF_INET;
    port = atoi(argv[2]);
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = inet_addr(argv[1]);


    for (i = 0; i < nproc; ++i) {
        pid = fork();
        if (pid > 0) {
            printf("forked %d\n", pid);
            continue;
        }
        if (pid < 0) {
            printf("fork fail\n");
            return -1;
        }

        event_init();

        if (-1 == (fd = socket(AF_INET, SOCK_STREAM, 0))) perror("socket");

        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse))) perror("reuse");
       
        if (bind(fd, (struct sockaddr*)&sin, sizeof(sin))) perror("bind");
        if (listen(fd, 1024)) perror("listen");
        if (setnonblock(fd)) printf("setnonblock error\n");

        ev = calloc(1, sizeof(struct event));
        event_set(ev, fd, EV_READ|EV_PERSIST, accept_cb, ev);
        event_add(ev, NULL);
        printf("added ev %d\n", fd);

        event_dispatch();
    }
    wait(NULL);
    return 0;
}
