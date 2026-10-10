/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "art-aot-targets.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char boot_class_path[] =
    "-Xbootclasspath:/apex/com.android.art/javalib/core-oj.jar:"
    "/apex/com.android.art/javalib/core-libart.jar:/apex/com.android.art/javalib/okhttp.jar:"
    "/apex/com.android.art/javalib/bouncycastle.jar:/apex/com.android.art/javalib/apache-xml.jar:"
    "/apex/com.android.i18n/javalib/core-icu4j.jar:/apex/com.android.conscrypt/javalib/conscrypt.jar";

static int wait_child(pid_t child)
{
    int status = -1;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child || !WIFEXITED(status)) {
        fprintf(stderr, "compiler child did not exit normally: pid=%d waited=%d status=%d\n", child, waited, status);
        return -1;
    }
    return WEXITSTATUS(status);
}

static int compile(const char *dex, const char *oat, const char *classpath, const char *context, int speed, unsigned seconds)
{
    char dex_arg[512], oat_arg[512], context_arg[1024];
    if (snprintf(dex_arg, sizeof(dex_arg), "--dex-file=%s", dex) >= (int)sizeof(dex_arg) ||
        snprintf(oat_arg, sizeof(oat_arg), "--oat-file=%s", oat) >= (int)sizeof(oat_arg) ||
        snprintf(context_arg, sizeof(context_arg), "--class-loader-context=%s", context) >= (int)sizeof(context_arg))
        return -1;
    pid_t child = fork();
    if (child < 0)
        return -1;
    if (!child) {
        alarm(seconds);
        execl("/system/bin/nce-dex2oat", "nce-dex2oat", "-j1", "--no-watch-dog",
              dex_arg, oat_arg, context_arg,
              speed ? "--compiler-filter=speed" : "--compiler-filter=verify",
              "--inline-max-code-units=0", "--generate-debug-info",
              "--instruction-set=arm64", "--boot-image=/data/no-boot-image.art",
              "--runtime-arg", classpath, "--runtime-arg", "-Xms16m",
              "--runtime-arg", "-Xmx256m", "--dump-timings", (char *)NULL);
        _exit(127);
    }
    return wait_child(child);
}

static int prepare_properties(void)
{
    int ready[2];
    if (pipe(ready))
        return -1;
    pid_t child = fork();
    if (child < 0) {
        close(ready[0]);
        close(ready[1]);
        return -1;
    }
    if (!child) {
        close(ready[0]);
        char descriptor[24];
        snprintf(descriptor, sizeof(descriptor), "%d", ready[1]);
        setenv("LD_PRELOAD", "/system/lib64/libnce-framework-log.so", 1);
        execl("/system/bin/android-services-test", "android-services-test", "properties", descriptor, (char *)NULL);
        _exit(127);
    }
    close(ready[1]);
    char byte = 0;
    int result = -1;
    if (read(ready[0], &byte, 1) == 1 && byte == 'r') {
        int (*initialize)(void) = dlsym(RTLD_DEFAULT, "__system_properties_init");
        if (initialize && !initialize() &&
            !__system_property_set("ro.build.version.sdk", "36") &&
            !__system_property_set("ro.build.version.codename", "REL") &&
            !__system_property_set("ro.build.version.all_codenames", "REL"))
            result = 0;
    }
    close(ready[0]);
    // The property area remains available to compiler children after its writer exits.
    if (kill(child, SIGTERM))
        result = -1;
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFSIGNALED(status) || WTERMSIG(status) != SIGTERM)
        result = -1;
    return result;
}

static int desktop_classpath(char *result, size_t capacity, int include_helper)
{
    pid_t child = fork();
    if (child < 0)
        return -1;
    if (!child) {
        alarm(60);
        if (setenv("LD_PRELOAD", "/system/lib64/libnce-framework-log.so", 1))
            _exit(127);
        execl("/system/bin/derive_classpath", "derive_classpath", "/data/nce/classpath", (char *)NULL);
        _exit(127);
    }
    if (wait_child(child))
        return -1;
    FILE *file = fopen("/data/nce/classpath", "r");
    if (!file)
        return -1;
    char line[16384];
    int found = 0;
    while (fgets(line, sizeof(line), file)) {
        const char prefix[] = "export BOOTCLASSPATH ";
        if (strncmp(line, prefix, sizeof(prefix) - 1))
            continue;
        if (!strchr(line, '\n') || found) {
            found = 0;
            break;
        }
        line[strcspn(line, "\n")] = 0;
        // Zygote appends the helper; other app_process entries use the shared system path.
        int length = snprintf(result, capacity, "-Xbootclasspath:%s%s", line + sizeof(prefix) - 1,
                              include_helper ? ":/system/framework/nce-framework.jar" : "");
        if (length < 0 || (size_t)length >= capacity)
            break;
        found = 1;
    }
    int failed = ferror(file);
    fclose(file);
    return found && !failed ? 0 : -1;
}

static off_t check_file(const char *path, const char *magic)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat info;
    char bytes[4];
    if (fd < 0)
        return -1;
    int valid = !fstat(fd, &info) && info.st_size > 4 && read(fd, bytes, sizeof(bytes)) == sizeof(bytes) && !memcmp(bytes, magic, sizeof(bytes));
    close(fd);
    return valid ? info.st_size : -1;
}

static int inspect_boot(const char *path)
{
    pid_t child = fork();
    if (child < 0)
        return -1;
    if (!child) {
        alarm(30);
        execl("/system/bin/nce-dex2oat", "nce-dex2oat", "--nce-inspect-image", path, (char *)NULL);
        _exit(127);
    }
    return wait_child(child);
}

static int compile_boot(int desktop)
{
    if ((mkdir("/data/nce/boot", 0700) && errno != EEXIST) ||
        (mkdir("/data/nce/boot/arm64", 0700) && errno != EEXIST))
        return 1;
    struct timespec begin, end;
    if (clock_gettime(CLOCK_MONOTONIC, &begin))
        return 1;
    pid_t child = fork();
    if (child < 0)
        return 1;
    if (!child) {
        alarm(desktop ? 600 : 180);
        char classpath[16384];
        if (desktop) {
            if (prepare_properties() || desktop_classpath(classpath, sizeof(classpath), 0))
                _exit(126);
        } else {
            snprintf(classpath, sizeof(classpath), "%s", boot_class_path);
        }
        char paths[sizeof(classpath)];
        snprintf(paths, sizeof(paths), "%s", classpath + strlen("-Xbootclasspath:"));
        char dex_args[128][512];
        const char *args[160] = {"nce-dex2oat", "-j1", "--no-watch-dog"};
        size_t count = 3, dex_count = 0;
        for (char *path = strtok(paths, ":"); path; path = strtok(NULL, ":")) {
            if (dex_count == 128 || snprintf(dex_args[dex_count], sizeof(dex_args[0]), "--dex-file=%s", path) >= (int)sizeof(dex_args[0]))
                _exit(126);
            args[count++] = dex_args[dex_count++];
        }
        if (!dex_count)
            _exit(126);
        const char *options[] = {
            "--image=/data/nce/boot/arm64/boot.art",
            "--oat-file=/data/nce/boot/arm64/boot.oat", "--single-image",
            "--nce-image-offset=200000", "--image-format=uncompressed",
            "--compiler-filter=verify", "--generate-debug-info", "--instruction-set=arm64",
            "--runtime-arg", classpath, "--runtime-arg", "-Xms16m",
            "--runtime-arg", "-Xmx256m", "--dump-timings", NULL};
        for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); ++i)
            args[count++] = options[i];
        execv("/system/bin/nce-dex2oat", (char *const *)args);
        _exit(127);
    }
    int result = wait_child(child);
    off_t image = check_file("/data/nce/boot/arm64/boot.art", "art\n");
    off_t oat = check_file("/data/nce/boot/arm64/boot.oat", "\177ELF");
    off_t vdex = check_file("/data/nce/boot/arm64/boot.vdex", "vdex");
    if (result || image < 0 || oat < 0 || vdex < 0 || clock_gettime(CLOCK_MONOTONIC, &end)) {
        fprintf(stderr, "Boot image generation failed: exit=%d\n", result);
        return 1;
    }
    if (inspect_boot("/data/nce/boot/arm64/boot.art"))
        return 1;
    unsigned char header[512];
    int input = open("/data/nce/boot/arm64/boot.art", O_RDONLY | O_CLOEXEC);
    if (input < 0 || read(input, header, sizeof(header)) != sizeof(header))
        return 1;
    close(input);
    header[4] ^= 1;
    const size_t lengths[] = {8, sizeof(header)};
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        int bad = open("/data/nce/boot/invalid.art", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (bad < 0 || write(bad, header, lengths[i]) != (ssize_t)lengths[i])
            return 1;
        close(bad);
        if (inspect_boot("/data/nce/boot/invalid.art") != 3)
            return 1;
    }
    if (unlink("/data/nce/boot/invalid.art"))
        return 1;
    puts("{\"test\":\"android_art_boot_image_invalid_headers\",\"passed\":true,\"cases\":2}");
    double elapsed = (end.tv_sec - begin.tv_sec) * 1000.0 + (end.tv_nsec - begin.tv_nsec) / 1000000.0;
    printf("{\"test\":\"android_art_boot_image_generated\",\"passed\":true,\"image_bytes\":%lld,\"oat_bytes\":%lld,\"vdex_bytes\":%lld,\"elapsed_ms\":%.3f}\n",
           (long long)image, (long long)oat, (long long)vdex, elapsed);
    return 0;
}

int main(void)
{
    char mode[32] = {};
    if (!fgets(mode, sizeof(mode), stdin))
        mode[0] = 0;
    int speed = !strcmp(mode, "speed\n");
    int desktop = !strcmp(mode, "desktop\n");
    setenv("ANDROID_ROOT", "/system", 1);
    setenv("ANDROID_DATA", "/data", 1);
    setenv("ANDROID_ART_ROOT", "/apex/com.android.art", 1);
    setenv("ANDROID_I18N_ROOT", "/apex/com.android.i18n", 1);
    setenv("ANDROID_TZDATA_ROOT", "/apex/com.android.tzdata", 1);
    setenv("ICU_DATA", "/apex/com.android.i18n/etc/icu", 1);
    if (!strcmp(mode, "boot\n") || !strcmp(mode, "boot-desktop\n"))
        return compile_boot(!strcmp(mode, "boot-desktop\n"));
    if ((mkdir("/data/nce/oat", 0700) && errno != EEXIST) ||
        (mkdir("/data/nce/oat/arm64", 0700) && errno != EEXIST))
        return 1;
    if (desktop) {
        char classpath[16384];
        if (prepare_properties() || desktop_classpath(classpath, sizeof(classpath), 1))
            return 5;
        for (size_t i = 0; i < sizeof(aot_desktop_targets) / sizeof(aot_desktop_targets[0]); ++i) {
            char oat_path[512], vdex_path[512];
            snprintf(oat_path, sizeof(oat_path), "/data/nce/oat/arm64/%s.odex", aot_desktop_targets[i].name);
            snprintf(vdex_path, sizeof(vdex_path), "/data/nce/oat/arm64/%s.vdex", aot_desktop_targets[i].name);
            struct timespec begin, end;
            if (clock_gettime(CLOCK_MONOTONIC, &begin))
                return 5;
            fprintf(stderr, "Compiling desktop AOT: %s\n", aot_desktop_targets[i].dex);
            int result = compile(aot_desktop_targets[i].dex, oat_path, classpath, aot_desktop_targets[i].context, 1, 1500);
            off_t oat = check_file(oat_path, "\177ELF"), vdex = check_file(vdex_path, "vdex");
            if (result || oat < 0 || vdex < 0 || clock_gettime(CLOCK_MONOTONIC, &end)) {
                fprintf(stderr, "Desktop AOT failed: %s exit=%d\n", aot_desktop_targets[i].name, result);
                return 6;
            }
            double elapsed = (end.tv_sec - begin.tv_sec) * 1000.0 + (end.tv_nsec - begin.tv_nsec) / 1000000.0;
            printf("{\"test\":\"android_art_compiler_desktop\",\"passed\":true,\"target\":\"%s\",\"oat_bytes\":%lld,\"vdex_bytes\":%lld,\"elapsed_ms\":%.3f}\n",
                   aot_desktop_targets[i].name, (long long)oat, (long long)vdex, elapsed);
            fflush(stdout);
        }
        return 0;
    }
    unlink("/data/nce/oat/arm64/NceWorkload.odex");
    unlink("/data/nce/oat/arm64/NceWorkload.vdex");
    struct timespec begin, end;
    if (clock_gettime(CLOCK_MONOTONIC, &begin))
        return 1;
    int result = compile("/data/nce/NceWorkload.dex", "/data/nce/oat/arm64/NceWorkload.odex", boot_class_path, "PCL[]", speed, 180);
    if (result) {
        fprintf(stderr, "dex2oat failed: exit=%d\n", result);
        return 2;
    }
    off_t oat = check_file("/data/nce/oat/arm64/NceWorkload.odex", "\177ELF");
    off_t vdex = check_file("/data/nce/oat/arm64/NceWorkload.vdex", "vdex");
    if (oat < 0 || vdex < 0 || clock_gettime(CLOCK_MONOTONIC, &end))
        return 3;
    double elapsed = (end.tv_sec - begin.tv_sec) * 1000.0 + (end.tv_nsec - begin.tv_nsec) / 1000000.0;
    printf("{\"test\":\"android_art_compiler_%s\",\"passed\":true,\"oat_bytes\":%lld,\"vdex_bytes\":%lld,\"elapsed_ms\":%.3f}\n", speed ? "speed" : "verify", (long long)oat, (long long)vdex, elapsed);
    fflush(stdout);
    int invalid = open("/data/nce/invalid.dex", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (invalid < 0 || write(invalid, "invalid dex", 11) != 11 || close(invalid))
        return 4;
    result = compile("/data/nce/invalid.dex", "/data/nce/compiler-invalid.odex", boot_class_path, "PCL[]", speed, 180);
    if (result <= 0 || result == 127)
        return 4;
    printf("{\"test\":\"android_art_compiler_invalid_input\",\"passed\":true,\"exit\":%d}\n", result);
    return 0;
}
