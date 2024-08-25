# OutRun2006Tweaks
[![GitHub Downloads](https://img.shields.io/github/downloads/emoose/OutRun2006Tweaks/total)](https://github.com/emoose/OutRun2006Tweaks/releases)

A wrapper DLL that can patch in some minor fixes & tweaks into OutRun 2006: Coast 2 Coast.

Latest builds can be found under the releases section: https://github.com/emoose/OutRun2006Tweaks/releases

### Features
**Graphics:**
- UI can now scale to different aspect ratios without stretching (requires `UIScalingMode = 1` in INI)
- Game scene & UI textures can be dumped out, and replaced with higher-resolution versions
- Allows disabling vehicle LODs, reducing the ugly pop-in as they get closer
- Fixed Z-buffer precision issues that caused heavy Z-fighting and distant object pop-in
- Lens flare effect now loads from correct path without needing to change game files
- Stage objects such as traffic cones now only disappear once they're actually off-screen
- Fixes certain effects like engine backfiring which failed to appear when using controllers
- Anisotropic filtering & transparency supersampling can be forced, greatly reducing aliasing around the edges of the track
- Reflection rendering resolution can be increased from the default 128x128
- Restores the car base shadow from the C2C console ports, which was missing on PC for some reason

**Gameplay:**
- Built-in framelimiter to prevent speedups, framerate can be partially unlocked with game running at 60FPS internally
- Points game toward new online servers, restoring the online multiplayer modes
- Restored XInput rumble code from the Xbox release, allowing gear shifts/drifts/crashes/etc to give feedback
- Xbox Series impulse triggers are supported and can be tweaked inside INI
- Horn button can be made functional during normal gameplay, outside of the "honk your horn!" girl requests
- Allows randomizing the set of highway animations to use, instead of only using the set for the game mode being played
- In-game HUD can be optionally toggled via bindable keypress

**Bugfixes:**
- Prevents save corruption bug when remapping controls with many input devices connected
- Fixed C2C ranking scoreboards not updating on Steam and other releases due to faulty anti-piracy checks
- Pegasus animation's clopping sound effect will now end correctly
- Text related to the now-defunct online service can be hidden
- Automatically disables DPI scaling on the game window to fix scaling issues
- Game can be forced to run on a single core, to help with freezing issues on some modern systems
- Bink movie files larger than 1024 pixels will now play without crashes
- Game crashes will now write a crash report into CrashDumps folder (please feel free to post any crash reports to the issues page!)

**Enhancements:**
- Game can now run in borderless windowed mode; mouse cursor will now be hidden while game is active
- Will use desktop resolution for the game if outrun2006.ini isn't present
- Load times heavily reduced via improved framelimiter
- Draw distance for the stage can be increased, greatly reducing pop-in/fade-ins on the level
- Music can now be loaded from uncompressed WAV or lossless FLAC files, if they exist with the same filename
- Allows intro splash screens to be skipped
- Music track can be changed mid-race via Z and X buttons, or Back/RS+Back on controller (`CDSwitcher` must be enabled in INI first)

All the above can be customized via the OutRun2006Tweaks.ini file.

The partial FPS unlock allows game to render out at higher FPS, **but will still run at 60FPS internally**.  
This won't give as much benefit as a true framerate unlock since frames will be repeated, but it can help reduce load times & improve some effects like the reflections update rate.  
(high refresh rate monitors that have poor 60Hz response times may also benefit from this too)

### Setup
Since Steam/DVD releases are packed with ancient DRM that doesn't play well with DLL wrappers, this pack includes a replacement game EXE to run the game with.

This EXE should be compatible with both the Steam release & the original DVD version, along with most OR2006 mods.

To set it up:

- Extract the files from the release ZIP into your **Outrun2006 Coast 2 Coast** folder, where **OR2006C2C.EXE** is located, replacing the original EXE.
- Edit **OutRun2006Tweaks.ini** to customize the tweaks to your liking (by default all tweaks are enabled, other than `CDSwitcher`)
- **Important:** Install the latest x86 VC redist from (https://aka.ms/vs/17/release/vc_redist.x86.exe), a redist from 2024 is needed for Tweaks to launch correctly (**even if you already have it installed please try installing it again**)
- Run the game, your desktop resolution will be used by default if `outrun2006.ini` file isn't present.
- (optional) the [SoundtrackFix package](https://github.com/emoose/OutRun2006Tweaks/releases/download/v0.3.0-release/OutRun2006Tweaks-SoundtrackFix-1.0.zip) can be applied to fix the missing first 2 seconds in "Rush a Difficulty"
- (optional) texture improvements can be found in the texture pack releases thread (please feel free to create your own too!): https://github.com/emoose/OutRun2006Tweaks/issues/20

Steam Deck/Linux users may need to run the game with `WINEDLLOVERRIDES="dinput8=n,b" %command%` launch parameters for the mod to load in.

### Building
Building requires Visual Studio 2022, CMake & git to be installed, with those setup just clone this repo and then run `generate_2022.bat`.

If the batch script succeeds you should see a `build\outrun2006tweaks-proj.sln` solution file, just open that in VS and build it.

(if you have issues building with this setup please let me know)

### Thanks
Thanks to [debugging.games](http://debugging.games) for hosting debug symbols for OutRun 2 SP (Lindburgh), very useful for looking into Outrun2006.

(**if you own any prototype of Coast 2 Coast or Online Arcade** it may also contain debug symbols inside, which would let us improve even more on the C2C side of the game - please consider getting in touch at my email: abc at cock dot li)
