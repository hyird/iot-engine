#pragma once

/* Runs argv without a shell. Negative file descriptors keep stdin/stdout unchanged. */
int edge_process_run(const char *const argv[], int stdin_fd, int stdout_fd);
