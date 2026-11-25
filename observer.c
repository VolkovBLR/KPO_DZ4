#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static volatile sig_atomic_t caught_signal;

static void handle_signal(int signal_number)
{
    caught_signal = signal_number;
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    return 0;
}

static int wait_for_socket(int fd, short events, int timeout_ms)
{
    struct pollfd descriptor;
    int result;

    descriptor.fd = fd;
    descriptor.events = events;
    descriptor.revents = 0;
    do {
        result = poll(&descriptor, 1, timeout_ms);
    } while (result < 0 && errno == EINTR && !caught_signal);
    if (result <= 0)
        return -1;
    if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))
        return -1;
    return descriptor.revents & events ? 0 : -1;
}

static int connect_to_server(const char *host, const char *port)
{
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *address;
    int result;
    int connected = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    result = getaddrinfo(host, port, &hints, &addresses);
    if (result != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(result));
        return -1;
    }
    for (address = addresses; address != NULL; address = address->ai_next) {
        int fd = socket(address->ai_family, address->ai_socktype,
                        address->ai_protocol);

        if (fd < 0)
            continue;
        if (set_nonblocking(fd) != 0) {
            close(fd);
            continue;
        }
        result = connect(fd, address->ai_addr, address->ai_addrlen);
        if (result == 0) {
            connected = fd;
            break;
        }
        if (errno == EINPROGRESS && wait_for_socket(fd, POLLOUT, 5000) == 0) {
            int socket_error = 0;
            socklen_t error_size = sizeof(socket_error);

            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error,
                           &error_size) == 0 && socket_error == 0) {
                connected = fd;
                break;
            }
            if (socket_error != 0)
                errno = socket_error;
        }
        close(fd);
    }
    freeaddrinfo(addresses);
    return connected;
}

static int send_line(int fd, const char *line)
{
    size_t length = strlen(line);
    size_t sent = 0;

    while (sent < length) {
        ssize_t count = send(fd, line + sent, length - sent, 0);

        if (count > 0) {
            sent += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
            wait_for_socket(fd, POLLOUT, 5000) == 0)
            continue;
        return -1;
    }
    return 0;
}

static void print_line(const char *name, const char *line)
{
    printf("[%s] %s\n", name, line);
    fflush(stdout);
}

static int observe(int fd, const char *name)
{
    char bytes[2048];
    char line[4096];
    size_t line_length = 0;
    int saw_end = 0;

    while (!caught_signal) {
        struct pollfd descriptor;
        int poll_result;

        descriptor.fd = fd;
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        poll_result = poll(&descriptor, 1, 1000);
        if (poll_result < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            return -1;
        }
        if (poll_result == 0)
            continue;
        if (descriptor.revents & POLLIN) {
            ssize_t count = recv(fd, bytes, sizeof(bytes), 0);

            if (count == 0)
                return saw_end ? 0 : -1;
            if (count < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                perror("recv");
                return -1;
            }
            for (ssize_t i = 0; i < count; ++i) {
                if (bytes[i] == '\n') {
                    line[line_length] = '\0';
                    print_line(name, line);
                    if (strncmp(line, "EV END ", 7) == 0)
                        saw_end = 1;
                    line_length = 0;
                } else if (line_length + 1 < sizeof(line)) {
                    line[line_length++] = bytes[i];
                } else {
                    fprintf(stderr, "server sent an overlong line\n");
                    return -1;
                }
            }
            if (saw_end)
                return 0;
        }
        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))
            return saw_end ? 0 : -1;
    }
    return 0;
}

static void usage(const char *program)
{
    printf("Usage: %s [--host HOST] [--port PORT] [--name NAME]\n", program);
}

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1";
    const char *port = "5000";
    char default_name[64];
    const char *name;
    char hello[128];
    int fd;
    int result;

    snprintf(default_name, sizeof(default_name), "observer-%ld", (long)getpid());
    name = default_name;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc)
            host = argv[++i];
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = argv[++i];
        else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc)
            name = argv[++i];
        else {
            fprintf(stderr, "unknown or incomplete option: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }
    if (strlen(name) == 0 || strlen(name) >= 60 ||
        strpbrk(name, "\r\n") != NULL) {
        fprintf(stderr, "observer name must contain 1 to 59 characters without line breaks\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    fd = connect_to_server(host, port);
    if (fd < 0) {
        fprintf(stderr, "could not connect to %s:%s\n", host, port);
        return 1;
    }
    snprintf(hello, sizeof(hello), "HELLO %s\n", name);
    if (send_line(fd, hello) != 0) {
        perror("send");
        close(fd);
        return 1;
    }
    printf("[%s] connected to %s:%s as read-only observer\n", name, host, port);
    fflush(stdout);
    result = observe(fd, name);
    if (caught_signal)
        printf("[%s] observer disconnected by signal %d\n", name, caught_signal);
    else if (result == 0)
        printf("[%s] observation ended cleanly\n", name);
    else
        fprintf(stderr, "[%s] server connection ended unexpectedly\n", name);
    close(fd);
    return result == 0 ? 0 : 1;
}
