#ifndef CONFIG_H
#define CONFIG_H

#include <xkbcommon/xkbcommon.h>
#include <wlr/types/wlr_keyboard.h>
#include <stdbool.h>
#include "server.h"

#define MAX_KEYBINDINGS 64

struct keybinding {
    uint32_t modifiers;
    xkb_keysym_t key;
    char *command;
    bool is_internal;
};

struct config {
    char *startup_cmd;
    struct keybinding keybindings[MAX_KEYBINDINGS];
    int num_keybindings;

    char **nodecoration;
    size_t nodecoration_count;
    char **ontop;
    size_t ontop_count;
};

struct config *config_load(const char *path);
void config_destroy(struct config *config);

bool handle_internal_command(struct planar_server *server, const char *cmd);
bool handle_external_command(const char *cmd);

#endif
