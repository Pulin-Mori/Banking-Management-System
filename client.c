#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define SERVER_IP     "127.0.0.1"
#define SERVER_PORT   8080

#define MSG_DISPLAY   1
#define MSG_PROMPT    2
#define MSG_EXIT      3

#define MAX_PAYLOAD   16384
#define MAX_INPUT_LEN 128

typedef struct {
    int msg_type;
    int length;
    char payload[MAX_PAYLOAD];
} NetPacket;

/* Write all bytes over a file descriptor or socket */
static ssize_t write_all(int fd, const void *buf, size_t count) {
    size_t written = 0;
    const char *ptr = (const char *)buf;
    while (written < count) {
        ssize_t nw = write(fd, ptr + written, count - written);
        if (nw < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (nw == 0) break;
        written += (size_t)nw;
    }
    return (ssize_t)written;
}

/* Read exact number of bytes from a file descriptor or socket */
static ssize_t read_exact(int fd, void *buf, size_t count) {
    size_t total = 0;
    char *ptr = (char *)buf;
    while (total < count) {
        ssize_t nr = read(fd, ptr + total, count - total);
        if (nr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (nr == 0) break;
        total += (size_t)nr;
    }
    return (ssize_t)total;
}

/* Read one line from keyboard (STDIN_FILENO) using read() syscall */
static ssize_t read_line_fd(int fd, char *buf, size_t max_len) {
    size_t idx = 0;
    char c = 0;
    while (idx < max_len - 1) {
        ssize_t nr = read(fd, &c, 1);
        if (nr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (nr == 0 || c == '\n') break;
        if (c != '\r') buf[idx++] = c;
    }
    buf[idx] = '\0';
    return (ssize_t)idx;
}

int main(int argc, char *argv[]) {
    const char *ip = (argc >= 2) ? argv[1] : SERVER_IP;
    int port = (argc >= 3) ? atoi(argv[2]) : SERVER_PORT;

    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons((uint16_t)port);

    if (inet_pton(AF_INET, ip, &serv_addr.sin_addr) <= 0 ||
        connect(sock_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("connect");
        close(sock_fd);
        return 1;
    }

    NetPacket pkt;
    while (read_exact(sock_fd, &pkt, sizeof(NetPacket)) == (ssize_t)sizeof(NetPacket)) {
        if (pkt.length > 0 && pkt.length < MAX_PAYLOAD) {
            write_all(STDOUT_FILENO, pkt.payload, (size_t)pkt.length);
        }
        if (pkt.msg_type == MSG_PROMPT) {
            char input[MAX_INPUT_LEN];
            memset(input, 0, sizeof(input));
            if (read_line_fd(STDIN_FILENO, input, sizeof(input)) < 0) break;
            if (write_all(sock_fd, input, sizeof(input)) != (ssize_t)sizeof(input)) break;
        } else if (pkt.msg_type == MSG_EXIT) {
            break;
        }
    }

    close(sock_fd);
    return 0;
}