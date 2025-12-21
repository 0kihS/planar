#ifndef PLANAR_CURSOR_H
#define PLANAR_CURSOR_H

#include "server.h"
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>

void server_new_pointer(struct planar_server *server, struct wlr_input_device *device);
void cursor_init(struct planar_server *server);
void cursor_destroy(struct planar_server *server);

void process_cursor_motion(struct planar_server *server, double cx, double cy, uint32_t time);
void process_cursor_move(struct planar_server *server, uint32_t time);
void process_cursor_resize(struct planar_server *server, uint32_t time);

void reset_cursor_mode(struct planar_server *server);

#endif // PLANAR_CURSOR_H