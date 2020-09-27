// vim: ts=4:sw=4:et:tw=80

/*
 * mapper-devusb.c
 *
 * Copyright 2019, 2020 Sébastien Millet
 *
*/

/*
  This file is part of mapper-devusb.

  mapper-devusb is free software: you can redistribute it and/or modify it under
  the terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version.

  mapper-devusb is distributed in the hope that it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
  FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  mapper-devusb. If not, see <https://www.gnu.org/licenses/>.
*/

/*
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
#include <linux/limits.h>

#ifdef HAVE_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

#include "serial_speed.h"

/*
 * Should rather be set from Makefile
 * Left here, in case you'd need to bypass Makefile
*/
/*#define DEBUG*/

#define VERSION "1.1"

#define DEFAULT_CONFFILE "mapper-devusb.conf"
#define DEFAULT_ABSOLUTE_CONFFILE SYSCONFDIR "/" DEFAULT_CONFFILE

#define DEFAULT_FIFO_FILE_NAME "/tmp/arduino"

    // Send a noop instruction to Arduino every that many seconds
#define KEEP_ALIVE_WHILE_SUCCESS 60
#define KEEP_ALIVE_WHILE_FAILURE 5
#define LOG_KEEPALIVE_NEVER  0
#define LOG_KEEPALIVE_ERROR  1
#define LOG_KEEPALIVE_ALWAYS 2
int log_keepalive = LOG_KEEPALIVE_ERROR;
    // Keepalive instruction
const char *KEEPALIVE_CMD = "noop\n";

int debug_on = 0;
int run_as_a_daemon = 0;
int fifo_fd = -1;

    // PATH_MAX + 1 to avoid warnings while compiling 'fortified'.
    // Warning comes from strncpy called by s_strncpy. It is:
    //   /usr/include/bits/string_fortified.h:106:10: warning:
    //   '__builtin_strncpy' output may be truncated copying 4095 bytes from a
    //   string of length 4095 [-Wstringop-truncation]
#define MY_PATH_MAX (PATH_MAX + 1)

    // Typically: DEFAULT_ABSOLUTE_CONFFILE, that is, by default:
    //   /usr/local/etc/mapper-devusb.conf
char abs_cfgfile[MY_PATH_MAX];
    // Typically: /tmp/arduino or /var/arduino
char fifo_file_name[MY_PATH_MAX];
    // Typically: /dev/ttyUSB0 or /dev/ttyACM0
char dev_file_name[MY_PATH_MAX];
    // Typically: /var/log/mapper-devusb/activity.log
char log_file_name[MY_PATH_MAX];

FILE *flog = NULL;

int clear_hupcl(const int fd);

void output_datetime_of_day(FILE *f) {
    if (!f)
        return;

    time_t ltime = time(NULL);
    struct tm ts;
    ts = *localtime(&ltime);

    struct timeval tv;
    struct timezone tz;
    if (gettimeofday(&tv, &tz)) {
        fprintf(f, "[gettimeofday(): error]  ");
        return;
    }

    fprintf(f, "%02i/%02i/%02i %02i:%02i:%02i.%06lu ",
            ts.tm_mday, ts.tm_mon + 1, ts.tm_year % 100,
            ts.tm_hour, ts.tm_min, ts.tm_sec, tv.tv_usec);
}

void DBG(const char *fmt, ...)
     __attribute__((format(printf, 1, 2)));
#ifdef DEBUG
void DBG(const char *fmt, ...) {
    if (!debug_on)
        return;

    if (!flog)
        return;

    output_datetime_of_day(flog);
    fprintf(flog, "%s", "[D] ");
    va_list args;
    va_start(args, fmt);
    vfprintf(flog, fmt, args);
    va_end(args);
    fprintf(flog, "\n");
    fflush(flog);
}
#else
#define DBG(...)
#endif
void l(const char *fmt, ...)
     __attribute__((format(printf, 1, 2)));
void l(const char *fmt, ...) {
    if (!flog)
        return;

    output_datetime_of_day(flog);
    fprintf(flog, "%s", "    ");
    va_list args;
    va_start(args, fmt);
    vfprintf(flog, fmt, args);
    va_end(args);
    fprintf(flog, "\n");
    fflush(flog);
}

void usage() {
    printf("Usage:\n\
  mapper-devusb [OPTIONS] [DEVICE_FILE]\n\
Provides a named pipe to receive and forward\n\
everything to DEVICE_FILE.\n\
In-between, control HUPCL flag of DEVICE_FILE, to prevent\n\
an Arduino reset at each write.\n\
\n\
to set options. See file installed by default.\n\
\n\
  -h       Print this help screen\n\
  -v       Print version information and quit\n\
  -c FILE  Read FILE for the configuration, default:\n\
           " DEFAULT_ABSOLUTE_CONFFILE "\n\
  -d       Start as a daemon\n\
             *IMPORTANT*\n\
           This option implies old unix-style daemon execution (double\n\
           fork()). It is not compatible with systemd service management.\n\
  -l FILE  Logs data into FILE\n\
  -f FIFO  FIFO to use\n\
  -D       Print out debug information\n\
\n\
Copyright 2019, 2020 Sébastien Millet\n");
}

void version() {
    printf("mapper-devusb version " VERSION "\n");
}

void s_strncpy(char *dest, const char *src, size_t n) {
    if (n >= 1) {
        strncpy(dest, src, n - 1);
        dest[n - 1] = '\0';
    }
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

// Sends bytes to the device.
// Returns 0 if success, -1 if failure.
int write_buf(const char *buf, size_t len, int stay_silent_if_error) {
    int out_fd;
    if ((out_fd = open(dev_file_name, O_WRONLY)) == -1) {
        if (!stay_silent_if_error) {
            l("error: cannot open '%s': %s", dev_file_name, strerror(errno));
        }
        return -1;
    }

    int retval = 0;
    do {
        if (clear_hupcl(out_fd)) {
            if (!stay_silent_if_error) {
                l("error: cannot clear HUPCL of "
                  "'%s': %s", dev_file_name, strerror(errno));
            }
            retval = -1;
            break;
        }

        if (!len) {
            retval = -1;
            break;
        }

        if (write(out_fd, buf, len) == -1) {
            if (!stay_silent_if_error) {
                l("error: write to device file: %s", strerror(errno));
            }
            retval = -1;
            break;
        }
    } while (0);

    close(out_fd);

    return retval;
}

    // From
    //   https://stackoverflow.com/questions/17954432/creating-a-daemon-in-linux
static void skeleton_daemon() {
    pid_t pid;

    /* Fork off the parent process */
    pid = fork();

    /* An error occurred */
    if (pid < 0) {
        l("fork() returned a negative value: error, pid=%u", getpid());
        exit(EXIT_FAILURE);
    }

    /* Success: Let the parent terminate */
    if (pid > 0)
        exit(EXIT_SUCCESS);

    /* On success: The child process becomes session leader */
    if (setsid() < 0) {
        l("setsid() returned a negative value: error, pid=%u", getpid());
        exit(EXIT_FAILURE);
    }

    /* Catch, ignore and handle signals */
    //TODO: Implement a working signal handler */
    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    /* Fork off for the second time*/
    pid = fork();

    /* An error occurred */
    if (pid < 0) {
        l("second fork() returned a negative value: error, pid=%u", getpid());
        exit(EXIT_FAILURE);
    }

    /* Success: Let the parent terminate */
    if (pid > 0)
        exit(EXIT_SUCCESS);

    /* Set new file permissions */
    umask(0);

    /* Change the working directory to the root directory */
    /* or another appropriated directory */
    if (chdir("/")) {
        l("chdir() returned a negative value: error, pid=%u", getpid());
        exit(EXIT_FAILURE);
    }

    /* Close all open file descriptors */
    close(0);
    close(1);
    close(2);
}

void close_log() {
    if (strlen(log_file_name) != 0 && flog) {
        fclose(flog);
        flog = NULL;
    }
}

void exit_handler() {
    l("termination");
    close_log();
}

void get_required_argument(int *idx, int argc, char *argv[],
        const char *opt_name, char *arg, const size_t arg_len) {
    if (*idx < argc - 1) {
        s_strncpy(arg, argv[*idx + 1], arg_len);
        ++*idx;
    } else {
        fprintf(stderr, "Option requires an argument -- '%s'\n", opt_name);
        fprintf(stderr, "Try `mapper-devusb -h' for more information.\n");
        exit(1);
    }
}

int str_to_boolean(const char *s) {
    if (!strcmp(s, "0") || !strcmp(s, "no") || !strcmp(s, "n")
            || !strcmp(s, ""))
        return 0;
    return 1;
}

char *trim(char *s) {
    int p = strlen(s) - 1;
    while (p >= 0 && (s[p] == ' ' || s[p] == '\t')) {
        s[p] = '\0';
        --p;
    }
    while (*s == ' ' || *s == '\t')
        ++s;
    return s;
}

void read_cfg_from_cmdline_opts_round1(int argc, char *argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            exit(0);
        } else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
            version();
            exit(0);
        } else if (!strcmp(argv[i], "-c")) {
            get_required_argument(&i, argc, argv, "l",
                abs_cfgfile, sizeof(abs_cfgfile));
        }
    }
}

void read_cfg_from_config_file() {
    FILE *config;
    if ((config = fopen(abs_cfgfile, "r")) == NULL) {
        fprintf(stderr, "%s: error: unable to open for reading\n", abs_cfgfile);
        exit(EXIT_FAILURE);
    } else {
        ssize_t nb;
        size_t z = 0;
        char *line;
        char tmp_varname[PATH_MAX]; // PATH_MAX might look weird here... Don't
                                    // know what other constant I could use
                                    // instead.
        char *tmp_varval;
        int line_no = 0;
        while ((nb = getline(&line, &z, config)) != -1) {
            ++line_no;

            int idx;
            for (idx = 0; line[idx] != '\0'; ++idx) {
                if (line[idx] != ' ' && line[idx] != '\t')
                    break;
            }
            if (line[idx] == '#')
                continue;

            s_strncpy(tmp_varname, line, sizeof(tmp_varname));
            remove_trailing_newline(tmp_varname);

            for (tmp_varval = tmp_varname; *tmp_varval != '\0'; ++tmp_varval) {
                if (*tmp_varval == '=') {
                    *tmp_varval = '\0';
                    ++tmp_varval;
                    break;
                }
            }
            char *varname = trim(tmp_varname);
            char *varval = trim(tmp_varval);

            if (!strlen(varname) && !strlen(varval))
                continue;

            if (!strcmp(varname, "log")) {
                s_strncpy(log_file_name, varval, sizeof(log_file_name));
            } else if (!strcmp(varname, "fifo")) {
                s_strncpy(fifo_file_name, varval, sizeof(fifo_file_name));
            } else if (!strcmp(varname, "device")) {
                s_strncpy(dev_file_name, varval, sizeof(dev_file_name));
            } else if (!strcmp(varname, "debug")) {
                debug_on = str_to_boolean(varval);
            } else if (!strcmp(varname, "daemon")) {
                run_as_a_daemon = str_to_boolean(varval);
            } else if (!strcmp(varname, "log_keepalive")) {
                if (!strcmp(varval, "always")) {
                    log_keepalive = LOG_KEEPALIVE_ALWAYS;
                } else if (!strcmp(varval, "error")) {
                    log_keepalive = LOG_KEEPALIVE_ERROR;
                } else if (!strcmp(varval, "never")) {
                    log_keepalive = LOG_KEEPALIVE_NEVER;
                } else {
                    fprintf(stderr, "%s:%i: error: log_keepalive: unknown "
                        "value '%s' (choose one of 'always', 'error', "
                        "'never')\n", abs_cfgfile, line_no, varval);
                    exit(EXIT_FAILURE);
                }
            } else {
                fprintf(stderr, "%s:%i: error: unknown variable '%s'\n",
                    abs_cfgfile, line_no, varname);
                exit(EXIT_FAILURE);
            }
        }
        if (line)
            free(line);

        if (!feof(config)) {
            fprintf(stderr, "%s: error reading\n", abs_cfgfile);
            exit(EXIT_FAILURE);
        }

        fclose(config);
    }

}

void read_cfg_from_cmdline_opts_round2(int argc, char *argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-D")) {
            debug_on = 1;
        } else if (!strcmp(argv[i], "-d")) {
            run_as_a_daemon = 1;
        } else if (!strcmp(argv[i], "-l")) {
            get_required_argument(&i, argc, argv, "l",
                log_file_name, sizeof(log_file_name));
        } else if (!strcmp(argv[i], "-f")) {
            get_required_argument(&i, argc, argv, "f",
                fifo_file_name, sizeof(fifo_file_name));
        } else if (!strcmp(argv[i], "-c")) {
                // Option -c got already taken into account (in
                // read_cfg_from_cmdline_opts_round1), but still, we must
                // consider it to avoid an error 'unknown option'.
            if (i >= argc - 1) {
                    // Such a condition should have been dealt with in the call
                    // to read_cfg_from_cmdline_opts_round1.
                fprintf(stderr, "FATAL: %s:%i: INCONSISTENT STATUS!\n",
                        __FILE__, __LINE__);
                exit(EXIT_FAILURE);
            }
            ++i;
        } else {
            s_strncpy(dev_file_name, argv[i], sizeof(dev_file_name));
            if (i != argc - 1) {
                fprintf(stderr,
                        "Trailing options\n");
                fprintf(stderr,
                        "Try `mapper-devusb -h' for more information.\n");
                exit(1);
            }
        }
    }
}

void infinite_loop() {
    int last_write_buf_result = -1;
    while (1) {
        fd_set rfds;
        struct timeval tv;
        int retval;

        FD_ZERO(&rfds);
        FD_SET(fifo_fd, &rfds);
        tv.tv_sec = (last_write_buf_result == 0 ?
                       KEEP_ALIVE_WHILE_SUCCESS :
                       KEEP_ALIVE_WHILE_FAILURE);
        tv.tv_usec = 0;

        retval = select(fifo_fd + 1, &rfds, NULL, NULL, &tv);

        if (retval == -1) {
            l("error: select: %s", strerror(errno));
            continue;
        } else if (retval == 0) {
            if (log_keepalive == LOG_KEEPALIVE_ALWAYS) {
                l("sending keepalive instruction (noop)");
            }
            int stay_silent_if_error = (log_keepalive == LOG_KEEPALIVE_NEVER);
            last_write_buf_result =
                write_buf(KEEPALIVE_CMD, strlen(KEEPALIVE_CMD),
                          stay_silent_if_error);
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
                break;
            } else {
                last_write_buf_result = write_buf(buf, len, 0);
            }
        }
    }

}

int main(int argc, char *argv[]) {
    flog = stderr;

    s_strncpy(abs_cfgfile, DEFAULT_ABSOLUTE_CONFFILE,
              sizeof(abs_cfgfile));
    s_strncpy(log_file_name, "", sizeof(log_file_name));
    s_strncpy(fifo_file_name, DEFAULT_FIFO_FILE_NAME, sizeof(fifo_file_name));
    s_strncpy(dev_file_name, "", sizeof(dev_file_name));

        // Command-line options parsing is done in 2 rounds because reading the
        // config file can lead to error display (unknown option, missing config
        // file etc.) and it makes no sense if option -h or -v is provided.
        // Also obviously -c option must be parsed before config file reading.
        //
        // On the other hand, command-line options take precedence over config
        // file therefore they are finally read *after* config file reading.

        // Round 1: options that cause immediate stop (-h, -v) and option to
        // enforce the configuration file (-c).
    read_cfg_from_cmdline_opts_round1(argc, argv);
    read_cfg_from_config_file();
        // Round 2: all kinds of options, done after config file read as they
        // take precedence.
    read_cfg_from_cmdline_opts_round2(argc, argv);

#ifndef DEBUG
    if (debug_on) {
        fprintf(stderr, "Error: compiled without debug support, "
                "cannot honor -D option\n");
        exit(EXIT_FAILURE);
    }
#endif

    if (!strlen(dev_file_name)) {
        fprintf(stderr, "Unknown device filename\n");
        fprintf(stderr, "Try `mapper-devusb -h' for more information.\n");
        exit(1);
    }

    if (strlen(log_file_name)) {
        flog = fopen(log_file_name, "a");
        if (flog == NULL) {
            fprintf(stderr, "Error: cannot open log file: "
                "%s\n", strerror(errno));
            exit(3);
        }
        setvbuf(flog, NULL, _IONBF, 0);
    }

    l("start");
    DBG("config file:    [%s]", abs_cfgfile);
    DBG("debug on:       [%s]", (debug_on ? "yes" : "no"));
    DBG("device file:    [%s]", dev_file_name);
    DBG("fifo file name: [%s]", fifo_file_name);
    if (log_file_name != NULL) {
        DBG("log file name:  [%s]", log_file_name);
    } else {
        DBG("log file name:  <stderr>");
    }
    DBG("daemon mode:    [%s]", run_as_a_daemon ? "yes" : "no");

    if (access(fifo_file_name, R_OK ) != -1) {
        l("fifo '%s' already exists", fifo_file_name);
    } else if (mkfifo(fifo_file_name, 0600) == -1) {
        l("warning: unable to create fifo '%s'",
          fifo_file_name);
    } else {
        l("created fifo '%s'", fifo_file_name);
    }

    if ((fifo_fd = open(fifo_file_name, O_RDWR)) < 0) {
        fprintf(stderr, "Error: unable to open '%s': %s\n",
                fifo_file_name, strerror(errno));
        exit(2);
    }

    if (run_as_a_daemon)
        skeleton_daemon();

    atexit(exit_handler);

#ifdef HAVE_SYSTEMD
    sd_notify(0, "READY=1");
#endif

    infinite_loop();
    close(fifo_fd);

    return 0;
}

