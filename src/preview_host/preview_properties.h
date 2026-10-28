#pragma once

// Details-pane properties (dimensions, duration, author...) read from the
// shell property store for Properties requests.

#include <cstdint>
#include <string>
#include <vector>

struct IPropertyStore;

namespace pulse::preview {

struct PreviewPropertyValue { std::wstring label, value; };

// At most six formatted label/value pairs; empty when the store is unavailable.
// rich (Quick Look): a few more rows for images and audio, at most twelve.
std::vector<PreviewPropertyValue> ReadPropertiesFromStore(const std::wstring& path, IPropertyStore* store,
                                                          bool rich = false);
std::vector<PreviewPropertyValue> ReadProperties(const std::wstring& path, bool rich = false);

// Playing time of a video file in milliseconds (grid thumbnail duration
// chip); 0 for other types or when the property is unavailable.
uint32_t ReadMediaDurationMs(const std::wstring& path);

} // namespace pulse::preview
