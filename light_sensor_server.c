#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/i2c-dev.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define I2C_DEVICE "/dev/i2c-1"
#define PCF8591_ADDRESS 0x48
#define DEFAULT_CHANNEL 0
#define DEFAULT_PORT 5000
#define SAMPLE_INTERVAL_MS 100
#define RECEIVE_BUFFER_SIZE 4096
#define JSON_VALUE_SIZE 32

static volatile sig_atomic_t keep_running = 1;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    keep_running = 0;
}

static int install_signal_handlers(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);

    if (sigaction(SIGINT, &action, NULL) == -1 ||
        sigaction(SIGTERM, &action, NULL) == -1) {
        return -1;
    }

    struct sigaction ignore_pipe;
    memset(&ignore_pipe, 0, sizeof(ignore_pipe));
    ignore_pipe.sa_handler = SIG_IGN;
    sigemptyset(&ignore_pipe.sa_mask);
    return sigaction(SIGPIPE, &ignore_pipe, NULL);
}

static int open_adc(void)
{
    int fd = open(I2C_DEVICE, O_RDWR);
    if (fd == -1) {
        return -1;
    }

    if (ioctl(fd, I2C_SLAVE, PCF8591_ADDRESS) == -1) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }

    return fd;
}

static int read_adc_channel(int fd, uint8_t channel, uint8_t *value)
{
    if (channel > 3 || value == NULL) {
        errno = EINVAL;
        return -1;
    }

    uint8_t control = channel & 0x03U;
    ssize_t written = write(fd, &control, sizeof(control));
    if (written != (ssize_t)sizeof(control)) {
        if (written >= 0) {
            errno = EIO;
        }
        return -1;
    }

    /* PCF8591 returns the previous conversion first, so discard it. */
    uint8_t samples[2];
    ssize_t received = read(fd, samples, sizeof(samples));
    if (received != (ssize_t)sizeof(samples)) {
        if (received >= 0) {
            errno = EIO;
        }
        return -1;
    }

    *value = samples[1];
    return 0;
}

static int create_server_socket(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        return -1;
    }

    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                   &enabled, sizeof(enabled)) == -1) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) == -1 ||
        listen(fd, 4) == -1) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }

    return fd;
}

static int send_all(int fd, const char *message)
{
    const char *cursor = message;
    size_t remaining = strlen(message);

    while (remaining > 0) {
        ssize_t sent = send(fd, cursor, remaining, 0);
        if (sent > 0) {
            cursor += sent;
            remaining -= (size_t)sent;
            continue;
        }
        if (sent == -1 && errno == EINTR) {
            continue;
        }
        return -1;
    }

    return 0;
}

static const char *skip_spaces(const char *cursor)
{
    while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
        ++cursor;
    }
    return cursor;
}

static int parse_json_string(const char **input, char *output,
                             size_t output_size)
{
    const char *cursor = *input;
    if (*cursor != '"' || output_size == 0) {
        return -1;
    }

    ++cursor;
    size_t used = 0;
    while (*cursor != '\0' && *cursor != '"') {
        /* Escaped strings are unnecessary for the small command vocabulary. */
        if (*cursor == '\\' || (unsigned char)*cursor < 0x20U ||
            used + 1 >= output_size) {
            return -1;
        }
        output[used++] = *cursor++;
    }

    if (*cursor != '"') {
        return -1;
    }
    output[used] = '\0';
    *input = cursor + 1;
    return 0;
}

static int parse_json_uint32(const char **input, uint32_t *value)
{
    const char *cursor = *input;
    if (!isdigit((unsigned char)*cursor)) {
        return -1;
    }
    if (*cursor == '0' && isdigit((unsigned char)cursor[1])) {
        return -1;
    }

    errno = 0;
    char *end = NULL;
    unsigned long parsed = strtoul(cursor, &end, 10);
    if (errno != 0 || end == cursor || parsed > UINT32_MAX) {
        return -1;
    }

    *value = (uint32_t)parsed;
    *input = end;
    return 0;
}

struct parsed_command {
    char type[JSON_VALUE_SIZE];
    char command[JSON_VALUE_SIZE];
    uint32_t request_id;
};

static int parse_command_json(const char *line, struct parsed_command *message)
{
    const char *cursor = skip_spaces(line);
    if (*cursor != '{') {
        return -1;
    }
    ++cursor;

    bool has_type = false;
    bool has_request_id = false;
    bool has_command = false;

    for (;;) {
        cursor = skip_spaces(cursor);
        if (*cursor == '}' || *cursor == '\0') {
            return -1;
        }

        char key[JSON_VALUE_SIZE];
        if (parse_json_string(&cursor, key, sizeof(key)) == -1) {
            return -1;
        }

        cursor = skip_spaces(cursor);
        if (*cursor != ':') {
            return -1;
        }
        cursor = skip_spaces(cursor + 1);

        if (strcmp(key, "type") == 0) {
            if (has_type ||
                parse_json_string(&cursor, message->type,
                                  sizeof(message->type)) == -1) {
                return -1;
            }
            has_type = true;
        } else if (strcmp(key, "request_id") == 0) {
            if (has_request_id ||
                parse_json_uint32(&cursor, &message->request_id) == -1) {
                return -1;
            }
            has_request_id = true;
        } else if (strcmp(key, "cmd") == 0) {
            if (has_command ||
                parse_json_string(&cursor, message->command,
                                  sizeof(message->command)) == -1) {
                return -1;
            }
            has_command = true;
        } else {
            return -1;
        }

        cursor = skip_spaces(cursor);
        if (*cursor == ',') {
            ++cursor;
            continue;
        }
        if (*cursor != '}') {
            return -1;
        }

        cursor = skip_spaces(cursor + 1);
        if (*cursor != '\0') {
            return -1;
        }
        break;
    }

    return has_type && has_request_id && has_command ? 0 : -1;
}

static int send_response(int client_fd, uint32_t request_id,
                         const char *result, const char *state,
                         const char *error)
{
    char message[256];
    int length;
    if (error == NULL) {
        length = snprintf(message, sizeof(message),
                          "{\"type\":\"response\",\"request_id\":%u,"
                          "\"result\":\"%s\",\"state\":\"%s\"}\n",
                          request_id, result, state);
    } else {
        length = snprintf(message, sizeof(message),
                          "{\"type\":\"response\",\"request_id\":%u,"
                          "\"result\":\"%s\",\"state\":\"%s\","
                          "\"error\":\"%s\"}\n",
                          request_id, result, state, error);
    }

    if (length < 0 || (size_t)length >= sizeof(message)) {
        errno = EOVERFLOW;
        return -1;
    }
    return send_all(client_fd, message);
}

static int send_protocol_error(int client_fd, const char *error)
{
    char message[192];
    int length = snprintf(message, sizeof(message),
                          "{\"type\":\"error\",\"result\":\"error\","
                          "\"error\":\"%s\"}\n", error);
    if (length < 0 || (size_t)length >= sizeof(message)) {
        errno = EOVERFLOW;
        return -1;
    }
    return send_all(client_fd, message);
}

static int handle_command(int client_fd, const char *line,
                          bool *collecting, uint32_t *sequence,
                          struct timespec *next_sample)
{
    struct parsed_command message;
    if (parse_command_json(line, &message) == -1) {
        return send_protocol_error(client_fd, "invalid_message");
    }

    if (strcmp(message.type, "command") != 0) {
        return send_response(client_fd, message.request_id, "error",
                             *collecting ? "running" : "stopped",
                             "invalid_type");
    }

    if (strcmp(message.command, "start") == 0) {
        if (!*collecting) {
            *collecting = true;
            *sequence = 0;
            if (clock_gettime(CLOCK_MONOTONIC, next_sample) == -1) {
                return -1;
            }
        }
        return send_response(client_fd, message.request_id,
                             "ok", "running", NULL);
    }

    if (strcmp(message.command, "stop") == 0) {
        *collecting = false;
        return send_response(client_fd, message.request_id,
                             "ok", "stopped", NULL);
    }

    if (strcmp(message.command, "status") == 0) {
        return send_response(client_fd, message.request_id, "ok",
                             *collecting ? "running" : "stopped", NULL);
    }

    return send_response(client_fd, message.request_id, "error",
                         *collecting ? "running" : "stopped",
                         "unsupported_command");
}

static int64_t milliseconds_until(const struct timespec *deadline,
                                  const struct timespec *now)
{
    int64_t seconds = (int64_t)deadline->tv_sec - (int64_t)now->tv_sec;
    int64_t nanoseconds = (int64_t)deadline->tv_nsec - (int64_t)now->tv_nsec;
    int64_t milliseconds = seconds * 1000 + nanoseconds / 1000000;
    if (nanoseconds > 0 && nanoseconds % 1000000 != 0) {
        ++milliseconds;
    }
    return milliseconds;
}

static void add_milliseconds(struct timespec *value, long milliseconds)
{
    value->tv_sec += milliseconds / 1000;
    value->tv_nsec += (milliseconds % 1000) * 1000000L;
    if (value->tv_nsec >= 1000000000L) {
        ++value->tv_sec;
        value->tv_nsec -= 1000000000L;
    }
}

static int send_sample(int client_fd, int adc_fd, uint8_t channel,
                       uint32_t *sequence)
{
    uint8_t value;
    if (read_adc_channel(adc_fd, channel, &value) == -1) {
        if (send_protocol_error(client_fd, "sensor_read_failed") == -1) {
            return -1;
        }
        return 1;
    }

    char message[160];
    int length = snprintf(message, sizeof(message),
                          "{\"type\":\"sample\",\"seq\":%u,"
                          "\"adc_raw\":%u,\"status\":\"ok\"}\n",
                          *sequence, value);
    if (length < 0 || (size_t)length >= sizeof(message)) {
        errno = EOVERFLOW;
        return -1;
    }

    ++*sequence;
    return send_all(client_fd, message);
}

static int parse_long(const char *text, long minimum, long maximum, long *value)
{
    errno = 0;
    char *end = NULL;
    long parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        parsed < minimum || parsed > maximum) {
        return -1;
    }
    *value = parsed;
    return 0;
}

int main(int argc, char **argv)
{
    long channel = DEFAULT_CHANNEL;
    long port = DEFAULT_PORT;

    if (argc > 3 ||
        (argc >= 2 && parse_long(argv[1], 0, 3, &channel) == -1) ||
        (argc == 3 && parse_long(argv[2], 1024, 65535, &port) == -1)) {
        fprintf(stderr, "Usage: %s [channel:0-3] [port:1024-65535]\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (install_signal_handlers() == -1) {
        fprintf(stderr, "Signal handler setup failed: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    int adc_fd = open_adc();
    if (adc_fd == -1) {
        fprintf(stderr, "PCF8591 open failed (%s, 0x%02X): %s\n",
                I2C_DEVICE, PCF8591_ADDRESS, strerror(errno));
        return EXIT_FAILURE;
    }

    int server_fd = create_server_socket((uint16_t)port);
    if (server_fd == -1) {
        fprintf(stderr, "TCP server setup failed on port %ld: %s\n",
                port, strerror(errno));
        close(adc_fd);
        return EXIT_FAILURE;
    }

    printf("Light sensor TCP server ready: port=%ld channel=AIN%ld\n",
           port, channel);
    printf("Waiting for one PC client. Press Ctrl+C to stop.\n");

    int client_fd = -1;
    bool collecting = false;
    uint32_t sequence = 0;
    struct timespec next_sample = {0, 0};
    char receive_buffer[RECEIVE_BUFFER_SIZE];
    size_t receive_used = 0;

    int exit_status = EXIT_SUCCESS;
    while (keep_running) {
        struct pollfd descriptors[2];
        nfds_t count = 1;
        descriptors[0].fd = server_fd;
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;

        if (client_fd != -1) {
            descriptors[1].fd = client_fd;
            descriptors[1].events = POLLIN;
            descriptors[1].revents = 0;
            count = 2;
        }

        int timeout = -1;
        if (client_fd != -1 && collecting) {
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
                fprintf(stderr, "Clock read failed: %s\n", strerror(errno));
                exit_status = EXIT_FAILURE;
                break;
            }
            int64_t remaining = milliseconds_until(&next_sample, &now);
            timeout = remaining <= 0 ? 0 :
                      (remaining > INT_MAX ? INT_MAX : (int)remaining);
        }

        int ready = poll(descriptors, count, timeout);
        if (ready == -1) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "poll failed: %s\n", strerror(errno));
            exit_status = EXIT_FAILURE;
            break;
        }

        if ((descriptors[0].revents & POLLIN) != 0) {
            struct sockaddr_in peer;
            socklen_t peer_length = sizeof(peer);
            int accepted = accept(server_fd, (struct sockaddr *)&peer, &peer_length);
            if (accepted == -1) {
                if (errno != EINTR) {
                    fprintf(stderr, "accept failed: %s\n", strerror(errno));
                }
            } else if (client_fd != -1) {
                (void)send_protocol_error(accepted, "server_busy");
                close(accepted);
            } else {
                client_fd = accepted;
                collecting = false;
                receive_used = 0;
                printf("Client connected: %s:%u\n",
                       inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
            }
        }

        if (client_fd != -1 && count == 2 &&
            (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0) {
            if ((descriptors[1].revents & POLLIN) != 0) {
                ssize_t received = recv(client_fd,
                                        receive_buffer + receive_used,
                                        sizeof(receive_buffer) - receive_used, 0);
                if (received > 0) {
                    receive_used += (size_t)received;

                    size_t consumed = 0;
                    bool command_io_failed = false;
                    while (consumed < receive_used) {
                        char *newline = memchr(receive_buffer + consumed, '\n',
                                               receive_used - consumed);
                        if (newline == NULL) {
                            break;
                        }

                        size_t line_length = (size_t)(newline -
                                                     (receive_buffer + consumed));
                        if (line_length > 0 &&
                            receive_buffer[consumed + line_length - 1] == '\r') {
                            --line_length;
                        }

                        char line[RECEIVE_BUFFER_SIZE];
                        memcpy(line, receive_buffer + consumed, line_length);
                        line[line_length] = '\0';
                        consumed = (size_t)(newline - receive_buffer) + 1;

                        if (line_length == 0) {
                            if (send_protocol_error(client_fd, "empty_message") == -1) {
                                command_io_failed = true;
                                break;
                            }
                        } else if (handle_command(client_fd, line, &collecting,
                                                  &sequence, &next_sample) == -1) {
                            command_io_failed = true;
                            break;
                        }
                    }

                    if (consumed > 0) {
                        memmove(receive_buffer, receive_buffer + consumed,
                                receive_used - consumed);
                        receive_used -= consumed;
                    }

                    if (command_io_failed) {
                        close(client_fd);
                        client_fd = -1;
                        collecting = false;
                        receive_used = 0;
                        printf("Client disconnected during response send\n");
                    }

                    if (client_fd != -1 &&
                        receive_used == sizeof(receive_buffer)) {
                        (void)send_protocol_error(client_fd, "message_too_long");
                        close(client_fd);
                        client_fd = -1;
                        collecting = false;
                        receive_used = 0;
                        printf("Client disconnected: message too long\n");
                    }
                } else if (received == 0 || errno != EINTR) {
                    close(client_fd);
                    client_fd = -1;
                    collecting = false;
                    receive_used = 0;
                    printf("Client disconnected\n");
                }
            } else {
                close(client_fd);
                client_fd = -1;
                collecting = false;
                receive_used = 0;
                printf("Client disconnected\n");
            }
        }

        if (client_fd != -1 && collecting) {
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
                fprintf(stderr, "Clock read failed: %s\n", strerror(errno));
                exit_status = EXIT_FAILURE;
                break;
            }

            if (milliseconds_until(&next_sample, &now) <= 0) {
                int sample_result = send_sample(client_fd, adc_fd,
                                                (uint8_t)channel, &sequence);
                if (sample_result == -1) {
                    close(client_fd);
                    client_fd = -1;
                    collecting = false;
                    receive_used = 0;
                    printf("Client disconnected during sample send\n");
                } else if (sample_result == 1) {
                    collecting = false;
                    printf("Measurement stopped: PCF8591 read failed\n");
                } else {
                    do {
                        add_milliseconds(&next_sample, SAMPLE_INTERVAL_MS);
                    } while (milliseconds_until(&next_sample, &now) <= 0);
                }
            }
        }
    }

    if (client_fd != -1) {
        close(client_fd);
    }
    close(server_fd);
    close(adc_fd);
    printf("Server stopped.\n");
    return exit_status;
}
