{
  description = "OBS audio source that reads Twitch chat aloud";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = {nixpkgs, ...}: {
    packages = nixpkgs.lib.genAttrs ["x86_64-linux" "aarch64-linux"] (system: let
      pkgs = nixpkgs.legacyPackages.${system};
      flite = pkgs.flite.overrideAttrs {
        buildInputs = [];
        configureFlags = ["--enable-shared" "--with-audio=none"];
      };
    in {
      default = pkgs.runCommandCC "obs-twitch-tts" {buildInputs = [pkgs.obs-studio];} ''
        mkdir -p $out/lib/obs-plugins
        $CC -shared -fPIC -O2 -Wall -Wextra -Werror \
          -I${pkgs.obs-studio}/include/obs \
          -DFLITE='"${flite.bin}/bin/flite"' \
          -o $out/lib/obs-plugins/twitch-tts.so ${./twitch-tts.c} -lobs
      '';
    });
  };
}
