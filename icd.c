#include <errno.h>
#include <limits.h>
#include <linux/limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <termios.h>
#include <unistd.h>
#include <poll.h>

enum {
        KEY_NONE = -1,
        KEY_UP = 0x100, KEY_DOWN, KEY_RIGHT, KEY_LEFT,
        KEY_ENTER, KEY_ESC
};

#define ESC_TIMEOUT_MS 25

static unsigned char inbuf[64];
static size_t inlen, inpos;

#define ALT_ON    "\033[?1049h"
#define ALT_OFF   "\033[?1049l"
#define CLEAR     "\033[2J\033[H"

#define EMIT(s) emit(s, sizeof (s) - 1)

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t caught;

static int next_byte(int timeout_ms) {
        if (inpos == inlen) {
                struct pollfd p = { .fd = STDIN_FILENO, .events = POLLIN };
                int r = poll(&p, 1, timeout_ms);

                if (r < 0) {
                        if (errno != EINTR)
                                running = 0;
                        return -1;
                }
                if (r == 0)
                        return -1; 

                ssize_t n = read(STDIN_FILENO, inbuf, sizeof inbuf);
                if (n <= 0) {
                        if (n == 0 || errno != EINTR)
                                running = 0;
                        return -1;
                }
                inlen = (size_t)n;
                inpos = 0;
        }
        return inbuf[inpos++];
}

static int key_read(void) {
        int c = next_byte(-1);

        if (c < 0)
                return KEY_NONE;
        if (c == '\r' || c == '\n')
                return KEY_ENTER;
        if (c != 033)
                return c;

        int c1 = next_byte(ESC_TIMEOUT_MS);
        if (c1 < 0)
                return KEY_ESC;

        if (c1 == '[' || c1 == 'O') {
                int c2 = next_byte(ESC_TIMEOUT_MS);

                if (c1 == 'O' && c2 == 'M')
                        return KEY_ENTER;
                switch (c2) {
                case 'A': return KEY_UP;
                case 'B': return KEY_DOWN;
                case 'C': return KEY_RIGHT;
                case 'D': return KEY_LEFT;
                }
                while (c2 >= 0x30 && c2 <= 0x3F) // CSI params
                        c2 = next_byte(ESC_TIMEOUT_MS);
        }
        return KEY_NONE;
}

static void emit(const char *s, size_t n) {
        while (n) {
                ssize_t k = write(STDOUT_FILENO, s, n);
                if (k <= 0) {
                        if (k < 0 && errno == EINTR)
                                continue;
                        return;
                }
                s += k;
                n -= (size_t)k;
        }
}

static struct termios saved_term;
static int term_saved;

static int term_setup(void) {
        if (tcgetattr(STDIN_FILENO, &saved_term) == 0) {
                struct termios raw = saved_term;
                raw.c_lflag &= ~(unsigned)(ICANON | ECHO);
                raw.c_iflag &= ~(unsigned)(IXON);
                raw.c_cc[VMIN]  = 1;
                raw.c_cc[VTIME] = 0;
                term_saved = tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
        }
        EMIT(ALT_ON);
        return term_saved;
}

static void term_restore(void) {
        EMIT(ALT_OFF);
        if (term_saved)
                tcsetattr(STDIN_FILENO, TCSANOW, &saved_term);
}

static void install(int sig, void (*handler)(int), int flags) {
        struct sigaction sa = { .sa_handler = handler, .sa_flags = flags };
        sigfillset(&sa.sa_mask);
        sigaction(sig, &sa, (void*)0);
}

static void on_quit(int sig) {
        caught = sig;
        running = 0;
}

static void on_stop(int sig) {
        term_restore();
        install(sig, SIG_DFL, 0);
        raise(sig);
}

static void on_cont(int sig) {
        (void)sig;
        (void)term_setup();
        install(SIGTSTP, on_stop, 0);
}

// interrupt poll() so the loop redraws
static void on_resize(int sig) {
        (void)sig;
}

static const char *key_name(int k) {
        switch (k) {
        case KEY_NONE:  return "none";
        case KEY_UP:    return "up";
        case KEY_DOWN:  return "down";
        case KEY_RIGHT: return "right";
        case KEY_LEFT:  return "left";
        case KEY_ENTER: return "enter";
        case KEY_ESC:   return "esc";
        default:        return (void*)0;
        }
}

static void draw(DIR *dir) {
        struct dirent *entry;
        char buf[PATH_MAX];
        size_t offset;
        int n;

        n = snprintf(buf, sizeof buf, CLEAR);
        offset = (size_t)n;

        while ((entry = readdir(dir)) != NULL) {
                if (entry->d_type != DT_DIR)
                        continue;

                n = snprintf(buf + offset, sizeof buf - offset,
                        "%s\n", entry->d_name);
                if (n < 0 || (size_t)n >= sizeof buf - offset)
                        break; // out of room
                offset += (size_t)n;
        }

        // dir! brilliant

        emit(buf, offset);
}

int main(void) {
        int status = 0;

        if (!isatty(STDOUT_FILENO)) {
                fprintf(stderr, "icd: stdout is not a terminal\n");
                return 1;
        }

        install(SIGINT,   on_quit,   0);
        install(SIGTERM,  on_quit,   0);
        install(SIGHUP,   on_quit,   0);
        install(SIGQUIT,  on_quit,   0);
        install(SIGTSTP,  on_stop,   0);
        install(SIGCONT,  on_cont,   SA_RESTART);
        install(SIGWINCH, on_resize, 0);
        install(SIGPIPE,  SIG_IGN,   0);
        install(SIGTTOU,  SIG_IGN,   0);
        install(SIGTTIN,  SIG_IGN,   0);

        if (!term_setup()) {
                term_restore();
                fprintf(stderr, "icd: cannot put the terminal in raw mode\n");
                return 1;
        }

        int last = KEY_NONE;

        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) == NULL) {
                running = 0;
                status = 1;
        }

        while (running) {
                DIR *dir = opendir(cwd);
                if (dir == NULL) {
                        perror("icd: unable to open directory");
                        status = 1;
                        break;
                }

                draw(dir);
                closedir(dir);

                int k = key_read();
                if (!running)
                        break;

                switch (k) {
                case 'q':
                case 'Q':
                case KEY_ESC:
                        running = 0;
                        break;
                case KEY_ENTER:
                        /* TODO: exit and drop into selected dir */
                        break;
                case KEY_UP:
                        /* TODO: move the selection up */
                        break;
                case KEY_DOWN:
                        /* TODO: move the selection down */
                        break;
                case KEY_LEFT:
                        /* TODO: go to the parent dir */
                        break;
                case KEY_RIGHT:
                        /* TODO: go into the selected dir */
                        break;
                case KEY_NONE:
                        break;
                }

                last = k;

        }

        term_restore();

        if (caught) {
                install((int)caught, SIG_DFL, 0);
                raise((int)caught);
        }

        return status;
}
