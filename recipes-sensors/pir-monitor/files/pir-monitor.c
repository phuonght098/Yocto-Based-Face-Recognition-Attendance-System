#define _GNU_SOURCE

#include <gpiod.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define GPIO_CHIP_NAME       "gpiochip0"
#define PIR_GPIO_OFFSET      17
#define SOCKET_PATH          "/run/pir-monitor/events.sock"
#define MAX_CLIENTS          8
#define POLL_INTERVAL_MS     50
#define DEBOUNCE_MS          150
#define COOLDOWN_MS          5000

static volatile sig_atomic_t running = 1;

static void handle_signal(int signo)
{
    (void)signo;
    running = 0;
}

static long long monotonic_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;

    return (long long)ts.tv_sec * 1000LL +
           (long long)ts.tv_nsec / 1000000LL;
}

static void timestamp_utc(char *buffer, size_t size)
{
    struct timespec ts;
    struct tm tm_utc;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0 ||
        gmtime_r(&ts.tv_sec, &tm_utc) == NULL ||
        strftime(buffer, size, "%Y-%m-%dT%H:%M:%S", &tm_utc) == 0) {
        snprintf(buffer, size, "unknown-time");
        return;
    }

    size_t used = strlen(buffer);
    if (used < size)
        snprintf(buffer + used, size - used, "Z");
}

static void log_message(const char *message)
{
    char ts[40];

    timestamp_utc(ts, sizeof(ts));
    printf("[%s] %s\n", ts, message);
    fflush(stdout);
}

static int create_server_socket(void)
{
    int fd = -1;
    struct sockaddr_un address;

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        fprintf(stderr, "socket(): %s\n", strerror(errno));
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", SOCKET_PATH);

    (void)unlink(SOCKET_PATH);

    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        fprintf(stderr, "bind(%s): %s\n", SOCKET_PATH, strerror(errno));
        close(fd);
        return -1;
    }

    if (chmod(SOCKET_PATH, 0600) < 0) {
        fprintf(stderr, "chmod(%s): %s\n", SOCKET_PATH, strerror(errno));
        close(fd);
        (void)unlink(SOCKET_PATH);
        return -1;
    }

    if (listen(fd, MAX_CLIENTS) < 0) {
        fprintf(stderr, "listen(): %s\n", strerror(errno));
        close(fd);
        (void)unlink(SOCKET_PATH);
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    return fd;
}

static void remove_client(int clients[], int index)
{
    if (clients[index] >= 0)
        close(clients[index]);
    clients[index] = -1;
}

static void broadcast_event(int clients[], const char *json_line)
{
    size_t length = strlen(json_line);

    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i] < 0)
            continue;

        ssize_t sent = send(clients[i], json_line, length,
                            MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent < 0 || (size_t)sent != length)
            remove_client(clients, i);
    }

    printf("%s", json_line);
    fflush(stdout);
}

static void publish_event(int clients[], const char *event_name,
                          int gpio_value)
{
    char timestamp[40];
    char line[256];

    timestamp_utc(timestamp, sizeof(timestamp));
    snprintf(line, sizeof(line),
             "{\"event\":\"%s\",\"sensor\":\"HC-SR501\","
             "\"gpio\":%d,\"value\":%d,\"timestamp\":\"%s\"}\n",
             event_name, PIR_GPIO_OFFSET, gpio_value, timestamp);

    broadcast_event(clients, line);
}

static void accept_clients(int server_fd, int clients[])
{
    for (;;) {
        int client_fd = accept4(server_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);

        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;

            fprintf(stderr, "accept(): %s\n", strerror(errno));
            return;
        }

        int slot = -1;
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (clients[i] < 0) {
                slot = i;
                break;
            }
        }

        if (slot < 0) {
            close(client_fd);
            continue;
        }

        clients[slot] = client_fd;
        log_message("Event client connected");
    }
}

int main(void)
{
    struct gpiod_chip *chip = NULL;
    struct gpiod_line *line = NULL;
    int server_fd = -1;
    int clients[MAX_CLIENTS];

    int candidate_state;
    int stable_state;
    long long candidate_since;
    long long last_trigger = -1;
    bool armed;

    int exit_code = EXIT_FAILURE;

    for (int i = 0; i < MAX_CLIENTS; ++i)
        clients[i] = -1;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    chip = gpiod_chip_open_by_name(GPIO_CHIP_NAME);
    if (chip == NULL) {
        fprintf(stderr, "Cannot open %s: %s\n",
                GPIO_CHIP_NAME, strerror(errno));
        goto cleanup;
    }

    line = gpiod_chip_get_line(chip, PIR_GPIO_OFFSET);
    if (line == NULL) {
        fprintf(stderr, "Cannot get GPIO offset %d: %s\n",
                PIR_GPIO_OFFSET, strerror(errno));
        goto cleanup;
    }

    if (gpiod_line_request_input(line, "pir-monitor") < 0) {
        fprintf(stderr, "Cannot request GPIO17 as input: %s\n",
                strerror(errno));
        goto cleanup;
    }

    server_fd = create_server_socket();
    if (server_fd < 0)
        goto cleanup;

    stable_state = gpiod_line_get_value(line);
    if (stable_state < 0) {
        fprintf(stderr, "Cannot read GPIO17: %s\n", strerror(errno));
        goto cleanup;
    }

    candidate_state = stable_state;
    candidate_since = monotonic_ms();
    if (candidate_since < 0) {
        fprintf(stderr, "Cannot read monotonic clock\n");
        goto cleanup;
    }

    /*
     * Require a LOW state before arming. If the PIR is HIGH at boot,
     * this avoids treating the startup state as a fresh motion event.
     */
    armed = (stable_state == 0);

    log_message("PIR monitor started; GPIO17 input requested");
    log_message(stable_state ? "Initial state: HIGH" : "Initial state: LOW");
    if (!armed)
        log_message("Not armed until a stable LOW state is observed");

    while (running) {
        struct pollfd pfd;
        pfd.fd = server_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int poll_result = poll(&pfd, 1, POLL_INTERVAL_MS);
        if (poll_result > 0 && (pfd.revents & POLLIN))
            accept_clients(server_fd, clients);
        else if (poll_result < 0 && errno != EINTR)
            fprintf(stderr, "poll(): %s\n", strerror(errno));

        if (!running)
            break;

        int value = gpiod_line_get_value(line);
        if (value < 0) {
            fprintf(stderr, "GPIO17 read error: %s\n", strerror(errno));
            continue;
        }

        long long now = monotonic_ms();
        if (now < 0) {
            fprintf(stderr, "Cannot read monotonic clock\n");
            continue;
        }

        if (value != candidate_state) {
            candidate_state = value;
            candidate_since = now;
            continue;
        }

        if (candidate_state != stable_state &&
            now - candidate_since >= DEBOUNCE_MS) {
            stable_state = candidate_state;

            if (stable_state == 1) {
                if (armed &&
                    (last_trigger < 0 || now - last_trigger >= COOLDOWN_MS)) {
                    publish_event(clients, "MOTION_DETECTED", 1);
                    last_trigger = now;
                    armed = false;
                } else {
                    log_message("HIGH state ignored (not armed or cooldown)");
                }
            } else {
                publish_event(clients, "MOTION_CLEARED", 0);

                if (last_trigger < 0 ||
                    now - last_trigger >= COOLDOWN_MS) {
                    armed = true;
                    log_message("PIR armed");
                }
            }
        }

        if (stable_state == 0 && !armed && last_trigger >= 0 &&
            now - last_trigger >= COOLDOWN_MS) {
            armed = true;
            log_message("PIR armed after cooldown");
        }
    }

    exit_code = EXIT_SUCCESS;
    log_message("PIR monitor stopping");

cleanup:
    for (int i = 0; i < MAX_CLIENTS; ++i)
        remove_client(clients, i);

    if (server_fd >= 0)
        close(server_fd);

    (void)unlink(SOCKET_PATH);

    if (line != NULL)
        gpiod_line_release(line);

    if (chip != NULL)
        gpiod_chip_close(chip);

    return exit_code;
}
