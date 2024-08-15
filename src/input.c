#include "input.h"
#include "cursor.h"
#include "output.h"
#include <stdlib.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_input_device.h>
#include <xkbcommon/xkbcommon.h>

#define KEY_REPEAT_DELAY 400
#define KEY_REPEAT_RATE 40

int keyboard_repeat_func(void *data) {
    struct planar_server *server = data;
    float move_step = 10.0f;

    if (server->key_state.left_pressed) {
        server->global_offset.x += move_step;
    }
    if (server->key_state.right_pressed) {
        server->global_offset.x -= move_step;
    }
    if (server->key_state.up_pressed) {
        server->global_offset.y += move_step;
    }
    if (server->key_state.down_pressed) {
        server->global_offset.y -= move_step;
    }

    if (server->key_state.left_pressed || server->key_state.right_pressed ||
        server->key_state.up_pressed || server->key_state.down_pressed) {
        wl_event_source_timer_update(server->keyboard_repeat_source, KEY_REPEAT_RATE);
        // Trigger a redraw
        struct planar_output *output;
        wl_list_for_each(output, &server->outputs, link) {
            wlr_output_schedule_frame(output->wlr_output);
        }
    } else {
        wl_event_source_timer_update(server->keyboard_repeat_source, 0);
    }

    return 0;
}

static void keyboard_handle_modifiers(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a modifier key, such as shift or alt, is
	 * pressed. We simply communicate this to the client. */
	struct planar_keyboard *keyboard =
		wl_container_of(listener, keyboard, modifiers);
	/*
	 * A seat can only have one keyboard, but this is a limitation of the
	 * Wayland protocol - not wlroots. We assign all connected keyboards to the
	 * same seat. You can swap out the underlying wlr_keyboard like this and
	 * wlr_seat handles this transparently.
	 */
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
	/* Send modifiers to the client. */
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
		&keyboard->wlr_keyboard->modifiers);
}

static bool handle_keybinding(struct planar_server *server, xkb_keysym_t sym) {
	/*
	 * Here we handle compositor keybindings. This is when the compositor is
	 * processing keys, rather than passing them on to the client for its own
	 * processing.
	 *
	 * This function assumes Alt is held down.
	 */
	float move_step = 10.0f;
	switch (sym) {
	case XKB_KEY_Escape:
		wl_display_terminate(server->wl_display);
		break;
	case XKB_KEY_F1:
		/* Cycle to the next toplevel */
		if (wl_list_length(&server->toplevels) < 2) {
			break;
		}
        case XKB_KEY_Left:
            server->key_state.left_pressed = true;
            break;
        case XKB_KEY_Right:
            server->key_state.right_pressed = true;
            break;
        case XKB_KEY_Up:
            server->key_state.up_pressed = true;
            break;
        case XKB_KEY_Down:
            server->key_state.down_pressed = true;
            break;
            // Add more cases for additional navigation keys if needed
        }
	}

static void keyboard_handle_key(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a key is pressed or released. */
	struct planar_keyboard *keyboard =
		wl_container_of(listener, keyboard, key);
	struct planar_server *server = keyboard->server;
	struct wlr_keyboard_key_event *event = data;
	struct wlr_seat *seat = server->seat;

	/* Translate libinput keycode -> xkbcommon */
	uint32_t keycode = event->keycode + 8;
	/* Get a list of keysyms based on the keymap for this keyboard */
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(
			keyboard->wlr_keyboard->xkb_state, keycode, &syms);

	bool handled = false;
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
	if ((modifiers & WLR_MODIFIER_ALT) &&
			event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		/* If alt is held down and this button was _pressed_, we attempt to
		 * process it as a compositor keybinding. */
		for (int i = 0; i < nsyms; i++) {
			handled = handle_keybinding(server, syms[i]);
		}
	}
	else if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        for (int i = 0; i < nsyms; i++) {
            switch (syms[i]) {
                case XKB_KEY_Left:
                    server->key_state.left_pressed = false;
                    break;
                case XKB_KEY_Right:
                    server->key_state.right_pressed = false;
                    break;
                case XKB_KEY_Up:
                    server->key_state.up_pressed = false;
                    break;
                case XKB_KEY_Down:
                    server->key_state.down_pressed = false;
                    break;
            }
        }
	}
	if (handled) {
        wl_event_source_timer_update(server->keyboard_repeat_source, KEY_REPEAT_DELAY);
        keyboard_repeat_func(server);
    }


	if (!handled) {
		/* Otherwise, we pass it along to the client. */
		wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
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
    wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

    keyboard->modifiers.notify = keyboard_handle_modifiers;
    wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
    keyboard->key.notify = keyboard_handle_key;
    wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
    keyboard->destroy.notify = keyboard_handle_destroy;
    wl_signal_add(&device->events.destroy, &keyboard->destroy);

    wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);

    wl_list_insert(&server->keyboards, &keyboard->link);
}

void server_new_pointer(struct planar_server *server, struct wlr_input_device *device) {
    wlr_cursor_attach_input_device(server->cursor, device);
}

void server_new_input(struct wl_listener *listener, void *data) {
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