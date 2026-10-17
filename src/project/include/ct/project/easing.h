#pragma once
// ct/project/easing.h
// Easing curves used by keyframes and the camera, and their JSON names ("linear", "in", "out", "inout", "hold").
// Thread model: pure functions.

#include <string_view>

namespace ct::project {

enum class Ease { Linear, In, Out, InOut, Hold };

// u in [0,1] (clamped). Linear u; In u^3; Out 1-(1-u)^3; InOut u<.5 ? 4u^3 : 1-(-2u+2)^3/2; Hold 0.
float ApplyEase(Ease e, float u);

const char* EaseName(Ease e);
bool ParseEase(std::string_view name, Ease* out);   // false (and *out untouched) for unknown names

} // namespace ct::project
