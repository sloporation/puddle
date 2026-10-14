/* puddled: headless Puddle service. Logs to stderr (journald / launchd). */
#include "config.h"
#include "log.h"
#include "server.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

static puddle_server *volatile g_server;

static void on_signal(int sig)
{
    (void)sig;
    puddle_server_stop(g_server);
}

static void usage(FILE *out)
{
    fprintf(out, "usage: puddled [-c|--config FILE]\n");
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-c") || !strcmp(argv[i], "--config")) && i + 1 < argc) {
            path = argv[++i];
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(stdout);
            return 0;
        } else {
            usage(stderr);
            return 2;
        }
    }

    char err[512];
    puddle_config cfg;
    if (puddle_config_load(&cfg, path, err, sizeof err) != 0) {
        fprintf(stderr, "puddled: %s\n", err);
        return 1;
    }

    puddle_server *srv = puddle_server_create(&cfg, err, sizeof err);
    if (!srv) {
        fprintf(stderr, "puddled: %s\n", err);
        return 1;
    }
    g_server = srv;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int rc = puddle_server_run(srv);
    puddle_server_destroy(srv);
    return rc == 0 ? 0 : 1;
}
