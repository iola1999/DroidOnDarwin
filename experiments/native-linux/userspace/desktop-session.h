/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NCE_DESKTOP_SESSION_H
#define NCE_DESKTOP_SESSION_H
#include <initializer_list>

void nce_android_command(std::initializer_list<const char *> arguments, const char *expected = nullptr);
void nce_surface_check(const char *package = nullptr, const char *png = nullptr);
void nce_framework_check(const char *name, unsigned seconds = 30);
void nce_desktop_start(bool interactive);
#endif
