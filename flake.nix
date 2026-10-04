{
  description = "OBS global audio input that reads Twitch chat aloud";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = {nixpkgs, ...}: {
    packages = nixpkgs.lib.genAttrs ["x86_64-linux" "aarch64-linux"] (system: let
      pkgs = nixpkgs.legacyPackages.${system};
      kokoro = pkgs.fetchzip {
        url = "https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/kokoro-multi-lang-v1_0.tar.bz2";
        hash = "sha256-HKrjB25Dqlu7p000Js6S6+y32h/dDEjn7vQqWnCU74E=";
      };
    in {
      default = pkgs.runCommandCC "obs-twitch-tts" {buildInputs = [pkgs.obs-studio pkgs.sherpa-onnx];} ''
        mkdir -p $out/bin $out/lib/obs-plugins
        $CC -O2 -Wall -Wextra -Werror -DTHREADS=4 \
          -o $out/bin/twitch-tts-say ${./say.c} -lsherpa-onnx-c-api
        $CC -shared -fPIC -O2 -Wall -Wextra -Werror \
          -I${pkgs.obs-studio}/include/obs \
          -DSAY="\"$out/bin/twitch-tts-say\"" -DMODEL='"${kokoro}"' \
          -o $out/lib/obs-plugins/twitch-tts.so ${./twitch-tts.c} -lobs -lobs-frontend-api
      '';
    });
  };
}
