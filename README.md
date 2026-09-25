# LightOS Workspace

Hyprland workspace commands for LightOS. The CLI lists workspaces, focuses one, moves the focused window, or opens an interactive picker.

```sh
./install.sh
lightos-workspace list
lightos-workspace focus 3
lightos-workspace move 4
lightos-workspace pick
```

Requires Hyprland, `hyprctl`, `jq`, and `wofi` for the picker. MIT licensed; see [LICENSE](LICENSE).
