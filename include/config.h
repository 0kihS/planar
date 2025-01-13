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
};

struct config *config_load(const char *path);
void config_destroy(struct config *config);

#endif
