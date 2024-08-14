#ifndef PLANAR_INPUT_H
#define PLANAR_INPUT_H

#include "server.h"

struct planar_keyboard {
    struct wl_list link;
    struct planar_server *server;
    struct wlr_keyboard *wlr_keyboard;

    struct wl_listener modifiers;
    struct wl_listener key;
    struct wl_listener destroy;
};

int keyboard_repeat_func(void *data);


static void keyboard_handle_modifiers(struct wl_listener *listener, void *data);
static void keyboard_handle_key(struct wl_listener *listener, void *data);
void keyboard_handle_destroy(struct wl_listener *listener, void *data);
void server_new_keyboard(struct planar_server *server, struct wlr_input_device *device);
void server_new_pointer(struct planar_server *server, struct wlr_input_device *device);
static void server_new_input(struct wl_listener *listener, void *data);

#endif // PLANAR_INPUT_H