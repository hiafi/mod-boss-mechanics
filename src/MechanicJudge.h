/*
 * mod-boss-mechanics: the registry's pure judging rules (STAGE-M3-HANDOFF §3.3, decisions §7.7 and §7.8).
 *
 * No AzerothCore includes: MechanicRegistry.cpp turns live units into the plain inputs below and calls these, and
 * tests/judge_test.cpp compiles them with the host compiler. Each rule here is the single place the registry decides
 * it, so the bots (M3 view) can read the same semantics from this header.
 */

#ifndef MOD_BOSS_MECHANICS_MECHANIC_JUDGE_H
#define MOD_BOSS_MECHANICS_MECHANIC_JUDGE_H

#include "MechanicGeometry.h"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace BossMechanics
{
    enum class Kind : std::uint8_t
    {
        Zone,
        Frontal,
        Spread,
        RunOut,
        Stack,
        Soak,
        Bait,
        HoldStill,
        Spot,
        Jump        // Stratholme X3: a sweep at its resolve that spares whoever is in the air (MechanicJump.h)
    };

    namespace Judge
    {
        using Ms = std::chrono::milliseconds;

        constexpr bool IsShapeKind(Kind k)
        {
            return k == Kind::Zone || k == Kind::Frontal;
        }

        // Decision §7.8: the circles of Spread, RunOut and Bait hit players only (bots cannot place pets); Zones and
        // Frontals still hit pets and guardians. A Jump (Stratholme X3) trips players only: a pet cannot jump.
        constexpr bool PlayersOnly(Kind k)
        {
            return k == Kind::Spread || k == Kind::RunOut || k == Kind::Bait || k == Kind::Jump;
        }

        // Decision §7.7: a shape kind anchored on a player never hits that player (Cookie's chowder cone; the apex
        // of a cone counts as inside). A unit given as `to` (a line aimed at a player) is still hit.
        constexpr bool SparesAnchor(Kind k, bool anchorIsPlayer)
        {
            return IsShapeKind(k) && anchorIsPlayer;
        }

        // What the resolve filter knows about one unit of the spell's area search.
        struct Candidate
        {
            bool alive = true;
            bool playerControlled = true;   // a player, or a pet / guardian a player owns
            bool player = true;             // the unit itself is a player
            bool gameMaster = false;
            bool inside = true;             // inside the pass's shape (sweep band, only / exclude applied)
            bool eligible = true;           // Registry::IsEligible (pets: their owner's eligibility)
            bool tankExempted = false;      // Registry::IsTankExempted
            bool hitThisPass = false;       // already kept by an earlier cast of the same pass
            bool decoy = false;             // the mechanic is a Decoy() (DecoyFits): it keeps nobody
            bool airborne = false;          // Jump: in the air at the resolve (Jump::Clear, MechanicJump.h)
        };

        // Decoy() (Scholomance build WP-X: Jandice's heroic fake sigil and fake lane): a Zone or Frontal drawn exactly
        // like a real one that hits nobody. Its resolve keeps no target (KeepResolveTarget), so it deals no damage,
        // posts no failure, stacks no Weakened, casts no rider and calls no onHit; onResolve still fires, with no
        // hits (the script's pop visual). Bots never see it: the view skips it like an empty floor. A mine cannot be
        // one (its armed ring is a tell of its own, and a body sets it off).
        constexpr bool DecoyFits(Kind k, bool mine)
        {
            return IsShapeKind(k) && !mine;
        }

        // Lethal() (Stratholme X4: The Unforgiven's Cordon, Galford's Pyre and north wall, the kit's LethalWall): a
        // Zone or Frontal the bots never cross. Not a mine (its grace is a crossing rule of its own) and not a decoy
        // (bots never see one). Bots only: the resolve is unchanged (the kit's OnResolve kills whoever stands inside).
        constexpr bool LethalFits(Kind k, bool mine, bool decoy)
        {
            return IsShapeKind(k) && !mine && !decoy;
        }

        // The resolve filter's rule (Registry::FilterResolveTargets and the telegraph-only pass).
        constexpr bool KeepResolveTarget(Kind k, Candidate const& c)
        {
            if (c.decoy)
                return false;
            if (!c.alive || !c.playerControlled || c.gameMaster)
                return false;
            if (PlayersOnly(k) && !c.player)
                return false;
            if (k == Kind::Jump && c.airborne)
                return false;   // Stratholme X3: the sweep passes under a player in the air
            return c.inside && c.eligible && !c.tankExempted && !c.hitThisPass;
        }

        // B7: whose victim a tank-exempt cone or line spares. A cone sourced on a creature (the boss, an add, the
        // `.mech test` mob) spares that creature's victim; any other anchor (a point, a player, a creature that is
        // gone) falls back to the owner's victim.
        enum class ExemptFrom : std::uint8_t
        {
            Anchor,
            Owner
        };

        constexpr ExemptFrom TankExemptSource(bool anchorIsCreature, bool anchorFound)
        {
            return anchorIsCreature && anchorFound ? ExemptFrom::Anchor : ExemptFrom::Owner;
        }

        // Soak: fewer than `count` eligible bodies inside fails the group; extra bodies never fail (bots claim
        // exactly `count`, everyone else keeps out of it: decision §7.3).
        constexpr bool SoakShort(std::uint32_t inside, std::uint32_t count)
        {
            return inside < count;
        }

        // HoldStill: moved farther than the mechanic's tolerance (B5) from the window-start position.
        constexpr bool HoldStillMoved(float movedYd, float toleranceYd)
        {
            return movedYd > toleranceYd;
        }

        // Bait (B4): the landing circle's radius (0 = the picked player only, measured as a 0.5 yd circle).
        constexpr float BaitHitRadius(float radius)
        {
            return radius > 0.5f ? radius : 0.5f;
        }

        constexpr bool BaitHitsPickOnly(float radius)
        {
            return radius <= 0.f;
        }

        // A resolve hit is the job, not a failure, when it is the Bait landing on the player it chose: the bait taken
        // (user ruling 2026-11-04). That player still takes the damage and any rider (the drill's Baited (drill)
        // 205008), but no ledger failure and no Weakened stack. Anyone else inside the landing circle, and every other
        // kind's hit, failed: a ledger Hit plus Weakened when the mechanic has it.
        constexpr bool HitIsJob(Kind k, bool isBaitTarget)
        {
            return k == Kind::Bait && isBaitTarget;
        }

        // Stack Cluster(n) (B3): how many of the other players stand within `circle` of `at`.
        inline std::uint32_t ClusterCount(Shape const& circle, Geo::Vec3 const& at,
            std::vector<Geo::Vec3> const& others)
        {
            Geo::Pose const pose{ at.x, at.y, at.z, 0.f };
            std::uint32_t n = 0;
            for (Geo::Vec3 const& o : others)
                if (Geo::Contains(circle, pose, o))
                    ++n;
            return n;
        }

        // Stack Cluster(n): the indices of the eligible players with fewer than `minOthers` other eligible players
        // within the circle (each posts MissedStack).
        inline std::vector<std::size_t> ClusterMisses(Shape const& circle, std::vector<Geo::Vec3> const& players,
            std::uint32_t minOthers)
        {
            std::vector<std::size_t> out;
            for (std::size_t i = 0; i < players.size(); ++i)
            {
                Geo::Pose const pose{ players[i].x, players[i].y, players[i].z, 0.f };
                std::uint32_t n = 0;
                for (std::size_t j = 0; j < players.size() && n < minOthers; ++j)
                    if (j != i && Geo::Contains(circle, pose, players[j]))
                        ++n;
                if (n < minOthers)
                    out.push_back(i);
            }
            return out;
        }

        // A lasting mechanic's pass clock (Persistent zones, and Persistent Stack/Soak from M3). Tick 0: one pass
        // at resolveAt, then the mechanic lingers until `until`. A Detonate before the first pass pulls it to now.
        struct PassClock
        {
            Ms nextTick{ 0 };
            Ms until{ 0 };
            Ms tick{ 0 };
            std::uint32_t ticks = 0;    // passes done; also the id of the next one
        };

        // True when a pass is due at `now` (sets `pass` and advances the clock). `forever` is the registry's "never".
        inline bool TakePass(PassClock& c, Ms now, bool& detonateRequested, bool resolved, Ms forever,
            std::uint32_t& pass)
        {
            if (detonateRequested)
            {
                detonateRequested = false;
                if (!resolved && now < c.nextTick)
                    c.nextTick = now;
            }
            if (now < c.nextTick || (c.nextTick >= c.until && c.ticks != 0))
                return false;
            pass = c.ticks++;
            c.nextTick = c.tick > Ms(0) ? c.nextTick + c.tick : forever;
            return true;
        }

        // The lasting mechanic is over (pruned at the end of the tick).
        inline bool PassesDone(PassClock const& c, Ms now, bool resolved)
        {
            return resolved && now >= c.until;
        }

        // Reservations (B10). `R` has `bot`, `since`, `claim` and `slot`; `same(a, b)` says whether what other bots
        // plan against (point, facing, slot, claim) is unchanged. Replaces the bot's earlier entry; `since` is when
        // the bot's current claim began: kept while the claim (and, for a claim, its slot) is unchanged, restarted at
        // `now` when it changes (an avoider turned claimer claims from now, not from its first reservation:
        // STAGE-M3-R-REPORT m-4). A new entry with since 0 starts at `now`. Returns true when other bots should
        // re-plan (a new entry or a change), false when only the expiry moved.
        template <class R, class Same>
        bool UpsertReservation(std::vector<R>& list, R r, Ms now, Same const& same)
        {
            for (R& existing : list)
            {
                if (existing.bot != r.bot)
                    continue;
                bool const changed = !same(existing, r);
                bool const sameClaim = existing.claim == r.claim && (!r.claim || existing.slot == r.slot);
                r.since = sameClaim ? existing.since : now;
                existing = r;
                return changed;
            }
            if (r.since == Ms(0))
                r.since = now;
            list.push_back(r);
            return true;
        }

        template <class R, class G>
        bool EraseReservation(std::vector<R>& list, G const& bot)
        {
            std::size_t const before = list.size();
            std::erase_if(list, [&bot](R const& r) { return r.bot == bot; });
            return list.size() != before;
        }

        // Drops the reservations whose `until` passed. Returns true when any was dropped.
        template <class R>
        bool PruneReservations(std::vector<R>& list, Ms now)
        {
            std::size_t const before = list.size();
            std::erase_if(list, [now](R const& r) { return r.until <= now; });
            return list.size() != before;
        }
    }
}

#endif
