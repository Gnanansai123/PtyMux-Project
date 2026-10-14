#define _XOPEN_SOURCE 600
#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define MAX_SESSIONS 8
#define NAME_LEN 32
#define READ_SIZE 4096

typedef struct {
    int id;
    int master_fd;
    pid_t child_pid;
    bool alive;
    char name[NAME_LEN];
} Session;

static Session sessions[MAX_SESSIONS];
static int session_count = 0;
static int active_index = -1;
static struct termios original_termios;
static bool raw_enabled = false;
static volatile sig_atomic_t resize_pending = 0;
static volatile sig_atomic_t child_event_pending = 0;
static bool command_prefix = false;
static bool running = true;

static void restore_terminal(void) {
    if (raw_enabled) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios);
        raw_enabled = false;
    }
}

static void enable_raw_terminal(void) {
    struct termios raw;
    if (tcgetattr(STDIN_FILENO, &original_termios) == -1) {
        perror("tcgetattr");
        exit(EXIT_FAILURE);
    }
    raw = original_termios;
    raw.c_lflag &= (tcflag_t) ~(ECHO | ICANON | IEXTEN);
    raw.c_iflag &= (tcflag_t) ~(IXON | ICRNL);
    raw.c_oflag |= OPOST;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        perror("tcsetattr");
        exit(EXIT_FAILURE);
    }
    raw_enabled = true;
}

static void on_resize(int sig) {
    (void)sig;
    resize_pending = 1;
}

static void on_child(int sig) {
    (void)sig;
    child_event_pending = 1;
}

static void get_terminal_size(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 &&
        ws.ws_row > 0 && ws.ws_col > 0) {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
    } else {
        *rows = 24;
        *cols = 80;
    }
}

static void propagate_size(Session *s) {
    if (!s || !s->alive || s->master_fd < 0) return;
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == -1) return;
    (void)ioctl(s->master_fd, TIOCSWINSZ, &ws);
    if (s->child_pid > 0) kill(s->child_pid, SIGWINCH);
}

static void propagate_all_sizes(void) {
    for (int i = 0; i < session_count; i++) propagate_size(&sessions[i]);
}

static void close_session(Session *s, bool terminate_child) {
    if (!s) return;
    if (s->master_fd >= 0) {
        close(s->master_fd);
        s->master_fd = -1;
    }
    if (terminate_child && s->child_pid > 0 && s->alive) {
        kill(s->child_pid, SIGHUP);
        kill(s->child_pid, SIGTERM);
    }
    s->alive = false;
}

static int create_session(const char *name) {
    if (session_count >= MAX_SESSIONS) {
        const char msg[] = "\r\nPtyMux: maximum session count reached.\r\n";
        (void)write(STDOUT_FILENO, msg, sizeof(msg) - 1);
        return -1;
    }

    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master == -1) {
        perror("posix_openpt");
        return -1;
    }
    if (grantpt(master) == -1 || unlockpt(master) == -1) {
        perror("grantpt/unlockpt");
        close(master);
        return -1;
    }

    char *slave_name = ptsname(master);
    if (!slave_name) {
        perror("ptsname");
        close(master);
        return -1;
    }

    int rows, cols;
    get_terminal_size(&rows, &cols);
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;
    (void)ioctl(master, TIOCSWINSZ, &ws);

    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        close(master);
        return -1;
    }

    if (pid == 0) {
        close(master);
        if (setsid() == -1) _exit(126);

        int slave = open(slave_name, O_RDWR);
        if (slave == -1) _exit(126);
        if (ioctl(slave, TIOCSCTTY, 0) == -1) {
            /* Some environments may already establish the controlling terminal. */
        }
        if (dup2(slave, STDIN_FILENO) == -1 ||
            dup2(slave, STDOUT_FILENO) == -1 ||
            dup2(slave, STDERR_FILENO) == -1) _exit(126);
        if (slave > STDERR_FILENO) close(slave);

        struct termios child_termios;
        if (tcgetattr(STDIN_FILENO, &child_termios) == 0) {
            child_termios.c_lflag |= (ECHO | ICANON | ISIG);
            (void)tcsetattr(STDIN_FILENO, TCSANOW, &child_termios);
        }
        execl("/bin/bash", "bash", "--login", (char *)NULL);
        execl("/bin/sh", "sh", (char *)NULL);
        _exit(127);
    }

    Session *s = &sessions[session_count];
    s->id = session_count + 1;
    s->master_fd = master;
    s->child_pid = pid;
    s->alive = true;
    snprintf(s->name, sizeof(s->name), "%s", (name && *name) ? name : "bash");
    active_index = session_count;
    session_count++;
    propagate_size(s);

    char message[160];
    int n = snprintf(message, sizeof(message),
                     "\r\n[PtyMux] Created session %d (%s), PID %ld\r\n",
                     s->id, s->name, (long)s->child_pid);
    (void)write(STDOUT_FILENO, message, (size_t)n);
    return s->id;
}

static int find_alive_index_by_id(int id) {
    for (int i = 0; i < session_count; i++)
        if (sessions[i].id == id && sessions[i].alive) return i;
    return -1;
}

static void show_sessions(void) {
    char line[256];
    (void)write(STDOUT_FILENO, "\r\n--- PtyMux sessions ---\r\n",
                sizeof("\r\n--- PtyMux sessions ---\r\n") - 1);
    for (int i = 0; i < session_count; i++) {
        int n = snprintf(line, sizeof(line), "%c ID %d | %-12s | PID %-6ld | %s\r\n",
                         (i == active_index) ? '*' : ' ',
                         sessions[i].id, sessions[i].name,
                         (long)sessions[i].child_pid,
                         sessions[i].alive ? "running" : "stopped");
        if (n > 0) (void)write(STDOUT_FILENO, line, (size_t)n);
    }
    (void)write(STDOUT_FILENO,
        "Keys: Ctrl-B then 1..8 switch | c create | l list | k kill active | n next | q quit\r\n",
        sizeof("Keys: Ctrl-B then 1..8 switch | c create | l list | k kill active | n next | q quit\r\n") - 1);
}

static void switch_to_index(int index) {
    if (index < 0 || index >= session_count || !sessions[index].alive) {
        const char msg[] = "\r\n[PtyMux] That session is not running.\r\n";
        (void)write(STDOUT_FILENO, msg, sizeof(msg) - 1);
        return;
    }
    active_index = index;
    propagate_size(&sessions[active_index]);
    const char clear[] = "\033[2J\033[H";
    (void)write(STDOUT_FILENO, clear, sizeof(clear) - 1);
    char msg[128];
    int n = snprintf(msg, sizeof(msg),
                     "[PtyMux] Active session %d (%s). Ctrl-B then l for list.\r\n",
                     sessions[index].id, sessions[index].name);
    (void)write(STDOUT_FILENO, msg, (size_t)n);
}

static void kill_active_session(void) {
    if (active_index < 0 || active_index >= session_count ||
        !sessions[active_index].alive) {
        const char msg[] = "\r\n[PtyMux] No active running session.\r\n";
        (void)write(STDOUT_FILENO, msg, sizeof(msg) - 1);
        return;
    }
    Session *s = &sessions[active_index];
    kill(s->child_pid, SIGHUP);
    kill(s->child_pid, SIGTERM);
    close_session(s, false);
    const char msg[] = "\r\n[PtyMux] Active session stopped.\r\n";
    (void)write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    active_index = -1;
    for (int i = 0; i < session_count; i++) {
        if (sessions[i].alive) { active_index = i; break; }
    }
    if (active_index >= 0) switch_to_index(active_index);
}

static void reap_children(void) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < session_count; i++) {
            if (sessions[i].child_pid == pid) {
                sessions[i].alive = false;
                if (sessions[i].master_fd >= 0) {
                    close(sessions[i].master_fd);
                    sessions[i].master_fd = -1;
                }
                if (i == active_index) {
                    const char msg[] = "\r\n[PtyMux] Active shell exited. Ctrl-B then n to switch.\r\n";
                    (void)write(STDOUT_FILENO, msg, sizeof(msg) - 1);
                }
                break;
            }
        }
    }
    child_event_pending = 0;
}

static void handle_prefix_command(unsigned char ch) {
    command_prefix = false;
    if (ch >= '1' && ch <= '8') {
        int idx = find_alive_index_by_id((int)(ch - '0'));
        switch_to_index(idx);
    } else if (ch == 'c') {
        char name[NAME_LEN];
        snprintf(name, sizeof(name), "bash%d", session_count + 1);
        int id = create_session(name);
        if (id > 0) switch_to_index(find_alive_index_by_id(id));
    } else if (ch == 'l') {
        show_sessions();
    } else if (ch == 'k') {
        kill_active_session();
    } else if (ch == 'n') {
        if (session_count > 0) {
            int start = (active_index + 1 + session_count) % session_count;
            for (int step = 0; step < session_count; step++) {
                int idx = (start + step) % session_count;
                if (sessions[idx].alive) { switch_to_index(idx); break; }
            }
        }
    } else if (ch == 'q') {
        running = false;
    } else if (ch == 0x02) {
        unsigned char ctrl_b = 0x02;
        if (active_index >= 0 && sessions[active_index].alive)
            (void)write(sessions[active_index].master_fd, &ctrl_b, 1);
    } else {
        const char msg[] = "\r\n[PtyMux] Unknown command. Ctrl-B then l for help.\r\n";
        (void)write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    }
}

static void process_keyboard(void) {
    unsigned char buf[READ_SIZE];
    ssize_t count = read(STDIN_FILENO, buf, sizeof(buf));
    if (count <= 0) {
        running = false;
        return;
    }
    for (ssize_t i = 0; i < count; i++) {
        unsigned char ch = buf[i];
        if (command_prefix) {
            handle_prefix_command(ch);
            continue;
        }
        if (ch == 0x02) {
            command_prefix = true;
            continue;
        }
        if (active_index >= 0 && active_index < session_count &&
            sessions[active_index].alive && sessions[active_index].master_fd >= 0) {
            if (write(sessions[active_index].master_fd, &ch, 1) < 0 && errno != EINTR) {
                /* Session may have exited; SIGCHLD/reap logic handles cleanup. */
            }
        }
    }
}

int main(void) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        sessions[i].master_fd = -1;
        sessions[i].child_pid = -1;
        sessions[i].alive = false;
    }

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "PtyMux must run in an interactive terminal.\n");
        return EXIT_FAILURE;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_resize;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGWINCH, &sa, NULL);
    sa.sa_handler = on_child;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    if (atexit(restore_terminal) != 0) {
        fprintf(stderr, "Warning: could not register terminal restore handler.\n");
    }
    enable_raw_terminal();

    const char welcome[] =
        "\033[2J\033[H"
        "========================================\r\n"
        "             PtyMux (C11)               \r\n"
        "========================================\r\n"
        "Starting first Bash session...\r\n";
    (void)write(STDOUT_FILENO, welcome, sizeof(welcome) - 1);

    if (create_session("bash1") < 0) {
        restore_terminal();
        return EXIT_FAILURE;
    }
    switch_to_index(active_index);

    while (running) {
        if (resize_pending) {
            resize_pending = 0;
            propagate_all_sizes();
        }
        if (child_event_pending) reap_children();

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        int maxfd = STDIN_FILENO;
        for (int i = 0; i < session_count; i++) {
            if (sessions[i].alive && sessions[i].master_fd >= 0) {
                FD_SET(sessions[i].master_fd, &readfds);
                if (sessions[i].master_fd > maxfd) maxfd = sessions[i].master_fd;
            }
        }

        int ready = select(maxfd + 1, &readfds, NULL, NULL, NULL);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        if (FD_ISSET(STDIN_FILENO, &readfds)) process_keyboard();

        for (int i = 0; i < session_count; i++) {
            Session *s = &sessions[i];
            if (!s->alive || s->master_fd < 0 || !FD_ISSET(s->master_fd, &readfds))
                continue;
            char output[READ_SIZE];
            ssize_t n = read(s->master_fd, output, sizeof(output));
            if (n > 0) {
                if (i == active_index) {
                    (void)write(STDOUT_FILENO, output, (size_t)n);
                }
            } else if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) {
                s->alive = false;
                close(s->master_fd);
                s->master_fd = -1;
                if (s->child_pid > 0) (void)waitpid(s->child_pid, NULL, WNOHANG);
            }
        }
    }

    restore_terminal();
    for (int i = 0; i < session_count; i++) {
        if (sessions[i].alive) {
            kill(sessions[i].child_pid, SIGHUP);
            kill(sessions[i].child_pid, SIGTERM);
        }
        if (sessions[i].master_fd >= 0) close(sessions[i].master_fd);
    }
    while (waitpid(-1, NULL, 0) > 0) {}
    printf("\r\n[PtyMux] Exited cleanly.\n");
    return 0;
}
