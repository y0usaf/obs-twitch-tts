# obs-twitch-tts

An OBS global audio input that reads one Twitch channel's chat aloud with
a local neural voice.

It won't log in, post or moderate, and it doesn't download models. It
reads every chat line in order as "<user> says <message>", dropping the
name when the same person speaks again and leaving out links.

## Use

**Twitch Chat TTS** appears automatically in the Audio Mixer. Open its
**Properties** to set the channel. It stays active across scenes and
never adds scene items. Set Audio Monitoring in Advanced Audio Properties
if you want to hear it yourself.

Its settings, volume, mute and filters are saved with the scene collection.
It uses a free global channel outside OBS's six desktop/microphone slots,
so it leaves your audio devices alone. It isn't listed in the scene source
menu.

For an existing scene-source setup, note its channel, model and voices,
remove the old TTS sources from your scenes, then reload the scene
collection and configure the new mixer input. Until then, the plugin
leaves the old sources alone and does not add a second reader.

Each chatter gets one voice from **Voices**, picked by a hash of their name,
so regulars keep theirs across streams. The default model is Kokoro v1.0
and the default list, `0-3,5-7,9,14,16,18,21,22,25,26`, is every English
voice rated C or better in Kokoro's VOICES.md. In that model 0-10 are
American female, 11-19 American male, 20-23 British female and 24-27
British male. A single number gives everyone the same voice.

A word counts as a link if it contains `://`, starts with `www.`, looks
like `name.tld/path`, or is a bare `name.tld` with a TLD common in chat
(`.com`, `.tv`, `.gg`, `.io` and a few more). A message that is only
links isn't read at all.

To use another model, point **Model directory** at any Kokoro or Piper
(`vits-piper-*`) directory from sherpa-onnx's
[tts-models](https://github.com/k2-fsa/sherpa-onnx/releases/tag/tts-models)
release, unpacked. A directory with `voices.bin` loads as Kokoro, anything
else as VITS. Its first `*.onnx` file is the model.

## How

Chat comes in over anonymous plaintext IRC (`irc.chat.twitch.tv:6667`, nick
`justinfan12345`). The input keeps one `twitch-tts-say MODEL_DIR` process,
which loads the model once and then turns each `SPEAKER TEXT` line on
stdin into `u32 rate, u32 frames, f32 samples[frames]` on stdout. It runs
outside OBS because the phonemizer (espeak-ng) parses untrusted chat text.
If it crashes or takes more than 30 s on a message, the plugin kills it and
starts a fresh one for the next message.

## Build

```nix
inputs.obs-twitch-tts.url = "github:y0usaf/obs-twitch-tts";

pkgs.wrapOBS { plugins = [ obs-twitch-tts.packages.${system}.default ]; }
```

`nix build` produces `lib/obs-plugins/twitch-tts.so`, which links only
libobs, the OBS frontend API and libc, and `bin/twitch-tts-say`, which links
sherpa-onnx. The default model is fetched into the store at build time.

GPL-2.0-or-later, like libobs.
