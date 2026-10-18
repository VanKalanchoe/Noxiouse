1. Check first all docs md files where we at curretnly E:\dev\noxiouse\docs whats missing currently

2. make contentbrowser modern which left side shows the folder and right the content and top idk search the same as modern engines do it currently very basic and show thumbnials materials as spheres and meshes so on maybe it opens a window where you can change the materials properties instead of having mateiral inside the properties panel ?

3. finish animation graph node add a animation player node make it more robust the animationsequencer and stuff like that with a way to play around wtih animation editiding or something idk if thats used in 3d or its only useful for 2d im not sure

4. finish hybrid renderer to get close to path tracer but way cheaper and have fallbacks for lower end devices for me lower end means raytracing support minimum and my modern rendering stuff support so i dont need shadowmaps but the more expensive raytraced stuff gets a fallback. fix those issues like when dlss enabled edge detection jittering. transparancy transmission and so on make robust 

5. multi viewport support max 4 which you can then move around and resize however you want and predefined window placement so it snaps directly you know like if 2 then left right if 4 viewport into 4 split you know what i mean and then you can interact on each viewport independtly to support differen camera perspective view you know

6. add custom sky instead of just a hdr ibl as alternative. i think its called hazemut or something sky rendering.

7. add runtimeassetmanager and the runtime project so we can test at runtime instead of editor overhead

8. how do we package when building to distrubbtion do we make a pch ? or how do modern engines do that

9. fix stats overlay text is looking broken as if something controls its behavior at runtime or something and make it look messed up wasnt the case in the earlier commits maybe compare what changed there im not sure.

10. add support for basic shapes idk cube, cylinder, pyramid, and so on.

11. lets say we want to make a minecraft clone in engine do we do that in scripting or c++ ? if so we should expose functions for that so acces to job system for chunk generation and gpu rendering you know idk if thats good.

12. add history so if i make a mistake i just ctrl + z revert changes like all engines and editors do it you know

13. if token left then add support for vfx, particle and cloud rendering. i think vfx is done in a program and then imported into engines i believe not sure how it actually works.