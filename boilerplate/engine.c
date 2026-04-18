/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Implements:
 * - UNIX Domain Socket IPC for Control Plane
 * - clone() namespace isolation
 * - Bounded-Buffer producer/consumer logging pipeline
 * - container metadata tracking
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 4096 /* INCREASED: So the 'ps' table fits in the message */
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* Arguments passed to the per-container producer thread */
typedef struct {
    int read_fd;
    char container_id[CONTAINER_ID_LEN];
    supervisor_ctx_t *ctx;
} producer_args_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag, const char *value, unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;
    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') return -1;
    if (mib > ULONG_MAX / (1UL << 20)) return -1;
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req, int argc, char *argv[], int start_index)
{
    int i;
    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;
        if (i + 1 >= argc) return -1;
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0) return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0) return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' || nice_value < -20 || nice_value > 19) return -1;
            req->nice_value = (int)nice_value;
            continue;
        }
        return -1;
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) return -1;
    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING: return "running";
    case CONTAINER_STOPPED: return "stopped";
    case CONTAINER_KILLED: return "killed";
    case CONTAINER_EXITED: return "exited";
    default: return "unknown";
    }
}

/* ---------------------------------------------------------
 * Bounded Buffer Implementation
 * --------------------------------------------------------- */
static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    memset(buffer, 0, sizeof(*buffer));
    if (pthread_mutex_init(&buffer->mutex, NULL) != 0) return -1;
    if (pthread_cond_init(&buffer->not_empty, NULL) != 0) return -1;
    if (pthread_cond_init(&buffer->not_full, NULL) != 0) return -1;
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    
    /* Wait until there is space in the buffer */
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }
    
    /* If we are shutting down, bail out early */
    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }
    
    /* Insert the item */
    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;
    
    /* Wake up the consumer thread */
    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    
    /* Wait until there is at least one item */
    while (buffer->count == 0 && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }
    
    /* If empty AND shutting down, we are done */
    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }
    
    /* Remove the item */
    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;
    
    /* Wake up any waiting producer threads */
    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/* ---------------------------------------------------------
 * Logging Threads (Producer & Consumer)
 * --------------------------------------------------------- */
void *producer_thread(void *arg)
{
    producer_args_t *pargs = (producer_args_t *)arg;
    log_item_t item;
    ssize_t n;

    strncpy(item.container_id, pargs->container_id, CONTAINER_ID_LEN);

    /* Read from the pipe until the container exits (EOF) */
    while ((n = read(pargs->read_fd, item.data, LOG_CHUNK_SIZE)) > 0) {
        item.length = n;
        if (bounded_buffer_push(&pargs->ctx->log_buffer, &item) < 0) {
            break; /* Shutdown requested */
        }
    }
    
    close(pargs->read_fd);
    free(pargs);
    return NULL;
}

void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    char filepath[PATH_MAX];

    /* Ensure the logs directory exists */
    mkdir(LOG_DIR, 0755);

    /* Constantly pop logs and write them to the correct file */
    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        snprintf(filepath, sizeof(filepath), "%s/%s.log", LOG_DIR, item.container_id);
        
        int fd = open(filepath, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            write(fd, item.data, item.length);
            close(fd);
        }
    }
    return NULL;
}

/* ---------------------------------------------------------
 * Container Execution
 * --------------------------------------------------------- */
int child_fn(void *arg)
{
    child_config_t *config = (child_config_t *)arg;
    char *argv[] = { "/bin/sh", "-c", config->command, NULL };

    /* Redirect stdout and stderr into the logging pipe */
    dup2(config->log_write_fd, STDOUT_FILENO);
    dup2(config->log_write_fd, STDERR_FILENO);
    close(config->log_write_fd);

    sethostname(config->id, strlen(config->id));

    /* CRITICAL FIX: Modern Linux requires mounts to be private in the new namespace */
    if (mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL) != 0) {
        printf("Error: Failed to make mounts private (errno: %s)\n", strerror(errno));
        return 1;
    }

    if (chroot(config->rootfs) != 0) {
        printf("Error: Failed to chroot to '%s'. Is the path correct? (errno: %s)\n", config->rootfs, strerror(errno));
        return 1;
    }
    if (chdir("/") != 0) {
        printf("Error: Failed to chdir to / (errno: %s)\n", strerror(errno));
        return 1;
    }

    /* Ensure the /proc directory actually exists before trying to mount to it */
    mkdir("/proc", 0755);

    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        printf("Error: Failed to mount /proc (errno: %s)\n", strerror(errno));
        return 1;
    }

    if (config->nice_value != 0) {
        nice(config->nice_value);
    }

    if (execvp(argv[0], argv) == -1) {
        printf("Error: execvp failed to run /bin/sh (errno: %s)\n", strerror(errno));
        return 1;
    }
    return 0;
}

int register_with_monitor(int monitor_fd, const char *container_id, pid_t host_pid, unsigned long soft, unsigned long hard)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft;
    req.hard_limit_bytes = hard;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0) return -1;
    return 0;
}

static int spawn_container(supervisor_ctx_t *ctx, control_request_t *req)
{
    child_config_t *config = malloc(sizeof(child_config_t));
    char *stack = malloc(STACK_SIZE);
    int pipefd[2];

    if (!config || !stack || pipe(pipefd) < 0) {
        if (config) free(config);
        if (stack) free(stack);
        return -1;
    }

    memset(config, 0, sizeof(*config));
    strncpy(config->id, req->container_id, sizeof(config->id) - 1);
    strncpy(config->rootfs, req->rootfs, sizeof(config->rootfs) - 1);
    strncpy(config->command, req->command, sizeof(config->command) - 1);
    config->nice_value = req->nice_value;
    config->log_write_fd = pipefd[1]; /* Give write end to child */

    pid_t pid = clone(child_fn, stack + STACK_SIZE,
                      SIGCHLD | CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS, config);

    if (pid < 0) {
        free(config);
        free(stack);
        return -1;
    }

    /* Parent handles the read end of the pipe */
    close(pipefd[1]); 
    producer_args_t *pargs = malloc(sizeof(producer_args_t));
    pargs->read_fd = pipefd[0];
    pargs->ctx = ctx;
    strncpy(pargs->container_id, req->container_id, CONTAINER_ID_LEN);

    /* Start a producer thread to read this container's output */
    pthread_t prod_tid;
    pthread_create(&prod_tid, NULL, producer_thread, pargs);
    pthread_detach(prod_tid); /* Fire and forget, it cleans itself up */

    if (ctx->monitor_fd >= 0) {
        register_with_monitor(ctx->monitor_fd, req->container_id, pid, req->soft_limit_bytes, req->hard_limit_bytes);
    }

    container_record_t *rec = malloc(sizeof(container_record_t));
    memset(rec, 0, sizeof(*rec));
    strncpy(rec->id, req->container_id, sizeof(rec->id) - 1);
    rec->host_pid = pid;
    rec->state = CONTAINER_RUNNING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    rec->started_at = time(NULL);

    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    return 0;
}

/* ---------------------------------------------------------
 * Supervisor & Client
 * --------------------------------------------------------- */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    struct sockaddr_un addr;
    (void)rootfs;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

    pthread_mutex_init(&ctx.metadata_lock, NULL);
    bounded_buffer_init(&ctx.log_buffer);

    /* Start the single Consumer thread */
    pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    
    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(ctx.server_fd, 5);

    printf("Supervisor running and listening on %s\n", CONTROL_PATH);

    while (!ctx.should_stop) {
        /* Reap zombie processes and update state */
        pid_t exited_pid;
        int status;
        while ((exited_pid = waitpid(-1, &status, WNOHANG)) > 0) {
            pthread_mutex_lock(&ctx.metadata_lock);
            container_record_t *curr = ctx.containers;
            while (curr) {
                if (curr->host_pid == exited_pid) {
                    curr->state = CONTAINER_EXITED;
                    curr->exit_code = WEXITSTATUS(status);
                }
                curr = curr->next;
            }
            pthread_mutex_unlock(&ctx.metadata_lock);
        }

        struct timeval tv = {1, 0};
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(ctx.server_fd, &fds);
        if (select(ctx.server_fd + 1, &fds, NULL, NULL, &tv) <= 0) continue;

        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) continue;

        control_request_t req;
        control_response_t resp;
        memset(&resp, 0, sizeof(resp));

        if (read(client_fd, &req, sizeof(req)) == sizeof(req)) {
            resp.status = 0;
            if (req.kind == CMD_START) {
                if (spawn_container(&ctx, &req) == 0) {
                    snprintf(resp.message, sizeof(resp.message), "Container '%s' started successfully.", req.container_id);
                } else {
                    resp.status = 1;
                    snprintf(resp.message, sizeof(resp.message), "Failed to start container '%s'.", req.container_id);
                }
            } else if (req.kind == CMD_PS) {
                /* Generate the metadata table for 'ps' */
                pthread_mutex_lock(&ctx.metadata_lock);
                int offset = snprintf(resp.message, sizeof(resp.message), "%-15s %-10s %-15s\n", "ID", "PID", "STATE");
                container_record_t *curr = ctx.containers;
                while (curr && offset < sizeof(resp.message) - 64) {
                    offset += snprintf(resp.message + offset, sizeof(resp.message) - offset,
                                       "%-15s %-10d %-15s\n", curr->id, curr->host_pid, state_to_string(curr->state));
                    curr = curr->next;
                }
                pthread_mutex_unlock(&ctx.metadata_lock);
            } else if (req.kind == CMD_STOP) {
                ctx.should_stop = 1;
                snprintf(resp.message, sizeof(resp.message), "Supervisor shutting down...");
            }
        } else {
            resp.status = 1;
            snprintf(resp.message, sizeof(resp.message), "Failed to parse request");
        }
        write(client_fd, &resp, sizeof(resp));
        close(client_fd);
    }

    /* Clean Shutdown */
    unlink(CONTROL_PATH);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL); /* Wait for consumer to flush logs */
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    return 0;
}

static int send_control_request(const control_request_t *req)
{
    int sock;
    struct sockaddr_un addr;
    control_response_t resp;

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return 1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("connect: Supervisor might not be running");
        close(sock);
        return 1;
    }

    if (write(sock, req, sizeof(*req)) != sizeof(*req)) {
        close(sock);
        return 1;
    }

    if (read(sock, &resp, sizeof(resp)) > 0) {
        printf("%s", resp.message);
        if (resp.message[strlen(resp.message) - 1] != '\n') printf("\n");
    }

    close(sock);
    return resp.status;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) return 1;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) return 1;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    if (argc < 3) return 1;
    char filepath[PATH_MAX];
    snprintf(filepath, sizeof(filepath), "%s/%s.log", LOG_DIR, argv[2]);
    
    FILE *f = fopen(filepath, "r");
    if (!f) {
        printf("No logs found for container '%s'.\n", argv[2]);
        return 1;
    }
    
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), f)) {
        printf("%s", buffer);
    }
    fclose(f);
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }
    if (strcmp(argv[1], "supervisor") == 0) return run_supervisor(argc > 2 ? argv[2] : "");
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "stop") == 0) return cmd_stop(argc, argv);
    if (strcmp(argv[1], "ps") == 0) return cmd_ps();
    if (strcmp(argv[1], "logs") == 0) return cmd_logs(argc, argv);
    usage(argv[0]);
    return 1;
}
