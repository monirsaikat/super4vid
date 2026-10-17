#pragma once
// ct/media/mf_runtime.h
// Forwarding header: the Media Foundation lifetime object lives in ct_platform (ct/platform/mf_runtime.h) so that
// ct_capture can use it without a ct_capture <-> ct_media cycle. media::MfRuntime / media::MfScope stay valid names.

#include <ct/platform/mf_runtime.h>

namespace ct::media {

using platform::MfRuntime;
using platform::MfScope;

} // namespace ct::media
