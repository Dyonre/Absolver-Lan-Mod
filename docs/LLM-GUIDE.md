# Making mods with an AI assistant

**TLDR:** give your assistant this repository and say *"Read AGENTS.md, then help me make an Absolver mod."* Ask for the simplest kind of mod. Copy the result into `<game>\Mods\`, start the game, and paste only the failing log lines back. When the chat gets long, ask it to update `NOTES.md` and start a new chat.

A free plan is enough for most mods. Back up your saves first, and play offline only.

## Prompts

> Read AGENTS.md and explain in simple terms what this is and what mods I can make.

> Read AGENTS.md. I want a mod that <what you want>. Pick the simplest kind of mod, write the files with full paths, and tell me how to test it.

> My mod does not work. These are the log lines that mention it: <paste 5-10 lines>. Change one thing at a time.

## Testing

Copy your mod folder into `<game>\Mods\` and start the game. Check these files in `Absolver\Binaries\Win64\LanNative\`:

* `rows.txt` - whether each edit applied, and why not
* `mods.txt` - load order and conflicts
* `lannative.log` - everything (search for your mod's name)

Press **F2** in game for the Mods page. If the game will not start, add `-NoMods` to the Steam launch options.

## Saving tokens

* Point at one file or section instead of pasting docs. Copy an example from `examples/` instead of starting from scratch.
* Ask for a plan in three lines before any code.
* Paste a few log lines, not the whole log.
* One change at a time, one task per chat.
* Tell it: "Do not read other files unless I ask."

## Switching models or chats

Keep the memory in a file, not in the chat. Put a `NOTES.md` in your mod folder:

```markdown
# <mod name>
Goal: <one sentence>
Game folder: <path>   Mod folder: <path>
Done: <what works>
Not done: <what is left>
Problems: <exact log line and what you tried>
Next step: <one thing>
```

At the end of a session: *"Update NOTES.md so another assistant can continue. Rewrite it, do not just append."*
At the start of a new one, in any model: *"Read AGENTS.md and NOTES.md, then continue."*

Never put keys or passwords in `NOTES.md`.

## Optional tools

* [Blender](https://www.blender.org) with a Blender MCP server such as [blender-mcp](https://github.com/ahujasid/blender-mcp) - lets an assistant inspect or build 3D models.
* [fal](https://fal.ai) - generate images, sound or 3D models. Paid, needs an API key; keep the key in a local `.env` file, never in chat or in the repository. Label generated assets as AI generated.
* FModel and UAssetGUI - browse the game's own data to find property names. Never share extracted game files.

## Safety

* Do not enable a plugin DLL (`Mods\plugins.txt`) you do not understand. A plugin is a program.
* Do not paste keys, tokens or personal data into an AI chat.
* AI-written mods can have wrong numbers or names. Check `rows.txt` and play-test before sharing.
