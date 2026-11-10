#if HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#if HAVE_UNISTD_H
#include <unistd.h>
#endif /* HAVE_UNISTD_H */

#if HAVE_STDLIB_H
#include <stdlib.h>
#endif /* HAVE_STDLIB_H */

#if HAVE_STRING_H
#include <string.h>
#endif /* HAVE_STRING_H */

#if HAVE_STDIO_H
#include <stdio.h>
#endif /* HAVE_STDIO_H */

#if HAVE_UNISTD_H
#include <unistd.h>
#endif /* HAVE_UNISTD_H */

#if HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif

#if HAVE_SIGNAL_H
#include <signal.h>
#endif /* HAVE_SIGNAL_H */

#if HAVE_ERRNO_H
#include <errno.h>
#endif /* HAVE_ERRNO_H */

#ifdef HAVE_MSYS_CONSOLE_HOST
#define NOMINMAX
#include <windows.h>
#include <sys/cygwin.h>
#include <limits.h>
#include <vector>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <pthread.h>
#endif

#include "sys_util.h"
#include "fork_util.h"
#include "fs_util.h"
#include "pseudo.h"
#include "terminal.h"

#ifdef HAVE_MSYS_CONSOLE_HOST
/* A raw Win32 pipe attached with cygwin_attach_handle_to_fd is not a
 * selectable MSYS pipe: select reports it ready even when it is empty.
 * Relay through a socketpair, whose read readiness and EOF are reliable.
 * Overlapped I/O and a stop event make both directions cancellable. */
struct native_mi_bridge {
    HANDLE pipe;
    HANDLE stop;
    int socket;
    pthread_t reader;
    pthread_t writer;
    bool reader_started;
    bool writer_started;
};

static bool native_pipe_io(native_mi_bridge *bridge, bool reading,
        void *buffer, DWORD size, DWORD *count)
{
    OVERLAPPED operation = {};
    operation.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!operation.hEvent) return false;
    bool ok = false;
    if (WaitForSingleObject(bridge->stop, 0) != WAIT_OBJECT_0) {
        BOOL completed = reading
            ? ReadFile(bridge->pipe, buffer, size, count, &operation)
            : WriteFile(bridge->pipe, buffer, size, count, &operation);
        if (completed) {
            ok = true;
        } else if (GetLastError() == ERROR_IO_PENDING) {
            HANDLE events[] = { bridge->stop, operation.hEvent };
            DWORD result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
            if (result != WAIT_OBJECT_0 + 1) {
                CancelIoEx(bridge->pipe, &operation);
                // Keep the OVERLAPPED alive until cancellation completes.
                GetOverlappedResult(bridge->pipe, &operation, count, TRUE);
            } else {
                ok = GetOverlappedResult(bridge->pipe, &operation, count, FALSE);
            }
        }
    }
    CloseHandle(operation.hEvent);
    return ok && *count != 0;
}

static void stop_native_bridge(native_mi_bridge *bridge)
{
    if (bridge->stop) SetEvent(bridge->stop);
    shutdown(bridge->socket, SHUT_RDWR);
}

static void *native_mi_reader(void *arg)
{
    native_mi_bridge *bridge = (native_mi_bridge *)arg;
    char buffer[4096];
    DWORD size = 0;
    while (native_pipe_io(bridge, true, buffer, sizeof(buffer), &size)) {
        DWORD offset = 0;
        while (offset < size) {
            ssize_t n = send(bridge->socket, buffer + offset, size - offset,
                    MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) goto done;
            offset += n;
        }
    }
done:
    stop_native_bridge(bridge);
    return NULL;
}

static void *native_mi_writer(void *arg)
{
    native_mi_bridge *bridge = (native_mi_bridge *)arg;
    char buffer[4096];
    for (;;) {
        ssize_t size = recv(bridge->socket, buffer, sizeof(buffer), 0);
        if (size < 0 && errno == EINTR) continue;
        if (size <= 0) break;
        ssize_t offset = 0;
        while (offset < size) {
            DWORD n = 0;
            if (!native_pipe_io(bridge, false, buffer + offset, size - offset, &n))
                goto done;
            offset += n;
        }
    }
done:
    stop_native_bridge(bridge);
    return NULL;
}

static void destroy_native_bridge(native_mi_bridge *bridge)
{
    if (!bridge) return;
    stop_native_bridge(bridge);
    if (bridge->reader_started) pthread_join(bridge->reader, NULL);
    if (bridge->writer_started) pthread_join(bridge->writer, NULL);
    close(bridge->socket);
    if (bridge->stop) CloseHandle(bridge->stop);
    delete bridge;
}

static native_mi_bridge *create_native_bridge(HANDLE pipe, int *fd)
{
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == -1)
        return NULL;
    native_mi_bridge *bridge = new native_mi_bridge{};
    bridge->pipe = pipe;
    bridge->socket = sockets[1];
    bridge->stop = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (bridge->stop) {
        bridge->reader_started = pthread_create(&bridge->reader, NULL,
                native_mi_reader, bridge) == 0;
        if (bridge->reader_started)
            bridge->writer_started = pthread_create(&bridge->writer, NULL,
                    native_mi_writer, bridge) == 0;
    }
    if (!bridge->writer_started) {
        destroy_native_bridge(bridge);
        close(sockets[0]);
        return NULL;
    }
    *fd = sockets[0];
    return bridge;
}
#endif

struct pty_pair {
    int masterfd;
    int slavefd;
    char slavename[SLAVE_SIZE];
#ifdef HAVE_MSYS_CONSOLE_HOST
    HANDLE native_pipe;
    bool named_pipe;
    native_mi_bridge *bridge;
#endif
};

pty_pair_ptr pty_pair_create(void)
{
    int val;
    static char local_slavename[SLAVE_SIZE];
    pty_pair_ptr ptr = (pty_pair_ptr) cgdb_malloc(sizeof (struct pty_pair));

    if (!ptr)
        return NULL;

    ptr->masterfd = -1;
    ptr->slavefd = -1;
    ptr->slavename[0] = 0;
#ifdef HAVE_MSYS_CONSOLE_HOST
    ptr->native_pipe = INVALID_HANDLE_VALUE;
    ptr->named_pipe = false;
    ptr->bridge = NULL;
#endif

    val = pty_open(&(ptr->masterfd), &(ptr->slavefd), local_slavename,
            SLAVE_SIZE, NULL, NULL);
    if (val == -1) {
        clog_error(CLOG_CGDB, "PTY open");
        free(ptr);
        return NULL;
    }

    strncpy(ptr->slavename, local_slavename, SLAVE_SIZE);

    return ptr;
}

pty_pair_ptr pty_pair_create_named_pipe(void)
{
#ifdef HAVE_MSYS_CONSOLE_HOST
    pty_pair_ptr ptr = (pty_pair_ptr)cgdb_malloc(sizeof(struct pty_pair));
    ptr->masterfd = ptr->slavefd = -1;
    ptr->named_pipe = true;
    ptr->bridge = NULL;
    char name[SLAVE_SIZE];
    snprintf(name, sizeof(name), "\\\\.\\pipe\\cgdb-mi-%lu-%llu",
            (unsigned long)GetCurrentProcessId(),
            (unsigned long long)GetTickCount64());
    ptr->native_pipe = CreateNamedPipeA(name,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 65536, 65536, 0, NULL);
    if (ptr->native_pipe == INVALID_HANDLE_VALUE) {
        free(ptr);
        return NULL;
    }
    // Forward slashes survive GDB's command argument parser.
    snprintf(ptr->slavename, sizeof(ptr->slavename),
            "//./pipe/%s", name + 9);
    return ptr;
#else
    errno = ENOTSUP;
    return NULL;
#endif
}

int pty_pair_destroy(pty_pair_ptr pty_pair)
{
    if (!pty_pair)
        return -1;

    if (pty_pair->masterfd != -1) cgdb_close(pty_pair->masterfd);
    if (pty_pair->slavefd != -1) cgdb_close(pty_pair->slavefd);
#ifdef HAVE_MSYS_CONSOLE_HOST
    if (pty_pair->named_pipe) {
        destroy_native_bridge(pty_pair->bridge);
        if (pty_pair->native_pipe != INVALID_HANDLE_VALUE)
            CloseHandle(pty_pair->native_pipe);
        free(pty_pair);
        return 0;
    }
#endif

    if (pty_release(pty_pair->slavename) == -1) {
        clog_error(CLOG_CGDB, "pty_release error");
        free(pty_pair);
        return -1;
    }

    free(pty_pair);

    return 0;
}

int pty_pair_get_masterfd(pty_pair_ptr pty_pair)
{
    if (!pty_pair)
        return -1;

    return pty_pair->masterfd;
}

int pty_pair_get_slavefd(pty_pair_ptr pty_pair)
{
    if (!pty_pair)
        return -1;

    return pty_pair->slavefd;
}

const char *pty_pair_get_slavename(pty_pair_ptr pty_pair)
{
    if (!pty_pair)
        return NULL;

    return pty_pair->slavename;
}

int pty_free_process(int *masterfd, char *sname)
{

    cgdb_close(*masterfd);

    if (pty_release(sname) == -1) {
        clog_error(CLOG_CGDB, "pty_release error");
        return -1;
    }

    return 0;
}

/** 
 * Utility function that frees up memory.
 *
 * \param argc
 * The number of items in argv
 *
 * \param argv
 * free is called on each of these
 */
void free_memory(int argc, char *argv[])
{
    int i;

    /* Free the local list */
    for (i = 0; i < argc; i++)
        free(argv[i]);

    free(argv);
}

/* free_memory: utility function that frees up memory.
 *
 * s:       if NULL, left alone, otherwise pty_release is called on it
 * fd:      if -1, left alone, otherwise close is called on it
 * argc:    The number of items in argv
 * argv:    free is called on each of these
 *
 * Returns -1 if any call fails, otherwise 0
 */
static int pty_free_memory(char *s, int fd, int argc, char *argv[])
{
    int error = 0, i;

    if (s && pty_release(s) == -1) {
        clog_error(CLOG_CGDB, "pty_release failed");
        error = -1;
    }

    if (fd != -1 && close(fd) == -1) {
        clog_error(CLOG_CGDB, "close failed");
        error = -1;
    }

    /* Free the local list */
    for (i = 0; i < argc; i++)
        free(argv[i]);

    free(argv);

    return error;
}

#ifdef HAVE_MSYS_CONSOLE_HOST
// Own the native GDB console independently of the MSYS terminal.
static void exec_console_debugger(char **argv, int rows, int cols)
{
    char self[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (len < 0 || len >= (ssize_t)sizeof(self) - 1)
        _exit(127);
    self[len] = 0;
    char *slash = strrchr(self, '/');
    if (!slash)
        _exit(127);
    *slash = 0;
    std::string helper = std::string(self) + "/cgdb-console-host.exe";
    // The helper owns the native console; its parent channels are pipes.
    std::string row_arg = std::to_string(rows), col_arg = std::to_string(cols);
    std::vector<char *> args;
    args.push_back((char *)helper.c_str());
    args.push_back((char *)"--native-gdb");
    args.push_back((char *)row_arg.c_str());
    args.push_back((char *)col_arg.c_str());
    for (char **a = argv; *a; ++a) args.push_back(*a);
    args.push_back(NULL);
    execv(helper.c_str(), args.data());
    perror("cgdb: cannot start native debugger console host");
    _exit(127);
}

#endif

int invoke_debugger(const char *path,
        int argc, char *argv[], int gdb_win_rows, int gdb_win_cols,
        int *in, int *out, pty_pair_ptr new_ui_pair)
{
#ifdef HAVE_MSYS_CONSOLE_HOST
    if (!new_ui_pair || !new_ui_pair->named_pipe) {
        clog_error(CLOG_CGDB, "MSYS builds require the native UCRT64 GDB backend");
        errno = ENOTSUP;
        return -1;
    }
#endif
    pid_t pid;
    const char *const GDB = "gdb";
    const char *const NW = "--nw";
    const char *const EX = "-ex";
    char **local_argv;
    int i, j = 0, extra = 5;
    int malloc_size = argc + extra;
    char slavename[SLAVE_SIZE] = {};
    int masterfd = -1;
    int inputfd = -1;
    const char *device_name = pty_pair_get_slavename(new_ui_pair);
    int new_ui_masterfd = pty_pair_get_masterfd(new_ui_pair);
    int new_ui_slavefd = pty_pair_get_slavefd(new_ui_pair);

    struct winsize size, *winsize;
    size.ws_row = gdb_win_rows;
    size.ws_col = gdb_win_cols;
    size.ws_xpixel = 0;
    size.ws_ypixel = 0;

    if (gdb_win_rows == 0 && gdb_win_cols == 0) {
        winsize = 0;
    } else {
        winsize = &size;
    }


    /* Copy the argv into the local_argv, and NULL terminate it.
     * sneak in the path name, the user did not type that */
    local_argv = (char **) cgdb_malloc((malloc_size) * sizeof (char *));
    if (path)
        local_argv[j++] = cgdb_strdup(path);
    else
        local_argv[j++] = cgdb_strdup(GDB);

    /* NOTE: These options have to come first, since if the user
     * typed '--args' to GDB, everything at the end of the 
     * users options become parameters to the inferior.
     */
    local_argv[j++] = cgdb_strdup(NW);

    local_argv[j++] = cgdb_strdup(EX);
    local_argv[j++] = cgdb_strdup(std::string("new-ui mi ").
            append(device_name).c_str());

    /* copy in all the data the user entered */
    for (i = 0; i < argc; i++)
        local_argv[j++] = cgdb_strdup(argv[i]);

    local_argv[j] = NULL;

    if (fs_util_file_exists_in_path(local_argv[0]) == -1) {
        clog_error(CLOG_CGDB, "Debugger \"%s\" not found", local_argv[0]);
        pty_free_memory(NULL, masterfd, j, local_argv);
        return -1;
    }

    /* Log the gdb invocation line */
    clog_info(CLOG_GDBIO, "Invoking program:");
    for (i = 0; i < j; i++) {
        clog_info(CLOG_GDBIO, "  argv[%d]=%s ", i, local_argv[i]);
    }

    /* Fork into two processes with a shared pty pipe */
#ifdef HAVE_MSYS_CONSOLE_HOST
    if (new_ui_pair->named_pipe) {
        int console_input[2], console_output[2];
        if (pipe2(console_input, O_CLOEXEC) == -1) {
            free_memory(j, local_argv);
            return -1;
        }
        if (pipe2(console_output, O_CLOEXEC) == -1) {
            close(console_input[0]); close(console_input[1]);
            free_memory(j, local_argv);
            return -1;
        }
        pid = fork();
        if (pid == 0) {
            // Keyboard signals belong to CGDB. It forwards them as console
            // input to GDB's separate ConPTY, not to this transport helper.
            if (setsid() == -1) _exit(127);
            if (dup2(console_input[0], STDIN_FILENO) == -1 ||
                    dup2(console_output[1], STDOUT_FILENO) == -1 ||
                    dup2(console_output[1], STDERR_FILENO) == -1) _exit(127);
            close(console_input[0]); close(console_input[1]);
            close(console_output[0]); close(console_output[1]);
        } else {
            close(console_input[0]); close(console_output[1]);
            inputfd = console_input[1];
            masterfd = console_output[0];
        }
    } else
#endif
    {
        pid = pty_fork(&masterfd, slavename, SLAVE_SIZE, NULL, winsize);
        inputfd = masterfd;
    }

    if (pid == -1) {            /* error, free memory and return  */
        if (inputfd != masterfd && inputfd != -1) close(inputfd);
        pty_free_memory(NULL, masterfd, j, local_argv);
        clog_error(CLOG_CGDB, "fork failed");
        return -1;
    } else if (pid == 0) {      /* child */
        // Close the master fd for new ui
        if (new_ui_masterfd != -1) close(new_ui_masterfd);
        
        #ifdef HAVE_MSYS_CONSOLE_HOST
        if (new_ui_pair->named_pipe) {
            const char *old = getenv("MSYS2_ARG_CONV_EXCL");
            std::string exclusions = old ? old : "";
            if (!exclusions.empty()) exclusions += ";";
            exclusions += "new-ui mi ";
            setenv("MSYS2_ARG_CONV_EXCL", exclusions.c_str(), 1);
        }
        exec_console_debugger(local_argv, gdb_win_rows, gdb_win_cols);
#else
        execvp(local_argv[0], local_argv);
#endif

        /* Will get here if exec failed. This will happen when the 
         * - "gdb" is not on the users path, or if 
         * - user specified a different program via the -d option and it was
         *   not able to be exec'd.
         */
        _exit(127);
    } else {
        if (new_ui_slavefd != -1) close(new_ui_slavefd);
        new_ui_pair->slavefd = -1;
    }

#ifdef HAVE_MSYS_CONSOLE_HOST
    if (new_ui_pair->named_pipe) {
        HANDLE pipe = new_ui_pair->native_pipe;
        OVERLAPPED connection = {};
        connection.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        bool connected = false;
        bool child_exited = false;
        if (connection.hEvent) {
            connected = ConnectNamedPipe(pipe, &connection);
            if (!connected) {
                DWORD error = GetLastError();
                if (error == ERROR_PIPE_CONNECTED) {
                    connected = true;
                } else if (error == ERROR_IO_PENDING) {
                    DWORD count;
                    ULONGLONG deadline = GetTickCount64() + 10000;
                    while (GetTickCount64() < deadline) {
                        DWORD waited = WaitForSingleObject(connection.hEvent, 50);
                        if (waited == WAIT_OBJECT_0) {
                            connected = GetOverlappedResult(pipe, &connection, &count, FALSE);
                            break;
                        }
                        if (waited != WAIT_TIMEOUT) break;
                        int status;
                        pid_t result = waitpid(pid, &status, WNOHANG);
                        if (result == pid || (result == -1 && errno == ECHILD)) {
                            child_exited = true;
                            break;
                        }
                    }
                    if (!connected) {
                        CancelIoEx(pipe, &connection);
                        GetOverlappedResult(pipe, &connection, &count, TRUE);
                    }
                }
            }
            CloseHandle(connection.hEvent);
        }
        if (connected)
            new_ui_pair->bridge = create_native_bridge(pipe, &new_ui_pair->masterfd);
        if (new_ui_pair->masterfd == -1) {
            clog_error(CLOG_CGDB, "Native GDB MI pipe connection failed");
            if (!child_exited) kill(pid, SIGTERM);
            close(masterfd);
            if (inputfd != masterfd) close(inputfd);
            free_memory(j, local_argv);
            return -1;
        }
    }
#endif

    *in = inputfd;
    *out = masterfd;

    free_memory(j, local_argv);
    return pid;
}
