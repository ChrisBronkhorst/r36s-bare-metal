#!/bin/bash
# Record your own voice, one clip at a time.
#   ./record-voice.sh numbers          zero .. nine            (shared by all games)
#   ./record-voice.sh letters          letter sounds: "mmm", "sss", ...
#   ./record-voice.sh words            apple, ball, cat, ...
#   ./record-voice.sh prompts          "Where is", "That's"
#   ./record-voice.sh croc             the croc's voice: hi, giggle, rawr, yum
#   ./record-voice.sh balloon-pop      red, orange, ... (Balloon Pop only)
# Add names to redo just some clips:
#   ./record-voice.sh numbers 6 8      ./record-voice.sh letters m s
# For each clip: press Enter, wait for "Go!", say it, and it stops by itself.
# It plays the clip back; press Enter to keep it or type r to redo.
set -e
cd "$(dirname "$0")"
set_name="${1:?usage: ./record-voice.sh numbers|letters|words|prompts|croc|balloon-pop [names...]}"

files=(); prompts=()
case "$set_name" in
  numbers)
    out="shared/voice/numbers"
    files=(0 1 2 3 4 5 6 7 8 9)
    prompts=(Zero! One! Two! Three! Four! Five! Six! Seven! Eight! Nine!) ;;
  letters|words)
    out="shared/voice/$set_name"
    while IFS='|' read -r letter sound word _; do
      [[ "$letter" == \#* || -z "$letter" ]] && continue
      files+=("$letter")
      if [ "$set_name" = letters ]; then
        prompts+=("the SOUND of $sound, short and clear")
      else
        prompts+=("$word!")
      fi
    done < shared/letters.txt ;;
  prompts)
    out="shared/voice/prompts"
    files=(where-is thats)
    prompts=("Where is...? (just those two words, like asking a question)" "That's... (just that word)") ;;
  croc)
    out="shared/voice/croc"
    files=(hi giggle rawr yum)
    prompts=("Hi! (in a silly croc voice)" "a giggle: hee hee hee!" "a friendly roar: Rawwr!" "Yum yum!") ;;
  balloon-pop)
    out="games/balloon-pop/sounds"
    files=(red orange yellow green blue purple pink)
    prompts=(Red! Orange! Yellow! Green! Blue! Purple! Pink!) ;;
  *) echo "don't know the clips for $set_name"; exit 1 ;;
esac

# the croc's clips get pitched up so your voice sounds like a cartoon croc
# (set CROC_PITCH=1 to keep your normal voice, or e.g. 1.5 for squeakier)
pitch=""
if [ "$set_name" = croc ]; then
  pitch="asetrate=44100*${CROC_PITCH:-1.3},aresample=44100,"
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

echo "Recording with the MacBook microphone. Speak about an arm's length away."
echo "(The first time, macOS may ask to allow Terminal to use the microphone.)"
echo

only=" ${*:2} " # optional: just redo these

for i in "${!files[@]}"; do
  if [ "$only" != "  " ] && [[ "$only" != *" ${files[$i]} "* ]]; then continue; fi
  prompt="${prompts[$i]}"
  while true; do
    read -r -p "Next: $prompt   Press Enter when ready... "
    ffmpeg -loglevel error -y -f avfoundation -i ":0" -t 2.5 -ac 1 -ar 44100 "$tmp/raw.wav" &
    sleep 0.5 # give the microphone a moment to start
    echo "  🎙  Go! Say: $prompt"
    wait
    # a very quiet take would get boosted into loud crackling, so catch it here
    peak=$(ffmpeg -hide_banner -i "$tmp/raw.wav" -af astats=measure_perchannel=none -f null - 2>&1 \
      | awk '/Overall/ {o=1} o && /Peak level dB/ {print int($NF); exit}')
    if [ -z "$peak" ] || [ "$peak" -lt -30 ]; then
      echo "  ⚠️  That was very quiet (${peak:-silent} dB), so it would sound crackly."
      echo "     Speak up or come closer. If it keeps happening, turn up the microphone:"
      echo "     System Settings → Sound → Input → Input volume."
      continue
    fi
    # cut the quiet before and after, even out the loudness, save as the games expect
    ffmpeg -loglevel error -y -i "$tmp/raw.wav" -af "\
silenceremove=start_periods=1:start_threshold=-50dB:start_silence=0.08,\
areverse,silenceremove=start_periods=1:start_threshold=-50dB:start_silence=0.2,areverse,\
${pitch}loudnorm=I=-16:TP=-2" -ar 22050 -ac 1 -c:a pcm_s16le "$tmp/clean.wav"
    afplay "$tmp/clean.wav"
    read -r -p "  Keep it? [Enter = keep, r = redo] " answer
    if [ "$answer" != "r" ]; then
      cp "$tmp/clean.wav" "$out/${files[$i]}.wav"
      break
    fi
  done
done

echo
echo "Done! Saved to $out."
