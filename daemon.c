#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <linux/landlock.h>

#define SOCKET_PATH "/tmp/sandbox.sock"
#define WORKSPACES_PATH "/tmp/workspaces"
#define BACKLOG 512
#define MAX_CMD_LEN 4096
#define MAX_ARGS 128

#ifndef SYS_landlock_create_ruleset
#define SYS_landlock_create_ruleset 444
#endif
#ifndef SYS_landlock_add_rule
#define SYS_landlock_add_rule 445
#endif
#ifndef SYS_landlock_restrict_self
#define SYS_landlock_restrict_self 446
#endif

static volatile sig_atomic_t g_running = 1;

// Reaps zombie child processes asynchronously
static void sigchld_handler(int sig) {
    (void)sig;
    int saved_errno = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0);
    errno = saved_errno;
}

// Graceful shutdown on SIGINT / SIGTERM
static void sigterm_handler(int sig) {
    (void)sig;
    g_running = 0;
}

// Read the protocol header up to '\n' byte-by-byte to leave STDIN stream uncorrupted
static ssize_t read_command_line(int fd, char *buf, size_t max_len) {
    size_t idx = 0;
    while (idx < max_len - 1) {
        char ch;
        ssize_t n = read(fd, &ch, 1);
        if (n == 1) {
            if (ch == '\n') {
                break;
            }
            buf[idx++] = ch;
        } else if (n == 0) {
            // EOF before newline
            break;
        } else {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
    }
    buf[idx] = '\0';
    return (ssize_t)idx;
}

// Helper to add Landlock rule for a directory if it exists
static int add_landlock_path(int ruleset_fd, const char *path, uint64_t access) {
    int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0) {
        return 0; // If path does not exist on this machine, ignore safely
    }
    struct landlock_path_beneath_attr path_beneath = {
        .allowed_access = access,
        .parent_fd = fd,
    };
    int ret = syscall(SYS_landlock_add_rule, ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &path_beneath, 0);
    close(fd);
    return ret;
}

// Create Landlock ruleset restricting filesystem access, with exceptions for runtimes & /tmp/workspaces
static int setup_landlock(void) {
    // Ensure the sandbox workspace base directory exists
    struct stat st;
    if (stat(WORKSPACES_PATH, &st) == -1) {
        mkdir(WORKSPACES_PATH, 0777);
    }

    // Restrict all standard filesystem access actions
    struct landlock_ruleset_attr ruleset_attr = {
        .handled_access_fs =
            LANDLOCK_ACCESS_FS_EXECUTE |
            LANDLOCK_ACCESS_FS_WRITE_FILE |
            LANDLOCK_ACCESS_FS_READ_FILE |
            LANDLOCK_ACCESS_FS_READ_DIR |
            LANDLOCK_ACCESS_FS_REMOVE_DIR |
            LANDLOCK_ACCESS_FS_REMOVE_FILE |
            LANDLOCK_ACCESS_FS_MAKE_CHAR |
            LANDLOCK_ACCESS_FS_MAKE_DIR |
            LANDLOCK_ACCESS_FS_MAKE_REG |
            LANDLOCK_ACCESS_FS_MAKE_SOCK |
            LANDLOCK_ACCESS_FS_MAKE_FIFO |
            LANDLOCK_ACCESS_FS_MAKE_BLOCK |
            LANDLOCK_ACCESS_FS_MAKE_SYM |
            LANDLOCK_ACCESS_FS_REFER |
            LANDLOCK_ACCESS_FS_TRUNCATE,
    };

    int ruleset_fd = syscall(SYS_landlock_create_ruleset, &ruleset_attr, sizeof(ruleset_attr), 0);
    if (ruleset_fd < 0) {
        perror("syscall(SYS_landlock_create_ruleset)");
        return -1;
    }

    // Access flags:
    // 1. Read-only + Execute for system runtime binaries and shared libraries (libc, ld.so, python, java)
    uint64_t ro_exec = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
    // 2. Read-only configuration data (e.g. /etc/ld.so.cache, timezones)
    uint64_t ro_data = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
    // 3. Full Read + Write + Execute strictly beneath /tmp/workspaces
    uint64_t rw_workspace =
        ro_exec |
        LANDLOCK_ACCESS_FS_WRITE_FILE |
        LANDLOCK_ACCESS_FS_REMOVE_DIR |
        LANDLOCK_ACCESS_FS_REMOVE_FILE |
        LANDLOCK_ACCESS_FS_MAKE_DIR |
        LANDLOCK_ACCESS_FS_MAKE_REG |
        LANDLOCK_ACCESS_FS_TRUNCATE;

    // Allow system runtimes and dynamic loaders (read-only)
    add_landlock_path(ruleset_fd, "/usr", ro_exec);
    add_landlock_path(ruleset_fd, "/lib", ro_exec);
    add_landlock_path(ruleset_fd, "/lib64", ro_exec);
    add_landlock_path(ruleset_fd, "/bin", ro_exec);
    add_landlock_path(ruleset_fd, "/etc", ro_data);
    add_landlock_path(ruleset_fd, "/proc", ro_data);
    add_landlock_path(ruleset_fd, "/dev", ro_data);

    // Allow read, write, and execute strictly inside /tmp/workspaces
    if (add_landlock_path(ruleset_fd, WORKSPACES_PATH, rw_workspace) < 0) {
        perror("add_landlock_path(" WORKSPACES_PATH ")");
        close(ruleset_fd);
        return -1;
    }

    return ruleset_fd;
}

int main(void) {
    // 1. Setup SIGCHLD handler to reap child processes
    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = sigchld_handler;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa_chld, NULL) == -1) {
        perror("sigaction(SIGCHLD)");
        exit(EXIT_FAILURE);
    }

    // Handle termination signals for socket cleanup
    struct sigaction sa_term;
    memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = sigterm_handler;
    sigemptyset(&sa_term.sa_mask);
    sigaction(SIGINT, &sa_term, NULL);
    sigaction(SIGTERM, &sa_term, NULL);

    // 2. Create Unix Domain Socket
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    unlink(SOCKET_PATH);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, BACKLOG) == -1) {
        perror("listen");
        close(server_fd);
        unlink(SOCKET_PATH);
        exit(EXIT_FAILURE);
    }

    printf("[Daemon] Fork-server listening on %s (PID: %d)\n", SOCKET_PATH, getpid());
    fflush(stdout);

    // 3. Accept loop
    while (g_running) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            close(client_fd);
            continue;
        }

        if (pid == 0) {
            // --- Child Process ---
            close(server_fd);

            // Read the command string before '\n'
            char cmd_line[MAX_CMD_LEN];
            ssize_t len = read_command_line(client_fd, cmd_line, sizeof(cmd_line));
            if (len <= 0) {
                dprintf(client_fd, "Error: Empty or invalid command protocol\n");
                close(client_fd);
                _exit(EXIT_FAILURE);
            }

            // Parse pipe-delimited arguments: command|arg1|arg2
            char *argv[MAX_ARGS];
            int argc = 0;
            char *token = strtok(cmd_line, "|");
            while (token != NULL && argc < MAX_ARGS - 1) {
                argv[argc++] = token;
                token = strtok(NULL, "|");
            }
            argv[argc] = NULL;

            if (argc == 0) {
                dprintf(client_fd, "Error: No command arguments parsed\n");
                close(client_fd);
                _exit(EXIT_FAILURE);
            }

            // Wire STDIN, STDOUT, STDERR directly to client socket
            if (dup2(client_fd, STDIN_FILENO) == -1 ||
                dup2(client_fd, STDOUT_FILENO) == -1 ||
                dup2(client_fd, STDERR_FILENO) == -1) {
                perror("dup2");
                close(client_fd);
                _exit(EXIT_FAILURE);
            }

            if (client_fd > STDERR_FILENO) {
                close(client_fd);
            }

            // --- Landlock LSM Sandbox Setup ---
            int ruleset_fd = setup_landlock();
            if (ruleset_fd < 0) {
                perror("setup_landlock");
                _exit(EXIT_FAILURE);
            }

            // Landlock requires PR_SET_NO_NEW_PRIVS before restricting
            if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == -1) {
                perror("prctl(PR_SET_NO_NEW_PRIVS)");
                close(ruleset_fd);
                _exit(EXIT_FAILURE);
            }

            // Apply Landlock restrictions to the child process
            if (syscall(SYS_landlock_restrict_self, ruleset_fd, 0) == -1) {
                perror("syscall(SYS_landlock_restrict_self)");
                close(ruleset_fd);
                _exit(EXIT_FAILURE);
            }
            close(ruleset_fd);

            // --- Resource Limits (Security Hardening) ---
            // 1. CPU Time Limit: 3 seconds hard limit (SIGKILL after 3s CPU time)
            struct rlimit rl_cpu = {
                .rlim_cur = 3,
                .rlim_max = 3,
            };
            if (setrlimit(RLIMIT_CPU, &rl_cpu) == -1) {
                perror("setrlimit(RLIMIT_CPU)");
                _exit(EXIT_FAILURE);
            }

            // 2. Address Space (Virtual Memory) Limit
            // Java HotSpot requires ~2-4GB virtual reservation for Compressed OOPs and JImage metadata.
            // Non-JVM runtimes (C, C++, Python) are capped at 512 MB.
            int is_java = (strstr(argv[0], "java") != NULL);
            struct rlimit rl_as = {
                .rlim_cur = is_java ? (4096ULL * 1024 * 1024) : (512ULL * 1024 * 1024),
                .rlim_max = is_java ? (4096ULL * 1024 * 1024) : (512ULL * 1024 * 1024),
            };
            if (setrlimit(RLIMIT_AS, &rl_as) == -1) {
                perror("setrlimit(RLIMIT_AS)");
                _exit(EXIT_FAILURE);
            }

            // Execute the target binary
            execve(argv[0], argv, NULL);

            // If execve fails
            perror("execve");
            _exit(EXIT_FAILURE);
        }

        // --- Parent Process ---
        // Close client socket and continue listening
        close(client_fd);
    }

    close(server_fd);
    unlink(SOCKET_PATH);
    printf("[Daemon] Shutdown cleanly.\n");
    return 0;
}
