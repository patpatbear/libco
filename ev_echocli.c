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

void read_write_cb(int fd, short event, void *arg)
{
    int ret = 0;
    struct event *ev = arg;
	char str[8]="sarlmol";
	char buf[ 1024 * 16 ];

    if (event & EV_READ) {
        for (;;) {
            ret = read(fd, buf, sizeof(buf));
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
                AddSuccCnt();//由于每次可能read的大小不为8，导致统计不准确
            }
        }
    }

    if (event & EV_WRITE) {
        for (;;) {
            ret = write(fd, str, sizeof(buf));
            if (ret < 0) {
                if (errno == EINTR) {
                    continue;
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                } else {
                    perror("write");
                    goto err;
                }
            } else if (ret == 0) {
                perror("write return 0");
                goto err;
            } else {
                (void)0;
            }
        }
    }

    return ;

err:
    close(fd);
    event_del(ev);
    return ;
}


int main(int argc, char *argv[])
{
    int i, j, fd, nproc, ncon, port;
    int flags;
    struct event *ev;
    pid_t pid;
    struct sockaddr_in sin;

    if (argc != 5) {
        printf("Usage: %s <ip> <port> <ncon> <nroc>\n", argv[0]);
        return -1;
    }

    nproc = atoi(argv[4]);
    ncon = atoi(argv[3]);
    sin.sin_family = AF_INET;
    port = atoi(argv[2]);
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = inet_addr(argv[1]);


    for (i = 0; i < nproc; ++i) {
        pid = fork();
        if (pid > 0) {
            printf("forked %d \n", pid);
            continue;
        }
        if (pid < 0) {
            printf("fork fail\n");
            return -1;
        }

        event_init();

        for (j = 0; j < ncon; ++j) {
            if (-1 == (fd = socket(AF_INET, SOCK_STREAM, 0))) perror("socket");
            if (connect(fd, (struct sockaddr*)&sin, sizeof(sin))) perror("connect");

            flags = fcntl(fd, F_GETFL, 0);
            flags |= O_NONBLOCK;
            flags |= O_NDELAY;
            if (fcntl(fd, F_SETFL, flags)) perror("fcntl");

            ev = calloc(1, sizeof(struct event));
            event_set(ev, fd, EV_READ|EV_WRITE|EV_PERSIST, read_write_cb, ev);

            event_add(ev, NULL);

            printf("added ev %d\n", fd);
        }

        event_dispatch();
    }
    wait(NULL);
    return 0;
}
