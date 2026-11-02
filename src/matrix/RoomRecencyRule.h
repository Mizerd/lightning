#pragma once

#include "matrix/TimelineEvent.h"

// Which timeline rows may move a room up the room list and which may set its
// preview. One rule for the live-append path and the snapshot/diff path, which
// had drifted apart: the live path refused call rows and the snapshot path
// refused nothing, so a call row replaced the preview with an empty string.
//
// What reaches C++ as a CallEvent is only m.call.invite and m.rtc.notification
// (rust/src/timeline.rs): MatrixRTC MEMBERSHIP churn is filtered out of every
// timeline (lightning_event_filter) and never becomes a row, so it needs no rule
// here. A call row is somebody calling: it raises the room, but it carries no
// body, so it never replaces the preview.
namespace roomrecency {

inline bool raisesRecency(const TimelineEvent &event)
{
    return !event.isVirtual();
}

inline bool replacesPreview(const TimelineEvent &event)
{
    return raisesRecency(event) && event.type != TimelineEvent::CallEvent;
}

}
