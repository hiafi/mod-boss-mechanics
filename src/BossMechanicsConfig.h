/*
 * mod-boss-mechanics: cached configuration (STAGE-M1-HANDOFF §3.11). Read once in WorldScript::OnAfterConfigLoad
 * (C2's hook calls LoadConfig) at startup and on `.reload config`; never read sConfigMgr in a hot path.
 *
 * Named BossMechanicsConfig.h (not Config.h) so it never shadows the core's Config.h on the modules include path.
 */

#ifndef MOD_BOSS_MECHANICS_CONFIG_H
#define MOD_BOSS_MECHANICS_CONFIG_H

#include "Define.h"
#include "MechanicMath.h"
#include <string>
#include <vector>

namespace BossMechanics
{
    enum class FollowMode : uint8
    {
        SplineStep = 0,     // one spline per interval: move + face together (default)
        MoveFollow = 1,     // MotionMaster::MoveFollow (circles only, no facing)
        Teleport = 2        // NearTeleportTo every interval (jumpy, always correct)
    };

    struct ModuleConfig
    {
        bool enable = true;                                     // BossMechanics.Enable
        bool debug = false;                                     // BossMechanics.Debug
        std::string referenceHealthText = "20:1000:1500 60:5000:8000";  // BossMechanics.ReferenceHealth.Rows
        std::vector<ReferenceHealthRow> referenceHealthRows;
        FollowMode followMode = FollowMode::SplineStep;         // BossMechanics.Telegraph.FollowMode
        uint32 followIntervalMs = 200;                          // BossMechanics.Telegraph.FollowIntervalMs
        float lift = 0.1f;                                      // BossMechanics.Telegraph.Lift
        bool shrinkProjectileLines = false;                     // BossMechanics.Telegraph.ShrinkProjectileLines
        bool fill = true;                                       // BossMechanics.Telegraph.Fill
        uint32 fillStepMs = 150;                                // BossMechanics.Telegraph.FillStepMs
        uint32 fillLeadMs = 150;                                // BossMechanics.Telegraph.FillLeadMs
        uint32 maxActivePerMap = 256;                           // BossMechanics.MaxActivePerMap
        float openWorldCandidateRadius = 100.f;                 // BossMechanics.OpenWorld.CandidateRadius (0 = map)

        ModuleConfig();
    };

    ModuleConfig const& GetConfig();
    void LoadConfig(bool reload);
}

#endif
