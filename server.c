#define _POSIX_C_SOURCE 200809L

#include "race.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define SERVER_MAX_CLIENTS 128
#define SERVER_OUT_CAPACITY 32768
#define SERVER_LINE_CAPACITY 1024

typedef struct {
    int fd;
    int hello_received;
    char name[64];
    char input[SERVER_LINE_CAPACITY];
    size_t input_length;
    char output[SERVER_OUT_CAPACITY];
    size_t output_start;
    size_t output_length;
} Observer;

typedef struct {
    Race race;
    Observer observers[SERVER_MAX_CLIENTS];
    int listener;
    int input_open;
    int autostart;
    int once;
    int stopping;
    int stop_deadline_set;
    unsigned delay_ms;
    int64_t next_round_ms;
    int64_t stop_deadline_ms;
} Server;

static volatile sig_atomic_t caught_signal;

static void handle_signal(int signal_number)
{
    caught_signal = signal_number;
}

static int64_t monotonic_milliseconds(void)
{
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return 0;
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

static void log_message(const char *format, ...)
{
    va_list args;

    va_start(args, format);
    fputs("[server] ", stdout);
    vprintf(format, args);
    putchar('\n');
    fflush(stdout);
    va_end(args);
}

static void close_observer(Server *server, unsigned index, const char *reason)
{
    Observer *observer = &server->observers[index];

    if (observer->fd < 0)
        return;
    log_message("observer %s disconnected (%s)",
                observer->name[0] ? observer->name : "pending",
                reason);
    close(observer->fd);
    memset(observer, 0, sizeof(*observer));
    observer->fd = -1;
}

static int queue_to_observer(Server *server, unsigned index,
                             const char *message)
{
    Observer *observer = &server->observers[index];
    size_t length = strlen(message);
    size_t pending;

    if (observer->fd < 0)
        return -1;
    if (observer->output_start > 0) {
        pending = observer->output_length - observer->output_start;
        memmove(observer->output, observer->output + observer->output_start,
                pending);
        observer->output_start = 0;
        observer->output_length = pending;
    }
    pending = observer->output_length;
    if (length > sizeof(observer->output) - pending) {
        close_observer(server, index, "observer output queue is full");
        return -1;
    }
    memcpy(observer->output + observer->output_length, message, length);
    observer->output_length += length;
    return 0;
}

static void queue_formatted(Server *server, unsigned index,
                            const char *format, ...)
{
    char message[2048];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    queue_to_observer(server, index, message);
}

static void broadcast_event(void *context, const char *event)
{
    Server *server = context;
    char message[2304];

    log_message("%s", event);
    snprintf(message, sizeof(message), "EV %s\n", event);
    for (unsigned i = 0; i < SERVER_MAX_CLIENTS; ++i) {
        if (server->observers[i].fd >= 0 &&
            server->observers[i].hello_received)
            queue_to_observer(server, i, message);
    }
}

static void send_snapshot(Server *server, unsigned index)
{
    const char *state = server->race.state == RACE_WAITING ? "waiting" :
        server->race.state == RACE_RUNNING ? "running" : "finished";

    queue_formatted(server, index,
                    "EV SNAPSHOT state=%s round=%u lanes=%u cells=%u laps=%u "
                    "teams=%u cars=%u\n",
                    state, server->race.rounds, server->race.config.lanes,
                    server->race.config.cells, server->race.config.laps,
                    server->race.config.teams, server->race.car_count);
    for (unsigned i = 0; i < server->race.car_count; ++i) {
        const RaceCar *car = &server->race.cars[i];
        const char *status = car->finished ? "FINISH" :
            car->retired ? "RETIRED" : "RUNNING";

        queue_formatted(server, index,
                        "EV CAR id=%u team=%u vehicle=%u status=%s "
                        "progress=%lld lane=%u cell=%u points=%llu rank=%u\n",
                        car->id, car->team, car->vehicle, status,
                        (long long)car->progress, car->lane,
                        race_cell(&server->race, car->progress),
                        (unsigned long long)car->points,
                        car->final_rank);
    }
    if (race_is_done(&server->race))
        queue_formatted(server, index, "EV END status=complete rounds=%u\n",
                        server->race.rounds);
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    return 0;
}

static int create_listener(const char *host, const char *port)
{
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *address;
    int listener = -1;
    int result;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    result = getaddrinfo(host, port, &hints, &addresses);
    if (result != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(result));
        return -1;
    }
    for (address = addresses; address != NULL; address = address->ai_next) {
        int reuse = 1;

        listener = socket(address->ai_family, address->ai_socktype,
                          address->ai_protocol);
        if (listener < 0)
            continue;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        if (set_nonblocking(listener) == 0 &&
            bind(listener, address->ai_addr, address->ai_addrlen) == 0 &&
            listen(listener, 32) == 0)
            break;
        close(listener);
        listener = -1;
    }
    freeaddrinfo(addresses);
    if (listener < 0)
        perror("listen");
    return listener;
}

static void send_observer_snapshot(Server *server, unsigned index)
{
    Observer *observer = &server->observers[index];

    observer->hello_received = 1;
    queue_formatted(server, index, "WELCOME role=observer name=%s\n",
                    observer->name);
    send_snapshot(server, index);
    log_message("observer %s connected", observer->name);
}

static void process_observer_line(Server *server, unsigned index, char *line)
{
    Observer *observer = &server->observers[index];

    if (!observer->hello_received) {
        char *name;
        size_t length;

        if (strncmp(line, "HELLO ", 6) != 0) {
            queue_to_observer(server, index, "ERROR expected HELLO name\n");
            close_observer(server, index, "invalid handshake");
            return;
        }
        name = line + 6;
        while (*name == ' ')
            ++name;
        length = strlen(name);
        if (length == 0 || length >= sizeof(observer->name)) {
            queue_to_observer(server, index, "ERROR invalid observer name\n");
            close_observer(server, index, "invalid observer name");
            return;
        }
        for (size_t i = 0; i < length; ++i) {
            unsigned char character = (unsigned char)name[i];

            if (!isalnum(character) && character != '-' &&
                character != '_' && character != '.')
                name[i] = '_';
        }
        memcpy(observer->name, name, length + 1);
        send_observer_snapshot(server, index);
        return;
    }
    queue_to_observer(server, index, "EV ERROR read-only observer\n");
}

static void read_observer(Server *server, unsigned index)
{
    Observer *observer = &server->observers[index];
    char bytes[1024];

    while (observer->fd >= 0) {
        ssize_t count = recv(observer->fd, bytes, sizeof(bytes), 0);

        if (count == 0) {
            close_observer(server, index, "connection closed");
            return;
        }
        if (count < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            close_observer(server, index, strerror(errno));
            return;
        }
        for (ssize_t i = 0; i < count; ++i) {
            char character = bytes[i];

            if (character == '\n') {
                observer->input[observer->input_length] = '\0';
                if (observer->input_length > 0 &&
                    observer->input[observer->input_length - 1] == '\r')
                    observer->input[--observer->input_length] = '\0';
                process_observer_line(server, index, observer->input);
                observer->input_length = 0;
                if (observer->fd < 0)
                    return;
            } else if (observer->input_length + 1 <
                       sizeof(observer->input)) {
                observer->input[observer->input_length++] = character;
            } else {
                close_observer(server, index, "input line too long");
                return;
            }
        }
    }
}

static void accept_observers(Server *server)
{
    for (;;) {
        int fd = accept(server->listener, NULL, NULL);
        unsigned slot;

        if (fd < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            perror("accept");
            return;
        }
        for (slot = 0; slot < SERVER_MAX_CLIENTS; ++slot) {
            if (server->observers[slot].fd < 0)
                break;
        }
        if (slot == SERVER_MAX_CLIENTS) {
            static const char message[] = "ERROR observer limit reached\n";

            send(fd, message, sizeof(message) - 1, 0);
            close(fd);
            log_message("connection rejected: observer limit reached");
            continue;
        }
        if (set_nonblocking(fd) != 0) {
            close(fd);
            continue;
        }
        memset(&server->observers[slot], 0, sizeof(server->observers[slot]));
        server->observers[slot].fd = fd;
        log_message("TCP observer connection accepted");
    }
}

static int flush_observer(Server *server, unsigned index)
{
    Observer *observer = &server->observers[index];

    while (observer->fd >= 0 &&
           observer->output_start < observer->output_length) {
        ssize_t count = send(observer->fd,
                             observer->output + observer->output_start,
                             observer->output_length - observer->output_start,
                             0);

        if (count > 0) {
            observer->output_start += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;
        close_observer(server, index,
                       count == 0 ? "socket write returned zero" :
                       strerror(errno));
        return -1;
    }
    if (observer->fd >= 0) {
        observer->output_start = 0;
        observer->output_length = 0;
    }
    return 0;
}

static int any_pending_output(const Server *server)
{
    for (unsigned i = 0; i < SERVER_MAX_CLIENTS; ++i) {
        const Observer *observer = &server->observers[i];

        if (observer->fd >= 0 &&
            observer->output_start < observer->output_length)
            return 1;
    }
    return 0;
}

static void begin_shutdown(Server *server, const char *reason)
{
    if (server->stopping)
        return;
    server->stopping = 1;
    log_message("shutdown requested: %s", reason);
    broadcast_event(server, "END status=server-shutdown");
    if (server->listener >= 0) {
        close(server->listener);
        server->listener = -1;
    }
    server->stop_deadline_ms = monotonic_milliseconds() + 700;
    server->stop_deadline_set = 1;
}

static void process_command(Server *server, char *line)
{
    size_t length = strlen(line);

    while (length > 0 && isspace((unsigned char)line[length - 1]))
        line[--length] = '\0';
    while (*line != '\0' && isspace((unsigned char)*line))
        ++line;

    if (strcmp(line, "start") == 0) {
        if (server->race.state == RACE_WAITING) {
            race_start(&server->race);
            server->next_round_ms = monotonic_milliseconds();
        } else {
            log_message("start ignored: race is already active or complete");
        }
    } else if (strcmp(line, "status") == 0) {
        log_message("state=%s round=%u active-observers:",
                    server->race.state == RACE_WAITING ? "waiting" :
                    server->race.state == RACE_RUNNING ? "running" : "finished",
                    server->race.rounds);
        for (unsigned i = 0; i < SERVER_MAX_CLIENTS; ++i) {
            if (server->observers[i].fd >= 0 &&
                server->observers[i].hello_received)
                log_message("  %s", server->observers[i].name);
        }
    } else if (strcmp(line, "quit") == 0) {
        begin_shutdown(server, "operator command");
    } else if (line[0] != '\0') {
        log_message("unknown command; use start, status or quit");
    }
}

static void read_server_commands(Server *server, char *line, size_t *line_length)
{
    char bytes[256];
    ssize_t count = read(STDIN_FILENO, bytes, sizeof(bytes));

    if (count == 0) {
        server->input_open = 0;
        return;
    }
    if (count < 0) {
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("stdin");
            server->input_open = 0;
        }
        return;
    }
    for (ssize_t i = 0; i < count; ++i) {
        if (bytes[i] == '\n') {
            line[*line_length] = '\0';
            process_command(server, line);
            *line_length = 0;
        } else if (*line_length + 1 < 255) {
            line[(*line_length)++] = bytes[i];
        }
    }
}

static int server_loop(Server *server)
{
    char command_line[256];
    size_t command_length = 0;

    if (server->autostart) {
        race_start(&server->race);
        server->next_round_ms = monotonic_milliseconds();
    } else {
        log_message("ready; connect observers, then enter start");
    }
    while (1) {
        struct pollfd descriptors[SERVER_MAX_CLIENTS + 2];
        int descriptor_slots[SERVER_MAX_CLIENTS + 2];
        nfds_t count = 0;
        int timeout = -1;
        int poll_result;
        int64_t now = monotonic_milliseconds();

        if (caught_signal && !server->stopping)
            begin_shutdown(server, caught_signal == SIGINT ? "SIGINT" : "SIGTERM");
        if (server->stopping &&
            (!any_pending_output(server) ||
             (server->stop_deadline_set && now >= server->stop_deadline_ms)))
            break;

        if (server->listener >= 0 && !server->stopping) {
            descriptors[count].fd = server->listener;
            descriptors[count].events = POLLIN;
            descriptors[count].revents = 0;
            descriptor_slots[count++] = -1;
        }
        if (server->input_open && !server->stopping) {
            descriptors[count].fd = STDIN_FILENO;
            descriptors[count].events = POLLIN;
            descriptors[count].revents = 0;
            descriptor_slots[count++] = -2;
        }
        for (unsigned i = 0; i < SERVER_MAX_CLIENTS; ++i) {
            Observer *observer = &server->observers[i];

            if (observer->fd < 0)
                continue;
            descriptors[count].fd = observer->fd;
            descriptors[count].events = POLLIN;
            if (observer->output_start < observer->output_length)
                descriptors[count].events |= POLLOUT;
            descriptors[count].revents = 0;
            descriptor_slots[count++] = (int)i;
        }

        if (server->stopping) {
            timeout = 100;
        } else if (server->race.state == RACE_RUNNING) {
            int64_t remaining = server->next_round_ms - now;

            timeout = remaining <= 0 ? 0 :
                remaining > 1000 ? 1000 : (int)remaining;
        } else {
            timeout = -1;
        }
        poll_result = poll(descriptors, count, timeout);
        if (poll_result < 0 && errno != EINTR) {
            perror("poll");
            return -1;
        }
        if (poll_result > 0) {
            for (nfds_t i = 0; i < count; ++i) {
                int slot = descriptor_slots[i];

                if (descriptors[i].revents == 0)
                    continue;
                if (slot == -1) {
                    if (descriptors[i].revents & POLLIN)
                        accept_observers(server);
                } else if (slot == -2) {
                    if (descriptors[i].revents & (POLLIN | POLLHUP))
                        read_server_commands(server, command_line,
                                             &command_length);
                } else if (server->observers[slot].fd >= 0) {
                    if (descriptors[i].revents & POLLIN)
                        read_observer(server, (unsigned)slot);
                    if (server->observers[slot].fd >= 0 &&
                        descriptors[i].revents & POLLOUT)
                        flush_observer(server, (unsigned)slot);
                    if (server->observers[slot].fd >= 0 &&
                        descriptors[i].revents & (POLLERR | POLLHUP | POLLNVAL))
                        close_observer(server, (unsigned)slot, "poll reported closure");
                }
            }
        }
        now = monotonic_milliseconds();
        if (!server->stopping && server->race.state == RACE_RUNNING &&
            now >= server->next_round_ms) {
            race_run_round(&server->race);
            if (race_is_done(&server->race)) {
                broadcast_event(server, "END status=complete");
                if (server->once)
                    begin_shutdown(server, "race complete (--once)");
            } else {
                server->next_round_ms = now + server->delay_ms;
            }
        }
    }
    return 0;
}

static char *trim(char *text)
{
    char *end;

    while (isspace((unsigned char)*text))
        ++text;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return text;
}

static int parse_uint64(const char *text, uint64_t *value)
{
    char *end;
    unsigned long long parsed;

    while (isspace((unsigned char)*text))
        ++text;
    if (*text == '-' || *text == '\0')
        return -1;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *trim(end) != '\0')
        return -1;
    *value = (uint64_t)parsed;
    return 0;
}

static int set_config_value(RaceConfig *config, const char *key,
                            const char *value)
{
    uint64_t number;

    if (strcmp(key, "classification") == 0) {
        if (strcmp(value, "finish") == 0)
            config->points_classification = 0;
        else if (strcmp(value, "points") == 0)
            config->points_classification = 1;
        else
            return -1;
        return 0;
    }
    if (parse_uint64(value, &number) != 0)
        return -1;
    if (strcmp(key, "seed") == 0) {
        config->seed = number;
        return 0;
    }
    if (number > UINT32_MAX)
        return -1;
    if (strcmp(key, "lanes") == 0)
        config->lanes = (unsigned)number;
    else if (strcmp(key, "cells") == 0)
        config->cells = (unsigned)number;
    else if (strcmp(key, "laps") == 0)
        config->laps = (unsigned)number;
    else if (strcmp(key, "teams") == 0)
        config->teams = (unsigned)number;
    else if (strcmp(key, "bonus_points") == 0)
        config->bonus_points = (unsigned)number;
    else if (strcmp(key, "retire_gap") == 0)
        config->retire_gap = (unsigned)number;
    else if (strcmp(key, "max_rounds") == 0)
        config->max_rounds = (unsigned)number;
    else
        return -1;
    return 0;
}

static int load_config(const char *path, RaceConfig *config)
{
    FILE *file = fopen(path, "r");
    char line[256];
    unsigned line_number = 0;

    if (file == NULL) {
        perror(path);
        return -1;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        char *entry;
        char *equal;
        ++line_number;
        entry = trim(line);
        if (*entry == '\0' || *entry == '#')
            continue;
        equal = strchr(entry, '=');
        if (equal == NULL) {
            fprintf(stderr, "%s:%u: expected key=value\n", path, line_number);
            fclose(file);
            return -1;
        }
        *equal = '\0';
        if (set_config_value(config, trim(entry), trim(equal + 1)) != 0) {
            fprintf(stderr, "%s:%u: invalid configuration entry\n",
                    path, line_number);
            fclose(file);
            return -1;
        }
    }
    if (ferror(file)) {
        perror(path);
        fclose(file);
        return -1;
    }
    fclose(file);
    return 0;
}

static void print_usage(const char *program)
{
    printf("Usage: %s [--bind IPv4|IPv6|host] [--port PORT] [options]\n"
           "Options: --config FILE --seed N --delay-ms N --retire-gap N\n"
           "         --max-rounds N --autostart --once --help\n"
           "Commands while running: start, status, quit\n", program);
}

static int parse_option_number(const char *option, const char *text,
                               uint64_t *value)
{
    if (parse_uint64(text, value) != 0) {
        fprintf(stderr, "%s expects a non-negative integer\n", option);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    RaceConfig config;
    static Server server;
    const char *bind_host = "0.0.0.0";
    const char *port = "5000";
    const char *config_path = NULL;
    char error[256];

    race_config_defaults(&config);
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc)
            config_path = argv[i + 1];
    }
    if (config_path != NULL && load_config(config_path, &config) != 0)
        return 2;

    memset(&server, 0, sizeof(server));
    server.listener = -1;
    server.input_open = 1;
    server.delay_ms = 100;
    for (unsigned i = 0; i < SERVER_MAX_CLIENTS; ++i)
        server.observers[i].fd = -1;

    for (int i = 1; i < argc; ++i) {
        uint64_t number;

        if (strcmp(argv[i], "--config") == 0) {
            ++i;
        } else if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc) {
            bind_host = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = argv[++i];
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            if (parse_option_number("--seed", argv[++i], &config.seed) != 0)
                return 2;
        } else if (strcmp(argv[i], "--delay-ms") == 0 && i + 1 < argc) {
            if (parse_option_number("--delay-ms", argv[++i], &number) != 0 ||
                number > 60000) {
                fprintf(stderr, "--delay-ms must be between 0 and 60000\n");
                return 2;
            }
            server.delay_ms = (unsigned)number;
        } else if (strcmp(argv[i], "--retire-gap") == 0 && i + 1 < argc) {
            if (parse_option_number("--retire-gap", argv[++i], &number) != 0 ||
                number > UINT32_MAX)
                return 2;
            config.retire_gap = (unsigned)number;
        } else if (strcmp(argv[i], "--max-rounds") == 0 && i + 1 < argc) {
            if (parse_option_number("--max-rounds", argv[++i], &number) != 0 ||
                number > UINT32_MAX)
                return 2;
            config.max_rounds = (unsigned)number;
        } else if (strcmp(argv[i], "--autostart") == 0) {
            server.autostart = 1;
        } else if (strcmp(argv[i], "--once") == 0) {
            server.once = 1;
        } else {
            fprintf(stderr, "unknown or incomplete option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 2;
        }
    }
    if (race_config_validate(&config, error, sizeof(error)) != 0) {
        fprintf(stderr, "configuration error: %s\n", error);
        return 2;
    }
    if (race_init(&server.race, &config, broadcast_event, &server,
                  error, sizeof(error)) != 0) {
        fprintf(stderr, "race initialization failed: %s\n", error);
        return 2;
    }
    server.listener = create_listener(bind_host, port);
    if (server.listener < 0)
        return 1;
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    log_message("listening on %s:%s; TCP observers are read-only", bind_host, port);
    log_message("track lanes=%u cells=%u laps=%u teams=%u seed=%llu",
                config.lanes, config.cells, config.laps, config.teams,
                (unsigned long long)config.seed);
    log_message("commands: start, status, quit");

    if (server_loop(&server) != 0) {
        for (unsigned i = 0; i < SERVER_MAX_CLIENTS; ++i)
            close_observer(&server, i, "server failure");
        if (server.listener >= 0)
            close(server.listener);
        return 1;
    }
    for (unsigned i = 0; i < SERVER_MAX_CLIENTS; ++i)
        close_observer(&server, i, "server stopped");
    if (server.listener >= 0)
        close(server.listener);
    log_message("server stopped");
    return 0;
}
