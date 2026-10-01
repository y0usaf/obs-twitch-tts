# obs-twitch-tts

An OBS audio source that reads one Twitch channel's chat aloud with a local
neural voice.

It won't log in, post, filter or skip messages, and it doesn't download
models. It reads every chat line in order as "<user> says <message>".

## Use

Add a **Twitch Chat TTS** source and set its channel. The source is plain
audio: set Audio Monitoring in Advanced Audio Properties if you want to
hear it yourself.

The default voice is Kokoro v1.0, speaker 3 (`af_heart`). Other English
speakers in that model: 0-10 American female (`af_*`), 11-19 American
male (`am_*`, 16 is `am_michael`), 20-23 British female, 24-27 British
male.

To use another model, point **Model directory** at any Kokoro or Piper
(`vits-piper-*`) directory from sherpa-onnx's
[tts-models](https://github.com/k2-fsa/sherpa-onnx/releases/tag/tts-models)
release, unpacked. A directory with `voices.bin` loads as Kokoro, anything
else as VITS. Its first `*.onnx` file is the model.

## How

Chat comes in over anonymous plaintext IRC (`irc.chat.twitch.tv:6667`, nick
`justinfan12345`). Each source keeps one `twitch-tts-say MODEL_DIR SPEAKER`
process, which loads the model once and then turns each line of text on
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
