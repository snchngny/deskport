// SPDX-License-Identifier: GPL-3.0-or-later
#include <libavcodec/version_major.h>
// Stop compilation if the client dependency prefix shadows the host headers.
static_assert(LIBAVCODEC_VERSION_MAJOR == DESKPORT_EXPECTED_AVCODEC_MAJOR,
              "Host FFmpeg headers do not match its static libraries");
