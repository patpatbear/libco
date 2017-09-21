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


#define DEBUG(...) printf(__VA_ARGS__)
#define INFO(...)  printf(__VA_ARGS__)
#define ERROR(...) do {printf(__VA_ARGS__); pause();} while(0)


void AddReadCnt()
{
    static int iSuccCnt = 0;
    static int iFailCnt = 0;
    static int iTime = 0;

	int now = time(NULL);
	if (now >iTime) {
		printf("Read: Succ %d Fail %d\n", iSuccCnt, iFailCnt);
		iTime = now;
		iSuccCnt = 0;
		iFailCnt = 0;
	} else {
		iSuccCnt++;
	}
}

void AddWriteCnt()
{
    static int iSuccCnt = 0;
    static int iFailCnt = 0;
    static int iTime = 0;

	int now = time(NULL);
	if (now >iTime) {
		printf("Write: Succ %d Fail %d\n", iSuccCnt, iFailCnt);
		iTime = now;
		iSuccCnt = 0;
		iFailCnt = 0;
	} else {
		iSuccCnt++;
	}
}

/* 
 * 在做性能测试的时候应该公平模拟客户端的行为：
 * 客户端）while(write->read)
 * 服务端）while(write); while(read)
 * libevent要实现write->read这种类似于同步的行为
 * 最好还是通过读写cb拆分的形式进行。
 */

struct rw_arg {
    struct event *evr;
    struct event *evw;
    int fd;
    char buf[8];
};


void read_cb(int fd, short event, void *arg)
{
    int ret;
    struct rw_arg *rw = arg;

    for (;;) {
        ret = read(fd, rw->buf, sizeof(rw->buf));
        if (ret < 0) {
            if (errno == EINTR) {
                DEBUG("read intr");
                continue;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                printf("read done\n");
                break;
            } else {
                ERROR("read");
                goto err;
            }
        } else if (ret == 0) {
            INFO("connection closed\n");
            goto err;
        } else {
            AddReadCnt();
            event_add(rw->evw, NULL);
            break;
        }
    }

    return ;

err:
    close(fd);
}

void write_cb(int fd, short event, void *arg)
{
    int ret = 0;
    struct rw_arg *rw = arg;
	char str[8]="sarlmol";

    for (;;) {
        ret = write(fd, str, 8);
        if (ret < 0) {
            if (errno == EINTR) {
                INFO("EINTR\n");
                continue;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /*
                 * 127.0.0.1的write效率很高, read效率更高, TPS能到400W左右
                 * 如果一直for循环写，其实svr已经read完成，所以不会EAGAIN
                 * 综上：write EAGAIN不是很经常发生的
                 */
                INFO("write done\n");
                break;
            } else {
                ERROR("write error\n");
                goto err;
            }
        } else if (ret == 0) {
            ERROR("write return 0");
            goto err;
        } else {
            AddWriteCnt();
            event_add(rw->evr, NULL);
            break;
        }
    }

    return ;

err:
    close(fd);
    return ;
}

static int setnonblock(int fd)
{
    int flags;
    if (fd < 0) {
        return -1;
    }

    flags = fcntl(fd, F_GETFL, 0);
    flags |= O_NONBLOCK;
    flags |= O_NDELAY;

    if (fcntl(fd, F_SETFL, flags)) {
        perror("setnonblock");
        return -1;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    int i, j, fd, nproc, ncon, port;
    pid_t pid;
    struct sockaddr_in sin;
    struct rw_arg *rw;

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

    event_init();

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

        for (j = 0; j < ncon; ++j) {
            if (-1 == (fd = socket(AF_INET, SOCK_STREAM, 0))) perror("socket");
            if (connect(fd, (struct sockaddr*)&sin, sizeof(sin))) perror("connect");
            if (setnonblock(fd)) ERROR("setnonblock\n");

            rw = calloc(1, sizeof(struct rw_arg));
            rw->evr = calloc(1, sizeof(struct event));
            rw->evw = calloc(1, sizeof(struct event));

            event_set(rw->evr, fd, EV_READ, read_cb, rw);
            event_set(rw->evw, fd, EV_WRITE, write_cb, rw);
            event_add(rw->evw, NULL);
        }

        event_dispatch();
    }

    wait(NULL);
    return 0;
}
