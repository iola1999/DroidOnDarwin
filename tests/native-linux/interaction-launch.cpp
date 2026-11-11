/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>

static std::string root;
static bool stop_fails, surface_fails;
static std::string path(const char *value)
{
    return root + value;
}
static int test_open(const char *name, int flags, ...)
{
    va_list arguments;
    va_start(arguments, flags);
    int mode = flags & O_CREAT ? va_arg(arguments, int) : 0;
    va_end(arguments);
    return open(path(name).c_str(), flags, mode);
}
static FILE *test_fopen(const char *name, const char *mode)
{
    return fopen(path(name).c_str(), mode);
}
static DIR *test_opendir(const char *name)
{
    return opendir(path(name).c_str());
}
static int test_unlink(const char *name)
{
    return unlink(path(name).c_str());
}
static int test_rename(const char *from, const char *to)
{
    return rename(path(from).c_str(), path(to).c_str());
}
static int test_mount(const char *, const char *, const char *, unsigned long, const void *)
{
    return 0;
}
static int test_execl(const char *file, const char *, ...)
{
    if (!strcmp(file, "/system/bin/dumpsys")) {
        if (!std::filesystem::exists(path("/proc/42/cmdline")))
            _exit(124);
        dprintf(1, "Stats since: 1ns\nTotal frames rendered: 0\n");
    }
    _exit(0);
}

// Substitute only OS boundaries; perform(), process validation and file writers
// below are the shipped implementation, including its fork/wait cleanup.
#define open test_open
#define fopen test_fopen
#define opendir test_opendir
#define unlink test_unlink
#define rename test_rename
#define mount test_mount
#define execl test_execl
#include "../../experiments/native-linux/userspace/interaction-profile.cpp"
#undef open
#undef fopen
#undef opendir
#undef unlink
#undef rename
#undef mount
#undef execl

static void write_file(const char *name, const std::string &text)
{
    std::filesystem::create_directories(std::filesystem::path(path(name)).parent_path());
    FILE *file = fopen(path(name).c_str(), "w");
    assert(file && fwrite(text.data(), 1, text.size(), file) == text.size());
    assert(!fclose(file));
}

static void process(int pid, const char *name, bool valid = true)
{
    std::string prefix = "/proc/" + std::to_string(pid);
    write_file((prefix + "/cmdline").c_str(), std::string(name) + '\0');
    std::string stat = std::to_string(pid) + " (name with ) spaces) S";
    for (int field = 4; field < 22; ++field)
        stat += " 0";
    write_file((prefix + "/stat").c_str(), valid ? stat + " 777 0\n" : "invalid\n");
}

void nce_android_command(std::initializer_list<const char *> arguments, const char *)
{
    auto command = arguments.begin() + 1;
    if (!strcmp(*command, "force-stop")) {
        if (!stop_fails) {
            std::filesystem::remove_all(path("/proc/42"));
            std::filesystem::remove_all(path("/proc/43"));
        }
    } else {
        assert(!strcmp(*command, "start-activity"));
        process(42, "com.android.settings");
    }
}

bool nce_surface_check(const char *, const char *, bool)
{
    return !surface_fails;
}
int nce_wait_desktop_child(pid_t child, bool)
{
    int status;
    assert(waitpid(child, &status, 0) == child);
    return status;
}
extern "C" int __system_property_get(const char *, char *value)
{
    strcpy(value, "0");
    return 1;
}

int main()
{
    char temporary[] = "/tmp/dod-interaction-launch-XXXXXX";
    char *created = mkdtemp(temporary);
    assert(created);
    root = created;
    std::filesystem::create_directories(path("/proc"));
    std::filesystem::create_directories(path("/data/nce"));
    assert(!settings_process());
    process(40, "com.android.settingsx");
    assert(!settings_process());
    process(43, "com.android.settings:remote");
    assert(settings_process() == -EBUSY);
    pid_t pid;
    uint64_t ticks;
    assert(settings_process(&pid, &ticks) == -ESRCH);
    process(42, "com.android.settings", false);
    assert(settings_process(&pid, &ticks) == -EIO);
    process(42, "com.android.settings");
    assert(!settings_process(&pid, &ticks) && pid == 42 && ticks == 777);
    stop_fails = true;
    assert(perform(NCE_PROFILE_SETTINGS_PREPARE) == -EIO && !first_open && !capturing && !tracing);
    stop_fails = false;
    assert(!perform(NCE_PROFILE_SETTINGS_PREPARE) && !settings_process());
    process(42, "com.android.settings");
    assert(perform(NCE_PROFILE_FRAMES) == -EBUSY && !capturing);
    std::filesystem::remove_all(path("/proc/42"));
    assert(!perform(NCE_PROFILE_FRAMES) && capturing);
    assert(!perform(NCE_PROFILE_SETTINGS));
    assert(perform(NCE_PROFILE_SETTINGS) == -EBUSY);
    assert(!perform(NCE_PROFILE_END) && !capturing && !first_open);
    FILE *metadata = fopen(path(NCE_PROFILE_OUTPUT "launch.jsonl").c_str(), "r");
    assert(metadata);
    char line[1024];
    for (const char *phase : {"prepared", "begin", "ready", "end"}) {
        assert(fgets(line, sizeof(line), metadata) && strstr(line, phase));
    }
    assert(!fgets(line, sizeof(line), metadata));
    fclose(metadata);
    assert(!perform(NCE_PROFILE_SETTINGS));
    assert(!perform(NCE_PROFILE_SETTINGS));
    assert(!perform(NCE_PROFILE_FRAMES));
    assert(!std::filesystem::exists(path(NCE_PROFILE_OUTPUT "launch.jsonl")));
    assert(perform(NCE_PROFILE_SETTINGS) == -EBUSY);
    assert(!perform(NCE_PROFILE_END));
    assert(!perform(NCE_PROFILE_SETTINGS_PREPARE));
    assert(!perform(NCE_PROFILE_FRAMES));
    tracing = true;
    write_file("/sys/kernel/tracing/tracing_on", "1");
    surface_fails = true;
    write_file("/sys/kernel/tracing/trace", "failed launch trace\n");
    write_file("/sys/kernel/tracing/per_cpu/cpu0/stats", "overrun: 0\n");
    write_file("/sys/kernel/tracing/trace_clock", "[mono]\n");
    assert(perform(NCE_PROFILE_SETTINGS) == -EIO);
    assert(!capturing && !tracing && !first_open);
    for (const char *name : {"kernel.ftrace", "cpu0.txt", "clock.txt", "launch.jsonl"}) {
        assert(std::filesystem::exists(path((std::string(NCE_PROFILE_OUTPUT) + name).c_str())));
    }
    assert(!std::filesystem::exists(path(NCE_PROFILE_OUTPUT "frames.txt")));
    metadata = fopen(path(NCE_PROFILE_OUTPUT "launch.jsonl").c_str(), "r");
    assert(metadata);
    for (const char *phase : {"prepared", "begin", "failed"}) {
        assert(fgets(line, sizeof(line), metadata) && strstr(line, phase));
    }
    assert(!fgets(line, sizeof(line), metadata));
    fclose(metadata);
    FILE *state = fopen(path("/sys/kernel/tracing/tracing_on").c_str(), "r");
    assert(state && fgetc(state) == '0');
    fclose(state);
    std::filesystem::remove_all(root);
    puts("interaction launch checks passed: process identity, force-stop, capture order, duplicates and failed-open trace cleanup");
}
