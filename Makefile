
# Compiler and pkg-config
CC ?= gcc
PKG_CONFIG ?= pkg-config

# Wayland protocols and scanner
WAYLAND_PROTOCOLS != $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols
WAYLAND_SCANNER != $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner

# Packages and flags
PKGS = wlroots-0.19 wayland-server xkbcommon
CFLAGS_PKG_CONFIG!=$(PKG_CONFIG) --cflags $(PKGS)
CFLAGS+=$(CFLAGS_PKG_CONFIG)
LIBS!=$(PKG_CONFIG) --libs $(PKGS)
CFLAGS += -Werror -I./include -DWLR_USE_UNSTABLE -g
INC=-I/include

# Directories
SRC_DIR = src
OBJ_DIR = output
BIN_DIR = builds

# Source files
SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))

# Binary name
TARGET = $(BIN_DIR)/planar

# Default target
all: $(TARGET)

# Rule for Wayland protocol header
xdg-shell-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@
wlr-layer-shell-unstable-v1-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		./protocols/wlr_layer_shell_unstable_v1.xml $@

# Rule to create directories
$(OBJ_DIR) $(BIN_DIR):
	mkdir -p $@

# Rule to compile .c files into .o files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c xdg-shell-protocol.h wlr-layer-shell-unstable-v1-protocol.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Rule to link object files into the final binary
$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(OBJS) $(CFLAGS) $(LIBS) -o $@

# Clean rule
clean:
	rm -rf $(OBJ_DIR) $(TARGET) xdg-shell-protocol.h

# Phony targets
.PHONY: all clean

# Include dependencies
-include $(OBJS:.o=.d)

# Rule to generate dependency files
$(OBJ_DIR)/%.d: $(SRC_DIR)/%.c | $(OBJ_DIR)
	@set -e; rm -f $@; \
	$(CC) -MM $(CFLAGS) $< > $@.$$$$; \
	sed 's,\($*\)\.o[ :]*,$(OBJ_DIR)/\1.o $@ : ,g' < $@.$$$$ > $@; \
	rm -f $@.$$$$