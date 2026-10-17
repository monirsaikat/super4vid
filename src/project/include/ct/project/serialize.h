#pragma once
// ct/project/serialize.h
// .ctproj (de)serialization (docs/04 section 3): fixed key order, 2-space pretty printing, floats as %.6g, times as
// integers, colors as #RRGGBB[AA], enums as lowercase strings, unknown keys re-emitted after the known ones. Files
// written by this version round-trip byte for byte (Save(Load(x)) == x). Older versions are migrated, never refused.
// Thread model: pure functions; Save/Load do blocking file IO (call them from ct::Async jobs in the UI).

#include <ct/core/error.h>
#include <ct/core/json.h>
#include <ct/project/project.h>

#include <string>
#include <string_view>

namespace ct::project {

// Media paths are written exactly as they are in memory (undo snapshots, autosave).
std::string ToJson(const Project& p);
Json ToJsonValue(const Project& p);
// Replaces `p` (path is kept). Fills defaults for missing keys, ignores values of the wrong type, migrates older
// versions. next_id is recomputed from the ids. False only for unparsable JSON or a non-project document.
bool FromJson(std::string_view text, Project& p, Err* err);
bool FromJsonValue(Json j, Project& p, Err* err);

// Atomic write (ct::fs::WriteAtomic). Media paths are stored relative to the project directory (with '/') when they
// can be (same drive/share), else absolute.
bool Save(const Project& p, std::string_view path, Err* err);
// Reads, migrates and validates. Relative media paths are resolved against the project directory; a missing file
// is also looked for next to the project; media that cannot be found get `missing = true` (the project still
// loads). Sets p.path to the absolute project path. Fails when the timeline violates Project::Validate.
bool Load(std::string_view path, Project& p, Err* err);

// Upgrades a parsed document from `from_version` to kProjectVersion in place.
// Version 0 (pre-release drafts): no "format"/"version"; clips had "duration_us" (timeline length) instead of
// "out_us"; canvas "background" was a "#RRGGBB" string; camera keys were called "keys".
bool Migrate(Json& j, int from_version, Err* err);

// Float formatting used by the writer: %.6g, "-0" normalized to 0, parsed back to the double it prints as.
double RoundFloat(float v);

} // namespace ct::project
