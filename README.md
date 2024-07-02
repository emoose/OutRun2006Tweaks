# OutRun2006Tweaks
A wrapper DLL that can patch in some minor fixes & tweaks into OutRun 2006: Coast 2 Coast.

Latest builds can be found under the releases section: https://github.com/emoose/OutRun2006Tweaks/releases

### Features
**Bugfixes:**
- UI can now scale to different aspect ratios properly without stretching (requires `UIScalingMode = 2` in INI)
- Prevents save corruption bug when remapping controls with many dinput devices connected
- Fixed C2C ranking scoreboards not updating on Steam and other releases due to faulty anti-piracy checks
- Pegasus animation's clopping sound effect will now end correctly
- Lens flare effect now loads from correct path, without needing to change game files
- Fixed Z-buffer precision issues that caused heavy z-fighting and distant object pop-in
- Stage objects such as traffic cones now only disappear once they're actually off-screen
- Fixes certain effects like engine backfiring which failed to appear when using controllers
- Text related to the now-defunct online service can be hidden

**Enhancements:**
- Built-in framelimiter to prevent the game from speeding up
- FPS can now be partially unlocked, drawing at unlocked FPS while game runs at 60FPS tickrate
- UI and game scene textures can be dumped & replaced
- Allows disabling vehicle LODs, reducing the ugly pop-in as they get closer
- Restored XInput rumble code from the Xbox release, allowing gear shifts/drifts/crashes/etc to give feedback
- Xbox Series impulse triggers are supported and can be tweaked inside INI
- Anisotropic filtering & transparency supersampling can be forced, greatly reducing aliasing around the edges of the track
- Reflection rendering resolution can be increased from the default 128x128
- Game can now run in borderless windowed mode; mouse cursor will now be hidden while game is active
- Automatically disables DPI scaling on the game window, fixing certain scaling issues
- Load times heavily reduced by disabling framelimiter/vsync during load screens
- Music can now be loaded from uncompressed WAV or lossless FLAC files, if they exist with the same filename
- Allows intro splash screens to be skipped
- Music track can be changed mid-race via Q and E buttons, or Back/RS+Back on controller (`CDSwitcher` must be enabled in INI first)

All the above can be customized via the OutRun2006Tweaks.ini file.

FPS can be partially unlocked by increasing the `FramerateLimit` setting above 60 (or disabling it), with `FramerateUnlockExperimental` enabled.  
This will then lock the games tickrate to 60FPS while draw-rate itself is unlocked, allowing game speed to stay consistent.

### Setup
Since Steam/DVD releases are packed with ancient DRM that doesn't play well with DLL wrappers, this pack includes a replacement game EXE to run the game with.

This EXE should be compatible with both the Steam release & the original DVD version, along with most OR2006 mods.

To set it up:

- Extract the files from the release ZIP into your `Outrun2006 Coast 2 Coast` folder, where `OR2006C2C.EXE` is located, replacing the original EXE.
- Edit the `Outrun2006Tweaks.ini` to customize the tweaks to your liking (by default all tweaks are enabled)
- Run the game, your desktop resolution will be used by default if `outrun2006.ini` file isn't present.

Steam Deck/Linux users may need to run the game with `WINEDLLOVERRIDES="dinput8=n,b" %command%` launch parameters for the mod to load in.

### Building
Building requires Visual Studio 2022, CMake & git to be installed, with those setup just clone this repo and then run `build_2022.bat`.

If the batch script succeeds you should see a `build\outrun2006tweaks-proj.sln` solution file, just open that in VS and build it.

(if you have issues building with this setup please let me know)

### TODO
- even with LODs & culling disabled some distant cars still pop into view, can distance of them loading in be increased?
- likewise certain parts of the stage have pop-in, usually happens when some part is obscured by some other geometry, occlusion culling maybe?
- car shadow improvements? seems player car uses different shadowing to other cars, could that be added to those too?
-   (the existing car shadows also seem to disappear after some distance, could it be increased?)
- input deadzone improvements?
- fix broken car horn (haven't seen any code for it yet though...)
- game retiming? probably a pipe dream - seems game is meant for 60.2Hz tickrate, lots of things coded for that & using 0.0166112... frametimes

### Thanks
Thanks to [debugging.games](http://debugging.games) for hosting debug symbols for OutRun 2 SP (Lindburgh), very useful for looking into Outrun2006.
