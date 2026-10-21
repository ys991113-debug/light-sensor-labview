#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define I2C_DEVICE "/dev/i2c-1"
#define PCF8591_ADDRESS 0x48
#define SAMPLE_INTERVAL_MS 200

static volatile sig_atomic_t keep_running = 1;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    keep_running = 0;
}

static void sleep_ms(long milliseconds)
{
    struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000L,
    };

    while (nanosleep(&delay, &delay) == -1 && errno == EINTR) {
        /* Continue sleeping for the remaining time. */
    }
}

static int read_adc_channel(int fd, uint8_t channel, uint8_t *value)
{
    if (channel > 3 || value == NULL) {
        errno = EINVAL;
        return -1;
    }

    /* Single-ended input mode, analog output disabled, selected channel 0-3. */
    uint8_t control = channel & 0x03U;
    ssize_t written = write(fd, &control, sizeof(control));
    if (written != (ssize_t)sizeof(control)) {
        if (written >= 0) {
            errno = EIO;
        }
        return -1;
    }

    /* The first byte is the previous conversion; the second is current. */
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

static double elapsed_seconds(const struct timespec *start,
                              const struct timespec *now)
{
    time_t seconds = now->tv_sec - start->tv_sec;
    long nanoseconds = now->tv_nsec - start->tv_nsec;
    return (double)seconds + (double)nanoseconds / 1000000000.0;
}

int main(void)
{
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    int fd = open(I2C_DEVICE, O_RDWR);
    if (fd == -1) {
        fprintf(stderr, "I2C open failed (%s): %s\n",
                I2C_DEVICE, strerror(errno));
        return EXIT_FAILURE;
    }

    if (ioctl(fd, I2C_SLAVE, PCF8591_ADDRESS) == -1) {
        fprintf(stderr, "PCF8591 select failed (0x%02X): %s\n",
                PCF8591_ADDRESS, strerror(errno));
        close(fd);
        return EXIT_FAILURE;
    }

    struct timespec start;
    if (clock_gettime(CLOCK_MONOTONIC, &start) == -1) {
        fprintf(stderr, "Clock initialization failed: %s\n", strerror(errno));
        close(fd);
        return EXIT_FAILURE;
    }

    printf("PCF8591 connected: device=%s address=0x%02X\n",
           I2C_DEVICE, PCF8591_ADDRESS);
    printf("Cover and uncover the LDR. Press Ctrl+C to stop.\n");
    printf("time_s,AIN0,AIN1,AIN2,AIN3\n");

    while (keep_running) {
        uint8_t values[4];

        for (uint8_t channel = 0; channel < 4; ++channel) {
            if (read_adc_channel(fd, channel, &values[channel]) == -1) {
                fprintf(stderr, "AIN%u read failed: %s\n",
                        channel, strerror(errno));
                close(fd);
                return EXIT_FAILURE;
            }
        }

        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
            fprintf(stderr, "Clock read failed: %s\n", strerror(errno));
            close(fd);
            return EXIT_FAILURE;
        }

        printf("%.3f,%u,%u,%u,%u\n",
               elapsed_seconds(&start, &now),
               values[0], values[1], values[2], values[3]);
        fflush(stdout);
        sleep_ms(SAMPLE_INTERVAL_MS);
    }

    printf("Stopped.\n");
    close(fd);
    return EXIT_SUCCESS;
}
