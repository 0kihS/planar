#include "toplevel.h"
#include "server.h"
#include "cursor.h"
#include "workspaces.h"
#include "decoration.h"
#include "ipc.h"

#include <stdlib.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>

static void begin_interactive(struct planar_toplevel *toplevel,
        enum planar_cursor_mode mode, uint32_t edges) {
    struct planar_server *server = toplevel->server;
    struct wlr_surface *focused_surface =
        server->seat->pointer_state.focused_surface;
    if (toplevel->xdg_toplevel->base->surface !=
            wlr_surface_get_root_surface(focused_surface)) {
        return;
    }
    server->grabbed_toplevel = toplevel;
    server->cursor_mode = mode;

    if (mode == PLANAR_CURSOR_MOVE) {
        server->grab_x = server->cursor->x - toplevel->scene_tree->node.x;
        server->grab_y = server->cursor->y - toplevel->scene_tree->node.y;
    } else {
        struct wlr_box *geo_box = &toplevel->xdg_toplevel->base->geometry;

        double border_x = (toplevel->scene_tree->node.x + geo_box->x) +
            ((edges & WLR_EDGE_RIGHT) ? geo_box->width : 0);
        double border_y = (toplevel->scene_tree->node.y + geo_box->y) +
            ((edges & WLR_EDGE_BOTTOM) ? geo_box->height : 0);
        server->grab_x = server->cursor->x - border_x;
        server->grab_y = server->cursor->y - border_y;

        server->grab_geobox = *geo_box;
        server->grab_geobox.x += toplevel->scene_tree->node.x;
        server->grab_geobox.y += toplevel->scene_tree->node.y;

        server->resize_edges = edges;
    }
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
    (void)data;
    struct planar_toplevel *toplevel = wl_container_of(listener, toplevel, map);
    wlr_scene_node_set_enabled(&toplevel->scene_tree->node, true);

    toplevel->decoration = decoration_create(toplevel);
    if (toplevel->decoration) {
        decoration_update_geometry(toplevel->decoration);
    }

    focus_toplevel(toplevel, toplevel->xdg_toplevel->base->surface);

    const char *app_id = toplevel->xdg_toplevel->app_id;
    ipc_broadcast_event(toplevel->server, "window_open", app_id ? app_id : "");
}

static void xdg_toplevel_maximize(struct wl_listener *listener, void *data) {
    (void)data;
    struct planar_toplevel *toplevel =
        wl_container_of(listener, toplevel, request_maximize);
    if (toplevel->xdg_toplevel->base->initialized) {
        wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
    }
}

static void xdg_toplevel_fullscreen(struct wl_listener *listener, void *data) {
    (void)data;
    struct planar_toplevel *toplevel =
        wl_container_of(listener, toplevel, request_fullscreen);
    if (toplevel->xdg_toplevel->base->initialized) {
        wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
    }
}

static void xdg_toplevel_resize(struct wl_listener *listener, void *data) {
    struct wlr_xdg_toplevel_resize_event *event = data;
    struct planar_toplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
    begin_interactive(toplevel, PLANAR_CURSOR_RESIZE, event->edges);
}

static void xdg_toplevel_move(struct wl_listener *listener, void *data) {
    (void)data;
    struct planar_toplevel *toplevel = wl_container_of(listener, toplevel, request_move);
    begin_interactive(toplevel, PLANAR_CURSOR_MOVE, 0);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
    (void)data;
    struct planar_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);

    ipc_broadcast_event(toplevel->server, "window_close", NULL);

    if (toplevel->server->grabbed_toplevel == toplevel) {
        toplevel->server->grabbed_toplevel = NULL;
        toplevel->server->cursor_mode = PLANAR_CURSOR_PASSTHROUGH;
    }

    if (toplevel->decoration) {
        decoration_destroy(toplevel->decoration);
        toplevel->decoration = NULL;
    }

    wl_list_remove(&toplevel->link);
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
    (void)data;
    struct planar_toplevel *toplevel = wl_container_of(listener, toplevel, commit);
    if (toplevel->xdg_toplevel->base->initial_commit) {
        wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
    }

    if (toplevel->decoration) {
        decoration_update_geometry(toplevel->decoration);
    }

    if (toplevel->workspace && toplevel->workspace->scale != 1.0) {
        scale_toplevel(toplevel, toplevel->workspace->scale);
    }
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct planar_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);

    if (toplevel->decoration) {
        decoration_destroy(toplevel->decoration);
        toplevel->decoration = NULL;
    }

    wl_list_remove(&toplevel->map.link);
    wl_list_remove(&toplevel->unmap.link);
    wl_list_remove(&toplevel->commit.link);
    wl_list_remove(&toplevel->destroy.link);
    wl_list_remove(&toplevel->request_move.link);
    wl_list_remove(&toplevel->request_resize.link);
    wl_list_remove(&toplevel->request_maximize.link);
    wl_list_remove(&toplevel->request_fullscreen.link);
    free(toplevel);
}

static void scale_buffer_iterator(struct wlr_scene_buffer *buffer, int sx, int sy, void *data) {
    (void)sx;
    (void)sy;
    double scale = *(double *)data;

    if (buffer->buffer) {
        wlr_scene_buffer_set_dest_size(buffer,
            buffer->buffer->width * scale,
            buffer->buffer->height * scale);
    }
}

void scale_toplevel(struct planar_toplevel *toplevel, double scale) {
    if (!toplevel || !toplevel->container) return;

    wlr_scene_node_set_position(&toplevel->container->node,
        (int)(toplevel->logical_x * scale),
        (int)(toplevel->logical_y * scale));

    wlr_scene_node_for_each_buffer(&toplevel->scene_tree->node, scale_buffer_iterator, &scale);

    int border = toplevel->server ? toplevel->server->settings.border_width : 4;
    wlr_scene_node_set_position(&toplevel->scene_tree->node, border, border);

    if (toplevel->decoration) {
        decoration_update_geometry_scaled(toplevel->decoration, scale);
    }
}

void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
    struct planar_server *server = wl_container_of(listener, server, new_xdg_toplevel);
    struct wlr_xdg_toplevel *xdg_toplevel = data;
    struct planar_toplevel *toplevel = calloc(1, sizeof(*toplevel));
    struct planar_workspace *workspace = server->active_workspace;

    toplevel->server = server;
    toplevel->xdg_toplevel = xdg_toplevel;

    toplevel->container = wlr_scene_tree_create(workspace->scene_tree);
    toplevel->container->node.data = toplevel;

    int border = server->settings.border_width;
    toplevel->scene_tree = wlr_scene_xdg_surface_create(toplevel->container, xdg_toplevel->base);
    wlr_scene_node_set_position(&toplevel->scene_tree->node, border, border);

    xdg_toplevel->base->data = toplevel->scene_tree;
    toplevel->workspace = workspace;
    wl_list_insert(&workspace->toplevels, &toplevel->link);

    toplevel->logical_x = 0;
    toplevel->logical_y = 0;

    wlr_scene_node_set_enabled(&toplevel->container->node, true);

    toplevel->map.notify = xdg_toplevel_map;
    wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
    toplevel->unmap.notify = xdg_toplevel_unmap;
    wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
    toplevel->commit.notify = xdg_toplevel_commit;
    wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);

    toplevel->destroy.notify = xdg_toplevel_destroy;
    wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);

    toplevel->request_move.notify = xdg_toplevel_move;
    wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);
    toplevel->request_resize.notify = xdg_toplevel_resize;
    wl_signal_add(&xdg_toplevel->events.request_resize, &toplevel->request_resize);
    toplevel->request_maximize.notify = xdg_toplevel_maximize;
    wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);
    toplevel->request_fullscreen.notify = xdg_toplevel_fullscreen;
    wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);
}

void focus_toplevel(struct planar_toplevel *toplevel, struct wlr_surface *surface) {
    if (toplevel == NULL) {
        return;
    }
    struct planar_server *server = toplevel->server;
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
    if (prev_surface == surface) {
        return;
    }
    if (prev_surface) {
        struct wlr_xdg_toplevel *prev_toplevel = wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
        if (prev_toplevel != NULL) {
            wlr_xdg_toplevel_set_activated(prev_toplevel, false);
        }
    }
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
    wlr_scene_node_raise_to_top(&toplevel->container->node);

    wl_list_remove(&toplevel->link);
    wl_list_insert(&toplevel->workspace->toplevels, &toplevel->link);

    wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);

    if (keyboard != NULL) {
        wlr_seat_keyboard_notify_enter(seat, toplevel->xdg_toplevel->base->surface,
                                       keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
    }

    const char *app_id = toplevel->xdg_toplevel->app_id;
    ipc_broadcast_event(server, "window_focus", app_id ? app_id : "");
}

void kill_active_toplevel(struct planar_server *server) {
    struct planar_toplevel *toplevel;
    wl_list_for_each_reverse(toplevel, &server->active_workspace->toplevels, link) {
        if (toplevel->xdg_toplevel->base->surface == server->seat->keyboard_state.focused_surface) {
            wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
            return;
        }
    }
}
