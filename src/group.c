#include "group.h"
#include "server.h"
#include "toplevel.h"
#include "decoration.h"
#include "workspaces.h"
#include "ipc.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static uint32_t next_group_id = 0;

struct planar_group *group_create(struct planar_server *server, const char *group_id) {
    struct planar_group *group = calloc(1, sizeof(*group));
    if (!group) {
        return NULL;
    }

    group->server = server;

    if (group_id) {
        group->group_id = strdup(group_id);
    } else {
        // Auto-generate an ID
        char buf[32];
        snprintf(buf, sizeof(buf), "group:%u", next_group_id++);
        group->group_id = strdup(buf);
    }

    if (!group->group_id) {
        free(group);
        return NULL;
    }

    wl_list_init(&group->members);

    // Default group color - a distinct teal/cyan
    group->border_color[0] = 0.2f;
    group->border_color[1] = 0.7f;
    group->border_color[2] = 0.8f;
    group->border_color[3] = 1.0f;

    wl_list_insert(&server->groups, &group->link);

    // Broadcast group_create event
    char event_data[512];
    snprintf(event_data, sizeof(event_data), "{\"id\":\"%s\"}", group->group_id);
    ipc_broadcast_event(server, "group_create", event_data);

    return group;
}

void group_destroy(struct planar_group *group) {
    if (!group) return;

    // Broadcast group_delete event before destroying
    if (group->server) {
        char event_data[512];
        snprintf(event_data, sizeof(event_data), "{\"id\":\"%s\"}", group->group_id);
        ipc_broadcast_event(group->server, "group_delete", event_data);
    }

    // Remove all members first
    struct planar_group_member *member, *tmp;
    wl_list_for_each_safe(member, tmp, &group->members, link) {
        if (member->toplevel) {
            member->toplevel->group = NULL;
            // Reset decoration color
            if (member->toplevel->decoration) {
                decoration_update_geometry(member->toplevel->decoration);
            }
        }
        wl_list_remove(&member->link);
        free(member);
    }

    wl_list_remove(&group->link);
    free(group->group_id);
    free(group);
}

bool group_add_toplevel(struct planar_group *group, struct planar_toplevel *toplevel) {
    if (!group || !toplevel) return false;

    // Already in a group? Remove from it first
    if (toplevel->group) {
        group_remove_toplevel(toplevel->group, toplevel);
    }

    struct planar_group_member *member = calloc(1, sizeof(*member));
    if (!member) return false;

    member->toplevel = toplevel;

    // Calculate offset from anchor (first member)
    struct planar_toplevel *anchor = group_get_anchor(group);
    if (anchor) {
        member->offset_x = toplevel->logical_x - anchor->logical_x;
        member->offset_y = toplevel->logical_y - anchor->logical_y;
    } else {
        // This is the first member (anchor), offset is 0
        member->offset_x = 0;
        member->offset_y = 0;
    }

    wl_list_insert(group->members.prev, &member->link);  // append to end
    toplevel->group = group;

    // Update decoration to show group color
    group_update_decorations(group);

    // Broadcast group_add event
    if (group->server && toplevel->window_id) {
        char event_data[512];
        snprintf(event_data, sizeof(event_data),
            "{\"group\":\"%s\",\"window\":\"%s\"}",
            group->group_id, toplevel->window_id);
        ipc_broadcast_event(group->server, "group_add", event_data);
    }

    return true;
}

bool group_remove_toplevel(struct planar_group *group, struct planar_toplevel *toplevel) {
    if (!group || !toplevel) return false;

    struct planar_group_member *member = group_find_member(group, toplevel);
    if (!member) return false;

    bool was_anchor = (member == wl_container_of(group->members.next, member, link));

    // Broadcast group_remove event before removing
    if (group->server && toplevel->window_id) {
        char event_data[512];
        snprintf(event_data, sizeof(event_data),
            "{\"group\":\"%s\",\"window\":\"%s\"}",
            group->group_id, toplevel->window_id);
        ipc_broadcast_event(group->server, "group_remove", event_data);
    }

    wl_list_remove(&member->link);
    toplevel->group = NULL;
    free(member);

    // Reset decoration color for removed window
    if (toplevel->decoration) {
        decoration_update_geometry(toplevel->decoration);
    }

    // If we removed the anchor, recalculate offsets for remaining members
    if (was_anchor && !group_is_empty(group)) {
        group_recalculate_offsets(group);
    }

    return true;
}

void group_remove_toplevel_from_any(struct planar_server *server, struct planar_toplevel *toplevel) {
    (void)server;  // May be used for future cleanup operations
    if (!toplevel || !toplevel->group) return;

    struct planar_group *group = toplevel->group;
    group_remove_toplevel(group, toplevel);

    // Destroy empty groups
    if (group_is_empty(group)) {
        group_destroy(group);
    }
}

struct planar_group *find_group_by_id(struct planar_server *server, const char *group_id) {
    if (!server || !group_id) return NULL;

    struct planar_group *group;
    wl_list_for_each(group, &server->groups, link) {
        if (strcmp(group->group_id, group_id) == 0) {
            return group;
        }
    }
    return NULL;
}

struct planar_group_member *group_find_member(struct planar_group *group, struct planar_toplevel *toplevel) {
    if (!group || !toplevel) return NULL;

    struct planar_group_member *member;
    wl_list_for_each(member, &group->members, link) {
        if (member->toplevel == toplevel) {
            return member;
        }
    }
    return NULL;
}

struct planar_toplevel *group_get_anchor(struct planar_group *group) {
    if (!group || wl_list_empty(&group->members)) return NULL;

    struct planar_group_member *first = wl_container_of(group->members.next, first, link);
    return first->toplevel;
}

bool group_is_empty(struct planar_group *group) {
    return !group || wl_list_empty(&group->members);
}

int group_member_count(struct planar_group *group) {
    if (!group) return 0;

    int count = 0;
    struct planar_group_member *member;
    wl_list_for_each(member, &group->members, link) {
        count++;
    }
    return count;
}

void group_move_by(struct planar_group *group, double delta_x, double delta_y) {
    if (!group) return;

    struct planar_group_member *member;
    wl_list_for_each(member, &group->members, link) {
        struct planar_toplevel *toplevel = member->toplevel;
        if (!toplevel) continue;

        toplevel->logical_x += delta_x;
        toplevel->logical_y += delta_y;

        // Update scene position
        double scale = toplevel->workspace ? toplevel->workspace->scale : 1.0;
        wlr_scene_node_set_position(&toplevel->container->node,
            toplevel->logical_x * scale,
            toplevel->logical_y * scale);
    }
}

void group_move_to(struct planar_group *group, double x, double y) {
    if (!group) return;

    struct planar_toplevel *anchor = group_get_anchor(group);
    if (!anchor) return;

    double delta_x = x - anchor->logical_x;
    double delta_y = y - anchor->logical_y;

    group_move_by(group, delta_x, delta_y);
}

void group_move_to_workspace(struct planar_group *group, struct planar_workspace *target_ws) {
    if (!group || !target_ws) return;

    struct planar_group_member *member;
    wl_list_for_each(member, &group->members, link) {
        struct planar_toplevel *toplevel = member->toplevel;
        if (!toplevel || toplevel->workspace == target_ws) continue;

        // Remove from current workspace's toplevel list
        wl_list_remove(&toplevel->link);

        // Add to target workspace's toplevel list
        wl_list_insert(&target_ws->toplevels, &toplevel->link);

        // Reparent in scene graph
        wlr_scene_node_reparent(&toplevel->container->node, target_ws->scene_tree);

        // Update workspace reference
        toplevel->workspace = target_ws;

        // Rescale for new workspace
        double scale = target_ws->scale;
        wlr_scene_node_set_position(&toplevel->container->node,
            toplevel->logical_x * scale,
            toplevel->logical_y * scale);
    }
}

void group_recalculate_offsets(struct planar_group *group) {
    if (!group || group_is_empty(group)) return;

    struct planar_toplevel *anchor = group_get_anchor(group);
    if (!anchor) return;

    struct planar_group_member *member;
    wl_list_for_each(member, &group->members, link) {
        if (member->toplevel == anchor) {
            member->offset_x = 0;
            member->offset_y = 0;
        } else {
            member->offset_x = member->toplevel->logical_x - anchor->logical_x;
            member->offset_y = member->toplevel->logical_y - anchor->logical_y;
        }
    }
}

void group_update_decorations(struct planar_group *group) {
    if (!group) return;

    struct planar_group_member *member;
    wl_list_for_each(member, &group->members, link) {
        if (member->toplevel && member->toplevel->decoration) {
            decoration_update_geometry(member->toplevel->decoration);
        }
    }
}
