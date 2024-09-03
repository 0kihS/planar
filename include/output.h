#ifndef PLANAR_OUTPUT_H
#define PLANAR_OUTPUT_H

#include "server.h"

struct planar_output {
    struct wl_list link;
    struct planar_server *server;
    struct wlr_output *wlr_output;
    struct wl_listener frame;
    struct wl_listener request_state;
    struct wl_listener destroy;

    struct wl_list layer_views;

    struct wlr_box usable_area;
};

void output_frame(struct wl_listener *listener, void *data);
void output_request_state(struct wl_listener *listener, void *data);
void output_destroy(struct wl_listener *listener, void *data);
void output_create(struct wl_listener *listener, void *data);

#endif // PLANAR_OUTPUT_H