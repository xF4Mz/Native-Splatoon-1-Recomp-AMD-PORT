// This copy of the source leaves out the Bot Lobby (bot_lobby.c) and the Battle Dojo bot (dojo_bot.c).
// These stand-ins keep the rest of the program building, with both features switched off.

int splat_lobby_on(void) { return 0; }                                    // (never the Bot Lobby)
void splat_lobby_key(int key, int shift) { (void)key; (void)shift; }
void splat_lobby_tick(double now) { (void)now; }
int splat_lobby_hud(const char*** out) { (void)out; return 0; }           // (no lobby lines on screen)

int splat_bot_pad(double now, float sticks[4], unsigned* bits) {          // (no bot driving player 2)
    (void)now; (void)sticks; (void)bits;
    return 0;
}

int splat_bot_in_battle(void) { return 0; }                              // (no bot: never in a bot battle)
