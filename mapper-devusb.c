// vim: ts=4:sw=4:et:tw=80

/*
 * mapper-devusb.c
 *
 * Copyright 2019, 2020 Sébastien Millet
 *
 * Provides a named pipe to send data to Arduino, controlling en passant the
 * HUPCL flag of Arduino'a device file.
 * This HUPCL flag control is performed to avoid an Arduino reset each time a
 * process opens then closes the Arduino' device file.
 *
 * "Equivalent" (but with advantage of resiliency) of stty -hupcl.
 *
 * Also performs a keep-alive sending, so that an unexpected unplug/replug of
 * Arduino will trigger serial reset hopefully *before* a sending re-occurs.
*/

#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <termios.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>

#include "serial_speed.h"

/*
 * Should rather be set from Makefile
 * Left here, in case you'd need to bypass Makefile
*/
#define DEBUG

    // Send a noop instruction to Arduino every that many seconds
#define KEEP_ALIVE_NOOP 60
    // Can result in a lot of repetitive logs
#define LOG_NOOP

int debug_on = 0;

const char *fifo_name = "/tmp/arduino";
const char *dev_file_name = NULL;

const char *log_file_name = NULL;
FILE *flog = NULL;

int clear_hupcl(const int fd);

void output_datetime_of_day(FILE *f) {
    time_t ltime = time(NULL);
    struct tm ts;
    ts = *localtime(&ltime);

    struct timeval tv;
    struct timezone tz;
    if (gettimeofday(&tv, &tz)) {
        fprintf(f, "[gettimeofday(): error]  ");
        return;
    }

    fprintf(f, "%02i/%02i/%02i %02i:%02i:%02i.%06lu  ",
            ts.tm_mday, ts.tm_mon + 1, ts.tm_year % 100,
            ts.tm_hour, ts.tm_min, ts.tm_sec, tv.tv_usec);
}

void DBG(const char *fmt, ...)
     __attribute__((format(printf, 1, 2)));
#ifdef DEBUG
void DBG(const char *fmt, ...) {
    if (!debug_on)
        return;

    output_datetime_of_day(flog);
    fprintf(flog, "%s", "<D>  ");
    va_list args;
    va_start(args, fmt);
    vfprintf(flog, fmt, args);
    va_end(args);
    fprintf(flog, "\n");
}
#else
#define DBG(...)
#endif
void l(const char *fmt, ...)
     __attribute__((format(printf, 1, 2)));
void l(const char *fmt, ...) {
    output_datetime_of_day(flog);
    fprintf(flog, "%s", "<I>  ");
    va_list args;
    va_start(args, fmt);
    vfprintf(flog, fmt, args);
    va_end(args);
    fprintf(flog, "\n");
}

void usage() {
    printf("Usage:\n");
    printf("  mapper-devusb [OPTIONS] DEVICE_FILE\n");
    printf("Provides a named pipe to receive and forward\n");
    printf("everything to DEVICE_FILE.\n");
    printf("Default FIFO name is %s.\n", fifo_name);
    printf("In-between, control HUPCL flag of DEVICE_FILE, to prevent\n");
    printf("an Arduino reset at each write.\n");
    printf("\n");
    printf("  -h       Print this help screen\n");
    printf("  -D       Print out debug information\n");
    printf("  -d       Start as a daemon\n");
    printf("  -l FILE  Logs data into FILE\n");
    printf("  -f FIFO  FIFO to use\n");
    printf("\n");
    printf("Copyright 2019 Sébastien Millet\n");
}

void get_required_argument(int *idx, int argc, char *argv[],
                           const char *opt_name, const char **argument) {
    if (*idx < argc - 1) {
        *argument = argv[*idx + 1];
        ++*idx;
    } else {
        fprintf(stderr,
                "mapper-devusb: option requires an argument -- '%s'\n",
                opt_name);
        fprintf(stderr,
                "Try `mapper-devusb -h' for more information.\n");
        exit(1);
    }
}

void s_strncpy(char *dest, const char *src, size_t n) {
    strncpy(dest, src, n);
    if (n >= 1)
        dest[n - 1] = '\0';
}

void remove_trailing_newline(char *s) {
    size_t l = strlen(s);
    if (l >= 1 && s[l - 1] == '\n')
        s[--l] = '\0';
    if (l >= 1 && s[l - 1] == '\r')
        s[--l] = '\0';
}

int clear_hupcl(const int fd) {
    struct termios term;
    int r;

    if ((r = tcgetattr(fd, &term)) != 0)
        return r;
    else {
        term.c_cflag &= ~HUPCL;
        cfsetospeed(&term, SERIAL_SPEED_SPEED_T);
        if ((r = tcsetattr(fd, TCSANOW, &term)) != 0)
            return r;
    }
    return 0;
}

void write_buf(char *buf, size_t len) {
    int out_fd;
    if ((out_fd = open(dev_file_name, O_WRONLY)) == -1) {
        l("error: cannot open device file: %d (%s)",
            errno, strerror(errno));
        return;
    }

    do {
        if (clear_hupcl(out_fd)) {
            l("error: cannot clear HUPCL of "
                "device file: %d (%s)", errno, strerror(errno));
            break;
        }

        if (!len)
            break;

        if (write(out_fd, buf, len) == -1) {
            l("error: write to device file: %d (%s)",
                errno, strerror(errno));
            break;
        }
    } while (0);

    close(out_fd);
}

    // From
    //   https://stackoverflow.com/questions/17954432/creating-a-daemon-in-linux
static void skeleton_daemon() {
    pid_t pid;

    /* Fork off the parent process */
    pid = fork();

    /* An error occurred */
    if (pid < 0)
        exit(EXIT_FAILURE);

    /* Success: Let the parent terminate */
    if (pid > 0)
        exit(EXIT_SUCCESS);

    /* On success: The child process becomes session leader */
    if (setsid() < 0)
        exit(EXIT_FAILURE);

    /* Catch, ignore and handle signals */
    //TODO: Implement a working signal handler */
    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    /* Fork off for the second time*/
    pid = fork();

    /* An error occurred */
    if (pid < 0)
        exit(EXIT_FAILURE);

    /* Success: Let the parent terminate */
    if (pid > 0)
        exit(EXIT_SUCCESS);

    /* Set new file permissions */
    umask(0);

    /* Change the working directory to the root directory */
    /* or another appropriated directory */
    if (chdir("/"))
        exit(EXIT_FAILURE);

    /* Close all open file descriptors */
    close(0);
    close(1);
    close(2);
}

void close_log() {
    if (log_file_name && flog) {
        fclose(flog);
        flog = NULL;
    }
}

void exit_handler() {
    l("termination");
    close_log();
}

int main(int argc, char *argv[]) {

    int run_as_a_daemon = 0;

    flog = stderr;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            exit(0);
        } else if (!strcmp(argv[i], "-D")) {
#ifndef DEBUG
            fprintf(stderr, "mapper-devusb: error: compiled without debug "
                    "support, cannot honor -D option\n");
            exit(EXIT_FAILURE);
#endif
            debug_on = 1;
        } else if (!strcmp(argv[i], "-d")) {
            run_as_a_daemon = 1;
        } else if (!strcmp(argv[i], "-l")) {
            get_required_argument(&i, argc, argv, "l", &log_file_name);
        } else if (!strcmp(argv[i], "-f")) {
            get_required_argument(&i, argc, argv, "f", &fifo_name);
        } else {
            dev_file_name = argv[i];
            if (i != argc - 1) {
                fprintf(stderr,
                        "mapper-devusb: trailing options\n");
                fprintf(stderr,
                        "Try `mapper-devusb -h' for more information.\n");
                exit(1);
            }
        }
    }
    if (dev_file_name == NULL) {
        fprintf(stderr, "mapper-devusb: missing option\n");
        fprintf(stderr, "Try `mapper-devusb -h' for more information.\n");
        exit(1);
    }

    if (log_file_name != NULL) {
        flog = fopen(log_file_name, "a");
        if (flog == NULL) {
            fprintf(stderr, "mapper-devusb: error: cannot open log file: "
                            "%d (%s)\n", errno, strerror(errno));
            exit(3);
        }
        setvbuf(flog, NULL, _IONBF, 0);
    }

    atexit(exit_handler);

    l("start");
    DBG("device file:    [%s]", dev_file_name);
    DBG("fifo file name: [%s]", fifo_name);
    if (log_file_name != NULL) {
        DBG("log file name:  [%s]", log_file_name);
    } else {
        DBG("log file name:  <stderr>");
    }
    DBG("daemon mode:    [%s]", run_as_a_daemon ? "yes" : "no");

    if (access(fifo_name, R_OK ) != -1) {
        l("server fifo '%s' already exists", fifo_name);
    } else if (mkfifo(fifo_name, 0600) == -1) {
        l("warning: unable to create server fifo '%s'",
          fifo_name);
    } else {
        l("created server fifo '%s'", fifo_name);
    }

    int fifo_fd;
    if ((fifo_fd = open(fifo_name, O_RDWR)) < 0) {
        fprintf(stderr, "mapper-devusb: error: unable to open server FIFO "
                        "'%s'", fifo_name);
        exit(2);
    }

    if (run_as_a_daemon)
        skeleton_daemon();

    int loop = 1;
    while (loop) {
        fd_set rfds;
        struct timeval tv;
        int retval;

        FD_ZERO(&rfds);
        FD_SET(fifo_fd, &rfds);
        tv.tv_sec = KEEP_ALIVE_NOOP;
        tv.tv_usec = 0;

        retval = select(fifo_fd + 1, &rfds, NULL, NULL, &tv);
        if (retval == -1) {
            l("error: select: %d (%s)",
                errno, strerror(errno));
            continue;
        } else if (retval == 0) {
#ifdef LOG_NOOP
            l("sending noop()");
#endif
            write_buf("noop\n", 5);
            continue;
        }

        char buf[BUFSIZ];
        char bufcopy[BUFSIZ];

        ssize_t len;
        if ((len = read(fifo_fd, buf, sizeof(buf) - 1)) > 0) {
            buf[len] = '\0';

            s_strncpy(bufcopy, buf, len);
            remove_trailing_newline(bufcopy);
            l("received: [%s]", bufcopy);

            if (!strncmp(buf, "EOF()", 5)) {
                l("quitting");
                loop = 0;
            } else {
                write_buf(buf, len);
            }
        }
    }

    close(fifo_fd);
/*    unlink(fifo_name);*/

    return 0;
}

