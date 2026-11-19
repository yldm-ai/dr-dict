#pragma once
#include <algorithm>
#include <windows.h>
namespace dr
{
// Where the popup goes when an input method docks it beside its candidate window (popup.h
// dock_request). All in screen pixels; `anchor` is the candidate window's visible card, `work` the
// work area of its monitor, `size` the popup's (already no larger than `work`).
//   - Flush against the anchor's right edge; when that runs off the work area, flush against its
//     left edge instead; when neither side has room, on the roomier side, pushed back inside.
//   - Top aligned with the anchor's top, moved up as far as needed to keep the bottom on the work
//     area, and never above its top.
inline POINT dock_position(const RECT &anchor, SIZE size, const RECT &work)
{
    LONG x = 0;
    if (anchor.right + size.cx <= work.right)
        x = anchor.right;
    else if (anchor.left - size.cx >= work.left)
        x = anchor.left - size.cx;
    else
        x = work.right - anchor.right >= anchor.left - work.left ? work.right - size.cx : work.left;
    x = std::clamp<LONG>(x, work.left, std::max<LONG>(work.left, work.right - size.cx));
    LONG y = std::min<LONG>(anchor.top, work.bottom - size.cy);
    y = std::max<LONG>(y, work.top);
    return {x, y};
}
} // namespace dr
