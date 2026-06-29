#!/usr/bin/env bash
set -euo pipefail

artifact_dir="${SROBOTIS_TEST_ARTIFACT_DIR:-/tmp/audio-manual-smoke}"
mkdir -p "${artifact_dir}"

if [[ "${AUDIO_MANUAL_RUN:-0}" != "1" ]]; then
  cat >&2 <<'EOF'
NOT RUN: audio manual device smoke requires real audio hardware.
Set AUDIO_MANUAL_RUN=1 after confirming the capture/playback device is attached.
Optional: set AUDIO_MANUAL_PLAYBACK=1 to play the captured WAV back.
EOF
  exit 2
fi

command -v arecord >/dev/null
command -v aplay >/dev/null

arecord -l
aplay -l

capture_path="${artifact_dir}/capture.wav"
arecord -q -d 1 -f S16_LE -r 16000 -c 1 "${capture_path}"
test -s "${capture_path}"

if [[ "${AUDIO_MANUAL_PLAYBACK:-0}" == "1" ]]; then
  aplay -q "${capture_path}"
fi

echo "PASS audio manual device smoke: ${capture_path}"
