#pragma once

#include <string>

namespace OttoMusic {

/** True while a background stream task is running. */
bool IsPlaying();

/** Stop current playback (if any). */
void Stop();

/**
 * Search youtube.kytuoi.com, pick first result, start MP3 stream playback.
 * @return JSON string: {success, id, title} or {success:false, error}
 */
std::string PlayFirstSearchResult(const std::string& query);

}  // namespace OttoMusic
