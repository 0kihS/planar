#include "server.h"
#include "output.h"
#include "input.h"
#include "toplevel.h"
#include "popup.h"
#include "cursor.h"
#include "seat.h"

#include <unistd.h>
#include <assert.h>
#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/util/log.h>

void convert_scene_coords_to_global(struct planar_server *server, double *x, double *y) {
    *x += server->global_offset.x;
    *y += server->global_offset.y;
}

void convert_global_coords_to_scene(struct planar_server *server, double *x, double *y) {
    *x -= server->global_offset.x;
    *y -= server->global_offset.y;
}

static void server_new_input(struct wl_listener *listener, void *data) {
    struct planar_server *server = wl_container_of(listener, server, new_input);
    struct wlr_input_device *device = data;

    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        server_new_keyboard(server, device);
        break;
    case WLR_INPUT_DEVICE_POINTER:
        server_new_pointer(server, device);
        break;
    default:
        break;
    }

    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&server->keyboards)) {
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    wlr_seat_set_capabilities(server->seat, caps);
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

    wl_list_init(&server->outputs);
    server->new_output.notify = server_new_output;
    wl_signal_add(&server->backend->events.new_output, &server->new_output);

    server->output_layout = wlr_output_layout_create(server->wl_display);

    server->scene = wlr_scene_create();

    server->scene_layout = wlr_scene_attach_output_layout(server->scene, server->output_layout);

    server->xdg_shell = wlr_xdg_shell_create(server->wl_display, 3);
    assert(server->xdg_shell);

    wl_list_init(&server->toplevels);
    server->new_xdg_toplevel.notify = server_new_xdg_toplevel;
    wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->new_xdg_toplevel);

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

    server->keyboard_repeat_source = wl_event_loop_add_timer(
        wl_display_get_event_loop(server->wl_display),
        keyboard_repeat_func, server);

    seat_init(server);

    server->global_offset.x = 0;
    server->global_offset.y = 0;

    const char *socket = wl_display_add_socket_auto(server->wl_display);
    if (!socket) {
        wlr_log(WLR_ERROR, "Unable to create Wayland socket");
        return;
    }

    server->socket = socket;
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
    wlr_scene_node_destroy(&server->scene->tree.node);
    wlr_xcursor_manager_destroy(server->cursor_mgr);
    wlr_cursor_destroy(server->cursor);
    wlr_allocator_destroy(server->allocator);
    wlr_renderer_destroy(server->renderer);
    wlr_backend_destroy(server->backend);
    wl_display_destroy(server->wl_display);
    seat_finish(server);
}
