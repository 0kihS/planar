#ifndef PLANAR_TOPLEVEL_H
#define PLANAR_TOPLEVEL_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_xdg_shell.h>
#include "server.h"
#include "workspaces.h"

struct planar_decoration;

struct planar_toplevel {
    struct wl_list link;
    struct planar_server *server;
    struct wlr_xdg_toplevel *xdg_toplevel;
    struct wlr_scene_tree *container;  // Parent container that holds decoration + surface
    struct wlr_scene_tree *scene_tree; // The actual xdg surface tree
    struct planar_workspace *workspace;
    struct planar_decoration *decoration;
    double logical_x, logical_y;

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
void scale_toplevel(struct planar_toplevel *toplevel, double scale);

#endif // PLANAR_TOPLEVEL_H