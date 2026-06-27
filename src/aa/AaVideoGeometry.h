// AaVideoGeometry — aspect-correct placement of the Android Auto video frame
// inside the UI content rect, and the inverse touch mapping back to source-frame
// coordinates. Pure integer math, platform-independent, host-tested. This is the
// geometry half of AA_PLAN's "UI integration" (AA as a tab, chrome stays visible):
// AA video is a fixed standard size (480p = 800x480) that we scale-blit into the
// 696x432-ish content rect, so touches in that rect must be remapped to 800x480
// for the AA input channel.
#pragma once

namespace carsh::aa {

// A source frame (sw x sh) placed inside a destination rect (rx,ry,rw,rh),
// preserving aspect ratio (letterbox/pillarbox), centered, in absolute integer
// pixel coordinates. dw/dh are 0 if the inputs are degenerate.
struct VideoFit {
    int dx = 0, dy = 0, dw = 0, dh = 0;
};

// Largest centered rect with the source aspect ratio that fits in the dest rect.
// Uses 64-bit cross-multiplication instead of float so the result is exact and
// identical on host and device.
inline VideoFit fitVideo(int sw, int sh, int rx, int ry, int rw, int rh) {
    if (sw <= 0 || sh <= 0 || rw <= 0 || rh <= 0) return {rx, ry, 0, 0};
    // Compare source-aspect vs dest-aspect: rw/rh vs sw/sh  ->  rw*sh vs rh*sw.
    const long long widthFit = static_cast<long long>(rw) * sh;
    const long long heightFit = static_cast<long long>(rh) * sw;
    int dw, dh;
    if (widthFit <= heightFit) {
        // Dest is relatively narrower (or equal) -> width is the binding dimension.
        dw = rw;
        dh = static_cast<int>(static_cast<long long>(rw) * sh / sw);
    } else {
        // Height is the binding dimension.
        dh = rh;
        dw = static_cast<int>(static_cast<long long>(rh) * sw / sh);
    }
    return {rx + (rw - dw) / 2, ry + (rh - dh) / 2, dw, dh};
}

// Map a destination/panel point (px,py) back to source-frame coords (sx,sy in
// 0..sw-1 / 0..sh-1). Returns false if the point lies outside the placed video
// rect (e.g. in the letterbox bars), in which case sx/sy are untouched.
inline bool mapToSource(const VideoFit& f, int sw, int sh, int px, int py,
                        int& sx, int& sy) {
    if (f.dw <= 0 || f.dh <= 0) return false;
    if (px < f.dx || py < f.dy || px >= f.dx + f.dw || py >= f.dy + f.dh) return false;
    int x = static_cast<int>(static_cast<long long>(px - f.dx) * sw / f.dw);
    int y = static_cast<int>(static_cast<long long>(py - f.dy) * sh / f.dh);
    sx = x >= sw ? sw - 1 : x;
    sy = y >= sh ? sh - 1 : y;
    return true;
}

}  // namespace carsh::aa
