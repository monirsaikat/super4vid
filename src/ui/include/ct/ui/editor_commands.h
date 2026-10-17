#pragma once
// ct/ui/editor_commands.h
// Editor commands shared by shortcuts, menus, the toolbar and context menus. Each one is a single undo entry
// through EditorState::Edit/EditCopy and returns whether it changed the project (false sets state.last_error when
// there was a reason worth showing).
// Thread model: UI thread.

#include <ct/project/project.h>
#include <ct/ui/editor_state.h>

namespace ct::ui::cmd {

// Selected clips that contain the playhead; with nothing selected, every clip under the playhead on unlocked tracks.
bool SplitAtPlayhead(EditorState& s);
bool DeleteSelection(EditorState& s, bool ripple);
bool DuplicateSelection(EditorState& s);
void SelectAll(EditorState& s);
void CopySelection(EditorState& s);
bool CutSelection(EditorState& s);
bool PasteAtPlayhead(EditorState& s);   // keeps relative offsets and tracks; fails on overlap
bool NudgeSelection(EditorState& s, int frames);

bool AddMarkerAtPlayhead(EditorState& s);
bool RemoveMarker(EditorState& s, TimeUs t);
bool RenameMarker(EditorState& s, TimeUs t, const char* label);
bool AddCameraKeyAtPlayhead(EditorState& s);   // current camera view at the playhead, manual key
bool RemoveNearestCameraKey(EditorState& s);   // within 1 s of the playhead
bool SetCameraMode(EditorState& s, project::Camera::Mode mode);

bool AddTrack(EditorState& s, project::Track::Kind kind);
enum class TrackFlag { Mute, Lock, Hide };
bool ToggleTrackFlag(EditorState& s, const project::Id& track, TrackFlag f);

bool SetClipSpeed(EditorState& s, const project::Id& clip, double speed);
bool ToggleClipMute(EditorState& s, const project::Id& clip);
bool ResetTransform(EditorState& s, const project::Id& clip);
bool SetClipColor(EditorState& s, const project::Id& clip, ColorF c);
// Crossfade into the next adjacent clip on the same track (duration min(500 ms, shorter clip / 2)).
bool AddTransitionAfter(EditorState& s, const project::Id& clip);
// The clip that directly follows `clip` on its track within one frame (nullptr when none).
const project::Clip* NextAdjacent(const EditorState& s, const project::Id& clip);

void SetLoopIn(EditorState& s);
void SetLoopOut(EditorState& s);
void ClearLoopIn(EditorState& s);
void ClearLoopOut(EditorState& s);

// Media name for labels: the file name of the path (no allocation; points into m.path).
const char* MediaDisplayName(const project::Media& m);

} // namespace ct::ui::cmd
