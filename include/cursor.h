#ifndef PLANAR_CURSOR_H
#define PLANAR_CURSOR_H

#include "server.h"
#include <wayland-server-core.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>

void cursor_init(struct planar_server *server);
void cursor_destroy(struct planar_server *server);

void process_cursor_motion(struct planar_server *server, double cx, double cy, uint32_t time);
void process_cursor_move(struct planar_server *server, uint32_t time);
void process_cursor_resize(struct planar_server *server, uint32_t time);

static void server_cursor_motion(struct wl_listener *listener, void *data);
static void server_cursor_motion_absolute(struct wl_listener *listener, void *data);
static void server_cursor_button(struct wl_listener *listener, void *data);
static void server_cursor_axis(struct wl_listener *listener, void *data);
static void server_cursor_frame(struct wl_listener *listener, void *data);

void reset_cursor_mode(struct planar_server *server);

#endif // PLANAR_CURSOR_H