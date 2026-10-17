#pragma once
// ct/render/layer_math.h
// 2D affine math for the compositor (canvas pixels, y down) and the layout rules of docs/phases/P08: stage rect, fit,
// clip transform, camera transform, picture-in-picture placement. Pure functions; no GPU.
// Thread model: pure values and functions.

#include <ct/core/types.h>
#include <ct/project/project.h>

namespace ct::render {

using project::PointF;

// 3x3 affine matrix, row-major m[9] = { a b tx ; c d ty ; 0 0 1 }, column vectors: p' = M * p.
// A * B applies B first.
struct Mat3 {
    float m[9];

    static Mat3 Identity();
    static Mat3 Translate(float tx, float ty);
    static Mat3 Scale(float sx, float sy);
    static Mat3 Rotate(float deg);   // positive = clockwise on screen (y down)
    Mat3 operator*(const Mat3& o) const;
    PointF Apply(PointF p) const;
    Mat3 Inverse() const;   // identity when singular
    // Length of the transformed x unit vector (uniform scale factor for similarity transforms).
    float ScaleFactor() const;
};

// Axis-aligned bounds of a rect after a transform.
RectF TransformBounds(const Mat3& m, const RectF& r);
// The four corners of r after m: TL, TR, BL, BR.
void TransformCorners(const Mat3& m, const RectF& r, PointF out[4]);

namespace layer_math {

RectF StageRect(const project::Canvas&);                         // canvas minus padding on every side (at least 1 px)
RectF FitContain(SizeI content, RectF bounds);                   // centered, aspect-preserving
// base rect -> transform: x, y offsets in canvas fraction, scale about the rect centre, rotation about the centre.
// Keyframed values (transform.x/y/scale/rotation_deg) are evaluated at timeline time t.
Mat3 ClipTransform(const project::Clip&, TimeUs t, RectF base_rect, const project::Canvas&);
// Zoom about (cx, cy) in canvas fraction: the point (cx, cy) moves to the canvas centre and everything scales by zoom.
Mat3 CameraTransform(const project::Camera::View&, const project::Canvas&);
// Camera clips shown picture-in-picture: width = size * canvas width, height from the camera aspect (square for the
// circle mask), placed in the corner with margin_px.
RectF PipRect(const project::Pip&, SizeI cam_size, const project::Canvas&);

// Keyframed clip property without allocating (Clip::Prop builds a std::string key for the map lookup).
float PropAt(const project::Clip&, const char* key, TimeUs t, float static_value);

} // namespace layer_math
} // namespace ct::render
