#pragma once

#include "matrix/EventPreview.h"
#include "matrix/PresentableName.h"
#include "matrix/PreviewToken.h"
#include "matrix/TimelineEvent.h"

#include <QString>

// The words of a room-state or membership row, for every surface that shows
// one (the timeline, the room export). Header-only: test targets compile
// TimelineModel.cpp and RoomExport.cpp without EventPreview.cpp.
namespace matrix::preview {

// The sentence the bridge named, worded in the UI language with the sender as
// presented (`actorLabel`: the name the surface resolved, else the event's
// own) and the target member; else the bridge's English body with its leading
// user id replaced by that name. `isolate`: see stateSentence().
inline QString stateRowText(const TimelineEvent &event, const QString &actorLabel,
                            bool isolate = true)
{
    const QString label = actorLabel.isEmpty() ? event.senderDisplayName : actorLabel;
    if (!event.stateSentence.isEmpty()) {
        const QString actor = presentable_name::presentable(
            event.sender, label, event.senderNameAmbiguous);
        const QString worded = stateSentence(
            event.stateSentence, actor.isEmpty() ? event.sender : actor,
            presentable_name::sanitized(event.stateTarget), isolate);
        if (!worded.isEmpty())
            return worded;
    }
    return actorSentence(event.body, event.sender, label, event.senderNameAmbiguous);
}

} // namespace matrix::preview
