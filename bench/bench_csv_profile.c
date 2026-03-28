#define _POSIX_C_SOURCE 199309L
#include <rayforce.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void generate_csv(const char* path, int64_t n_rows, int64_t n_unique) {
    FILE* f = fopen(path, "w");
    fprintf(f, "id,val,sym\n");
    for (int64_t i = 0; i < n_rows; i++) {
        fprintf(f, "%lld,%.2f,sym_%lld\n",
                (long long)i, (double)i * 0.1, (long long)(i % n_unique));
    }
    fclose(f);
}

int main(int argc, char** argv) {
    int64_t n_rows = 500000;
    int64_t n_unique = 100000;

    if (argc > 1) n_rows = atoll(argv[1]);
    if (argc > 2) n_unique = atoll(argv[2]);

    const char* csv_path = "/tmp/rayforce_profile.csv";
    generate_csv(csv_path, n_rows, n_unique);

    /* Pre-fault the file into page cache */
    {
        ray_heap_init();
        { ray_err_t _e = ray_sym_init(); (void)_e; };
        ray_t* warmup = ray_read_csv(csv_path);
        if (warmup && !RAY_IS_ERR(warmup)) ray_release(warmup);
        ray_sym_destroy();
        ray_heap_destroy();
    }

    ray_heap_init();
    { ray_err_t _e = ray_sym_init(); (void)_e; };

    ray_t* t = ray_read_csv(csv_path);
    if (t && !RAY_IS_ERR(t)) ray_release(t);

    ray_sym_destroy();
    ray_heap_destroy();
    unlink(csv_path);
    return 0;
}
