/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/* Included after linux-runner's Linux syscall and file helpers. The TSV is
 * generated from the verified component manifest before host compilation. */
static void stock_art_setup(void)
{
    const char *root = getenv("NCE_STOCK_ART_DIRECTORY");
    const char *files = getenv("NCE_TRANSLATION_FILES");
    REQUIRE(root && root[0] == '/' && files);
    char path[4096];
    REQUIRE(snprintf(path, sizeof(path), "%s/stock-art-files.tsv", files) < (int)sizeof(path));
    FILE *input = fopen(path, "r");
    REQUIRE(input);
    char line[2048];
    while (fgets(line, sizeof(line), input)) {
        char guest[1024], extra;
        unsigned mode;
        size_t length = strlen(line);
        REQUIRE(length && line[length - 1] == '\n');
        if (line[0] == 'D') {
            REQUIRE(sscanf(line, "D\t%1023s %c", guest, &extra) == 1);
            REQUIRE(guest[0] == '/' && !strstr(guest, ".."));
            directory(guest);
        } else {
            REQUIRE(sscanf(line, "F\t%o\t%1023s %c", &mode, guest, &extra) == 2);
            REQUIRE((mode == 0644 || mode == 0755) && guest[0] == '/' && !strstr(guest, ".."));
            REQUIRE(snprintf(path, sizeof(path), "%s/root%s", root, guest) < (int)sizeof(path));
            put_host_file(guest, path, mode);
        }
    }
    REQUIRE(!ferror(input) && fclose(input) == 0);
    REQUIRE(snprintf(path, sizeof(path), "%s/stock-art-boot-classpath", files) < (int)sizeof(path));
    put_host_file("/data/stock-art/boot-classpath", path, 0644);
    put_file("/data/stock-art/jit-mode", "stock-art-jit-mode");
    directory("/dev");
    const char *devices[] = {"/dev/null", "/dev/random", "/dev/urandom"};
    const unsigned numbers[] = {259, 264, 265};
    for (unsigned i = 0; i < sizeof(devices) / sizeof(*devices); ++i) {
        long create[6] = {-100, (long)devices[i], 0020666, numbers[i]};
        REQUIRE(call(33, create) == 0);
    }
    seed_entropy();
}

static void stock_art_export(void)
{
    const char *output = getenv("NCE_STOCK_ART_RESULT_DIR");
    REQUIRE(output && output[0] == '/');
    const char *names[] = {"before-vm", "after-vm", "after-gc", "before-warmup", "after-warmup"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(*names); ++i) {
        char guest[128], host[4096];
        REQUIRE(snprintf(guest, sizeof(guest), "/data/stock-art/maps-%s.txt", names[i]) < (int)sizeof(guest));
        REQUIRE(snprintf(host, sizeof(host), "%s/maps-%s.txt", output, names[i]) < (int)sizeof(host));
        long args[6] = {-100, (long)guest, 0};
        long fd = call(56, args);
        if (fd == -2) {
            printf("{\"stock_art_export\":\"%s\",\"present\":false}\n", names[i]);
            continue;
        }
        REQUIRE(fd >= 0);
        FILE *file = fopen(host, "wb");
        REQUIRE(file);
        unsigned char buffer[65536];
        unsigned long total = 0;
        for (;;) {
            args[0] = fd;
            args[1] = (long)buffer;
            args[2] = sizeof(buffer);
            long bytes = call(63, args);
            if (bytes == -4)
                continue;
            REQUIRE(bytes >= 0);
            if (!bytes)
                break;
            REQUIRE(fwrite(buffer, 1, bytes, file) == (size_t)bytes);
            total += bytes;
        }
        REQUIRE(call(57, args) == 0 && fclose(file) == 0);
        printf("{\"stock_art_export\":\"%s\",\"present\":true,\"bytes\":%lu}\n", names[i], total);
    }
}
