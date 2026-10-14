#pragma once

// Show or hide the app's Dock icon (macOS). No-op elsewhere.
#ifdef __APPLE__
void puddle_set_dock_visible(bool visible);
#else
inline void puddle_set_dock_visible(bool) {}
#endif
