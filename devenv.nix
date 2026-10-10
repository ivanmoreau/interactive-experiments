{ pkgs, lib, ... }:

{
  assertions = [{
    assertion = pkgs.stdenv.hostPlatform.isDarwin;
    message = "These native Metal examples require macOS.";
  }];

  # xcrun selects the locally installed Xcode compiler, SDK, and Metal tools.
  # Adding Nix's Apple SDK would mix it with the Xcode toolchain.
  apple.sdk = null;

  packages = [
    pkgs.bashInteractive
    pkgs.git
    pkgs.python3
    pkgs.sdl3
  ];

  # SDL's headers and dylib are separate Nix outputs. The triangle build copies
  # the library before changing its install name and signing it.
  env.SDL3_ROOT = "${lib.getDev pkgs.sdl3}";
  env.SDL3_LIBRARY = "${lib.getLib pkgs.sdl3}/lib/libSDL3.0.dylib";
  env.SDL3_VERSION = pkgs.sdl3.version;

  scripts.project-doctor = {
    description = "Check Xcode, Metal tools, and the triangle's pinned SDL SDK.";
    exec = ''
      set -euo pipefail
      "$DEVENV_ROOT/tools/check-native-tools.sh"
      test -f "$SDL3_ROOT/include/SDL3/SDL_gpu.h"
      test -f "$SDL3_ROOT/include/SDL3/SDL_version.h"
      test -f "$SDL3_LIBRARY"
      printf 'SDL %s\nHeaders: %s\nLibrary: %s\n' "$SDL3_VERSION" "$SDL3_ROOT/include" "$SDL3_LIBRARY"
    '';
  };

  scripts.build-examples = {
    description = "Build and sign both example app bundles.";
    exec = ''
      set -euo pipefail
      "$DEVENV_ROOT/examples/floating-triangle/build.sh"
      "$DEVENV_ROOT/examples/fluid-hose/build.sh"
    '';
  };

  scripts.check-examples = {
    description = "Build both apps and run checks that need no GUI or sensors.";
    exec = ''
      exec "$DEVENV_ROOT/tools/check-examples.sh"
    '';
  };

  enterShell = ''
    printf 'SDL %s. Commands: project-doctor, build-examples, check-examples.\n' "$SDL3_VERSION"
  '';

  enterTest = ''
    check-examples
  '';
}
