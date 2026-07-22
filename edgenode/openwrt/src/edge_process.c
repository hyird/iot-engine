#include "edge_process.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

int edge_process_run(const char *const argv[], int stdin_fd, int stdout_fd) {
    if (argv == NULL || argv[0] == NULL)
        return -1;
    const pid_t child = fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        if (stdin_fd >= 0 && dup2(stdin_fd, STDIN_FILENO) < 0)
            _exit(126);
        if (stdout_fd >= 0 && dup2(stdout_fd, STDOUT_FILENO) < 0)
            _exit(126);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
