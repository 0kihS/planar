#ifndef PLANAR_IPC_H
#define PLANAR_IPC_H

#include <stdbool.h>
#include <wayland-server-core.h>

struct planar_server;
struct json_object;

struct ipc_client {
    struct wl_list link;
    int fd;
    struct wl_event_source *event_source;
    struct planar_server *server;
};

bool ipc_init(struct planar_server *server);
void ipc_finish(struct planar_server *server);
void ipc_broadcast_event(struct planar_server *server, const char *event_type, const char *data);
void ipc_broadcast_json_event(struct planar_server *server, const char *event_type,
                              struct json_object *data);

/* Execute a command string (used by both IPC and keybindings) */
bool ipc_dispatch_command(struct planar_server *server, const char *cmd);

#endif /* PLANAR_IPC_H */
