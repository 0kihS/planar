#include "server.h"
#include <stdlib.h>
#include <wlr/util/log.h>
#include <unistd.h>
#include <signal.h>

static struct planar_server *global_server = NULL;

static void handle_signal(int signo) {
    (void)signo;
    if (global_server && global_server->wl_display) {
        wl_display_terminate(global_server->wl_display);
    }
}

int main(void) {
    wlr_log_init(WLR_DEBUG, NULL);

    struct planar_server server = {0};
    global_server = &server;
    server_init(&server);

	setenv("WAYLAND_DISPLAY", server.socket, true);

    wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s", server.socket);
    wlr_log(WLR_INFO, "WAYLAND_DISPLAY set to %s", getenv("WAYLAND_DISPLAY"));

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    server_run(&server);
    server_finish(&server);

    return 0;
}