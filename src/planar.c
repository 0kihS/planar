#include "server.h"
#include <stdlib.h>
#include <wlr/util/log.h>
#include <unistd.h>
#include <signal.h>

static int handle_signal(int signo, void *data) {
    struct planar_server *server = data;
    wlr_log(WLR_INFO, "Received signal %d, shutting down", signo);
    wl_display_terminate(server->wl_display);
    return 0;
}

int main(void) {
    wlr_log_init(WLR_DEBUG, NULL);

    struct planar_server server = {0};
    if (!server_init(&server)) {
        server_finish(&server);
        return EXIT_FAILURE;
    }

    setenv("WAYLAND_DISPLAY", server.socket, true);

    wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s", server.socket);
    wlr_log(WLR_INFO, "WAYLAND_DISPLAY set to %s", getenv("WAYLAND_DISPLAY"));

    struct wl_event_loop *event_loop = wl_display_get_event_loop(server.wl_display);
    wl_event_loop_add_signal(event_loop, SIGINT, handle_signal, &server);
    wl_event_loop_add_signal(event_loop, SIGTERM, handle_signal, &server);

    struct sigaction ignore = { .sa_handler = SIG_IGN };
    sigemptyset(&ignore.sa_mask);
    sigaction(SIGCHLD, &ignore, NULL);
    sigaction(SIGPIPE, &ignore, NULL);

    bool ok = server_run(&server);
    server_finish(&server);

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
