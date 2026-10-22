# Smooth motion

A native port mod, `dc2tk.smooth`: frames between the game's own, for displays faster than the
game's 25 updates a second.

The game moves everything one step per update, 25 times a second in towns and dungeons and 50 at
the title, so it cannot simply run faster. Instead the port records each frame it draws, and while
the game waits for its next update it draws that recording again, with the camera, every model
and every bone blended from the update before to the last one. Copies of one mesh are matched to
the nearest copy of the frame before; anything that moves too far (a camera cut, a warp) jumps
instead of sliding.

What stays at the game's rate: particles, 3D sprites, water and the HUD, which the game places on
the screen itself. The picture is one update (40 ms) behind the game.

| Setting | Values | Default |
| --- | --- | --- |
| `enabled` | `true`, `false` | `true` |
| `refresh` | `display` (the screen's rate), `60` to `240` | `display` |

With vsync on, each in-between frame waits for the display, so fewer of them fit between updates.

To play with it, link this folder into the mods folder and choose it in a mod set, with the
display mod if you like:

    {"format": 2, "mods": [{"id": "dc2tk.smooth", "settings": {"refresh": "144"}}]}

    dc2tk play dc2.iso --native --set my-game.json

The port's `--smooth HZ` option (`0` for the display's rate) does the same without the mod.
