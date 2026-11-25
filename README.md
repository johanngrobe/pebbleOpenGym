# openGym for Pebble Time 2

Train with only your watch. Leave the phone in the locker: the Pebble keeps your
[openGym](https://github.com/DuarteSantos8/openGym) routines, runs the whole workout on its own
(sets, weights, reps, holds, rest timer) and uploads it to your self-hosted openGym server the next
time the phone is near.

## Setup

1. **Make an API token** in openGym: *Settings → Integrations → API tokens*, with
   - Workouts: **Change directly** (to save the workouts)
   - Plans & routines: **See** (to load the routines)
2. **Install** the app (see *Building*) and open its settings in the Pebble phone app. Enter your
   server address (e.g. `https://gym.example.com` or `http://192.168.1.20:8080`) and the token.
3. Open the app on the watch once with the phone nearby. It loads your routines and shows
   "Synced HH:MM" under *Phone → Sync now*.

The app syncs every time it starts with the phone in reach, and right after a workout is finished.
*Sync now* does it on request.

## At the gym

| Screen | Up / Down | Select | Hold Select | Back |
| --- | --- | --- | --- | --- |
| Set | change the highlighted value | set done | switch between weight and reps | workout menu |
| Hold (timed exercise) | – | stop early | – | workout menu |
| Rest | +15 s / −15 s | skip rest | – | workout menu |
| All sets done | – | finish and save | – | workout menu |

- The highlighted box is the one Up / Down change: hold Select to switch between weight and reps,
  or on the Pebble Time 2 tap the box.
- Symbols after the set number: a chain link for a superset or circuit (the line then stands out
  on an accent background), a half-filled circle for a one-sided exercise (reps or hold per side).
- A changed weight or rep count carries over to the remaining sets of that exercise.
- The watch buzzes 10 s before a rest ends and again (with the backlight) when it is over.
- The **workout menu** has: Resume, Exercises (jump to any exercise, or add one more set to a
  finished one), Skip this set, Add a set, Finish and save, Discard (press twice), Leave for now.
- The running workout is saved after every set. If the app is closed it reopens where you were.
- Finishing a saved workout plays a short celebration (any button skips it), then a summary to
  scroll through: time, sets, weight lifted and reps, average and maximum heart rate and active
  calories from Pebble Health (when the watch has them), the top set, and each exercise's sets
  with its best set.
- *Today* shows the routine planned for today in openGym (weekly plan and day overrides).
- Weights are prefilled from the routine, otherwise from your last workout with that exercise.

## What reaches openGym

Each finished workout is saved like one logged in the app: date, start and end, routine, every done
set (`w`/`r`, or `sec` for holds, `min` for cardio, `m` for distance), top weight, volume and
personal records. The confirmed weights (`exWeights`) are updated too. Workout ids are derived from
the start time (`pbl…`), so uploading twice never duplicates a workout.

On the way: the workout is stored on the watch, handed to the phone (kept there in the app's
storage, and only then removed from the watch), and written to the server over the revision it read.
If the server is unreachable it stays on the phone and goes with the next sync.

## Requirements

- A self-hosted openGym **1.4.5 or newer** with API tokens on (the default; `API_TOKENS=off`
  removes them). Make the token on the website, signed in: Settings → Integrations → API tokens,
  with Workouts at *Change directly* and Plans & routines at *See*.

## Limits

- Watch storage is small (4 KB on older firmware). The phone sends as many routines as fit,
  the ones planned for the next two weeks first. Up to 16 routines, 16 exercises per routine and
  12 sets per exercise. The watch keeps about four finished workouts while waiting for the phone.
- Exercise names are bundled in English and German. Other profile languages show English.
  Your own exercises keep the name you gave them.
- The app itself is in English and German and follows the watch's system language. The phone
  messages and the settings page follow the watch's language as well (after its first sync).
- Supersets and circuits show in the routine preview (a bracket with the group's name and rest),
  but the workout itself runs their exercises one after the other, not in rounds.
- Per-side exercises: reps show per side (openGym's total, halved); a timed hold runs a left and
  a right hold per set and uploads them as openGym's L/R rows.
- Warm-up sets (the routine's `warmupSets`) ramp up to the work weight like openGym's, rounded
  down to the weight step; the bar's own weight is not taken into account. Their rest is at most
  60 s, and they count for neither volume nor records.
- A workout note can be dictated from the workout menu (needs the phone for the transcription).
  Notes on single exercises, RIR/RPE, drop sets and bands are not on the watch.
- The vibration when a rest or hold ends is set on the settings page (normal, strong, short, off).
- Focus: when a workout starts the watch reminds you to turn on Do Not Disturb, so notifications
  don't interrupt the workout. Pebble apps can't switch Do Not Disturb on themselves. The reminder
  closes with any button or after 20 s, and can be turned off on the settings page.
- Heart rate: shown in the top right of the workout, a red heart and the number. While the
  workout is on screen the watch asks the sensor for a reading as often as set on the settings
  page (every 1, 5 (default), 10 or 30 s) and lets it go back to its normal rate afterwards; a
  reading older than three intervals, and at least 30 s, is hidden. *Off* leaves the sensor alone.
- Workout progress: next to the heart rate, the sets done or skipped out of all, as a little pie
  (default) or in numbers ("12/30"), set on the settings page.
- Text size is set on the settings page too: *Like the watch* (default) follows the watch's own
  text size setting and switches to the biggest layout when the watch is set to Extra large;
  *Normal*, *Large* and *Extra large* set it for this app only. The large sizes use bigger fonts
  everywhere and drop a few hint lines to make room.
- Colours follow openGym's: a dark or light theme and an accent colour. By default the watch
  takes both from the openGym profile with each sync, including your own colour, matched to the
  nearest of the watch's 64 colours that still reads on the background; openGym's *System* theme
  is dark on the watch. The settings page can set either for the watch alone (*Dark*, *Light*,
  and openGym's nine accent presets).
- Heart rate is shown during the workout but not saved to openGym.
- The rest alarm only works while the app is open.

## Building

Needs the Pebble SDK (`pebble` tool, SDK 4.x) and [uv](https://docs.astral.sh/uv/).

```sh
pebble build                       # build/pebbleOpenGym.pbw (Pebble Time 2 / emery)
pebble install --phone <phone-ip>  # or open the .pbw on the phone
pebble install --emulator emery    # try it in the emulator
```

Development tools (Python, run with uv):

```sh
uv run pebbleopengym names         # regenerate src/pkjs/exercise_names.js from the openGym catalogue
                                   # (--repo <clone> to use a local checkout, --lang en,de,fr …)
uv run pebbleopengym mock-server   # fake openGym token API on :8787, token "ogt_test"
node test/codec.test.js            # phone-side format tests

# read-only check of a real server: token scopes, and the routines as the watch would get them
OPENGYM_URL=https://gym.example.com OPENGYM_TOKEN=ogt_... node test/check-server.js
```

To try it end to end, run the mock server, install in the emulator and set the server to
`http://localhost:8787` with token `ogt_test` (`pebble emu-app-config --emulator emery`).

## How it fits together

```
src/c/main.c      app start, reopens a running workout
src/c/data.c      routines, running workout and upload queue in persistent storage
src/c/comm.c      sync with the phone (upload queue first, then routines)
src/c/home.c      today, routines, routine preview, sync status
src/c/workout.c   set / hold / rest / done screens, workout menu, summary
src/c/ui.c        text size and colours
src/pkjs/         phone side: openGym API, data formats, settings page, exercise names
src/pebbleopengym development tools (names generator, mock server)
```

## License

GNU Affero General Public License v3.0 or later, see [LICENSE](LICENSE). The exercise names in
`src/pkjs/exercise_names.js` come from the [openGym](https://github.com/DuarteSantos8/openGym)
exercise catalogue, which is AGPL-3.0 as well.
