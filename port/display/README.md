# Display

A native port mod, `dc2tk.display`: widescreen, window size, fullscreen, vsync and render
quality, chosen as the mod's settings.

| Setting | Values | Default |
| --- | --- | --- |
| `aspect` | `4:3`, `16:10`, `16:9`, `21:9` | `16:9` |
| `window` | `960x720` to `2560x1440` | `1280x720` |
| `fullscreen` | `true`, `false` (Alt+Enter or F11 toggles it) | `false` |
| `vsync` | `true`, `false` | `false` |
| `quality` | `auto` (the window's resolution), `1` to `4` | `auto` |
| `msaa` | `1`, `2`, `4`, `8` | `1` |
| `filter` | `smooth`, `sharp` | `smooth` |

Widescreen shows more of the scene to each side (the height stays as it was) in towns and
dungeons. Menus, the HUD and text keep their 4:3 layout in the middle, and screens
that are only menus are shown 4:3.

To play with it, link this folder into the mods folder and choose it in a mod set:

    {"format": 2, "mods": [{"id": "dc2tk.display", "settings": {"aspect": "21:9"}}]}

    dc2tk play dc2.iso --native --set my-game.json
