#ifndef PLANAR_POPUP_H
#define PLANAR_POPUP_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_xdg_shell.h>

struct planar_server;

struct planar_popup {
    struct wlr_xdg_popup *xdg_popup;
    struct wl_listener commit;
    struct wl_listener destroy;
};

void xdg_popup_commit(struct wl_listener *listener, void *data);
void xdg_popup_destroy(struct wl_listener *listener, void *data);
void server_new_xdg_popup(struct wl_listener *listener, void *data);

#endif // PLANAR_POPUP_H