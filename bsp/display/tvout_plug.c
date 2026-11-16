/* tvout_plug — see tvout_plug.h. */
#include "tvout_plug.h"

tvout_plug_event_t tvout_plug_update(tvout_plug_t *plug, bool line, uint32_t now_ms) {
    if (line == plug->connected) {
        plug->settling = false;
        return TVOUT_PLUG_NONE;
    }
    if (!plug->settling || line != plug->pending) {
        plug->settling = true;
        plug->pending = line;
        plug->since_ms = now_ms;
        return TVOUT_PLUG_NONE;
    }
    const uint32_t settle = line ? TVOUT_PLUG_CONNECT_SETTLE_MS : TVOUT_PLUG_REMOVE_SETTLE_MS;
    if ((uint32_t)(now_ms - plug->since_ms) < settle)
        return TVOUT_PLUG_NONE;
    plug->settling = false;
    plug->connected = line;
    return line ? TVOUT_PLUG_CONNECTED : TVOUT_PLUG_REMOVED;
}
