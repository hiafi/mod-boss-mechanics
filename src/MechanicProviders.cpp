/*
 * mod-boss-mechanics: provider stubs and installation (STAGE-M1-HANDOFF §3.7).
 */

#include "MechanicProviders.h"
#include "BossMechanicsConfig.h"
#include "Map.h"
#include "Player.h"
#include "Unit.h"

namespace BossMechanics
{
    namespace
    {
        StubReferenceHealthProvider const sStubReferenceHealth;
        StubContentKeyProvider const sStubContentKey;
        StubRoleProvider const sStubRole;

        ReferenceHealthProvider const* sReferenceHealth = &sStubReferenceHealth;
        ContentKeyProvider const* sContentKey = &sStubContentKey;
        RoleProvider const* sRole = &sStubRole;
    }

    void SetReferenceHealthProvider(ReferenceHealthProvider* provider)
    {
        sReferenceHealth = provider ? provider : &sStubReferenceHealth;
    }

    void SetContentKeyProvider(ContentKeyProvider* provider)
    {
        sContentKey = provider ? provider : &sStubContentKey;
    }

    void SetRoleProvider(RoleProvider* provider)
    {
        sRole = provider ? provider : &sStubRole;
    }

    ReferenceHealthProvider const& GetReferenceHealthProvider()
    {
        return *sReferenceHealth;
    }

    ContentKeyProvider const& GetContentKeyProvider()
    {
        return *sContentKey;
    }

    RoleProvider const& GetRoleProvider()
    {
        return *sRole;
    }

    bool IsReferenceHealthStubActive()
    {
        return sReferenceHealth == &sStubReferenceHealth;
    }

    ReferenceHealth StubReferenceHealthProvider::Get(ContentKey const& key) const
    {
        return InterpolateReferenceHealth(GetConfig().referenceHealthRows, key.level);
    }

    ContentKey StubContentKeyProvider::Get(Map const* map, Unit const* owner) const
    {
        ContentKey key;
        key.level = owner ? owner->GetLevel() : 0;
        key.heroic = map && map->IsHeroic();
        key.empoweredLevel = 0;
        return key;
    }

    Role StubRoleProvider::Get(Unit const* owner, Player const* player) const
    {
        if (!player)
            return Role::Damage;
        if (Unit const* victim = owner ? owner->GetVictim() : nullptr)
        {
            if (victim->GetGUID() == player->GetGUID())
                return Role::Tank;
        }
        // Player::HasTankSpec/HasHealSpec only read talents and forms; they are just not declared const.
        Player* p = const_cast<Player*>(player);
        if (!(owner && owner->GetVictim()) && p->HasTankSpec())
            return Role::Tank;
        if (p->HasHealSpec())
            return Role::Healer;
        return Role::Damage;
    }
}
