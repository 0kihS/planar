#include "server.h"
#include <stdlib.h>
#include <wlr/util/log.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    wlr_log_init(WLR_DEBUG, NULL);

    char *startup_cmd = NULL;

	int c;
	while ((c = getopt(argc, argv, "s:h")) != -1) {
		switch (c) {
		case 's':
			startup_cmd = optarg;
			break;
		default:
			printf("Usage: %s [-s startup command]\n", argv[0]);
			return 0;
		}
	}
	if (optind < argc) {
		printf("Usage: %s [-s startup command]\n", argv[0]);
		return 0;
	}

    struct planar_server server = {0};
    server_init(&server);

	if (startup_cmd) {
		if (fork() == 0) {
			execl("/bin/sh", "/bin/sh", "-c", startup_cmd, (void *)NULL);
		}
	}    

    server_run(&server);
    server_finish(&server);

    return 0;
}