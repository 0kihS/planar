#ifndef PLANAR_SELECTION_H
#define PLANAR_SELECTION_H

#include <wayland-server-core.h>
#include <stdbool.h>

struct planar_server;
struct planar_toplevel;
struct planar_group;

struct planar_selection_entry {
    struct wl_list link;
    struct planar_toplevel *toplevel;
};

bool selection_add(struct planar_server *server, struct planar_toplevel *toplevel);
bool selection_remove(struct planar_server *server, struct planar_toplevel *toplevel);
void selection_toggle(struct planar_server *server, struct planar_toplevel *toplevel);
void selection_clear(struct planar_server *server);

bool selection_contains(struct planar_server *server, struct planar_toplevel *toplevel);
int selection_count(struct planar_server *server);
const float *selection_get_color(void);

void selection_move_by(struct planar_server *server, double delta_x, double delta_y);
struct planar_group *selection_create_group(struct planar_server *server);

void selection_start_box(struct planar_server *server, double x, double y);
void selection_update_box(struct planar_server *server, double x, double y);
void selection_finish_box(struct planar_server *server);
void selection_cancel_box(struct planar_server *server);

void selection_get_bounds(struct planar_server *server,
                          double *out_x, double *out_y,
                          int *out_width, int *out_height);

#endif // PLANAR_SELECTION_H
