/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NCE_DESKTOP_SESSION_H
#define NCE_DESKTOP_SESSION_H
#include <initializer_list>
#include <sys/types.h>

void nce_android_command(std::initializer_list<const char *> arguments, const char *expected = nullptr);
// Return the child wait status, or a negative errno (ECANCELED after reaping).
int nce_wait_desktop_child(pid_t child, bool cancellable);
// False indicates an interactive stop; other child failures terminate the check.
bool nce_surface_check(const char *package = nullptr, const char *png = nullptr, bool cancellable = false);
void nce_framework_check(const char *name, unsigned seconds = 30);
bool nce_desktop_start(bool interactive);
void nce_interaction_profile_init();
int nce_interaction_profile_check();
void nce_interaction_profile_poll();
#endif
