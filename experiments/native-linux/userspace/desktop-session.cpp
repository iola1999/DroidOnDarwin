/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "diagnostics.h"
#include "desktop-session.h"
#include <dirent.h>
#include <errno.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void trace_systemui()
{
    if (access("/data/nce/trace-systemui", F_OK) != 0)
        return;
    DIR *processes = opendir("/proc");
    if (!processes)
        return;
    while (dirent *process = readdir(processes)) {
        if (process->d_name[0] < '1' || process->d_name[0] > '9')
            continue;
        char path[512], name[128] = {};
        snprintf(path, sizeof(path), "/proc/%s/cmdline", process->d_name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        ssize_t count = read(fd, name, sizeof(name) - 1);
        close(fd);
        if (count > 0 && (!strcmp(name, "com.android.systemui") || !strcmp(name, "system_server") || !strcmp(name, "com.android.launcher3"))) {
            pid_t pid = atoi(process->d_name);
            dprintf(2, "Requesting ART SIGQUIT trace: %s pid=%d result=%d\n", name, pid, kill(pid, SIGQUIT));
        }
    }
    closedir(processes);
}

int nce_wait_desktop_child(pid_t child, bool cancellable)
{
    for (;;) {
        int status;
        pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child)
            return status;
        if (waited < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (cancellable && access("/data/nce/session-stop", F_OK) == 0) {
            // The helper owns an ART runtime. Reap it before stopping its Binder services.
            if (kill(child, SIGKILL) < 0 && errno != ESRCH)
                return -errno;
            do {
                waited = waitpid(child, &status, 0);
            } while (waited < 0 && errno == EINTR);
            if (waited != child)
                return -errno;
            puts("{\"session\":\"android_surface_wait_cancelled\"}");
            return -ECANCELED;
        }
        usleep(100000);
    }
}

bool nce_desktop_start(bool interactive)
{
    nce_android_command({"input", "keyevent", "KEYCODE_WAKEUP"});
    nce_android_command({"window", "dismiss-keyguard"});
    // NceSurface waits for the unlocked, focused Activity and its actual first frame.
    nce_android_command({"activity", "start-activity", "-a", "android.intent.action.MAIN",
                         "-c", "android.intent.category.HOME"},
                        "Starting:");
    puts("{\"phase\":\"android_desktop_first_frame_wait\"}");
    if (!nce_surface_check("com.android.launcher3", nce_diagnostics() ? "/data/nce/desktop-launcher.png" : "--ready", interactive))
        return false;
    if (interactive && access("/data/nce/session-stop", F_OK) == 0)
        return false;
    trace_systemui();
    if (interactive) {
        puts("{\"session\":\"android_desktop_ready\"}");
        dprintf(2, "{\"phase\":\"android_desktop_running\"}\n");
        return true;
    }
    nce_android_command({"activity", "start-activity", "-a", "android.intent.action.SHOW_ALARMS",
                         "-p", "com.android.deskclock"},
                        "Starting:");
    nce_surface_check("com.android.deskclock", "/data/nce/desktop-clock.png");
    nce_framework_check("NceAppData", 60);
    nce_android_command({"activity", "start-activity", "-a", "android.intent.action.SHOW_ALARMS",
                         "-p", "com.android.deskclock"},
                        "Starting:");
    nce_surface_check("com.android.deskclock", "/data/nce/desktop-clock.png");
    puts("{\"test\":\"android_clock_alarm_activity\",\"passed\":true}");
    nce_android_command({"activity", "start-activity", "-n",
                         "com.google.android.documentsui/com.android.documentsui.files.FilesActivity"},
                        "Starting:");
    nce_surface_check("com.google.android.documentsui", "/data/nce/desktop-files.png");
    puts("{\"test\":\"android_desktop_launch_commands\",\"passed\":true,\"applications\":2}");
    return true;
}
