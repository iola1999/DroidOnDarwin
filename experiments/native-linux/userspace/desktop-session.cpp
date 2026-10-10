/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "diagnostics.h"
#include "desktop-session.h"
#include <dirent.h>
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
        if (count > 0 && (!strcmp(name, "com.android.systemui") || !strcmp(name, "system_server"))) {
            pid_t pid = atoi(process->d_name);
            dprintf(2, "Requesting ART SIGQUIT trace: %s pid=%d result=%d\n", name, pid, kill(pid, SIGQUIT));
        }
    }
    closedir(processes);
}

void nce_desktop_start(bool interactive)
{
    nce_android_command({"input", "keyevent", "KEYCODE_WAKEUP"});
    nce_android_command({"window", "dismiss-keyguard"});
    // NceSurface waits for the unlocked, focused Activity and its actual first frame.
    nce_android_command({"activity", "start-activity", "-a", "android.intent.action.MAIN",
                         "-c", "android.intent.category.HOME"},
                        "Starting:");
    trace_systemui();
    nce_surface_check("com.android.launcher3", nce_diagnostics() ? "/data/nce/desktop-launcher.png" : "--ready");
    if (interactive) {
        puts("{\"session\":\"android_desktop_ready\"}");
        dprintf(2, "{\"phase\":\"android_desktop_running\"}\n");
        return;
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
}
