#include "toplevel.h"
#include "server.h"
#include "workspaces.h"
#include "decoration.h"
#include "ipc.h"
#include "group.h"
#include "selection.h"

#include <scenefx/types/wlr_scene.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static void remove_listener(struct wl_listener *listener) {
    if (listener->link.prev != NULL) {
        wl_list_remove(&listener->link);
        listener->link.prev = NULL;
        listener->link.next = NULL;
    }
}

static void insert_workspace_toplevel(struct planar_toplevel *toplevel) {
    if (!toplevel->listed && toplevel->workspace) {
        wl_list_insert(&toplevel->workspace->toplevels, &toplevel->link);
        toplevel->listed = true;
    }
}

static void remove_workspace_toplevel(struct planar_toplevel *toplevel) {
    if (toplevel->listed) {
        wl_list_remove(&toplevel->link);
        toplevel->link.prev = NULL;
        toplevel->link.next = NULL;
        toplevel->listed = false;
    }
}

static int get_configure_border(struct planar_toplevel *toplevel) {
    const char *app_id = toplevel_get_app_id(toplevel);
    return window_rules_has_nodecoration(toplevel->server, app_id) ? 0 : toplevel->server->settings.border_width;
}

#if WLR_HAS_XWAYLAND
static struct planar_workspace *get_xwayland_workspace(struct planar_toplevel *toplevel) {
    if (toplevel->workspace) {
        return toplevel->workspace;
    }

    return toplevel->server->active_workspace;
}

static void workspace_coords_to_global(struct planar_workspace *workspace,
        double logical_x, double logical_y, double *global_x, double *global_y) {
    if (!workspace) {
        *global_x = logical_x;
        *global_y = logical_y;
        return;
    }

    *global_x = logical_x * workspace->scale + workspace->global_offset.x;
    *global_y = logical_y * workspace->scale + workspace->global_offset.y;
}

static void global_coords_to_workspace(struct planar_workspace *workspace,
        double global_x, double global_y, double *logical_x, double *logical_y) {
    if (!workspace || workspace->scale == 0.0) {
        *logical_x = global_x;
        *logical_y = global_y;
        return;
    }

    *logical_x = (global_x - workspace->global_offset.x) / workspace->scale;
    *logical_y = (global_y - workspace->global_offset.y) / workspace->scale;
}

static void get_xwayland_configure_position(struct planar_toplevel *toplevel, int *x, int *y) {
    int border = toplevel->decoration ? toplevel->server->settings.border_width : get_configure_border(toplevel);
    double global_x, global_y;
    workspace_coords_to_global(get_xwayland_workspace(toplevel),
        toplevel->logical_x + border, toplevel->logical_y + border,
        &global_x, &global_y);
    *x = (int)lround(global_x);
    *y = (int)lround(global_y);
}

static void configure_xwayland_toplevel(struct planar_toplevel *toplevel,
        int width, int height) {
    if (!toplevel || width <= 0 || height <= 0) {
        return;
    }

    int x, y;
    get_xwayland_configure_position(toplevel, &x, &y);
    wlr_xwayland_surface_configure(toplevel->xwayland_surface, x, y, width, height);
}
#endif

static void toplevel_emit_open_event(struct planar_toplevel *toplevel) {
    const char *app_id = toplevel_get_app_id(toplevel);
    const char *title = toplevel_get_title(toplevel);
    struct wlr_box geo;
    toplevel_get_geometry(toplevel, &geo);
    int width = toplevel->decoration ? toplevel->decoration->width : geo.width;
    int height = toplevel->decoration ? toplevel->decoration->height : geo.height;

    char event_data[512];
    snprintf(event_data, sizeof(event_data),
        "{\"id\":\"%s\",\"app_id\":\"%s\",\"title\":\"%s\",\"workspace\":%d,"
        "\"geometry\":{\"x\":%.0f,\"y\":%.0f,\"width\":%d,\"height\":%d}}",
        toplevel->window_id ? toplevel->window_id : "",
        app_id ? app_id : "",
        title ? title : "",
        toplevel->workspace ? toplevel->workspace->index + 1 : 0,
        toplevel->logical_x, toplevel->logical_y,
        width, height);

    ipc_broadcast_event(toplevel->server, "window_open", event_data);
}

static void toplevel_emit_close_event(struct planar_toplevel *toplevel) {
    char event_data[256];
    snprintf(event_data, sizeof(event_data), "{\"id\":\"%s\"}",
        toplevel->window_id ? toplevel->window_id : "");
    ipc_broadcast_event(toplevel->server, "window_close", event_data);
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

struct planar_toplevel *find_toplevel_by_surface(struct planar_server *server, struct wlr_surface *surface) {
    if (!surface) {
        return NULL;
    }

    struct wlr_surface *root_surface = wlr_surface_get_root_surface(surface);
    struct planar_workspace *ws;
    wl_list_for_each(ws, &server->workspaces, link) {
        struct planar_toplevel *toplevel;
        wl_list_for_each(toplevel, &ws->toplevels, link) {
            if (toplevel_get_surface(toplevel) == root_surface) {
                return toplevel;
            }
        }
    }
    return NULL;
}

struct wlr_surface *toplevel_get_surface(struct planar_toplevel *toplevel) {
    if (!toplevel) {
        return NULL;
    }

    switch (toplevel->type) {
    case PLANAR_TOPLEVEL_XDG:
        return toplevel->xdg_toplevel->base->surface;
    case PLANAR_TOPLEVEL_XWAYLAND:
#if WLR_HAS_XWAYLAND
        return toplevel->xwayland_surface->surface;
#else
        return NULL;
#endif
    }

    return NULL;
}

const char *toplevel_get_app_id(struct planar_toplevel *toplevel) {
    if (!toplevel) {
        return NULL;
    }

    switch (toplevel->type) {
    case PLANAR_TOPLEVEL_XDG:
        return toplevel->xdg_toplevel->app_id;
    case PLANAR_TOPLEVEL_XWAYLAND:
#if WLR_HAS_XWAYLAND
        if (toplevel->xwayland_surface->class && toplevel->xwayland_surface->class[0] != '\0') {
            return toplevel->xwayland_surface->class;
        }
        return toplevel->xwayland_surface->instance;
#else
        return NULL;
#endif
    }

    return NULL;
}

const char *toplevel_get_title(struct planar_toplevel *toplevel) {
    if (!toplevel) {
        return NULL;
    }

    switch (toplevel->type) {
    case PLANAR_TOPLEVEL_XDG:
        return toplevel->xdg_toplevel->title;
    case PLANAR_TOPLEVEL_XWAYLAND:
#if WLR_HAS_XWAYLAND
        return toplevel->xwayland_surface->title;
#else
        return NULL;
#endif
    }

    return NULL;
}

bool toplevel_is_mapped(struct planar_toplevel *toplevel) {
    return toplevel != NULL && toplevel->mapped;
}

void toplevel_get_geometry(struct planar_toplevel *toplevel, struct wlr_box *box) {
    memset(box, 0, sizeof(*box));

    if (!toplevel) {
        return;
    }

    switch (toplevel->type) {
    case PLANAR_TOPLEVEL_XDG:
        *box = toplevel->xdg_toplevel->base->geometry;
        break;
    case PLANAR_TOPLEVEL_XWAYLAND:
#if WLR_HAS_XWAYLAND
        box->width = toplevel->xwayland_surface->width;
        box->height = toplevel->xwayland_surface->height;
        break;
#else
        return;
#endif
    }

    if ((box->width <= 0 || box->height <= 0) && toplevel_get_surface(toplevel)) {
        struct wlr_surface *surface = toplevel_get_surface(toplevel);
        box->width = surface->current.width;
        box->height = surface->current.height;
    }

    if ((box->width <= 0 || box->height <= 0) && toplevel->base_width > 0 && toplevel->base_height > 0) {
        box->width = toplevel->base_width;
        box->height = toplevel->base_height;
    }
}

void toplevel_set_size(struct planar_toplevel *toplevel, int width, int height) {
    if (!toplevel || width <= 0 || height <= 0) {
        return;
    }

    switch (toplevel->type) {
    case PLANAR_TOPLEVEL_XDG:
        wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, width, height);
        break;
    case PLANAR_TOPLEVEL_XWAYLAND:
#if WLR_HAS_XWAYLAND
        configure_xwayland_toplevel(toplevel, width, height);
        break;
#else
        break;
#endif
    }
}

void toplevel_close(struct planar_toplevel *toplevel) {
    if (!toplevel) {
        return;
    }

    switch (toplevel->type) {
    case PLANAR_TOPLEVEL_XDG:
        wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
        break;
    case PLANAR_TOPLEVEL_XWAYLAND:
#if WLR_HAS_XWAYLAND
        wlr_xwayland_surface_close(toplevel->xwayland_surface);
        break;
#else
        break;
#endif
    }
}

void move_toplevel(struct planar_toplevel *toplevel, double x, double y) {
    if (!toplevel || !toplevel->container) {
        return;
    }

    toplevel->logical_x = x;
    toplevel->logical_y = y;
    wlr_scene_node_set_position(&toplevel->container->node, (int)x, (int)y);

#if WLR_HAS_XWAYLAND
    if (toplevel->type == PLANAR_TOPLEVEL_XWAYLAND) {
        struct wlr_box geo;
        toplevel_get_geometry(toplevel, &geo);
        if (geo.width > 0 && geo.height > 0) {
            configure_xwayland_toplevel(toplevel, geo.width, geo.height);
        }
    }
#endif
}

static void set_surface_activated(struct wlr_surface *surface, bool activated) {
    if (!surface) {
        return;
    }

    struct wlr_xdg_toplevel *xdg_toplevel = wlr_xdg_toplevel_try_from_wlr_surface(surface);
    if (xdg_toplevel != NULL) {
        wlr_xdg_toplevel_set_activated(xdg_toplevel, activated);
        return;
    }

#if WLR_HAS_XWAYLAND
    struct wlr_xwayland_surface *xwayland_surface = wlr_xwayland_surface_try_from_wlr_surface(surface);
    if (xwayland_surface != NULL) {
        if (activated) {
            if (xwayland_surface->override_redirect) {
                if (wlr_xwayland_surface_override_redirect_wants_focus(xwayland_surface)) {
                    wlr_xwayland_surface_offer_focus(xwayland_surface);
                }
            } else if (wlr_xwayland_surface_icccm_input_model(xwayland_surface) == WLR_ICCCM_INPUT_MODEL_GLOBAL) {
                wlr_xwayland_surface_offer_focus(xwayland_surface);
            } else {
                wlr_xwayland_surface_activate(xwayland_surface, true);
            }
        } else if (!xwayland_surface->override_redirect) {
            wlr_xwayland_surface_activate(xwayland_surface, false);
        }
    }
#endif
}

static void focus_surface(struct planar_server *server, struct wlr_surface *surface) {
    struct wlr_surface *root_surface = surface ? wlr_surface_get_root_surface(surface) : NULL;
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;

    if (prev_surface == root_surface) {
        return;
    }

    if (prev_surface) {
        set_surface_activated(prev_surface, false);
    }

    if (root_surface == NULL) {
        wlr_seat_keyboard_notify_clear_focus(seat);
        return;
    }

    set_surface_activated(root_surface, true);

    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
    if (keyboard != NULL) {
        wlr_seat_keyboard_notify_enter(seat, root_surface,
            keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
    }
}

static void begin_interactive(struct planar_toplevel *toplevel,
        enum planar_cursor_mode mode, uint32_t edges) {
    struct planar_server *server = toplevel->server;
    struct wlr_surface *focused_surface = server->seat->pointer_state.focused_surface;
    struct wlr_surface *window_surface = toplevel_get_surface(toplevel);

    if (!window_surface || window_surface != wlr_surface_get_root_surface(focused_surface)) {
        return;
    }

    server->grabbed_toplevel = toplevel;
    server->cursor_mode = mode;

    float total_scale = 1.0f;
    struct wlr_scene_node *it = toplevel->container->node.parent ? &toplevel->container->node.parent->node : NULL;
    while (it) {
        total_scale *= it->scale;
        it = it->parent ? &it->parent->node : NULL;
    }

    if (mode == PLANAR_CURSOR_MOVE) {
        server->grab_x = server->cursor->x - (toplevel->container->node.x * total_scale);
        server->grab_y = server->cursor->y - (toplevel->container->node.y * total_scale);
    } else {
        struct wlr_box geo_box;
        toplevel_get_geometry(toplevel, &geo_box);

        double border_x = toplevel->container->node.x +
            ((edges & WLR_EDGE_RIGHT) ? geo_box.width : 0);
        double border_y = toplevel->container->node.y +
            ((edges & WLR_EDGE_BOTTOM) ? geo_box.height : 0);
        server->grab_x = server->cursor->x - (border_x * total_scale);
        server->grab_y = server->cursor->y - (border_y * total_scale);

        int border = server->settings.border_width;
        server->grab_geobox = geo_box;
        server->grab_geobox.x = toplevel->container->node.x + border;
        server->grab_geobox.y = toplevel->container->node.y + border;

        server->resize_edges = edges;
    }
}

static void maybe_apply_fixed_size(struct planar_toplevel *toplevel) {
    if (!toplevel) {
        return;
    }

    struct wlr_box geo;
    toplevel_get_geometry(toplevel, &geo);

    switch (toplevel->type) {
    case PLANAR_TOPLEVEL_XDG:
    {
        if (toplevel->xdg_toplevel->base->initial_commit) {
            wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
        }

        struct wlr_xdg_toplevel_state *state = &toplevel->xdg_toplevel->current;
        if (state->min_width > 0 && state->min_height > 0 &&
            state->min_width == state->max_width &&
            state->min_height == state->max_height &&
            (geo.width != state->min_width || geo.height != state->min_height)) {
            wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
                state->min_width, state->min_height);
        }
        break;
    }
    case PLANAR_TOPLEVEL_XWAYLAND:
#if WLR_HAS_XWAYLAND
        if (toplevel->xwayland_surface->size_hints &&
            toplevel->xwayland_surface->size_hints->min_width > 0 &&
            toplevel->xwayland_surface->size_hints->min_height > 0 &&
            toplevel->xwayland_surface->size_hints->min_width == toplevel->xwayland_surface->size_hints->max_width &&
            toplevel->xwayland_surface->size_hints->min_height == toplevel->xwayland_surface->size_hints->max_height &&
            (geo.width != toplevel->xwayland_surface->size_hints->min_width ||
             geo.height != toplevel->xwayland_surface->size_hints->min_height)) {
            toplevel_set_size(toplevel,
                toplevel->xwayland_surface->size_hints->min_width,
                toplevel->xwayland_surface->size_hints->min_height);
        }
        break;
#else
        break;
#endif
    }
}

static void position_new_toplevel(struct planar_toplevel *toplevel, struct wlr_box *geo_box) {
    if (toplevel->type == PLANAR_TOPLEVEL_XWAYLAND) {
#if WLR_HAS_XWAYLAND
        if (toplevel->logical_x == 0 && toplevel->logical_y == 0 &&
            (toplevel->xwayland_surface->x != 0 || toplevel->xwayland_surface->y != 0)) {
            int border = get_configure_border(toplevel);
            double logical_x, logical_y;
            global_coords_to_workspace(get_xwayland_workspace(toplevel),
                toplevel->xwayland_surface->x, toplevel->xwayland_surface->y,
                &logical_x, &logical_y);
            toplevel->logical_x = logical_x - border;
            toplevel->logical_y = logical_y - border;
        }
#endif
    }

    if (toplevel->logical_x != 0 || toplevel->logical_y != 0) {
        wlr_scene_node_set_position(&toplevel->container->node,
            (int)toplevel->logical_x, (int)toplevel->logical_y);
        return;
    }

    struct planar_server *server = toplevel->server;
    struct wlr_output *output = wlr_output_layout_output_at(
        server->output_layout, server->cursor->x, server->cursor->y);
    if (!output) {
        return;
    }

    struct wlr_box output_box;
    wlr_output_layout_get_box(server->output_layout, output, &output_box);

    double new_x = ((output_box.width / 2.0) - server->active_workspace->global_offset.x) / server->active_workspace->scale;
    double new_y = ((output_box.height / 2.0) - server->active_workspace->global_offset.y) / server->active_workspace->scale;

    toplevel->logical_x = new_x - (geo_box->width / 2.0);
    toplevel->logical_y = new_y - (geo_box->height / 2.0);

    wlr_scene_node_set_position(&toplevel->container->node,
        (int)toplevel->logical_x, (int)toplevel->logical_y);
}

static void handle_toplevel_map(struct planar_toplevel *toplevel) {
    struct planar_server *server = toplevel->server;
    insert_workspace_toplevel(toplevel);
    toplevel->mapped = true;

    if (toplevel->scene_tree) {
        wlr_scene_node_set_enabled(&toplevel->scene_tree->node, true);
    }

    free(toplevel->window_id);
    toplevel->window_id = generate_window_id(server, toplevel_get_app_id(toplevel));

    struct wlr_box geo_box;
    toplevel_get_geometry(toplevel, &geo_box);
    if (geo_box.width <= 0) {
        geo_box.width = 640;
    }
    if (geo_box.height <= 0) {
        geo_box.height = 480;
    }

    toplevel->base_width = geo_box.width;
    toplevel->base_height = geo_box.height;

    position_new_toplevel(toplevel, &geo_box);

    if (!window_rules_has_nodecoration(server, toplevel_get_app_id(toplevel))) {
        toplevel->decoration = decoration_create(toplevel);
        if (toplevel->decoration) {
            decoration_update_geometry(toplevel->decoration);
        }
    }

    scale_toplevel(toplevel);

#if WLR_HAS_XWAYLAND
    if (toplevel->type == PLANAR_TOPLEVEL_XWAYLAND) {
        toplevel_set_size(toplevel, geo_box.width, geo_box.height);
    }
#endif

    focus_toplevel(toplevel, toplevel_get_surface(toplevel));
    toplevel_emit_open_event(toplevel);
}

static void handle_toplevel_unmap(struct planar_toplevel *toplevel) {
    toplevel->mapped = false;
    toplevel_emit_close_event(toplevel);

    if (toplevel->scene_tree) {
        wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);
    }

    if (toplevel->server->grabbed_toplevel == toplevel) {
        toplevel->server->grabbed_toplevel = NULL;
        toplevel->server->cursor_mode = PLANAR_CURSOR_PASSTHROUGH;
    }

    if (toplevel->decoration) {
        decoration_destroy(toplevel->decoration);
        toplevel->decoration = NULL;
    }

    remove_workspace_toplevel(toplevel);
}

static void handle_toplevel_commit(struct planar_toplevel *toplevel) {
    struct planar_server *server = toplevel->server;

    maybe_apply_fixed_size(toplevel);

    struct wlr_box geo;
    toplevel_get_geometry(toplevel, &geo);

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

static void handle_toplevel_destroy(struct planar_toplevel *toplevel) {
    selection_remove(toplevel->server, toplevel);
    group_remove_toplevel_from_any(toplevel->server, toplevel);

    if (toplevel->decoration) {
        decoration_destroy(toplevel->decoration);
        toplevel->decoration = NULL;
    }

    remove_workspace_toplevel(toplevel);

    remove_listener(&toplevel->map);
    remove_listener(&toplevel->unmap);
    remove_listener(&toplevel->commit);
    remove_listener(&toplevel->destroy);
    remove_listener(&toplevel->request_move);
    remove_listener(&toplevel->request_resize);
    remove_listener(&toplevel->request_maximize);
    remove_listener(&toplevel->request_fullscreen);
#if WLR_HAS_XWAYLAND
    remove_listener(&toplevel->associate);
    remove_listener(&toplevel->dissociate);
    remove_listener(&toplevel->request_configure);
    remove_listener(&toplevel->request_activate);
    remove_listener(&toplevel->request_close);
#endif

    free(toplevel->window_id);
    free(toplevel);
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
    (void)data;
    struct planar_toplevel *toplevel = wl_container_of(listener, toplevel, map);
    handle_toplevel_map(toplevel);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
    (void)data;
    struct planar_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
    handle_toplevel_unmap(toplevel);
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
    (void)data;
    struct planar_toplevel *toplevel = wl_container_of(listener, toplevel, commit);
    handle_toplevel_commit(toplevel);
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct planar_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);
    handle_toplevel_destroy(toplevel);
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

static void xdg_toplevel_maximize(struct wl_listener *listener, void *data) {
    (void)data;
    struct planar_toplevel *toplevel = wl_container_of(listener, toplevel, request_maximize);
    if (toplevel->xdg_toplevel->base->initialized) {
        wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
    }
}

static void xdg_toplevel_fullscreen(struct wl_listener *listener, void *data) {
    (void)data;
    struct planar_toplevel *toplevel = wl_container_of(listener, toplevel, request_fullscreen);
    if (toplevel->xdg_toplevel->base->initialized) {
        wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
    }
}

#if WLR_HAS_XWAYLAND
struct planar_xwayland_unmanaged {
    struct planar_server *server;
    struct wlr_xwayland_surface *xwayland_surface;
    struct wlr_scene_tree *scene_tree;

    struct wl_listener associate;
    struct wl_listener dissociate;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener destroy;
    struct wl_listener request_configure;
    struct wl_listener request_activate;
    struct wl_listener set_geometry;
};

static void unmanaged_xwayland_map(struct wl_listener *listener, void *data);
static void unmanaged_xwayland_unmap(struct wl_listener *listener, void *data);

static struct planar_toplevel *find_xwayland_toplevel(struct planar_server *server,
        struct wlr_xwayland_surface *surface) {
    struct planar_workspace *ws;
    wl_list_for_each(ws, &server->workspaces, link) {
        struct planar_toplevel *toplevel;
        wl_list_for_each(toplevel, &ws->toplevels, link) {
            if (toplevel->type == PLANAR_TOPLEVEL_XWAYLAND &&
                toplevel->xwayland_surface == surface) {
                return toplevel;
            }
        }
    }
    return NULL;
}

static void unmanaged_xwayland_update_position(struct planar_xwayland_unmanaged *unmanaged) {
    if (!unmanaged->scene_tree) {
        return;
    }

    struct planar_toplevel *parent_toplevel = NULL;
    if (unmanaged->xwayland_surface->parent) {
        parent_toplevel = find_xwayland_toplevel(unmanaged->server, unmanaged->xwayland_surface->parent);
    }

    struct wlr_scene_tree *parent_tree = parent_toplevel ? parent_toplevel->container : unmanaged->server->active_workspace->scene_tree;
    wlr_scene_node_reparent(&unmanaged->scene_tree->node, parent_tree);

    struct planar_workspace *workspace = parent_toplevel ? parent_toplevel->workspace : unmanaged->server->active_workspace;
    double logical_x, logical_y;
    global_coords_to_workspace(workspace,
        unmanaged->xwayland_surface->x, unmanaged->xwayland_surface->y,
        &logical_x, &logical_y);

    int x = (int)lround(logical_x);
    int y = (int)lround(logical_y);
    if (parent_toplevel) {
        x -= (int)parent_toplevel->logical_x;
        y -= (int)parent_toplevel->logical_y;
    }

    wlr_scene_node_set_position(&unmanaged->scene_tree->node, x, y);
}

static void unmanaged_xwayland_associate(struct wl_listener *listener, void *data) {
    (void)data;
    struct planar_xwayland_unmanaged *unmanaged = wl_container_of(listener, unmanaged, associate);

    unmanaged->scene_tree = wlr_scene_subsurface_tree_create(unmanaged->server->active_workspace->scene_tree,
        unmanaged->xwayland_surface->surface);
    wlr_scene_node_set_enabled(&unmanaged->scene_tree->node, false);
    unmanaged_xwayland_update_position(unmanaged);

    unmanaged->map.notify = unmanaged_xwayland_map;
    wl_signal_add(&unmanaged->xwayland_surface->surface->events.map, &unmanaged->map);
    unmanaged->unmap.notify = unmanaged_xwayland_unmap;
    wl_signal_add(&unmanaged->xwayland_surface->surface->events.unmap, &unmanaged->unmap);
}

static void unmanaged_xwayland_dissociate(struct wl_listener *listener, void *data) {
    (void)data;
    struct planar_xwayland_unmanaged *unmanaged = wl_container_of(listener, unmanaged, dissociate);

    remove_listener(&unmanaged->map);
    remove_listener(&unmanaged->unmap);

    if (unmanaged->scene_tree) {
        wlr_scene_node_destroy(&unmanaged->scene_tree->node);
        unmanaged->scene_tree = NULL;
    }
}

static void unmanaged_xwayland_map(struct wl_listener *listener, void *data) {
    (void)data;
    struct planar_xwayland_unmanaged *unmanaged = wl_container_of(listener, unmanaged, map);
    unmanaged_xwayland_update_position(unmanaged);
    wlr_scene_node_set_enabled(&unmanaged->scene_tree->node, true);
    wlr_scene_node_raise_to_top(&unmanaged->scene_tree->node);

    if (wlr_xwayland_surface_override_redirect_wants_focus(unmanaged->xwayland_surface)) {
        focus_surface(unmanaged->server, unmanaged->xwayland_surface->surface);
    }
}

static void unmanaged_xwayland_unmap(struct wl_listener *listener, void *data) {
    (void)data;
    struct planar_xwayland_unmanaged *unmanaged = wl_container_of(listener, unmanaged, unmap);
    if (unmanaged->scene_tree) {
        wlr_scene_node_set_enabled(&unmanaged->scene_tree->node, false);
    }
}

static void unmanaged_xwayland_request_configure(struct wl_listener *listener, void *data) {
    struct planar_xwayland_unmanaged *unmanaged = wl_container_of(listener, unmanaged, request_configure);
    struct wlr_xwayland_surface_configure_event *event = data;
    wlr_xwayland_surface_configure(unmanaged->xwayland_surface,
        event->x, event->y, event->width, event->height);
    unmanaged_xwayland_update_position(unmanaged);
}

static void unmanaged_xwayland_request_activate(struct wl_listener *listener, void *data) {
    (void)data;
    struct planar_xwayland_unmanaged *unmanaged = wl_container_of(listener, unmanaged, request_activate);
    if (wlr_xwayland_surface_override_redirect_wants_focus(unmanaged->xwayland_surface)) {
        focus_surface(unmanaged->server, unmanaged->xwayland_surface->surface);
    }
}

static void unmanaged_xwayland_set_geometry(struct wl_listener *listener, void *data) {
    (void)data;
    struct planar_xwayland_unmanaged *unmanaged = wl_container_of(listener, unmanaged, set_geometry);
    unmanaged_xwayland_update_position(unmanaged);
}

static void unmanaged_xwayland_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct planar_xwayland_unmanaged *unmanaged = wl_container_of(listener, unmanaged, destroy);

    remove_listener(&unmanaged->associate);
    remove_listener(&unmanaged->dissociate);
    remove_listener(&unmanaged->map);
    remove_listener(&unmanaged->unmap);
    remove_listener(&unmanaged->destroy);
    remove_listener(&unmanaged->request_configure);
    remove_listener(&unmanaged->request_activate);
    remove_listener(&unmanaged->set_geometry);

    if (unmanaged->scene_tree) {
        wlr_scene_node_destroy(&unmanaged->scene_tree->node);
    }

    free(unmanaged);
}

static void create_unmanaged_xwayland_surface(struct planar_server *server,
        struct wlr_xwayland_surface *xwayland_surface) {
    struct planar_xwayland_unmanaged *unmanaged = calloc(1, sizeof(*unmanaged));
    if (!unmanaged) {
        return;
    }

    unmanaged->server = server;
    unmanaged->xwayland_surface = xwayland_surface;

    unmanaged->associate.notify = unmanaged_xwayland_associate;
    wl_signal_add(&xwayland_surface->events.associate, &unmanaged->associate);
    unmanaged->dissociate.notify = unmanaged_xwayland_dissociate;
    wl_signal_add(&xwayland_surface->events.dissociate, &unmanaged->dissociate);
    unmanaged->destroy.notify = unmanaged_xwayland_destroy;
    wl_signal_add(&xwayland_surface->events.destroy, &unmanaged->destroy);
    unmanaged->request_configure.notify = unmanaged_xwayland_request_configure;
    wl_signal_add(&xwayland_surface->events.request_configure, &unmanaged->request_configure);
    unmanaged->request_activate.notify = unmanaged_xwayland_request_activate;
    wl_signal_add(&xwayland_surface->events.request_activate, &unmanaged->request_activate);
    unmanaged->set_geometry.notify = unmanaged_xwayland_set_geometry;
    wl_signal_add(&xwayland_surface->events.set_geometry, &unmanaged->set_geometry);
}

static void xwayland_toplevel_associate(struct wl_listener *listener, void *data) {
    (void)data;
    struct planar_toplevel *toplevel = wl_container_of(listener, toplevel, associate);

    if (toplevel->scene_tree) {
        wlr_scene_node_destroy(&toplevel->scene_tree->node);
    }

    toplevel->scene_tree = wlr_scene_subsurface_tree_create(toplevel->container,
        toplevel->xwayland_surface->surface);
    wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);

    int border = toplevel->decoration ? toplevel->server->settings.border_width : get_configure_border(toplevel);
    wlr_scene_node_set_position(&toplevel->scene_tree->node, border, border);

    toplevel->map.notify = xdg_toplevel_map;
    wl_signal_add(&toplevel->xwayland_surface->surface->events.map, &toplevel->map);
    toplevel->unmap.notify = xdg_toplevel_unmap;
    wl_signal_add(&toplevel->xwayland_surface->surface->events.unmap, &toplevel->unmap);
    toplevel->commit.notify = xdg_toplevel_commit;
    wl_signal_add(&toplevel->xwayland_surface->surface->events.commit, &toplevel->commit);
}

static void xwayland_toplevel_dissociate(struct wl_listener *listener, void *data) {
    (void)data;
    struct planar_toplevel *toplevel = wl_container_of(listener, toplevel, dissociate);

    remove_listener(&toplevel->map);
    remove_listener(&toplevel->unmap);
    remove_listener(&toplevel->commit);

    if (toplevel->scene_tree) {
        wlr_scene_node_destroy(&toplevel->scene_tree->node);
        toplevel->scene_tree = NULL;
    }
}

static void xwayland_toplevel_request_configure(struct wl_listener *listener, void *data) {
    struct planar_toplevel *toplevel = wl_container_of(listener, toplevel, request_configure);
    struct wlr_xwayland_surface_configure_event *event = data;

    if (!toplevel->mapped && toplevel->logical_x == 0 && toplevel->logical_y == 0 &&
        (event->x != 0 || event->y != 0)) {
        int border = get_configure_border(toplevel);
        double logical_x, logical_y;
        global_coords_to_workspace(get_xwayland_workspace(toplevel),
            event->x, event->y, &logical_x, &logical_y);
        toplevel->logical_x = logical_x - border;
        toplevel->logical_y = logical_y - border;
    }

    int width = event->width;
    int height = event->height;
    if (width <= 0 || height <= 0) {
        struct wlr_box geo;
        toplevel_get_geometry(toplevel, &geo);
        width = geo.width;
        height = geo.height;
    }

    configure_xwayland_toplevel(toplevel, width, height);
}

static void xwayland_toplevel_request_activate(struct wl_listener *listener, void *data) {
    (void)data;
    struct planar_toplevel *toplevel = wl_container_of(listener, toplevel, request_activate);
    focus_toplevel(toplevel, toplevel_get_surface(toplevel));
}

static void xwayland_toplevel_request_close(struct wl_listener *listener, void *data) {
    (void)listener;
    (void)data;
}

static void xwayland_toplevel_resize(struct wl_listener *listener, void *data) {
    struct wlr_xwayland_resize_event *event = data;
    struct planar_toplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
    begin_interactive(toplevel, PLANAR_CURSOR_RESIZE, event->edges);
}

static void xwayland_toplevel_move(struct wl_listener *listener, void *data) {
    (void)data;
    struct planar_toplevel *toplevel = wl_container_of(listener, toplevel, request_move);
    begin_interactive(toplevel, PLANAR_CURSOR_MOVE, 0);
}

static void xwayland_toplevel_maximize(struct wl_listener *listener, void *data) {
    (void)data;
    struct planar_toplevel *toplevel = wl_container_of(listener, toplevel, request_maximize);
    wlr_xwayland_surface_set_maximized(toplevel->xwayland_surface,
        toplevel->xwayland_surface->maximized_horz,
        toplevel->xwayland_surface->maximized_vert);
}

static void xwayland_toplevel_fullscreen(struct wl_listener *listener, void *data) {
    (void)data;
    struct planar_toplevel *toplevel = wl_container_of(listener, toplevel, request_fullscreen);
    wlr_xwayland_surface_set_fullscreen(toplevel->xwayland_surface,
        toplevel->xwayland_surface->fullscreen);
}
#endif

void scale_toplevel(struct planar_toplevel *toplevel) {
    if (!toplevel || !toplevel->container) {
        return;
    }

    wlr_scene_node_set_position(&toplevel->container->node,
        (int)toplevel->logical_x, (int)toplevel->logical_y);

    if (toplevel->scene_tree) {
        int border = toplevel->decoration ? toplevel->server->settings.border_width : 0;
        wlr_scene_node_set_position(&toplevel->scene_tree->node, border, border);
    }

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
    toplevel->type = PLANAR_TOPLEVEL_XDG;
    toplevel->xdg_toplevel = xdg_toplevel;
    toplevel->workspace = workspace;
    toplevel->container = wlr_scene_tree_create(workspace->scene_tree);
    toplevel->container->node.data = toplevel;
    toplevel->scene_tree = wlr_scene_xdg_surface_create(toplevel->container, xdg_toplevel->base);

    int border = server->settings.border_width;
    wlr_scene_node_set_position(&toplevel->scene_tree->node, border, border);

    xdg_toplevel->base->data = toplevel->container;

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

#if WLR_HAS_XWAYLAND
void server_new_xwayland_surface(struct wl_listener *listener, void *data) {
    struct planar_server *server = wl_container_of(listener, server, new_xwayland_surface);
    struct wlr_xwayland_surface *xwayland_surface = data;

    if (xwayland_surface->override_redirect) {
        create_unmanaged_xwayland_surface(server, xwayland_surface);
        return;
    }

    struct planar_toplevel *toplevel = calloc(1, sizeof(*toplevel));
    if (!toplevel) {
        return;
    }

    toplevel->server = server;
    toplevel->type = PLANAR_TOPLEVEL_XWAYLAND;
    toplevel->xwayland_surface = xwayland_surface;
    toplevel->workspace = server->active_workspace;
    toplevel->container = wlr_scene_tree_create(toplevel->workspace->scene_tree);
    toplevel->container->node.data = toplevel;
    xwayland_surface->data = toplevel;

    toplevel->associate.notify = xwayland_toplevel_associate;
    wl_signal_add(&xwayland_surface->events.associate, &toplevel->associate);
    toplevel->dissociate.notify = xwayland_toplevel_dissociate;
    wl_signal_add(&xwayland_surface->events.dissociate, &toplevel->dissociate);
    toplevel->destroy.notify = xdg_toplevel_destroy;
    wl_signal_add(&xwayland_surface->events.destroy, &toplevel->destroy);
    toplevel->request_move.notify = xwayland_toplevel_move;
    wl_signal_add(&xwayland_surface->events.request_move, &toplevel->request_move);
    toplevel->request_resize.notify = xwayland_toplevel_resize;
    wl_signal_add(&xwayland_surface->events.request_resize, &toplevel->request_resize);
    toplevel->request_maximize.notify = xwayland_toplevel_maximize;
    wl_signal_add(&xwayland_surface->events.request_maximize, &toplevel->request_maximize);
    toplevel->request_fullscreen.notify = xwayland_toplevel_fullscreen;
    wl_signal_add(&xwayland_surface->events.request_fullscreen, &toplevel->request_fullscreen);
    toplevel->request_configure.notify = xwayland_toplevel_request_configure;
    wl_signal_add(&xwayland_surface->events.request_configure, &toplevel->request_configure);
    toplevel->request_activate.notify = xwayland_toplevel_request_activate;
    wl_signal_add(&xwayland_surface->events.request_activate, &toplevel->request_activate);
    toplevel->request_close.notify = xwayland_toplevel_request_close;
    wl_signal_add(&xwayland_surface->events.request_close, &toplevel->request_close);
}
#endif

void focus_toplevel(struct planar_toplevel *toplevel, struct wlr_surface *surface) {
    if (toplevel == NULL) {
        return;
    }

    struct planar_server *server = toplevel->server;
    struct wlr_surface *root_surface = surface ? wlr_surface_get_root_surface(surface) : toplevel_get_surface(toplevel);
    if (!root_surface) {
        return;
    }

    struct wlr_surface *prev_surface = server->seat->keyboard_state.focused_surface;
    if (prev_surface == root_surface) {
        return;
    }

    wlr_scene_node_raise_to_top(&toplevel->container->node);

    if (toplevel->listed) {
        wl_list_remove(&toplevel->link);
        wl_list_insert(&toplevel->workspace->toplevels, &toplevel->link);
    }

    struct planar_toplevel *t;
    wl_list_for_each(t, &server->active_workspace->toplevels, link) {
        const char *app_id = toplevel_get_app_id(t);
        if (t != toplevel && app_id && window_rules_has_ontop(server, app_id)) {
            wlr_scene_node_raise_to_top(&t->container->node);
        }
    }

    focus_surface(server, root_surface);

    char event_data[256];
    snprintf(event_data, sizeof(event_data), "{\"id\":\"%s\",\"app_id\":\"%s\"}",
        toplevel->window_id ? toplevel->window_id : "",
        toplevel_get_app_id(toplevel) ? toplevel_get_app_id(toplevel) : "");
    ipc_broadcast_event(server, "window_focus", event_data);
}

void kill_active_toplevel(struct planar_server *server) {
    struct planar_toplevel *toplevel;
    wl_list_for_each_reverse(toplevel, &server->active_workspace->toplevels, link) {
        if (toplevel_get_surface(toplevel) == server->seat->keyboard_state.focused_surface) {
            toplevel_close(toplevel);
            return;
        }
    }
}
