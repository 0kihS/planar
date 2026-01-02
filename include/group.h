#ifndef PLANAR_GROUP_H
#define PLANAR_GROUP_H

#include <wayland-server-core.h>
#include <stdbool.h>

struct planar_server;
struct planar_toplevel;
struct planar_workspace;

struct planar_group {
    struct wl_list link;           // in server->groups
    struct wl_list members;        // list of planar_group_member
    struct planar_server *server;  // back-reference for IPC events
    char *group_id;                // unique identifier
    uint32_t instance_number;      // for generating IDs
    float border_color[4];         // visual indicator color (RGBA)
};

struct planar_group_member {
    struct wl_list link;           // in group->members
    struct planar_toplevel *toplevel;
    double offset_x, offset_y;     // offset from group anchor (first member)
};

// Group lifecycle
struct planar_group *group_create(struct planar_server *server, const char *group_id);
void group_destroy(struct planar_group *group);

// Membership
bool group_add_toplevel(struct planar_group *group, struct planar_toplevel *toplevel);
bool group_remove_toplevel(struct planar_group *group, struct planar_toplevel *toplevel);
void group_remove_toplevel_from_any(struct planar_server *server, struct planar_toplevel *toplevel);

// Queries
struct planar_group *find_group_by_id(struct planar_server *server, const char *group_id);
struct planar_group_member *group_find_member(struct planar_group *group, struct planar_toplevel *toplevel);
struct planar_toplevel *group_get_anchor(struct planar_group *group);
bool group_is_empty(struct planar_group *group);
int group_member_count(struct planar_group *group);

// Movement - moves all members together
void group_move_by(struct planar_group *group, double delta_x, double delta_y);
void group_move_to(struct planar_group *group, double x, double y);

// Move entire group to a different workspace
void group_move_to_workspace(struct planar_group *group, struct planar_workspace *target_ws);

// Recalculate offsets (call after anchor changes)
void group_recalculate_offsets(struct planar_group *group);

// Visual updates
void group_update_decorations(struct planar_group *group);

// Bounds
void group_get_bounds(struct planar_group *group,
                      double *out_x, double *out_y,
                      int *out_width, int *out_height);

#endif // PLANAR_GROUP_H
