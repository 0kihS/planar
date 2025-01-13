#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#include "keyboard.h"
#include "config.h"
#include "server.h"
#include "workspaces.h"
#include "toplevel.h"

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
    struct planar_keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
    struct planar_server *server = keyboard->server;
    
    wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
    wlr_seat_keyboard_notify_modifiers(server->seat, &keyboard->wlr_keyboard->modifiers);
}

static bool handle_keybinding(struct planar_server *server, xkb_keysym_t sym) {
    uint32_t modifiers = wlr_keyboard_get_modifiers(wlr_seat_get_keyboard(server->seat));
    
    if (!server->config) return false;
    
    for (int i = 0; i < server->config->num_keybindings; i++) {
        struct keybinding *bind = &server->config->keybindings[i];
        if (bind->modifiers == modifiers && bind->key == sym) {
            if (bind->is_internal) {
                return handle_internal_command(server, bind->command);
            } else {
                return handle_external_command(bind->command);
            }
        }
    }
    
    return false;
}

static bool handle_internal_command(struct planar_server *server, const char *cmd) {
    if (strncmp(cmd, "@workspace ", 10) == 0) {
        int workspace = atoi(cmd + 10);
        switch_to_workspace(server, workspace - 1);
        return true;
    }
    
    if (strcmp(cmd, "@killactive") == 0) {
        kill_active_toplevel(server);
        return true;
    }
    
    if (strncmp(cmd, "@move_to_workspace ", 18) == 0) {
        int workspace = atoi(cmd + 18);
        active_toplevel_to_workspace(server, workspace - 1);
        return true;
    }
    
    if (strncmp(cmd, "@move_workspace ", 15) == 0) {
        int offset_x, offset_y;
        char *args;
        strncpy(args, cmd + 15, sizeof(args) - 1);
        args[sizeof(args) - 1] = '\0';

        if (sscanf(args, "%d %d", &offset_x, &offset_y) == 2) {
            update_workspace_offset(server, offset_x, offset_y);
            return true;
            }
    
        return false;
    }

}

static bool handle_external_command(const char *cmd) {
    if (fork() == 0) {
        execl("/bin/sh", "/bin/sh", "-c", cmd, (void *)NULL);
        exit(0);
    }
    return true;
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
    struct planar_keyboard *keyboard = wl_container_of(listener, keyboard, key);
    struct planar_server *server = keyboard->server;
    struct wlr_keyboard_key_event *event = data;
    
    struct wlr_keyboard *wlr_keyboard = keyboard->wlr_keyboard;
    uint32_t keycode = event->keycode + 8;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(wlr_keyboard->xkb_state, keycode);

    bool handled = false;
    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        handled = handle_keybinding(server, sym);
    }

    if (!handled) {
        wlr_seat_set_keyboard(server->seat, wlr_keyboard);
        wlr_seat_keyboard_notify_key(server->seat, event->time_msec,
            event->keycode, event->state);
    }
}

void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
    struct planar_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
    wl_list_remove(&keyboard->modifiers.link);
    wl_list_remove(&keyboard->key.link);
    wl_list_remove(&keyboard->destroy.link);
    wl_list_remove(&keyboard->link);
    free(keyboard);
}

void server_new_keyboard(struct planar_server *server, struct wlr_input_device *device) {
    struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);
    struct planar_keyboard *keyboard = calloc(1, sizeof(*keyboard));
    
    keyboard->server = server;
    keyboard->wlr_keyboard = wlr_keyboard;

    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
        XKB_KEYMAP_COMPILE_NO_FLAGS);

    wlr_keyboard_set_keymap(wlr_keyboard, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    
    // Set up listeners
    keyboard->modifiers.notify = keyboard_handle_modifiers;
    wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
    
    keyboard->key.notify = keyboard_handle_key;
    wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
    
    keyboard->destroy.notify = keyboard_handle_destroy;
    wl_signal_add(&device->events.destroy, &keyboard->destroy);

    wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
    
    wl_list_insert(&server->keyboards, &keyboard->link);
}