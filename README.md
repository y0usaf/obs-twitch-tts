# obs-twitch-tts

An OBS audio source that reads one Twitch channel's chat aloud with a local
neural voice.

It won't log in, post, filter or skip messages, and it doesn't download
models. It reads every chat line in order as "<user> says <message>",
dropping the name when the same person speaks again.

## Use

Add a **Twitch Chat TTS** source and set its channel. The source is plain
audio: set Audio Monitoring in Advanced Audio Properties if you want to
hear it yourself.

Each chatter gets one voice from **Voices**, picked by a hash of their name,
so regulars keep theirs across streams. The default model is Kokoro v1.0
and the default list, `0-3,5-7,9,14,16,18,21,22,25,26`, is every English
voice rated C or better in Kokoro's VOICES.md. In that model 0-10 are
American female, 11-19 American male, 20-23 British female and 24-27
British male. A single number gives everyone the same voice.

To use another model, point **Model directory** at any Kokoro or Piper
(`vits-piper-*`) directory from sherpa-onnx's
[tts-models](https://github.com/k2-fsa/sherpa-onnx/releases/tag/tts-models)
release, unpacked. A directory with `voices.bin` loads as Kokoro, anything
else as VITS. Its first `*.onnx` file is the model.

## How

Chat comes in over anonymous plaintext IRC (`irc.chat.twitch.tv:6667`, nick
`justinfan12345`). Each source keeps one `twitch-tts-say MODEL_DIR` process,
which loads the model once and then turns each `SPEAKER TEXT` line on
stdin into `u32 rate, u32 frames, f32 samples[frames]` on stdout. It runs
outside OBS because the phonemizer (espeak-ng) parses untrusted chat text.
If it crashes or takes more than 30 s on a message, the plugin kills it and
starts a fresh one for the next message.

## Build

```nix
pkgs.wrapOBS { plugins = [ obs-twitch-tts.packages.${system}.default ]; }
```

`nix build` produces `lib/obs-plugins/twitch-tts.so`, which links only
libobs and libc, and `bin/twitch-tts-say`, which links sherpa-onnx. The
default model is fetched into the store at build time.

GPL-2.0-or-later, like libobs.
