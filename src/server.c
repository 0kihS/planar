#include "server.h"
#include "output.h"
#include "input.h"
#include "toplevel.h"
#include "popup.h"
#include "cursor.h"
#include "seat.h"
#include "layers.h"
#include "workspaces.h"
#include "config.h"
#include "keyboard.h"

#include <unistd.h>
#include <assert.h>
#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/util/log.h>

void convert_scene_coords_to_global(struct planar_server *server, double *x, double *y) {
    *x += server->active_workspace->global_offset.x;
    *y += server->active_workspace->global_offset.y;
}

void convert_global_coords_to_scene(struct planar_server *server, double *x, double *y) {
    *x -= server->active_workspace->global_offset.x;
    *y -= server->active_workspace->global_offset.y;
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
    server->wl_display = wl_display_create();
    server->backend = wlr_backend_autocreate(wl_display_get_event_loop(server->wl_display), NULL);
    server->renderer = wlr_renderer_autocreate(server->backend);
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

    server->workspace_content_tree = wlr_scene_tree_create(server->layers[1]);

    server->xdg_shell = wlr_xdg_shell_create(server->wl_display, 3);
    assert(server->xdg_shell);

    server->xdg_decoration_manager = wlr_xdg_decoration_manager_v1_create(server->wl_display);

    //server->new_toplevel_decoration.notify = server_new_toplevel_decoration;
    //wl_signal_add(&server->xdg_decoration_manager->events.new_toplevel_decoration, &server->new_toplevel_decoration);

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
    for (int i = 0; i < WORKSPACE_COUNT; i++) {
        struct planar_workspace *ws = calloc(1, sizeof(*ws));
        ws->index = i;
        ws->scale = 1.0;
        ws->scene_tree = wlr_scene_tree_create(server->layers[1]);
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

    switch_to_workspace(server, 0);

    if (server->config->startup_cmd) {
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
    wl_display_destroy_clients(server->wl_display);
    wl_list_remove(&server->new_input.link);
    wl_list_remove(&server->new_output.link);
    wl_list_remove(&server->new_xdg_toplevel.link);
    wl_list_remove(&server->new_xdg_popup.link);
    wl_list_remove(&server->new_layer_shell_surface.link);
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
