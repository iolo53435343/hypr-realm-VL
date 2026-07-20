{ pkgs ? import <nixpkgs> { } }:

let
  lock = builtins.fromJSON (builtins.readFile ./flake.lock);

  fetchLockedGitHub =
    name:
    let
      locked = lock.nodes.${name}.locked;
    in
    assert locked.type == "github";
    builtins.fetchTarball {
      url = "https://github.com/${locked.owner}/${locked.repo}/archive/${locked.rev}.tar.gz";
      sha256 = locked.narHash;
    };

  nixpkgsSource = fetchLockedGitHub "nixpkgs";

  pinnedPkgs = import nixpkgsSource {
    localSystem = pkgs.stdenv.hostPlatform.system;
  };

  guiutilsSource = fetchLockedGitHub "hyprland-guiutils";
  guiutilsRevision = lock.nodes.hyprland-guiutils.locked.rev;

  hyprland-guiutils = pinnedPkgs.callPackage "${guiutilsSource}/nix/default.nix" {
    stdenv = pinnedPkgs.gcc15Stdenv;
    version = "0.2.1+${builtins.substring 0 7 guiutilsRevision}";
  };

  glaze-hyprland = pinnedPkgs.glaze.override {
    enableSSL = false;
    enableInterop = false;
  };

  udis86-hyprland = pinnedPkgs.udis86.overrideAttrs {
    src = pinnedPkgs.fetchFromGitHub {
      owner = "canihavesomecoffee";
      repo = "udis86";
      rev = "5336633af70f3917760a6d441ff02d93477b0c86";
      hash = "sha256-HifdUQPGsKQKQprByeIznvRLONdOXeolOsU5nkwIv3g=";
    };
    patches = [ ];
  };

  hypragent = pinnedPkgs.callPackage ./nix/default.nix {
    stdenv = pinnedPkgs.gcc16Stdenv;
    inherit glaze-hyprland hyprland-guiutils udis86-hyprland;
    commit = "";
    revCount = "";
    date = "2026-07-20";
    version = "0.55.0+agent-realms";
  };
in
hypragent.overrideAttrs (oldAttrs: {
  passthru = (oldAttrs.passthru or { }) // {
    portalPackage = pinnedPkgs.xdg-desktop-portal-hyprland;
  };
})
