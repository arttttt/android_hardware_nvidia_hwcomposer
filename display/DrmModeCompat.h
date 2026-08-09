/*
 * Copyright (C) 2026 Artem Bambalov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <linux/types.h>

#include <drm/drm_mode.h>

/* What this tree's copy of the display headers predates.
 *
 * The way a display is told about high-dynamic-range content -- the transfer
 * function in use, the primaries and white point of the mastering display,
 * the luminance it was graded at -- was added to the kernel's own display
 * interface after the copy shipped here was taken. The composer core carries
 * one of these per display and hands it down with a frame, so the type has to
 * exist for that code to compile even where no display on this board would
 * ever be handed one.
 *
 * Declared exactly as the kernel declares it, so that a newer tree can drop
 * this file and change nothing else. Nothing here is an invention: this is a
 * copy of an interface, not a definition of one.
 */

extern "C" {

struct hdr_metadata_infoframe {
    __u8 eotf;
    __u8 metadata_type;

    struct {
        __u16 x;
        __u16 y;
    } display_primaries[3];

    struct {
        __u16 x;
        __u16 y;
    } white_point;

    __u16 max_display_mastering_luminance;
    __u16 min_display_mastering_luminance;

    __u16 max_cll;
    __u16 max_fall;
};

struct hdr_output_metadata {
    __u32 metadata_type;

    union {
        struct hdr_metadata_infoframe hdmi_metadata_type1;
    };
};

}  // extern "C"
