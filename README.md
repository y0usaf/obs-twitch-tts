# obs-twitch-tts

An OBS audio source that reads one Twitch channel's chat aloud with flite.

It won't log in, post, filter, skip, or offer voices. It reads every chat
line in order as "<user> says <message>".

## Use

Add a **Twitch Chat TTS** source and set its channel. The source is plain
audio: set Audio Monitoring in Advanced Audio Properties if you want to
hear it yourself.

Chat comes in over anonymous plaintext IRC (`irc.chat.twitch.tv:6667`, nick
`justinfan12345`). Each message runs `flite` as a child process, so a TTS
crash can't take OBS down. Synthesis has a 10 s budget per message.

## Build

```nix
pkgs.wrapOBS { plugins = [ obs-twitch-tts.packages.${system}.default ]; }
```

`nix build` produces `lib/obs-plugins/twitch-tts.so`. It links only libobs
and libc and calls flite by its store path. That flite is built without audio
output, so its closure is flite plus glibc.

GPL-2.0-or-later, like libobs.
