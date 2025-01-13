#ifndef PLANAR_KEYBOARD_H
#define PLANAR_KEYBOARD_H

#include <stdbool.h>
#include <xkbcommon/xkbcommon.h>
#include <wlr/types/wlr_keyboard.h>

#include "server.h"

struct planar_keyboard {
    struct wl_list link;
    struct planar_server *server;
    struct wlr_keyboard *wlr_keyboard;

    struct wl_listener modifiers;
    struct wl_listener key;
    struct wl_listener destroy;
};

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data);
static void keyboard_handle_key(struct wl_listener *listener, void *data);
void keyboard_handle_destroy(struct wl_listener *listener, void *data);
void server_new_keyboard(struct planar_server *server, struct wlr_input_device *device);
static bool handle_internal_command(struct planar_server *server, const char *cmd);
static bool handle_external_command(const char *cmd);

#endif