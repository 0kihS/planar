#ifndef PLANAR_TOPLEVEL_H
#define PLANAR_TOPLEVEL_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_xdg_shell.h>
#include "server.h"

struct planar_toplevel {
    struct wl_list link;
    struct planar_server *server;
    struct wlr_xdg_toplevel *xdg_toplevel;
    struct wlr_scene_tree *scene_tree;

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
static struct planar_toplevel *desktop_toplevel_at(struct planar_server *server, double lx, double ly,
                                            struct wlr_surface **surface, double *sx, double *sy);

#endif // PLANAR_TOPLEVEL_H