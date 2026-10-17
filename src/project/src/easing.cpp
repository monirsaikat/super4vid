#include <ct/project/easing.h>

#include <algorithm>

namespace ct::project {

float ApplyEase(Ease e, float u) {
    u = std::clamp(u, 0.0f, 1.0f);
    switch (e) {
    case Ease::Linear: return u;
    case Ease::In: return u * u * u;
    case Ease::Out: {
        const float v = 1.0f - u;
        return 1.0f - v * v * v;
    }
    case Ease::InOut: {
        if (u < 0.5f) return 4.0f * u * u * u;
        const float v = -2.0f * u + 2.0f;
        return 1.0f - v * v * v / 2.0f;
    }
    case Ease::Hold: return 0.0f;
    }
    return u;
}

const char* EaseName(Ease e) {
    switch (e) {
    case Ease::Linear: return "linear";
    case Ease::In: return "in";
    case Ease::Out: return "out";
    case Ease::InOut: return "inout";
    case Ease::Hold: return "hold";
    }
    return "linear";
}

bool ParseEase(std::string_view name, Ease* out) {
    static constexpr Ease kAll[] = {Ease::Linear, Ease::In, Ease::Out, Ease::InOut, Ease::Hold};
    for (Ease e : kAll) {
        if (name == EaseName(e)) {
            *out = e;
            return true;
        }
    }
    return false;
}

} // namespace ct::project
