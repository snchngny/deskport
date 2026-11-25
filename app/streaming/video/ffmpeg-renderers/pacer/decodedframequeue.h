#pragma once
#include <QQueue>

// Only decoded, ready-to-present frames may use this policy. Compressed frames
// retain inter-frame dependencies and must never be discarded this way.
template<class Frame, class Dispose>
Frame takeDecodedPresentation(QQueue<Frame>& frames, bool paced, Dispose dispose) {
    Q_ASSERT(!frames.isEmpty());
    if (!paced) while (frames.size()>1) dispose(frames.dequeue());
    return frames.dequeue();
}
