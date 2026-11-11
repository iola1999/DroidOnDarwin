/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NCE_INTERACTION_PROFILE_H
#define NCE_INTERACTION_PROFILE_H
/* Fixed guest commands shared by the benchmark controller and Android init. */
enum {
    NCE_PROFILE_SETTINGS = 1,
    NCE_PROFILE_FRAMES = 2,
    NCE_PROFILE_SCHEDULER = 3,
    NCE_PROFILE_END = 4,
    NCE_PROFILE_SETTINGS_PREPARE = 5,
};
#define NCE_PROFILE_REQUEST "/data/nce/interaction-request"
#define NCE_PROFILE_RESPONSE "/data/nce/interaction-response"
#define NCE_PROFILE_OUTPUT "/data/nce/interaction-"
#endif
