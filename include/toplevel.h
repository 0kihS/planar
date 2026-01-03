#ifndef PLANAR_TOPLEVEL_H
#define PLANAR_TOPLEVEL_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_xdg_shell.h>
#include "server.h"
#include "workspaces.h"

struct planar_decoration;
struct planar_group;

struct planar_toplevel {
    struct wl_list link;
    struct planar_server *server;
    struct wlr_xdg_toplevel *xdg_toplevel;
    struct wlr_scene_tree *container;  // Parent container that holds decoration + surface
    struct wlr_scene_tree *scene_tree; // The actual xdg surface tree
    struct planar_workspace *workspace;
    struct planar_decoration *decoration;
    struct planar_group *group;
    double logical_x, logical_y;
    int base_width, base_height;

    char *window_id;
    uint32_t instance_number;

    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_listener request_move;
    struct wl_listener request_resize;
    struct wl_listener request_maximize;
    struct wl_listener request_fullscreen;
};

void server_new_xdg_toplevel(struct wl_listener *listener, void *data);
void focus_toplevel(struct planar_toplevel *toplevel, struct wlr_surface *surface);
void kill_active_toplevel(struct planar_server *server);
void scale_toplevel(struct planar_toplevel *toplevel);
struct planar_toplevel *find_toplevel_by_id(struct planar_server *server, const char *window_id);

#endif // PLANAR_TOPLEVEL_H