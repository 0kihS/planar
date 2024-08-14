#ifndef PLANAR_SEAT_H
#define PLANAR_SEAT_H

#include <wayland-server-core.h>
#include "server.h"

void seat_init(struct planar_server *server);
void seat_finish(struct planar_server *server);

#endif // PLANAR_SEAT_H