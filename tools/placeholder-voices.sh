#!/bin/bash
# Fill in any missing voice clips with the Mac's voice, so games work before
# you've recorded everything. Never overwrites a clip that already exists.
set -e
cd "$(dirname "$0")/.."
v="Flo (English (US))"
make() { # make <file> <text>
  [ -f "$1" ] && return
  say -v "$v" -r 140 -o /tmp/ph.aiff "$2"
  afconvert -f WAVE -d LEI16@22050 -c 1 /tmp/ph.aiff "$1"
  echo "  placeholder: $1"
}
# how the Mac voice should attempt each letter sound (it's only a stand-in)
declare -a SOUNDS=(a:"ah" b:"buh" c:"kuh" d:"duh" e:"eh" f:"ffff" g:"guh" h:"huh" i:"ih"
  j:"juh" k:"kuh" l:"llll" m:"mmmm" n:"nnnn" o:"aw" p:"puh" q:"kwuh" r:"rrrr" s:"ssss"
  t:"tuh" u:"uh" v:"vvvv" w:"wuh" x:"ks" y:"yuh" z:"zzzz")
for pair in "${SOUNDS[@]}"; do make "shared/voice/letters/${pair%%:*}.wav" "${pair#*:}"; done
grep -v '^#' shared/letters.txt | while IFS='|' read -r l _ word _; do
  make "shared/voice/words/$l.wav" "$word"
done
i=0; for w in zero one two three four five six seven eight nine; do
  make "shared/voice/numbers/$i.wav" "$w"; i=$((i+1)); done
make shared/voice/prompts/where-is.wav "Where is"
make shared/voice/prompts/thats.wav "That's"
# the croc's voice: higher and quicker, like a cartoon
croc() { # croc <name> <text>
  local f="shared/voice/croc/$1.wav"
  [ -f "$f" ] && return
  say -v "$v" -r 170 -o /tmp/ph.aiff "$2"
  ffmpeg -loglevel error -y -i /tmp/ph.aiff -af "asetrate=22050*1.35,aresample=22050" -ac 1 -c:a pcm_s16le "$f"
  echo "  placeholder: $f"
}
croc hi "Hi!"
croc giggle "hee hee hee hee!"
croc rawr "Rawwr!"
croc yum "Yum yum!"
rm -f /tmp/ph.aiff
