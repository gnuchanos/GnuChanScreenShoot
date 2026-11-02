#define main screenshot_program_main
#include "../GnuChanScreenShoot.c"
#undef main

#include <assert.h>

int main(void) {
    assert(ensure_directory("") == -1);

    char first_path[SS_TEXT_LENGTH];
    char second_path[SS_TEXT_LENGTH];
    int first_fd = open_screenshot_file(first_path, sizeof(first_path),
                                        "src/tests", SS_FORMAT_PNG);
    assert(first_fd >= 0);
    assert(write(first_fd, "keep", 4) == 4);
    close(first_fd);

    int second_fd = open_screenshot_file(second_path, sizeof(second_path),
                                         "src/tests", SS_FORMAT_PNG);
    assert(second_fd >= 0);
    assert(strcmp(first_path, second_path) != 0);

    FILE *first_file = fopen(first_path, "rb");
    assert(first_file);
    char marker[4];
    assert(fread(marker, 1, sizeof(marker), first_file) == sizeof(marker));
    assert(memcmp(marker, "keep", sizeof(marker)) == 0);
    fclose(first_file);

    close(second_fd);
    assert(unlink(first_path) == 0);
    assert(unlink(second_path) == 0);
    return 0;
}
