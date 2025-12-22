#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

#define BUFFER_SIZE 4096

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s [options] <command> [args...]\n", prog);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  -e, --events                Subscribe to events (streams continuously)\n");
    fprintf(stderr, "  -h, --help                  Show this help\n");
    fprintf(stderr, "\nCommands:\n");
    fprintf(stderr, "  workspace <N>               Switch to workspace N\n");
    fprintf(stderr, "  killactive                  Close focused window\n");
    fprintf(stderr, "  move_to_workspace <N>       Move focused window to workspace N\n");
    fprintf(stderr, "  move_workspace <X> <Y>      Pan workspace by offset\n");
    fprintf(stderr, "  focus_window <id>           Focus window by ID\n");
    fprintf(stderr, "  close_window <id>           Close window by ID\n");
    fprintf(stderr, "  move_window <id> <X> <Y>    Move window to logical coordinates\n");
    fprintf(stderr, "  resize_window <id> <W> <H>  Resize window\n");
    fprintf(stderr, "  move_window_to_workspace <id> <N>\n");
    fprintf(stderr, "                              Move window to workspace N\n");
    fprintf(stderr, "  spawn <command>             Spawn a process\n");
    fprintf(stderr, "  set <setting> <value>       Set a runtime setting\n");
    fprintf(stderr, "  get <setting>               Get a runtime setting\n");
    fprintf(stderr, "\nSettings:\n");
    fprintf(stderr, "  border_width <N>            Border width in pixels (0-32)\n");
    fprintf(stderr, "  border_color <R G B A>      Border color (floats 0.0-1.0)\n");
    fprintf(stderr, "  zoom_min <N>                Minimum zoom level\n");
    fprintf(stderr, "  zoom_max <N>                Maximum zoom level\n");
    fprintf(stderr, "  zoom_step <N>               Zoom step size\n");
    fprintf(stderr, "  cursor_size <N>             Cursor size (8-128)\n");
    fprintf(stderr, "  workspaces                  Get workspace info (get only)\n");
    fprintf(stderr, "  focused                     Get focused window info (get only)\n");
    fprintf(stderr, "  windows                     List all windows (get only)\n");
    fprintf(stderr, "  window <id>                 Get window info (get only)\n");
}

static int connect_socket(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "Failed to create socket: %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Failed to connect to %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static char *get_socket_path(int event_socket) {
    const char *path = getenv(event_socket ? "PLANAR_EVENT_SOCKET" : "PLANAR_SOCKET");
    if (path) {
        return strdup(path);
    }

    /* Try to construct default path */
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    const char *wayland_display = getenv("WAYLAND_DISPLAY");

    if (!runtime_dir || !wayland_display) {
        fprintf(stderr, "Cannot determine socket path. Set PLANAR_SOCKET or ensure XDG_RUNTIME_DIR and WAYLAND_DISPLAY are set.\n");
        return NULL;
    }

    char *socket_path = malloc(256);
    if (event_socket) {
        snprintf(socket_path, 256, "%s/planar.%s.event.sock", runtime_dir, wayland_display);
    } else {
        snprintf(socket_path, 256, "%s/planar.%s.sock", runtime_dir, wayland_display);
    }
    return socket_path;
}

static int run_command(int argc, char **argv) {
    char *socket_path = get_socket_path(0);
    if (!socket_path) {
        return 1;
    }

    int fd = connect_socket(socket_path);
    free(socket_path);
    if (fd < 0) {
        return 1;
    }

    /* Build command string from arguments */
    char cmd[BUFFER_SIZE];
    int pos = 0;
    for (int i = 0; i < argc && pos < BUFFER_SIZE - 1; i++) {
        if (i > 0) {
            cmd[pos++] = ' ';
        }
        int len = strlen(argv[i]);
        if (pos + len >= BUFFER_SIZE - 1) {
            break;
        }
        memcpy(cmd + pos, argv[i], len);
        pos += len;
    }
    cmd[pos++] = '\n';
    cmd[pos] = '\0';

    /* Send command */
    if (write(fd, cmd, pos) < 0) {
        fprintf(stderr, "Failed to send command: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    /* Read response */
    char buf[BUFFER_SIZE];
    ssize_t len = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (len < 0) {
        fprintf(stderr, "Failed to read response: %s\n", strerror(errno));
        return 1;
    }

    buf[len] = '\0';
    printf("%s", buf);

    /* Check if response indicates success */
    if (strstr(buf, "\"ok\":true")) {
        return 0;
    }
    return 1;
}

static int subscribe_events(void) {
    char *socket_path = get_socket_path(1);
    if (!socket_path) {
        return 1;
    }

    int fd = connect_socket(socket_path);
    free(socket_path);
    if (fd < 0) {
        return 1;
    }

    printf("Subscribed to events. Press Ctrl+C to exit.\n");
    fflush(stdout);

    char buf[BUFFER_SIZE];
    while (1) {
        ssize_t len = read(fd, buf, sizeof(buf) - 1);
        if (len <= 0) {
            if (len < 0) {
                fprintf(stderr, "Read error: %s\n", strerror(errno));
            }
            break;
        }
        buf[len] = '\0';
        printf("%s", buf);
        fflush(stdout);
    }

    close(fd);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    /* Check for options */
    int arg_start = 1;
    int events_mode = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--events") == 0) {
            events_mode = 1;
            arg_start = i + 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        } else {
            arg_start = i;
            break;
        }
    }

    if (events_mode) {
        return subscribe_events();
    }

    if (arg_start >= argc) {
        fprintf(stderr, "No command specified\n");
        print_usage(argv[0]);
        return 1;
    }

    return run_command(argc - arg_start, argv + arg_start);
}
