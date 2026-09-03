{
  description = "Development environment for Planar";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs = { nixpkgs, ... }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
    in {
      devShells.${system}.default = pkgs.mkShell {
        packages = with pkgs; [
          # Build tools
          meson
          ninja
          pkg-config
          gcc
          clang-tools
          gdb

          # Wayland and graphics stack
          wlroots_0_20
          wayland
          wayland-scanner
          wayland-protocols
          libxkbcommon
          libdrm
          pixman
          mesa
          libgbm
          libglvnd
          glslang
          libxcb
          libxcb-wm

          # Planar dependencies
          json_c
        ];

        # Meson's default debug build uses -O0. Glibc warns when the Nix
        # compiler wrapper combines that with _FORTIFY_SOURCE, and this
        # project intentionally promotes warnings to errors.
        hardeningDisable = [ "fortify" ];
      };
    };
}
