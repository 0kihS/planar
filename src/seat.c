#include "seat.h"
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_seat.h>

static void seat_request_cursor(struct wl_listener *listener, void *data) {
    struct planar_server *server = wl_container_of(
            listener, server, request_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *event = data;
    struct wlr_seat_client *focused_client =
        server->seat->pointer_state.focused_client;

    if (focused_client == event->seat_client) {
        wlr_cursor_set_surface(server->cursor, event->surface,
                event->hotspot_x, event->hotspot_y);
    }
}

static void seat_request_cursor_shape(struct wl_listener *listener, void *data) {
    struct planar_server *server = wl_container_of(
            listener, server, request_cursor_shape);
    struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
    struct wlr_seat_client *focused_client =
        server->seat->pointer_state.focused_client;

    if (event->device_type != WLR_CURSOR_SHAPE_MANAGER_V1_DEVICE_TYPE_POINTER) {
        return;
    }

    if (focused_client == event->seat_client) {
        const char *cursor_name = wlr_cursor_shape_v1_name(event->shape);
        if (cursor_name) {
            wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, cursor_name);
        }
    }
}

static void seat_request_set_selection(struct wl_listener *listener, void *data) {
    struct planar_server *server = wl_container_of(
            listener, server, request_set_selection);
    struct wlr_seat_request_set_selection_event *event = data;
    wlr_seat_set_selection(server->seat, event->source, event->serial);
}

void seat_init(struct planar_server *server) {
    server->seat = wlr_seat_create(server->wl_display, "seat0");
    server->cursor_shape_mgr =
        wlr_cursor_shape_manager_v1_create(server->wl_display, 1);

    server->request_cursor.notify = seat_request_cursor;
    wl_signal_add(&server->seat->events.request_set_cursor,
            &server->request_cursor);

    if (server->cursor_shape_mgr) {
        server->request_cursor_shape.notify = seat_request_cursor_shape;
        wl_signal_add(&server->cursor_shape_mgr->events.request_set_shape,
                &server->request_cursor_shape);
    }

    server->request_set_selection.notify = seat_request_set_selection;
    wl_signal_add(&server->seat->events.request_set_selection,
            &server->request_set_selection);
}

void seat_finish(struct planar_server *server) {
    wl_list_remove(&server->request_cursor.link);
    if (server->request_cursor_shape.link.prev != NULL) {
        wl_list_remove(&server->request_cursor_shape.link);
    }
    wl_list_remove(&server->request_set_selection.link);
    // The wlr_seat is destroyed when the display is destroyed
}
