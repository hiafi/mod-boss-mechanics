/*
 * mod-boss-mechanics: damage and role providers (STAGE-M1-HANDOFF §3.7, §2.8).
 *
 * A mechanic's damage is a percent of reference health; the registry turns it into one flat number per target through
 * these providers. M1 ships stubs: the difficulties work (E3/E4) replaces the reference health and content key
 * providers, M3 may replace the role provider with playerbots roles. Providers are installed at startup (world
 * thread) and only read afterwards; the objects must live for the process.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_PROVIDERS_H
#define MOD_BOSS_MECHANICS_MECHANIC_PROVIDERS_H

#include "Define.h"
#include "MechanicMath.h"
#include "MechanicRegistry.h"

class Map;
class Player;
class Unit;

namespace BossMechanics
{
    struct ContentKey
    {
        uint8 level = 0;
        bool heroic = false;
        uint8 empoweredLevel = 0;
    };

    // ReferenceHealth { nonTank, tank } is defined in MechanicMath.h.

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

    // nullptr restores the stub. The getters never return nullptr.
    void SetReferenceHealthProvider(ReferenceHealthProvider* provider);
    void SetContentKeyProvider(ContentKeyProvider* provider);
    void SetRoleProvider(RoleProvider* provider);
    ReferenceHealthProvider const& GetReferenceHealthProvider();
    ContentKeyProvider const& GetContentKeyProvider();
    RoleProvider const& GetRoleProvider();
    bool IsReferenceHealthStubActive();

    // Stub: rows from BossMechanics.ReferenceHealth.Rows, linear by level, clamped; heroic/empowered ignored.
    class StubReferenceHealthProvider final : public ReferenceHealthProvider
    {
    public:
        ReferenceHealth Get(ContentKey const& key) const override;
    };

    // Stub: level = owner level, heroic = map->IsHeroic(), empoweredLevel = 0.
    class StubContentKeyProvider final : public ContentKeyProvider
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
