#include <rayforce.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    ray_heap_init();
    assert(ray_sym_init() == RAY_OK);

    char path[] = "/tmp/fuzz_col_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { ray_sym_destroy(); ray_heap_destroy(); return 0; }
    write(fd, data, size);
    close(fd);

    ray_t* result = ray_col_load(path);
    if (result && !RAY_IS_ERR(result))
        ray_release(result);

    unlink(path);
    ray_sym_destroy();
    ray_heap_destroy();
    return 0;
}
