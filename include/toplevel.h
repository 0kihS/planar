#ifndef PLANAR_TOPLEVEL_H
#define PLANAR_TOPLEVEL_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/config.h>
#include <wlr/types/wlr_xdg_shell.h>
#if WLR_HAS_XWAYLAND
#include <wlr/xwayland.h>
#endif
#include "server.h"
#include "workspaces.h"

struct planar_decoration;
struct planar_group;

enum planar_toplevel_type {
    PLANAR_TOPLEVEL_XDG,
    PLANAR_TOPLEVEL_XWAYLAND,
};

struct planar_toplevel {
    struct wl_list link;
    struct planar_server *server;
    enum planar_toplevel_type type;
    union {
        struct wlr_xdg_toplevel *xdg_toplevel;
#if WLR_HAS_XWAYLAND
        struct wlr_xwayland_surface *xwayland_surface;
#endif
    };
    struct wlr_scene_tree *container;  // Parent container that holds decoration + surface
    struct wlr_scene_tree *scene_tree; // The actual xdg surface tree
    struct planar_workspace *workspace;
    struct planar_decoration *decoration;
    struct planar_group *group;
    double logical_x, logical_y;
    int base_width, base_height;
    bool mapped;
    bool listed;

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
#if WLR_HAS_XWAYLAND
    struct wl_listener associate;
    struct wl_listener dissociate;
    struct wl_listener request_configure;
    struct wl_listener request_activate;
    struct wl_listener request_close;
#endif
};

void server_new_xdg_toplevel(struct wl_listener *listener, void *data);
#if WLR_HAS_XWAYLAND
void server_new_xwayland_surface(struct wl_listener *listener, void *data);
#endif
void focus_toplevel(struct planar_toplevel *toplevel, struct wlr_surface *surface);
void kill_active_toplevel(struct planar_server *server);
void scale_toplevel(struct planar_toplevel *toplevel);
void move_toplevel(struct planar_toplevel *toplevel, double x, double y);
struct planar_toplevel *find_toplevel_by_id(struct planar_server *server, const char *window_id);
struct planar_toplevel *find_toplevel_by_surface(struct planar_server *server, struct wlr_surface *surface);
struct wlr_surface *toplevel_get_surface(struct planar_toplevel *toplevel);
const char *toplevel_get_app_id(struct planar_toplevel *toplevel);
const char *toplevel_get_title(struct planar_toplevel *toplevel);
bool toplevel_is_mapped(struct planar_toplevel *toplevel);
void toplevel_get_geometry(struct planar_toplevel *toplevel, struct wlr_box *box);
void toplevel_set_size(struct planar_toplevel *toplevel, int width, int height);
void toplevel_close(struct planar_toplevel *toplevel);

#endif // PLANAR_TOPLEVEL_H
