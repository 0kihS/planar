#include "toplevel.h"
#include "server.h"
#include "workspaces.h"
#include "decoration.h"
#include "ipc.h"
#include "group.h"
#include "selection.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <scenefx/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>

static uint32_t allocate_window_instance(struct planar_server *server, const char *app_id) {
    const char *key = app_id ? app_id : "unknown";

    for (size_t i = 0; i < server->window_id_tracker.count; i++) {
        if (strcmp(server->window_id_tracker.app_ids[i], key) == 0) {
            return ++server->window_id_tracker.counters[i];
        }
    }

    if (server->window_id_tracker.count >= server->window_id_tracker.capacity) {
        size_t new_cap = server->window_id_tracker.capacity == 0 ? 16 : server->window_id_tracker.capacity * 2;
        server->window_id_tracker.app_ids = realloc(server->window_id_tracker.app_ids, new_cap * sizeof(char *));
        server->window_id_tracker.counters = realloc(server->window_id_tracker.counters, new_cap * sizeof(uint32_t));
        server->window_id_tracker.capacity = new_cap;
    }

    size_t idx = server->window_id_tracker.count++;
    server->window_id_tracker.app_ids[idx] = strdup(key);
    server->window_id_tracker.counters[idx] = 0;
    return 0;
}

static char *generate_window_id(struct planar_server *server, const char *app_id) {
    const char *key = app_id ? app_id : "unknown";
    uint32_t instance = allocate_window_instance(server, app_id);
    char *id = malloc(strlen(key) + 16);
    sprintf(id, "%s:%u", key, instance);
    return id;
}

struct planar_toplevel *find_toplevel_by_id(struct planar_server *server, const char *window_id) {
    struct planar_workspace *ws;
    wl_list_for_each(ws, &server->workspaces, link) {
        struct planar_toplevel *toplevel;
        wl_list_for_each(toplevel, &ws->toplevels, link) {
            if (toplevel->window_id && strcmp(toplevel->window_id, window_id) == 0) {
                return toplevel;
            }
        }
    }
    return NULL;
}

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

    // Find parent total scale
    float total_scale = 1.0;
    struct wlr_scene_node *it = toplevel->container->node.parent ? &toplevel->container->node.parent->node : NULL;
    while (it) {
        total_scale *= it->scale;
        it = it->parent ? &it->parent->node : NULL;
    }

    if (mode == PLANAR_CURSOR_MOVE) {
        server->grab_x = server->cursor->x - (toplevel->container->node.x * total_scale);
        server->grab_y = server->cursor->y - (toplevel->container->node.y * total_scale);
    } else {
        struct wlr_box *geo_box = &toplevel->xdg_toplevel->base->geometry;

        double border_x = toplevel->container->node.x +
            ((edges & WLR_EDGE_RIGHT) ? geo_box->width : 0);
        double border_y = toplevel->container->node.y +
            ((edges & WLR_EDGE_BOTTOM) ? geo_box->height : 0);
        server->grab_x = server->cursor->x - (border_x * total_scale);
        server->grab_y = server->cursor->y - (border_y * total_scale);

        int border = server->settings.border_width;
        server->grab_geobox = *geo_box;
        server->grab_geobox.x = toplevel->container->node.x + border;
        server->grab_geobox.y = toplevel->container->node.y + border;

        server->resize_edges = edges;
    }
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
    (void)data;
    struct planar_toplevel *toplevel = wl_container_of(listener, toplevel, map);
    struct planar_server *server = toplevel->server;
    wlr_scene_node_set_enabled(&toplevel->scene_tree->node, true);

    if (toplevel->window_id) {
        free(toplevel->window_id);
    }
    toplevel->window_id = generate_window_id(server, toplevel->xdg_toplevel->app_id);

    struct wlr_box geo_box = toplevel->xdg_toplevel->base->geometry;
    toplevel->base_width = geo_box.width;
    toplevel->base_height = geo_box.height;

    // Center new windows on the current output
    if (toplevel->logical_x == 0 && toplevel->logical_y == 0) {
        struct planar_server *server = toplevel->server;
        struct wlr_output *output = wlr_output_layout_output_at(
            server->output_layout, server->cursor->x, server->cursor->y);
        if (output) {
            struct wlr_box output_box;
            wlr_output_layout_get_box(server->output_layout, output, &output_box);
            
            struct wlr_box geo_box = toplevel->xdg_toplevel->base->geometry;

            double new_x = ((output_box.width / 2.0) - server->active_workspace->global_offset.x) / server->active_workspace->scale;
            double new_y = ((output_box.height / 2.0) - server->active_workspace->global_offset.y) / server->active_workspace->scale;
            
            toplevel->logical_x = new_x - (geo_box.width / 2.0);
            toplevel->logical_y = new_y - (geo_box.height / 2.0);

            wlr_scene_node_set_position(&toplevel->container->node,
                (int)(toplevel->logical_x),
                (int)(toplevel->logical_y));
        }
    }

    if (!window_rules_has_nodecoration(server, toplevel->xdg_toplevel->app_id)) {
        toplevel->decoration = decoration_create(toplevel);
        if (toplevel->decoration) {
            decoration_update_geometry(toplevel->decoration);
        }
    }

    scale_toplevel(toplevel);

    focus_toplevel(toplevel, toplevel->xdg_toplevel->base->surface);

    const char *app_id = toplevel->xdg_toplevel->app_id ? toplevel->xdg_toplevel->app_id : "";
    const char *title = toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title : "";
    int width = toplevel->decoration ? toplevel->decoration->width : 0;
    int height = toplevel->decoration ? toplevel->decoration->height : 0;

    char event_data[512];
    snprintf(event_data, sizeof(event_data),
        "{\"id\":\"%s\",\"app_id\":\"%s\",\"title\":\"%s\",\"workspace\":%d,"
        "\"geometry\":{\"x\":%.0f,\"y\":%.0f,\"width\":%d,\"height\":%d}}",
        toplevel->window_id ? toplevel->window_id : "",
        app_id, title,
        toplevel->workspace->index + 1,
        toplevel->logical_x, toplevel->logical_y,
        width, height);

    ipc_broadcast_event(toplevel->server, "window_open", event_data);
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

    char event_data[256];
    snprintf(event_data, sizeof(event_data), "{\"id\":\"%s\"}",
        toplevel->window_id ? toplevel->window_id : "");
    ipc_broadcast_event(toplevel->server, "window_close", event_data);

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
    struct planar_server *server = toplevel->server;

    if (toplevel->xdg_toplevel->base->initial_commit) {
        wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
    }

    // Check if client is requesting a specific size via min/max constraints
    struct wlr_xdg_toplevel_state *state = &toplevel->xdg_toplevel->current;
    if (state->min_width > 0 && state->min_height > 0 &&
        state->min_width == state->max_width &&
        state->min_height == state->max_height) {
        // Client wants exact size - send configure with that size
        struct wlr_box geo = toplevel->xdg_toplevel->base->geometry;
        if (geo.width != state->min_width || geo.height != state->min_height) {
            wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
                state->min_width, state->min_height);
        }
    }

    struct wlr_box geo = toplevel->xdg_toplevel->base->geometry;

    if (server->cursor_mode == PLANAR_CURSOR_RESIZE && server->grabbed_toplevel == toplevel) {
        int border = server->settings.border_width;
        if (server->resize_edges & WLR_EDGE_LEFT) {
            toplevel->logical_x = (server->grab_geobox.x + server->grab_geobox.width - geo.width) - border;
        }
        if (server->resize_edges & WLR_EDGE_TOP) {
            toplevel->logical_y = (server->grab_geobox.y + server->grab_geobox.height - geo.height) - border;
        }
    }

    toplevel->base_width = geo.width;
    toplevel->base_height = geo.height;

    scale_toplevel(toplevel);
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct planar_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);

    // Remove from selection if selected
    selection_remove(toplevel->server, toplevel);

    group_remove_toplevel_from_any(toplevel->server, toplevel);

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
    free(toplevel->window_id);
    free(toplevel);
}

void scale_toplevel(struct planar_toplevel *toplevel) {
    if (!toplevel || !toplevel->container) return;

    wlr_scene_node_set_position(&toplevel->container->node,
        (int)(toplevel->logical_x),
        (int)(toplevel->logical_y));

    int border = toplevel->decoration ? toplevel->server->settings.border_width : 0;
    wlr_scene_node_set_position(&toplevel->scene_tree->node, border, border);

    if (toplevel->decoration) {
        decoration_update_geometry(toplevel->decoration);
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

    xdg_toplevel->base->data = toplevel->container;
    toplevel->workspace = workspace;
    wl_list_insert(&workspace->toplevels, &toplevel->link);

    toplevel->logical_x = 0;
    toplevel->logical_y = 0;

    toplevel->window_id = NULL;  // Will be set at map time when app_id is available
    toplevel->instance_number = 0;

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

    // Ensure windows with ontop rule stay on top
    struct planar_toplevel *t;
    wl_list_for_each(t, &server->active_workspace->toplevels, link) {
        if (t != toplevel && t->xdg_toplevel->app_id &&
            window_rules_has_ontop(server, t->xdg_toplevel->app_id)) {
            wlr_scene_node_raise_to_top(&t->container->node);
        }
    }

    wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);

    if (keyboard != NULL) {
        wlr_seat_keyboard_notify_enter(seat, toplevel->xdg_toplevel->base->surface,
                                       keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
    }

    char event_data[256];
    snprintf(event_data, sizeof(event_data), "{\"id\":\"%s\",\"app_id\":\"%s\"}",
        toplevel->window_id ? toplevel->window_id : "",
        toplevel->xdg_toplevel->app_id ? toplevel->xdg_toplevel->app_id : "");
    ipc_broadcast_event(server, "window_focus", event_data);
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
