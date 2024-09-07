#ifndef PLANAR_SERVER_H
#define PLANAR_SERVER_H

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>


enum planar_cursor_mode {
    PLANAR_CURSOR_PASSTHROUGH,
    PLANAR_CURSOR_MOVE,
    PLANAR_CURSOR_RESIZE,
    PLANAR_CURSOR_PANNING,
};

struct planar_server {
	struct wl_display *wl_display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;

	struct wlr_xdg_shell *xdg_shell;
	struct wlr_layer_shell_v1 *layer_shell;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
	struct wl_list toplevels;

	struct wlr_scene_tree *layers[4];
    struct wl_listener new_layer_shell_surface;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;
	const char* socket;

	struct {
        bool left_pressed;
        bool right_pressed;
        bool up_pressed;
        bool down_pressed;
    } key_state;
    struct wl_event_source *keyboard_repeat_source;

	struct wlr_seat *seat;
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wl_listener request_set_selection;
	struct wl_list keyboards;
	enum planar_cursor_mode cursor_mode;
	struct planar_toplevel *grabbed_toplevel;
	double grab_x, grab_y;
	struct wlr_box grab_geobox;
	uint32_t resize_edges;

    struct {
        double x;
        double y;
    } global_offset;

	struct wlr_output_layout *output_layout;
	struct wl_list outputs;
	struct wl_listener new_output;
};

void convert_scene_coords_to_global(struct planar_server *server, double *x, double *y);
void convert_global_coords_to_scene(struct planar_server *server, double *x, double *y);

void server_init(struct planar_server *server);
void server_run(struct planar_server *server);
void server_finish(struct planar_server *server);

#endif // PLANAR_SERVER_H