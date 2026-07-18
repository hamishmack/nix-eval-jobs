{
  stdenv,
  lib,
  nixComponents,
  pkgs,
}:

let
  revision = "1";
in
stdenv.mkDerivation {
  pname = "nix-eval-jobs";
  version = "${lib.versions.majorMinor nixComponents.nix-cli.version}.${revision}";
  src = lib.fileset.toSource {
    fileset = lib.fileset.unions [
      ./.clang-tidy
      ./meson.build
      ./src/meson.build
      ./tests-unit/meson.build
      (lib.fileset.fileFilter (file: file.hasExt "cc") ./src)
      (lib.fileset.fileFilter (file: file.hasExt "hh") ./src)
      (lib.fileset.fileFilter (file: file.hasExt "cc") ./tests-unit)
      ./tests-unit/data
    ];
    root = ./.;
  };
  checkInputs = [
    pkgs.gtest
    nixComponents.nix-util-test-support
  ];
  buildInputs = with pkgs; [
    nlohmann_json
    curl
    nixComponents.nix-store
    nixComponents.nix-fetchers
    nixComponents.nix-expr
    # Model B (NIX_EVAL_JOBS_PATCH_2026-07-18): the v3 bytecode-VM accessor
    # surface. The v3 nix flake exposes this component via
    # packaging/components.nix; the meson.build patch's
    # dependency('nix-expr-v3') resolves against it. Required for the
    # `nix build --override-input nix <v3-nix>` deploy route.
    nixComponents.nix-expr-v3
    nixComponents.nix-flake
    nixComponents.nix-main
    nixComponents.nix-cmd
  ];
  nativeBuildInputs =
    with pkgs.buildPackages;
    [
      meson
      pkg-config
      ninja
      # nlohmann_json can be only discovered via cmake files
      cmake
    ]
    ++ lib.optional stdenv.cc.isClang (lib.hiPrio pkgs.llvmPackages.clang-tools);

  passthru = {
    inherit nixComponents;
  };

  doCheck = true;

  meta = {
    description = "Hydra's builtin hydra-eval-jobs as a standalone";
    homepage = "https://github.com/nix-community/nix-eval-jobs";
    license = lib.licenses.gpl3;
    maintainers = with lib.maintainers; [
      adisbladis
      mic92
    ];
    platforms = lib.platforms.unix;
  };
}
