# Text and Menus

`dc2tk setup` exports all menu, shop, and system message files (`.mes`) to `extracted/assets/text/<language>/` as UTF-8 `.txt` files.

## Quick steps

1. Copy a text asset into your mod folder (languages: `ja`, `en`, `fr`, `de`, `it`, `es`, `ko`):
   ```sh
   dc2tk mod asset dc2.iso my-mod text:en/shopname
   ```
   This writes `mods/my-mod/text/en/shopname.txt` and adds its line to `mod.json`:
   ```json
   {
     "text": {
       "en/shopname": "text/en/shopname.txt"
     }
   }
   ```
   A few messages can go in `mod.json` itself, by message number: `"text": {"en/shopname": {"3": "Wrench World"}}`.
2. Edit the messages you want to change. You can delete the `@<id>` blocks you leave unchanged, and add a new message with a new `@<id>`.
3. Build the ISO with `dc2tk build`. A `text:` id updates every copy of that file for that language on the disc (English `allmenu.mes` is in 20 menu packs).

## Where text is stored

| Text Type | Location | How to Edit |
| --- | --- | --- |
| **Menu, shop, and system strings (`.mes`)** | `extracted/assets/text/` | Copy with `dc2tk mod asset dc2.iso <mod> text:<lang>/<name>`. |
| **Item names and descriptions** | `data:items` | Set `name`, `names`, `description`, or `descriptions` in a mod's `data:items` table (see [Game Data](data.md#item-names-and-descriptions)). |
| **Cutscene and NPC dialogue** | `extracted/scripts/` | Override the script function (see [Change What an NPC Says](../scripting/tutorials/npc-line.md)). |

## The text form

Each message starts with `@<id>` on its own line, followed by its text. A line break in the text is a line break in the game:

```text
@0
Weapon Shop
@1
Polly's Bakery
@2
Press (O) to buy [Item1]
```

### Formatting tags

| Tag | Meaning |
| --- | --- |
| `<page>` | Wait for a button press and start a new page |
| `(O)` `(X)` `(A)` `(#)` | Circle, Cross, Triangle, and Square button icons |
| `[l1]` `[r1]` `[l2]` `[r2]` `[start]` `[select]` | Shoulder, Start, and Select button icons |
| `[heart]` and other `[...]` icons | Game font symbols (the same tags as script text) |
| `[Number1]`–`[Number16]`, `[Item1]`–`[Item16]`, `[String1]`–`[String16]` | Numbers, item names, and text the game fills in |
| `[Name1]`–`[Name6]` | Names the player gave |
| `{wait N}` | Pause for `N` frames |
| `{colour N}`, `{red N}`, `{green N}`, `{blue N}`, `{alpha N}` | Text color (`N` from `0` to `255`) |
| `{XXXX}` | A raw 16-bit code in hex |

To write a literal `{`, `[` that would start a tag, or `@` at the start of a line, write `{{}`, `{[}`, or `{@}`.

### Letters the English font lacks

The retail English font lacks most accented letters (such as `é`, `ü`, or `ŷ`). When a mod's English text or English item names use them, `dc2tk build` draws up to 32 extra letters into the English font texture (`meswin/FontTex_1_0.tm2`), copied from the European font or composed from a letter and its accent. More than 32 extra letters across all active mods is a build error.

## Limits

`meswin/system_N.mes`, `sysmes_N.mes`, and `etc/helpN.mes` must fit their fixed buffers (53,248, 80,000, and 4,096 bytes); `dc2tk build` refuses a file that does not. It also warns when a line is wider than the original text in that file, since menu windows have fixed widths.

See [DC2 Internals: Message Text](../internals/text.md) for `.mes` binary specifications.
