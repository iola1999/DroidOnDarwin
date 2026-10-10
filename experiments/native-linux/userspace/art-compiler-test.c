/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char boot_class_path[] =
    "-Xbootclasspath:/apex/com.android.art/javalib/core-oj.jar:"
    "/apex/com.android.art/javalib/core-libart.jar:/apex/com.android.art/javalib/okhttp.jar:"
    "/apex/com.android.art/javalib/bouncycastle.jar:/apex/com.android.art/javalib/apache-xml.jar:"
    "/apex/com.android.i18n/javalib/core-icu4j.jar:/apex/com.android.conscrypt/javalib/conscrypt.jar";

static int compile(int invalid_input)
{
    pid_t child = fork();
    if (child < 0)
        return -1;
    if (!child) {
        alarm(180);
        execl("/system/bin/nce-dex2oat", "nce-dex2oat", "-j1", "--no-watch-dog",
              invalid_input ? "--dex-file=/data/nce/invalid.dex" : "--dex-file=/data/nce/NceWorkload.dex",
              invalid_input ? "--oat-file=/data/nce/compiler-invalid.odex" : "--oat-file=/data/nce/oat/arm64/NceWorkload.odex",
              "--compiler-filter=verify",
              "--instruction-set=arm64", "--boot-image=/data/no-boot-image.art",
              "--runtime-arg", boot_class_path, "--runtime-arg", "-Xms16m",
              "--runtime-arg", "-Xmx256m", "--dump-timings", (char *)NULL);
        _exit(127);
    }
    int status = -1;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child || !WIFEXITED(status)) {
        fprintf(stderr, "dex2oat child did not exit normally: pid=%d waited=%d status=%d\n", child, waited, status);
        return -1;
    }
    return WEXITSTATUS(status);
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

int main(void)
{
    setenv("ANDROID_ROOT", "/system", 1);
    setenv("ANDROID_DATA", "/data", 1);
    setenv("ANDROID_ART_ROOT", "/apex/com.android.art", 1);
    setenv("ANDROID_I18N_ROOT", "/apex/com.android.i18n", 1);
    setenv("ANDROID_TZDATA_ROOT", "/apex/com.android.tzdata", 1);
    setenv("ICU_DATA", "/apex/com.android.i18n/etc/icu", 1);
    if ((mkdir("/data/nce/oat", 0700) && errno != EEXIST) ||
        (mkdir("/data/nce/oat/arm64", 0700) && errno != EEXIST))
        return 1;
    unlink("/data/nce/oat/arm64/NceWorkload.odex");
    unlink("/data/nce/oat/arm64/NceWorkload.vdex");
    struct timespec begin, end;
    if (clock_gettime(CLOCK_MONOTONIC, &begin))
        return 1;
    int result = compile(0);
    if (result) {
        fprintf(stderr, "dex2oat verify failed: exit=%d\n", result);
        return 2;
    }
    off_t oat = check_file("/data/nce/oat/arm64/NceWorkload.odex", "\177ELF");
    off_t vdex = check_file("/data/nce/oat/arm64/NceWorkload.vdex", "vdex");
    if (oat < 0 || vdex < 0 || clock_gettime(CLOCK_MONOTONIC, &end))
        return 3;
    double elapsed = (end.tv_sec - begin.tv_sec) * 1000.0 + (end.tv_nsec - begin.tv_nsec) / 1000000.0;
    printf("{\"test\":\"android_art_compiler_verify\",\"passed\":true,\"oat_bytes\":%lld,\"vdex_bytes\":%lld,\"elapsed_ms\":%.3f}\n", (long long)oat, (long long)vdex, elapsed);
    fflush(stdout);
    int invalid = open("/data/nce/invalid.dex", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (invalid < 0 || write(invalid, "invalid dex", 11) != 11 || close(invalid))
        return 4;
    result = compile(1);
    if (result <= 0 || result == 127)
        return 4;
    printf("{\"test\":\"android_art_compiler_invalid_input\",\"passed\":true,\"exit\":%d}\n", result);
    return 0;
}
