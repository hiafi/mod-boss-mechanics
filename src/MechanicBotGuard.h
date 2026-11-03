/*
 * mod-boss-mechanics: the playerbots guard (STAGE-M2-HANDOFF §3.1, WP-C).
 *
 * Every MechanicBot* file includes this first and wraps its body in `#if BOSS_MECHANICS_WITH_BOTS`, so the module
 * builds and runs without mod-playerbots. MOD_PLAYERBOTS is a PRIVATE define of the `modules` target whenever a
 * modules/mod-playerbots directory exists, even when that module is listed in DISABLED_AC_MODULES; the
 * __has_include check only sees the include directories of enabled modules, so it catches that case.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_BOT_GUARD_H
#define MOD_BOSS_MECHANICS_MECHANIC_BOT_GUARD_H

#if defined(MOD_PLAYERBOTS) && __has_include("PlayerbotAI.h")
#define BOSS_MECHANICS_WITH_BOTS 1
#else
#define BOSS_MECHANICS_WITH_BOTS 0
#endif

#endif
