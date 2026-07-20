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

  formatSecondsSinceEpoch =
    timestamp:
    let
      remainder = x: y: x - x / y * y;
      days = timestamp / 86400;
      secondsInDay = remainder timestamp 86400;
      hours = secondsInDay / 3600;
      minutes = (remainder secondsInDay 3600) / 60;
      seconds = remainder timestamp 60;
      shiftedDays = days + 719468;
      era = if shiftedDays >= 0 then shiftedDays / 146097 else (shiftedDays - 146096) / 146097;
      dayOfEra = shiftedDays - era * 146097;
      yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
      year = yearOfEra + era * 400;
      dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
      monthPrime = (5 * dayOfYear + 2) / 153;
      day = dayOfYear - (153 * monthPrime + 2) / 5 + 1;
      month = monthPrime + (if monthPrime < 10 then 3 else -9);
      adjustedYear = year + (if month <= 2 then 1 else 0);
      pad = value: if builtins.stringLength value < 2 then "0${value}" else value;
    in
    "${toString adjustedYear}${pad (toString month)}${pad (toString day)}${pad (toString hours)}${pad (toString minutes)}${pad (toString seconds)}";

  nixpkgsSource = fetchLockedGitHub "nixpkgs";
  nixpkgsLib = import "${nixpkgsSource}/lib";

  mkLockedInput =
    name:
    let
      locked = lock.nodes.${name}.locked;
      source = fetchLockedGitHub name;
      overlayFunction = import "${source}/nix/overlays.nix";
      overlayArguments = {
        self = input;
        inputs.self = input;
        lib = nixpkgsLib;
      };
      input = {
        outPath = source;
        inherit (locked) lastModified narHash rev;
        shortRev = builtins.substring 0 7 locked.rev;
        lastModifiedDate = formatSecondsSinceEpoch locked.lastModified;
        overlays = overlayFunction (builtins.intersectAttrs (builtins.functionArgs overlayFunction) overlayArguments);
      };
    in
    input;

  inputs = builtins.mapAttrs (_: name: mkLockedInput name) lock.nodes.root.inputs;

  self = {
    outPath = ./.;
    rev = "";
    shortRev = "classic";
    lastModifiedDate = "20260720";
    sourceInfo.revCount = 0;
    overlays = import ./nix/overlays.nix {
      inherit inputs self;
      lib = nixpkgsLib;
    };
  };

  pinnedPkgs = import nixpkgsSource {
    localSystem = pkgs.stdenv.hostPlatform.system;
    overlays = [
      self.overlays.hyprland-packages
      self.overlays.hyprland-extras
    ];
  };
in
pinnedPkgs.hyprland.overrideAttrs (oldAttrs: {
  passthru = (oldAttrs.passthru or { }) // {
    portalPackage = pinnedPkgs.xdg-desktop-portal-hyprland;
  };
})
