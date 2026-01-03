#include "snap.h"
#include "server.h"
#include "toplevel.h"
#include "decoration.h"
#include "selection.h"
#include "output.h"

#include <stdlib.h>
#include <math.h>
#include <wlr/types/wlr_scene.h>

// Snap guide color (magenta)
static const float guide_color[4] = {0.9f, 0.3f, 0.7f, 1.0f};

static void clear_guides(struct planar_snap_state *state) {
    struct planar_snap_guide *guide, *tmp;
    wl_list_for_each_safe(guide, tmp, &state->guides, link) {
        wlr_scene_node_destroy(&guide->rect->node);
        wl_list_remove(&guide->link);
        free(guide);
    }
}

static void create_guide(struct planar_snap_state *state, bool is_vertical,
                         double position, int viewport_size) {
    if (!state->tree) return;

    struct planar_snap_guide *guide = calloc(1, sizeof(*guide));
    if (!guide) return;

    guide->is_vertical = is_vertical;

    struct planar_server *server = state->server;
    struct planar_workspace *ws = server->active_workspace;
    double scale = ws ? ws->scale : 1.0;
    int offset_x = ws ? ws->global_offset.x : 0;
    int offset_y = ws ? ws->global_offset.y : 0;

    // Convert logical position to scene coordinates
    int scene_pos = (int)(position * scale);

    if (is_vertical) {
        // Vertical line: 1px wide, full viewport height
        guide->rect = wlr_scene_rect_create(state->tree, 1, viewport_size, guide_color);
        if (guide->rect) {
            wlr_scene_node_set_position(&guide->rect->node, scene_pos + offset_x, 0);
        }
    } else {
        // Horizontal line: full viewport width, 1px tall
        guide->rect = wlr_scene_rect_create(state->tree, viewport_size, 1, guide_color);
        if (guide->rect) {
            wlr_scene_node_set_position(&guide->rect->node, 0, scene_pos + offset_y);
        }
    }

    if (guide->rect) {
        wl_list_insert(&state->guides, &guide->link);
    } else {
        free(guide);
    }
}

void snap_init(struct planar_server *server) {
    struct planar_snap_state *state = calloc(1, sizeof(*state));
    if (!state) return;

    state->server = server;
    state->tree = NULL;
    state->active = false;
    wl_list_init(&state->guides);

    server->snap_state = state;
}

void snap_finish(struct planar_server *server) {
    if (!server->snap_state) return;

    snap_end(server);
    free(server->snap_state);
    server->snap_state = NULL;
}

void snap_start(struct planar_server *server) {
    if (!server->snap_state || !server->active_workspace) return;

    struct planar_snap_state *state = server->snap_state;

    // Create scene tree for guides above the workspace content
    state->tree = wlr_scene_tree_create(server->layers[2]);  // Use overlay layer
    state->active = true;
}

void snap_end(struct planar_server *server) {
    if (!server->snap_state) return;

    struct planar_snap_state *state = server->snap_state;

    clear_guides(state);

    if (state->tree) {
        wlr_scene_node_destroy(&state->tree->node);
        state->tree = NULL;
    }

    state->active = false;
}

// Check if a toplevel should be skipped for snap detection
static bool should_skip_toplevel(struct planar_server *server,
                                  struct planar_toplevel *moving,
                                  struct planar_toplevel *other) {
    // Skip self
    if (other == moving) return true;

    // Skip unmapped windows
    if (!other->xdg_toplevel->base->surface->mapped) return true;

    // Skip windows in same selection
    if (selection_count(server) > 1 &&
        selection_contains(server, moving) &&
        selection_contains(server, other)) {
        return true;
    }

    // Skip windows in same group
    if (moving->group && other->group && moving->group == other->group) {
        return true;
    }

    return false;
}

void snap_update(struct planar_server *server, struct planar_toplevel *moving,
                 double *logical_x, double *logical_y, int moving_width, int moving_height) {
    if (!server->snap_state || !server->snap_state->active) return;
    if (!server->settings.snap_enabled) return;

    struct planar_snap_state *state = server->snap_state;
    int threshold = server->settings.snap_threshold;

    // Clear existing guides
    clear_guides(state);

    // Get viewport size for guide lines
    int viewport_width = 4096;   // Default fallback
    int viewport_height = 2160;
    struct planar_output *output;
    wl_list_for_each(output, &server->outputs, link) {
        viewport_width = output->wlr_output->width;
        viewport_height = output->wlr_output->height;
        break;  // Use first output
    }

    // Moving window edges
    double m_left = *logical_x;
    double m_right = *logical_x + moving_width;
    double m_center_x = *logical_x + moving_width / 2.0;
    double m_top = *logical_y;
    double m_bottom = *logical_y + moving_height;
    double m_center_y = *logical_y + moving_height / 2.0;

    // Track best snaps
    double best_snap_x = 0;
    double best_snap_y = 0;
    double best_dist_x = threshold + 1;
    double best_dist_y = threshold + 1;
    bool found_snap_x = false;
    bool found_snap_y = false;

    // Check against all other windows
    int border = server->settings.border_width;
    struct planar_toplevel *other;
    wl_list_for_each(other, &server->active_workspace->toplevels, link) {
        if (should_skip_toplevel(server, moving, other)) continue;
        if (!other->decoration) continue;

        // Other window edges
        double o_left = other->logical_x;
        double o_right = other->logical_x + other->decoration->width + 2 * border;
        double o_center_x = other->logical_x + (other->decoration->width + 2 * border) / 2.0;
        double o_top = other->logical_y;
        double o_bottom = other->logical_y + other->decoration->height + 2 * border;
        double o_center_y = other->logical_y + (other->decoration->height + 2 * border) / 2.0;

        // Vertical snaps (X-axis alignment)
        double v_snaps[][2] = {
            {m_left, o_left},      // left-to-left
            {m_left, o_right},     // left-to-right
            {m_right, o_left},     // right-to-left
            {m_right, o_right},    // right-to-right
            {m_center_x, o_center_x}, // center-to-center
        };

        for (int i = 0; i < 5; i++) {
            double dist = fabs(v_snaps[i][0] - v_snaps[i][1]);
            if (dist <= threshold && dist < best_dist_x) {
                best_dist_x = dist;
                // Calculate adjustment: move moving edge to match other edge
                best_snap_x = v_snaps[i][1] - (v_snaps[i][0] - *logical_x);
                found_snap_x = true;
            }
        }

        // Horizontal snaps (Y-axis alignment)
        double h_snaps[][2] = {
            {m_top, o_top},        // top-to-top
            {m_top, o_bottom},     // top-to-bottom
            {m_bottom, o_top},     // bottom-to-top
            {m_bottom, o_bottom},  // bottom-to-bottom
            {m_center_y, o_center_y}, // center-to-center
        };

        for (int i = 0; i < 5; i++) {
            double dist = fabs(h_snaps[i][0] - h_snaps[i][1]);
            if (dist <= threshold && dist < best_dist_y) {
                best_dist_y = dist;
                // Calculate adjustment
                best_snap_y = h_snaps[i][1] - (h_snaps[i][0] - *logical_y);
                found_snap_y = true;
            }
        }
    }

    // Apply snaps and create guides
    if (found_snap_x) {
        // Find which edge we're snapping to for guide position
        double guide_x = *logical_x;
        if (fabs(m_left - (best_snap_x + (m_left - *logical_x))) < 1) {
            guide_x = best_snap_x;
        } else if (fabs(m_right - (best_snap_x + moving_width)) < threshold + 1) {
            guide_x = best_snap_x + moving_width;
        } else {
            guide_x = best_snap_x + moving_width / 2.0;
        }

        *logical_x = best_snap_x;
        create_guide(state, true, guide_x, viewport_height);
    }

    if (found_snap_y) {
        double guide_y = *logical_y;
        if (fabs(m_top - (best_snap_y + (m_top - *logical_y))) < 1) {
            guide_y = best_snap_y;
        } else if (fabs(m_bottom - (best_snap_y + moving_height)) < threshold + 1) {
            guide_y = best_snap_y + moving_height;
        } else {
            guide_y = best_snap_y + moving_height / 2.0;
        }

        *logical_y = best_snap_y;
        create_guide(state, false, guide_y, viewport_width);
    }
}
