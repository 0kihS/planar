#include "server.h"
#include "config.h"
#include "cursor.h"
#include "input.h"
#include "ipc.h"
#include "layers.h"
#include "output.h"
#include "popup.h"
#include "scenefx/render/fx_renderer/fx_renderer.h"
#include "seat.h"
#include "snap.h"
#include "toplevel.h"
#include "workspaces.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <scenefx/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/util/log.h>

void convert_scene_coords_to_global(struct planar_server *server, double *x, double *y) {
    *x += server->active_workspace->global_offset.x;
    *y += server->active_workspace->global_offset.y;
}

void convert_global_coords_to_scene(struct planar_server *server, double *x, double *y) {
    *x -= server->active_workspace->global_offset.x;
    *y -= server->active_workspace->global_offset.y;
}

static bool app_id_matches(const char *pattern, const char *app_id) {
    if (!pattern || !app_id) return false;
    return strcmp(pattern, app_id) == 0;
}

bool window_rules_has_nodecoration(struct planar_server *server, const char *app_id) {
    if (!app_id) return false;
    for (size_t i = 0; i < server->window_rules.nodecoration_count; i++) {
        if (app_id_matches(server->window_rules.nodecoration[i], app_id)) {
            return true;
        }
    }
    return false;
}

bool window_rules_has_ontop(struct planar_server *server, const char *app_id) {
    if (!app_id) return false;
    for (size_t i = 0; i < server->window_rules.ontop_count; i++) {
        if (app_id_matches(server->window_rules.ontop[i], app_id)) {
            return true;
        }
    }
    return false;
}

bool window_rules_add_nodecoration(struct planar_server *server, const char *app_id) {
    if (!app_id || window_rules_has_nodecoration(server, app_id)) return false;

    if (server->window_rules.nodecoration_count >= server->window_rules.nodecoration_capacity) {
        size_t new_cap = server->window_rules.nodecoration_capacity == 0 ? 8 : server->window_rules.nodecoration_capacity * 2;
        char **new_arr = realloc(server->window_rules.nodecoration, new_cap * sizeof(char *));
        if (!new_arr) return false;
        server->window_rules.nodecoration = new_arr;
        server->window_rules.nodecoration_capacity = new_cap;
    }

    server->window_rules.nodecoration[server->window_rules.nodecoration_count++] = strdup(app_id);
    return true;
}

bool window_rules_remove_nodecoration(struct planar_server *server, const char *app_id) {
    if (!app_id) return false;
    for (size_t i = 0; i < server->window_rules.nodecoration_count; i++) {
        if (app_id_matches(server->window_rules.nodecoration[i], app_id)) {
            free(server->window_rules.nodecoration[i]);
            for (size_t j = i; j < server->window_rules.nodecoration_count - 1; j++) {
                server->window_rules.nodecoration[j] = server->window_rules.nodecoration[j + 1];
            }
            server->window_rules.nodecoration_count--;
            return true;
        }
    }
    return false;
}

bool window_rules_add_ontop(struct planar_server *server, const char *app_id) {
    if (!app_id || window_rules_has_ontop(server, app_id)) return false;

    if (server->window_rules.ontop_count >= server->window_rules.ontop_capacity) {
        size_t new_cap = server->window_rules.ontop_capacity == 0 ? 8 : server->window_rules.ontop_capacity * 2;
        char **new_arr = realloc(server->window_rules.ontop, new_cap * sizeof(char *));
        if (!new_arr) return false;
        server->window_rules.ontop = new_arr;
        server->window_rules.ontop_capacity = new_cap;
    }

    server->window_rules.ontop[server->window_rules.ontop_count++] = strdup(app_id);
    return true;
}

bool window_rules_remove_ontop(struct planar_server *server, const char *app_id) {
    if (!app_id) return false;
    for (size_t i = 0; i < server->window_rules.ontop_count; i++) {
        if (app_id_matches(server->window_rules.ontop[i], app_id)) {
            free(server->window_rules.ontop[i]);
            for (size_t j = i; j < server->window_rules.ontop_count - 1; j++) {
                server->window_rules.ontop[j] = server->window_rules.ontop[j + 1];
            }
            server->window_rules.ontop_count--;
            return true;
        }
    }
    return false;
}

struct decoration_listeners {
    struct wl_listener request_mode;
    struct wl_listener destroy;
};

static void decoration_handle_request_mode(struct wl_listener *listener, void *data) {
    (void)listener;
    struct wlr_xdg_toplevel_decoration_v1 *decoration = data;
    if (decoration->toplevel->base->initialized) {
        wlr_xdg_toplevel_decoration_v1_set_mode(decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
}

static void decoration_handle_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct decoration_listeners *listeners = wl_container_of(listener, listeners, destroy);
    wl_list_remove(&listeners->request_mode.link);
    wl_list_remove(&listeners->destroy.link);
    free(listeners);
}

static void server_new_toplevel_decoration(struct wl_listener *listener, void *data) {
    (void)listener;
    struct wlr_xdg_toplevel_decoration_v1 *decoration = data;

    struct decoration_listeners *listeners = calloc(1, sizeof(*listeners));

    listeners->request_mode.notify = decoration_handle_request_mode;
    wl_signal_add(&decoration->events.request_mode, &listeners->request_mode);

    listeners->destroy.notify = decoration_handle_destroy;
    wl_signal_add(&decoration->events.destroy, &listeners->destroy);
}

static void server_new_output(struct wl_listener *listener, void *data) {
    struct planar_server *server = wl_container_of(listener, server, new_output);
    struct wlr_output *wlr_output = data;

    wlr_output_init_render(wlr_output, server->allocator, server->renderer);

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);

    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    if (mode != NULL) {
        wlr_output_state_set_mode(&state, mode);
    }

    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    output_create(listener, wlr_output);
}

void server_init(struct planar_server *server) {
    wl_list_init(&server->ipc_clients);
    wl_list_init(&server->ipc_event_clients);
    server->ipc_socket = -1;
    server->ipc_event_socket = -1;

    server->wl_display = wl_display_create();
    server->backend = wlr_backend_autocreate(wl_display_get_event_loop(server->wl_display), NULL);
    server->renderer = fx_renderer_create(server->backend);
    wlr_renderer_init_wl_display(server->renderer, server->wl_display);

    server->allocator = wlr_allocator_autocreate(server->backend, server->renderer);

    wlr_compositor_create(server->wl_display, 5, server->renderer);
    wlr_subcompositor_create(server->wl_display);
    wlr_data_device_manager_create(server->wl_display);
    wlr_screencopy_manager_v1_create(server->wl_display);

    wl_list_init(&server->outputs);
    server->new_output.notify = server_new_output;
    wl_signal_add(&server->backend->events.new_output, &server->new_output);

    server->output_layout = wlr_output_layout_create(server->wl_display);

    server->scene = wlr_scene_create();

    wlr_xdg_output_manager_v1_create(server->wl_display, server->output_layout);

    wlr_viewporter_create(server->wl_display);

    server->scene_layout = wlr_scene_attach_output_layout(server->scene, server->output_layout);

    for (int i = 0; i < 4; i++) {
        server->layers[i] = wlr_scene_tree_create(&server->scene->tree);
    }

    server->xdg_shell = wlr_xdg_shell_create(server->wl_display, 3);
    assert(server->xdg_shell);

    server->xdg_decoration_manager = wlr_xdg_decoration_manager_v1_create(server->wl_display);

    server->new_toplevel_decoration.notify = server_new_toplevel_decoration;
    wl_signal_add(&server->xdg_decoration_manager->events.new_toplevel_decoration, &server->new_toplevel_decoration);

    server->layer_shell = wlr_layer_shell_v1_create(server->wl_display, 4);

    server->new_layer_shell_surface.notify = server_layer_shell_surface;
    wl_signal_add(&server->layer_shell->events.new_surface, &server->new_layer_shell_surface);

    server->new_xdg_toplevel.notify = server_new_xdg_toplevel;
    wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->new_xdg_toplevel);

    server->config = config_load(NULL);
    if (!server->config) {
        wlr_log(WLR_INFO, "No config file found, using defaults");
    }

    wl_list_init(&server->workspaces);
    wl_list_init(&server->groups);
    wl_list_init(&server->selected_toplevels);
    server->selection_box = NULL;
    for (int i = 0; i < WORKSPACE_COUNT; i++) {
        struct planar_workspace *ws = calloc(1, sizeof(*ws));
        ws->index = i;
        ws->scale = 1.0;
        ws->scene_tree = wlr_scene_tree_create(server->layers[1]);
        wlr_scene_node_set_enabled(&ws->scene_tree->node, i == 0);
        wl_list_init(&ws->toplevels);
        wl_list_insert(&server->workspaces, &ws->link);
    }
    server->active_workspace = wl_container_of(server->workspaces.next, server->active_workspace, link);

    switch_to_workspace(server, 0);
    server->new_xdg_popup.notify = server_new_xdg_popup;
    wl_signal_add(&server->xdg_shell->events.new_popup, &server->new_xdg_popup);

    server->cursor = wlr_cursor_create();
    assert(server->cursor);
    wlr_cursor_attach_output_layout(server->cursor, server->output_layout);

    server->cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
    assert(server->cursor_mgr);

    server->cursor_mode = PLANAR_CURSOR_PASSTHROUGH;

    cursor_init(server);

    wl_list_init(&server->keyboards);
    server->new_input.notify = server_new_input;
    wl_signal_add(&server->backend->events.new_input, &server->new_input);

    seat_init(server);

    struct planar_workspace *workspace;
    wl_list_for_each(workspace, &server->workspaces, link) {
        if (workspace->index == 0) {
            server->active_workspace = workspace;
        }

        workspace->global_offset.x = 0;
        workspace->global_offset.y = 0;
    }

    const char *socket = wl_display_add_socket_auto(server->wl_display);
    if (!socket) {
        wlr_log(WLR_ERROR, "Unable to create Wayland socket");
        return;
    }

    server->socket = socket;

    /* Initialize default settings */
    server->settings.border_width = 4;
    server->settings.border_color[0] = 0.3f;
    server->settings.border_color[1] = 0.3f;
    server->settings.border_color[2] = 0.3f;
    server->settings.border_color[3] = 1.0f;
    server->settings.zoom_min = 0.1;
    server->settings.zoom_max = 5.0;
    server->settings.zoom_step = 0.1;
    server->settings.cursor_size = 24;
    server->settings.snap_enabled = true;
    server->settings.snap_threshold = 10;

    /* Initialize snap system */
    snap_init(server);

    /* Initialize window ID tracker */
    server->window_id_tracker.app_ids = NULL;
    server->window_id_tracker.counters = NULL;
    server->window_id_tracker.count = 0;
    server->window_id_tracker.capacity = 0;

    /* Initialize window rules */
    server->window_rules.nodecoration = NULL;
    server->window_rules.nodecoration_count = 0;
    server->window_rules.nodecoration_capacity = 0;
    server->window_rules.ontop = NULL;
    server->window_rules.ontop_count = 0;
    server->window_rules.ontop_capacity = 0;

    /* Apply window rules from config */
    if (server->config) {
        for (size_t i = 0; i < server->config->nodecoration_count; i++) {
            window_rules_add_nodecoration(server, server->config->nodecoration[i]);
        }
        for (size_t i = 0; i < server->config->ontop_count; i++) {
            window_rules_add_ontop(server, server->config->ontop[i]);
        }
    }

    /* Initialize IPC */
    if (!ipc_init(server)) {
        wlr_log(WLR_ERROR, "Failed to initialize IPC");
    }

    switch_to_workspace(server, 0);

    if (server->config && server->config->startup_cmd) {
        handle_external_command(server->config->startup_cmd);
    }
}

void server_run(struct planar_server *server) {

    if (!wlr_backend_start(server->backend)) {
        wlr_log(WLR_ERROR, "Unable to start backend");
        return;
    }
    wl_display_run(server->wl_display);
}

void server_finish(struct planar_server *server) {
    snap_finish(server);
    ipc_finish(server);
    wl_display_destroy_clients(server->wl_display);
    wl_list_remove(&server->new_input.link);
    wl_list_remove(&server->new_output.link);
    wl_list_remove(&server->new_xdg_toplevel.link);
    wl_list_remove(&server->new_xdg_popup.link);
    wl_list_remove(&server->new_layer_shell_surface.link);

    /* Cleanup window ID tracker */
    for (size_t i = 0; i < server->window_id_tracker.count; i++) {
        free(server->window_id_tracker.app_ids[i]);
    }
    free(server->window_id_tracker.app_ids);
    free(server->window_id_tracker.counters);

    /* Cleanup window rules */
    for (size_t i = 0; i < server->window_rules.nodecoration_count; i++) {
        free(server->window_rules.nodecoration[i]);
    }
    free(server->window_rules.nodecoration);
    for (size_t i = 0; i < server->window_rules.ontop_count; i++) {
        free(server->window_rules.ontop[i]);
    }
    free(server->window_rules.ontop);

    struct planar_workspace *workspace, *tmp_ws;
    wl_list_for_each_safe(workspace, tmp_ws, &server->workspaces, link) {
        wl_list_remove(&workspace->link);
        free(workspace);
    }
    if (server->keyboard_repeat_source) {
        wl_event_source_remove(server->keyboard_repeat_source);
    }
    if (server->config) {
        config_destroy(server->config);
    }
    seat_finish(server);
    wlr_scene_node_destroy(&server->scene->tree.node);
    wlr_xcursor_manager_destroy(server->cursor_mgr);
    wlr_cursor_destroy(server->cursor);
    wlr_output_layout_destroy(server->output_layout);
    wlr_allocator_destroy(server->allocator);
    wlr_renderer_destroy(server->renderer);
    wlr_backend_destroy(server->backend);
    wl_display_destroy(server->wl_display);
}
