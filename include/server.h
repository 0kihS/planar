#ifndef PLANAR_SERVER_H
#define PLANAR_SERVER_H

#include <stdbool.h>
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
#include <scenefx/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>


struct planar_snap_state;

enum planar_cursor_mode {
    PLANAR_CURSOR_PASSTHROUGH,
    PLANAR_CURSOR_MOVE,
    PLANAR_CURSOR_RESIZE,
    PLANAR_CURSOR_PANNING,
    PLANAR_CURSOR_DRAG_PENDING,
    PLANAR_CURSOR_BOX_SELECT,
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

	struct wlr_xdg_decoration_manager_v1 *xdg_decoration_manager;
	struct wl_listener new_toplevel_decoration;

	struct wlr_scene_tree *layers[4];
    struct wl_listener new_layer_shell_surface;

    struct wl_list workspaces;
    struct planar_workspace *active_workspace;

    struct wl_list groups;

    struct planar_snap_state *snap_state;

	struct wl_list selected_toplevels;
    struct wlr_scene_rect *selection_box;
    double selection_box_start_x, selection_box_start_y;

    struct config *config;

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
	double grab_workspace_x, grab_workspace_y;
	struct wlr_box grab_geobox;
	uint32_t resize_edges;

	struct wlr_output_layout *output_layout;
	struct wl_list outputs;
	struct wl_listener new_output;

	int ipc_socket;
	int ipc_event_socket;
	struct wl_event_source *ipc_event_source;
	struct wl_event_source *ipc_event_socket_source;
	struct wl_list ipc_clients;
	struct wl_list ipc_event_clients;

	struct {
		int border_width;
		float border_color[4];
		double zoom_min;
		double zoom_max;
		double zoom_step;
		int cursor_size;
		bool snap_enabled;
		int snap_threshold;
	} settings;

	struct {
		char **nodecoration;
		size_t nodecoration_count;
		size_t nodecoration_capacity;
		char **ontop;
		size_t ontop_count;
		size_t ontop_capacity;
	} window_rules;

	struct {
		char **app_ids;
		uint32_t *counters;
		size_t count;
		size_t capacity;
	} window_id_tracker;
};

void convert_scene_coords_to_global(struct planar_server *server, double *x, double *y);
void convert_global_coords_to_scene(struct planar_server *server, double *x, double *y);

bool window_rules_has_nodecoration(struct planar_server *server, const char *app_id);
bool window_rules_has_ontop(struct planar_server *server, const char *app_id);
bool window_rules_add_nodecoration(struct planar_server *server, const char *app_id);
bool window_rules_remove_nodecoration(struct planar_server *server, const char *app_id);
bool window_rules_add_ontop(struct planar_server *server, const char *app_id);
bool window_rules_remove_ontop(struct planar_server *server, const char *app_id);

void server_init(struct planar_server *server);
void server_run(struct planar_server *server);
void server_finish(struct planar_server *server);

#endif // PLANAR_SERVER_H
