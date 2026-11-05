/*
 * mod-boss-mechanics: damage and role providers (STAGE-M1-HANDOFF §3.7, §2.8).
 *
 * A mechanic's damage is a percent of reference health; the registry turns it into one flat number per target through
 * these providers. The reference health provider reads keyed rows from the config (dungeon difficulties E3,
 * MechanicRefHealth.h); the content key provider resolves the map's difficulty and pinned level, and E4 replaces it to
 * add the empowered level (building on DefaultContentKey). M3 may replace the role provider with playerbots roles.
 * Providers are installed at startup (world thread) and only read afterwards; the objects must live for the process.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_PROVIDERS_H
#define MOD_BOSS_MECHANICS_MECHANIC_PROVIDERS_H

#include "Define.h"
#include "MechanicMath.h"
#include "MechanicRefHealth.h"
#include "MechanicRegistry.h"

class Map;
class Player;
class Unit;

namespace BossMechanics
{
    // ContentKey { level, heroic, empoweredLevel } is defined in MechanicRefHealth.h, ReferenceHealth { nonTank, tank }
    // in MechanicMath.h.

    class ReferenceHealthProvider
    {
    public:
        virtual ~ReferenceHealthProvider() = default;
        virtual ReferenceHealth Get(ContentKey const& key) const = 0;
    };

    class ContentKeyProvider
    {
    public:
        virtual ~ContentKeyProvider() = default;
        virtual ContentKey Get(Map const* map, Unit const* owner) const = 0;     // owner may be nullptr
    };

    class RoleProvider
    {
    public:
        virtual ~RoleProvider() = default;
        virtual Role Get(Unit const* owner, Player const* player) const = 0;     // owner may be nullptr
    };

    // nullptr restores the default. The getters never return nullptr.
    void SetReferenceHealthProvider(ReferenceHealthProvider* provider);
    void SetContentKeyProvider(ContentKeyProvider* provider);
    void SetRoleProvider(RoleProvider* provider);
    ReferenceHealthProvider const& GetReferenceHealthProvider();
    ContentKeyProvider const& GetContentKeyProvider();
    RoleProvider const& GetRoleProvider();
    bool IsConfigReferenceHealthActive();

    // The default content key (RefHealth::ResolveContentKey): heroic = map->IsHeroic() (the open world and the sim are
    // normal), level = the map's BossMechanics.ReferenceHealth.MapLevels pin for that difficulty, else the owner's
    // level (0 without one), empoweredLevel = 0. E4's provider calls this and sets the empowered level.
    ContentKey DefaultContentKey(Map const* map, Unit const* owner);

    // Default: the keyed rows of BossMechanics.ReferenceHealth.Rows (RefHealth::Lookup).
    class ConfigReferenceHealthProvider final : public ReferenceHealthProvider
    {
    public:
        ReferenceHealth Get(ContentKey const& key) const override;
    };

    // Default: DefaultContentKey.
    class DefaultContentKeyProvider final : public ContentKeyProvider
    {
    public:
        ContentKey Get(Map const* map, Unit const* owner) const override;
    };

    // Stub: with an owner victim, the victim is the tank and nobody else is; with none, HasTankSpec(). Then
    // HasHealSpec() = Healer, else Damage. M5 C4: the same rules (MechanicRoleRules.h) on the player's own talent map
    // and a per-thread spec cache, not Player::GetSpec's walk of the whole talent store.
    class StubRoleProvider final : public RoleProvider
    {
    public:
        Role Get(Unit const* owner, Player const* player) const override;
    };
}

#endif
