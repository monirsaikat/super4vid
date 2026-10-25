# Changelog

All notable changes to super4vid are documented here. The format follows
[keep a changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[semantic versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.2.0] - 2026-10-25

### Added

- **Playback audio settings** (`Settings > Audio > Playback`): the **output device** the editor plays the timeline
  to, a **volume** level and a **mute** switch. All three are remembered between sessions. Choosing a device that has
  since been unplugged falls back to the system default instead of leaving the editor silent.
- **Output level meter** in the preview's volume popover, next to the name of the device being played to, with a link
  straight to `Settings > Audio`.
- The preview's volume and mute are now saved, so the level set in one session is the level in the next.

### Fixed

- **The app closed when media was dropped on the timeline with no track to put it on.** Creating the track mid-frame
  left the timeline's clip index out of date, and drawing it read past the end of that index. Dropping media on an
  empty timeline now simply creates the track it needs. Covered by `drop_media_with_no_tracks_creates_a_track`.
- **The window did not fit a laptop screen.** The editor asked for a 1600x900 window without consulting the monitor,
  so on a smaller screen - or a scaled one, where that is 2400x1350 real pixels at 150 per cent - it opened larger
  than the display with its title bar and controls off-screen. Windows are now kept inside the monitor's work area.
  Covered by the `place_window_*` tests.
- **Dialogs too tall for a small screen** (export, keyboard shortcuts, the unsaved-changes and recovery prompts) are
  now kept inside the window.
- **The preview's transport bar overlapped itself on a narrow panel.** It now drops detail in order - the zoom
  button's label, then the total duration, then the timecode, then the go-to-start/end buttons - instead of drawing
  controls on top of each other.
- **Error messages in the media panel wrapped one word per line.** A banner in a narrow panel now puts its buttons
  below the message.
- **Add to timeline** in the media bin's context menu creates a track when no compatible one exists, instead of
  refusing with "No compatible track with room at the playhead".
- Tick labels clipped at the right edge of the timeline ruler are no longer drawn as fragments.
- `--settings-demo --section <name>` opens the settings window again; the section name was being read as a file to
  open.

### Changed

- Property labels in the Properties panel are consistently lowercase; `Name`, `Start`, `Duration`, `Source`, `Speed`
  and `Color` were the only title-case ones.
- The media panel's first tab is named **Files**, so it no longer repeats the panel's own title.
- `settings.json` has a new `playback` section (`output_device`, `volume`, `muted`). Older files are read as before
  and gain the defaults.

## [0.1.1] - 2026-10-19

### Fixed

- Opening the **Export** menu, or hovering the **Export** button, showed a red "Programmer error: 2 visible items with
  conflicting ID" message. They had the same internal ID.
- Release builds no longer show the developer-only ID-conflict popup.

## [0.1.0] - 2026-10-19

First public release: screen, system audio, microphone and webcam recording through a GPU pipeline, a timeline editor
with automatic zoom, a re-rendered cursor, keystroke badges, annotations and backgrounds, and export to MP4, GIF, PNG
and M4A.

[Unreleased]: https://github.com/monirsaikat/super4vid/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/monirsaikat/super4vid/compare/v0.1.1...v0.2.0
[0.1.1]: https://github.com/monirsaikat/super4vid/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/monirsaikat/super4vid/releases/tag/v0.1.0
