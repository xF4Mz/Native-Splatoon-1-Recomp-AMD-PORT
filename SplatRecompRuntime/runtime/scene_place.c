// Where the player is (the scene's map and its name), for the benchmark, the 30 fps rule, the shader warm-up and the
// "Preparing graphics" picture, from the map model a scene loads.
#include <stdio.h>
#include <string.h>


// --- where the player is, from the map model a scene loads (fs.c: /vol/content/Model/Fld_*.szs) ---
// (names as the game's own English text has them: Message/CommonMsg_USen VSStageName, MissionName, LayoutMsg)
typedef struct { const char* map; const char* text; int mission; } Place;
static const Place k_places[] = {
    { "Plaza00", "Inkopolis Plaza", 0 }, { "PlazaLobby", "In the Lobby", 0 }, { "PlazaEvent", "Inkopolis Plaza (Splatfest)", 0 },
    { "World00", "Octo Valley", 0 }, { "Tutorial00", "Tutorial", 0 }, { "StaffRoll00", "Watching the credits", 0 },
    { "ShootingRange", "Trying out a weapon", 0 },
    { "Room_Weapons", "Shopping at Ammo Knights", 0 }, { "Room_Gear", "Shopping at Cooler Heads", 0 },
    { "Room_Tshirts", "Shopping at Jelly Fresh", 0 }, { "Room_Shoes", "Shopping at Shrimp Kicks", 0 },
    // battle stages (offline: the Battle Dojo)
    { "Amida00", "Battle: Port Mackerel", 0 }, { "Athletic00", "Battle: Camp Triggerfish", 0 }, { "Crank00", "Battle: Urchin Underpass", 0 },
    { "Crank01", "Battle: Urchin Underpass", 0 }, { "Hiagari00", "Battle: Mahi-Mahi Resort", 0 }, { "Jyoheki00", "Battle: Flounder Heights", 0 },
    { "Kaisou00", "Battle: Hammerhead Bridge", 0 }, { "Maze00", "Battle: Kelp Dome", 0 }, { "Office00", "Battle: Ancho-V Games", 0 },
    { "Pivot00", "Battle: Museum d'Alfonsino", 0 }, { "Quarry00", "Battle: Piranha Pit", 0 }, { "Ruins00", "Battle: Bluefin Depot", 0 },
    { "SeaPlant00", "Battle: Saltspray Rig", 0 }, { "SkatePark00", "Battle: Blackbelly Skatepark", 0 }, { "Tuzura00", "Battle: Moray Towers", 0 },
    { "UpDown00", "Battle: Arowana Mall", 0 }, { "Warehouse00", "Battle: Walleye Warehouse", 0 },
    // Octo Valley missions (MapInfo MsnStageNo) and bosses
    { "EasyHide00", "Octotrooper Hideout", 1 }, { "EasyClimb00", "Lair of the Octoballs", 2 }, { "EasyJump00", "Rise of the Octocopters", 3 },
    { "Geyser00", "Gusher Gauntlet", 4 }, { "Sponge00", "Floating Sponge Garden", 5 }, { "Propeller00", "Propeller-Lift Playground", 6 },
    { "PaintingLift00", "Spreader Splatfest", 7 }, { "RvlMaze00", "Octoling Invasion", 8 }, { "OctZero00", "Unidentified Flying Object", 9 },
    { "InkRail00", "Inkrail Skyscape", 10 }, { "Invisible00", "Inkvisible Avenues", 11 }, { "Dozer00", "Flooder Junkyard", 12 },
    { "SlideLift00", "Shifting Splatforms", 13 }, { "RvlSkatePark00", "Octoling Assault", 14 }, { "OctRuins00", "Undeniable Flying Object", 15 },
    { "Propeller01", "Propeller-Lift Fortress", 16 }, { "Charge00", "Octosniper Ramparts", 17 }, { "PaintingLift01", "Spinning Spreaders", 18 },
    { "TurnLift00", "Tumbling Splatforms", 19 }, { "RvlRuins00", "Octoling Uprising", 20 }, { "OctCrank00", "Unwelcome Flying Object", 21 },
    { "Trance00", "Splat-Switch Revolution", 22 }, { "Sponge01", "Spongy Observatory", 23 }, { "Fusya00", "Pinwheel Power Plant", 24 },
    { "Dozer01", "Far-Flung Flooders", 25 }, { "RvlSeaPlant00", "Octoling Onslaught", 26 }, { "OctSkatePark00", "Unavoidable Flying Object", 27 },
    { "BossStampKing", "The Mighty Octostomp", 101 }, { "BossCylinderKing", "The Dreaded Octonozzle", 102 }, { "BossBallKing", "The Rampaging Octowhirl", 103 },
    { "BossMouthKing", "The Ravenous Octomaw", 104 }, { "BossRailKing", "Enter the Octobot King!", 105 },
};
static char g_place[128] = "Starting up";

char splat_scene_map[64];   // the scene's map ("Plaza00", "World00", "EasyHide00"...): gx2.c's 30 fps per scene
volatile unsigned splat_scene_seq;   // (+1 at every scene: latte_render.c warms that scene's pipelines)
const char* splat_place_name(void) { return g_place; }
void splat_scene_seen(const char* path) {
    // (a map model opened: /vol/content/Model/Fld_X.szs; or every scene entry, model loaded or not -- the game
    // looks up /vol/content/Parameter/PostGame_StageView_Fld_X_DRC.camera.params: the plaza after Octo Valley
    // opens no model, its model stays loaded, and the place kept saying "Octo Valley")
    const char* m = strstr(path, "/Model/Fld_");
    const char* pg = strstr(path, "PostGame_StageView_Fld_");
    if (m) m += 11;
    else if (pg) m = pg + 23;
    else return;
    char key[64];
    size_t k = 0;
    while (m[k] && m[k] != '.' && k < sizeof(key) - 1) { key[k] = m[k]; k++; }
    key[k] = 0;
    if (pg && k > 4 && !strcmp(key + k - 4, "_DRC")) key[k - 4] = 0;
    if (!strcmp(key, "Room_anim")) return;   // (the shops' shared animations, not a place)
    if (strcmp(splat_scene_map, key)) { snprintf(splat_scene_map, sizeof(splat_scene_map), "%s", key); splat_scene_seq++; }
    char text[128];
    const Place* p = NULL;
    for (size_t i = 0; i < sizeof(k_places) / sizeof(k_places[0]); i++) if (!strcmp(k_places[i].map, key)) { p = &k_places[i]; break; }
    if (!p) snprintf(text, sizeof(text), "Playing");
    else if (p->mission > 100) snprintf(text, sizeof(text), "Octo Valley boss: %s", p->text);
    else if (p->mission) snprintf(text, sizeof(text), "Octo Valley mission %d: %s", p->mission, p->text);
    else snprintf(text, sizeof(text), "%s", p->text);
    if (!strcmp(text, g_place)) return;
    snprintf(g_place, sizeof(g_place), "%s", text);
    printf("[scene] place: %s (Fld_%s)\n", text, key);
}
