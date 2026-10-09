/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "desktop-session.h"
#include <stdio.h>
#include <unistd.h>

void nce_desktop_start(bool interactive)
{
    nce_android_command({"activity", "start-activity", "-W", "-a", "android.intent.action.MAIN",
                         "-c", "android.intent.category.HOME"},
                        "Status: ok");
    nce_surface_check("com.google.android.apps.nexuslauncher", "/data/nce/desktop-launcher.png");
    if (interactive) {
        puts("{\"session\":\"android_desktop_ready\"}");
        dprintf(2, "{\"phase\":\"android_desktop_running\"}\n");
        return;
    }
    nce_android_command({"activity", "start-activity", "-W", "-n",
                         "com.google.android.deskclock/com.android.deskclock.DeskClock"},
                        "Status: ok");
    nce_surface_check("com.google.android.deskclock", "/data/nce/desktop-clock.png");
    nce_android_command({"activity", "start-activity", "-W", "-n",
                         "com.google.android.documentsui/com.android.documentsui.files.FilesActivity"},
                        "Status: ok");
    nce_surface_check("com.google.android.documentsui", "/data/nce/desktop-files.png");
    puts("{\"test\":\"android_desktop_launch_commands\",\"passed\":true,\"applications\":2}");
}
