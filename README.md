# Outrun2006Tweaks
A wrapper DLL that can patch in some minor fixes & tweaks into Outrun 2006: Coast 2 Coast.

### Features
- Game can now run in borderless windowed mode; mouse cursor will now be hidden while game is active
- Adds a built-in framelimiter to prevent game from speeding up (along with an optional fix to allow higher FPS without speedup)
- Fixes objects being culled out before reaching edge of screen
- Allows disabling vehicle LODs to reduce the ugly pop-in of them
- Disables culling of certain stage objects
- Can force anisotropic filtering level & enable transparency supersampling, greatly reducing aliasing around the edges of track. (set `DX/ANTIALIASING = 2` in outrun2006.ini for best results)
- Automatically disables DPI scaling on the game window, fixing scaling issues with certain setups
- Allows game to load in lens flare data from correct path when needed, fixing lens flare issues.

All the above can be toggled/customized via the Outrun2006Tweaks.ini file.

There's also a semi-experimental fix to allow running above 60FPS without speedup, by locking the games tickrate to 60FPS while draw-rate is unlimited.  
Since the game will still internally update at 60FPS this won't give as much benefit as a true framerate-unlock though.  
(some things like animated textures & UI text also unfortunately have speed issues with it...)

### Setup
Since the Steam/DVD releases are packed with ancient DRM that doesn't play well with DLL wrappers, a replacement game EXE is included with this tweaks pack.

This EXE should be compatible with both the Steam release and the original DVD version.

To set it up just extract the files from the release ZIP into your `Outrun2006 Coast 2 Coast` folder, where `OR2006C2C.EXE` is located, replacing the original EXE.

After that you can edit the `Outrun2006Tweaks.ini` to customize the tweaks to your liking.

Recommend running the games Config.exe to setup video settings first before starting the game (if you want to use borderless windowed mode, edit `outrun2006.ini` and add `DX/WINDOWED = 1`)

### TODO
- even with LODs & culling disabled some distant cars still pop into view, can distance of them loading in be increased?
- likewise certain parts of the stage have pop-in, usually happens when some part is obscured by some other geometry, occlusion culling maybe?
- car shadow improvements? seems player car uses different shadowing to other cars, could that be added to those too?
-   (the existing car shadows also seem to disappear after some distance, could it be increased?)
- input improvements: deadzone, rumble?
- fix broken car horn (haven't seen any code for it yet though...)
- game retiming? probably a pipe dream - seems game is meant for 60.2Hz tickrate, lots of things coded for that & using 0.0166112... frametimes
- a way to change music mid-race would be sweet
- intro/splash skip

### Thanks
Thanks to [debugging.games](http://debugging.games) for hosting debug symbols for Outrun 2 SP (Lindburgh), very useful for looking into Outrun2006.
