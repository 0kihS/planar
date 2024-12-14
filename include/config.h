#ifndef CONFIG_H
#define CONFIG_H

#include <xkbcommon/xkbcommon.h>
#include "server.h"

#define MAX_KEYBINDINGS 32

struct keybinding {
    uint32_t modifiers;  // Modifier keys (alt, ctrl, etc)
    xkb_keysym_t key;    // The actual key
    char *command;       // Command to execute
};

struct config {
    struct keybinding keybindings[MAX_KEYBINDINGS];
    int num_keybindings;
    char *startup_cmd;
};

// Function declarations
struct config *config_load(const char *path);
void config_destroy(struct config *config);
bool handle_keybinding_from_config(struct planar_server *server, uint32_t modifiers, xkb_keysym_t sym);

#endif // CONFIG_H
