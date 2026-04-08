/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:

 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.

 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#include "app/repl.h"
#include "core/ipc.h"
#include "core/runtime.h"
#include <rayforce.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

int main(int argc, char** argv) {
    ray_runtime_t* rt = ray_runtime_create(argc, argv);
    if (!rt) { fprintf(stderr, "failed to create runtime\n"); return 1; }

    int rc = 0;
    int interactive = 0;
    const char* file = NULL;
    uint16_t port = 0;

    /* Parse args: [-i] [-p PORT] [file.rfl] */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interactive") == 0)
            interactive = 1;
        else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc)
            port = (uint16_t)atoi(argv[++i]);
        else
            file = argv[i];
    }

    /* Start IPC server if port specified */
    ray_ipc_server_t ipc_srv_storage;
    ray_ipc_server_t* ipc_srv = NULL;
    if (port > 0) {
        if (ray_ipc_server_init(&ipc_srv_storage, port) == RAY_OK) {
            ipc_srv = &ipc_srv_storage;
            fprintf(stderr, "listening on port %u\n", port);
        } else {
            fprintf(stderr, "failed to listen on port %u: %s\n", port, strerror(errno));
        }
    }

    /* Load script if specified */
    if (file) {
        rc = ray_repl_run_file(file);
        if (!interactive && !ipc_srv) goto done;
    }

    /* REPL or pure server mode */
    {
        ray_repl_t* repl = ray_repl_create();
        if (repl) {
            repl->ipc_srv = ipc_srv;
            ray_repl_run(repl);
            ray_repl_destroy(repl);
        } else if (ipc_srv) {
            /* No REPL possible — run pure server loop */
            while (ipc_srv->running)
                ray_ipc_poll(ipc_srv, 100);
        }
    }

done:
    if (ipc_srv) ray_ipc_server_destroy(ipc_srv);
    ray_runtime_destroy(rt);
    return rc;
}
