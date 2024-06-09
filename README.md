# OutRun2006Tweaks
A wrapper DLL that can patch in some minor fixes & tweaks into OutRun 2006: Coast 2 Coast.

### Features
- Game can now run in borderless windowed mode; mouse cursor will now be hidden while game is active
- Adds a built-in framelimiter to prevent game from speeding up (along with an optional fix that can allow higher FPS without speedup)
- Fixes objects being culled out before reaching edge of screen
- Fixes C2C scoreboard not saving scores on the Steam release due to broken anti-piracy checks
- Fixes Z-buffer precision issues, greatly reducing z-fighting & distant object pop-in
- Allows disabling vehicle LODs to reduce the ugly pop-in of them
- Disables culling of certain stage objects
- Can force anisotropic filtering level & enable transparency supersampling, greatly reducing aliasing around the edges of the track. (set `DX/ANTIALIASING = 2` in outrun2006.ini for best results)
- Automatically disables DPI scaling on the game window, fixing scaling issues with certain setups
- Fixes broken lens flare effect by making game load it from correct path
- Heavily reduced load times by disabling framelimiter/vsync during load screens

All the above can be toggled/customized via the Outrun2006Tweaks.ini file.

There's also a semi-experimental fix to allow running above 60FPS without speedup, by locking the games tickrate to 60FPS while draw-rate is unlimited.  
You can use this by increasing the `FramerateLimit` setting (or disabling it), with `FramerateUnlockExperimental` enabled.  
Since the game will still internally update at 60FPS this probably won't give as much benefit as a true framerate-unlock though.  
(some things like animated textures & menu text also unfortunately have speed issues with it...)

### Setup
Since Steam/DVD releases are packed with ancient DRM that doesn't play well with DLL wrappers, this pack includes a replacement game EXE to run the game with.

This EXE should be compatible with both the Steam release & the original DVD version, along with most OR2006 mods.

To set it up:

- Extract the files from the release ZIP into your `Outrun2006 Coast 2 Coast` folder, where `OR2006C2C.EXE` is located, replacing the original EXE.
- Edit the `Outrun2006Tweaks.ini` to customize the tweaks to your liking (by default all tweaks are enabled)
- Run the game, your desktop resolution will be used by default if `outrun2006.ini` file isn't present.

**If you have framerate issues where game doesn't run at your full FramerateLimit setting**, try adding `DX/WINDOWED = 1` to your `outrun2006.ini` file and check if it helps.

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
- input improvements: deadzone, rumble?
- fix broken car horn (haven't seen any code for it yet though...)
- game retiming? probably a pipe dream - seems game is meant for 60.2Hz tickrate, lots of things coded for that & using 0.0166112... frametimes
- a way to change music mid-race would be sweet

### Thanks
Thanks to [debugging.games](http://debugging.games) for hosting debug symbols for OutRun 2 SP (Lindburgh), very useful for looking into Outrun2006.
