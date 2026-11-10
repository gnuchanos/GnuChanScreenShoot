#define main screenshot_program_main
#include "../GnuChanScreenShoot.c"
#undef main

#include <assert.h>

int main(void) {
    assert(hotkey_modifier_mask(Mod1Mask, 0) == Mod1Mask);
    assert(hotkey_modifier_mask(Mod1Mask, 1) == (Mod1Mask | LockMask));
    assert(hotkey_modifier_mask(Mod1Mask | ShiftMask, 2) ==
           (Mod1Mask | ShiftMask | Mod2Mask));
    assert(hotkey_modifier_mask(0, 4) == Mod5Mask);

    assert(ensure_directory("") == -1);

    const char *path = getenv("PATH");
    char *saved_path = path ? strdup(path) : NULL;
    assert(setenv("PATH", "/no-such-xclip-directory", 1) == 0);
    assert(require_clipboard_helper() == -1);
    if (saved_path) {
        assert(setenv("PATH", saved_path, 1) == 0);
    } else {
        assert(unsetenv("PATH") == 0);
    }
    free(saved_path);

    int x = -1;
    int y = -1;
    unsigned int width = 0;
    unsigned int height = 0;
    assert(normalize_region(100, 200, 20, 30, 800, 600,
                            &x, &y, &width, &height) == 0);
    assert(x == 20 && y == 30 && width == 80 && height == 170);
    assert(normalize_region(10, 10, 10, 20, 800, 600,
                            &x, &y, &width, &height) == -1);
    assert(normalize_region(10, 10, 801, 20, 800, 600,
                            &x, &y, &width, &height) == -1);

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
