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

    // D1 (M2 handoff §3.8): human-like reaction delays and mistakes for bots. All off by default.
    struct BotFallibilityConfig
    {
        bool enable = false;                                    // BossMechanics.Bots.Fallibility.Enable
        uint32 reactionMs = 350;                                // .ReactionMs
        uint32 jitterMs = 250;                                  // .JitterMs
        float lateChancePct = 5.f;                              // .LateChancePct
        uint32 lateExtraMs = 700;                               // .LateExtraMs
        float wrongSideChancePct = 3.f;                         // .WrongSideChancePct
        uint32 realiseMs = 700;                                 // .RealiseMs
        float scaleNormal = 1.f;                                // .ScaleNormal
        float scaleHeroic = 1.f;                                // .ScaleHeroic
        uint64 seed = 0;                                        // .Seed as configured (0 = random per load)
        uint64 effectiveSeed = 0;                               // the seed in use: .Seed, or drawn at load when 0
    };

    // Bots (M2 handoff §3.2). Cached here by LoadConfig; the bot glue never reads sConfigMgr.
    struct BotConfig
    {
        bool enable = true;                                     // BossMechanics.Bots.Enable
        bool avoidAoeInInstances = true;                        // BossMechanics.Bots.AvoidAoeInInstances
        bool openWorldTestScope = true;                         // BossMechanics.Bots.OpenWorldTestScope
        uint32 gateSweepMs = 1000;                              // BossMechanics.Bots.GateSweepMs
        uint32 fastTickMs = 100;                                // BossMechanics.Bots.FastTickMs
        uint32 startDelayMs = 150;                              // BossMechanics.Bots.StartDelayMs
        float safetyMarginYd = 0.75f;                           // BossMechanics.Bots.SafetyMarginYd
        uint32 safetyMarginMs = 300;                            // BossMechanics.Bots.SafetyMarginMs
        uint32 postResolveHoldMs = 500;                         // BossMechanics.Bots.PostResolveHoldMs
        uint32 graceMarginMs = 250;                             // BossMechanics.Bots.GraceMarginMs
        uint32 horizonMs = 10000;                               // BossMechanics.Bots.HorizonMs
        uint32 holdCapMs = 3000;                                // BossMechanics.Bots.HoldCapMs
        uint32 maxCandidates = 400;                             // BossMechanics.Bots.MaxCandidates
        uint32 maxPathChecks = 8;                               // BossMechanics.Bots.MaxPathChecks
        float gridStepYd = 2.f;                                 // BossMechanics.Bots.GridStepYd
        uint32 minSolveIntervalMs = 150;                        // BossMechanics.Bots.MinSolveIntervalMs
        uint32 movingAnchorRecheckMs = 250;                     // BossMechanics.Bots.MovingAnchorRecheckMs
        uint32 roleSlackMs = 1000;                              // BossMechanics.Bots.RoleSlackMs
        float rangedDistanceYd = 22.f;                          // BossMechanics.Bots.RangedDistanceYd
        float facingSlackDeg = 15.f;                            // BossMechanics.Bots.FacingSlackDeg
        bool debug = false;                                     // BossMechanics.Bots.Debug
        BotFallibilityConfig fallibility;                       // BossMechanics.Bots.Fallibility.*
        // M3 (STAGE-M3-HANDOFF §3.10): the player kinds, tank tasks, healer sight and the route search.
        float humanSlackYd = 1.f;                               // BossMechanics.Bots.HumanSlackYd
        float regionComfortWeight = 1.5f;                       // BossMechanics.Bots.RegionComfortWeight
        float trackSlackYd = 0.75f;                             // BossMechanics.Bots.TrackSlackYd
        float lookAwayMarginDeg = 10.f;                         // BossMechanics.Bots.LookAway.MarginDeg
        bool tankFacingEnable = true;                           // BossMechanics.Bots.TankFacing.Enable
        bool parkEnable = true;                                 // BossMechanics.Bots.Park.Enable
        float parkDepthYd = 1.f;                                // BossMechanics.Bots.Park.DepthYd
        uint32 parkHoldCapMs = 8000;                            // BossMechanics.Bots.Park.HoldCapMs
        uint32 freezeSettleMs = 250;                            // BossMechanics.Bots.Freeze.SettleMs
        float healerSightWeight = 10.f;                         // BossMechanics.Bots.Healer.SightWeight
        uint32 healerMaxSightChecks = 8;                        // BossMechanics.Bots.Healer.MaxSightChecks
        bool routeEnable = true;                                // BossMechanics.Bots.Route.Enable
        float routePadYd = 8.f;                                 // BossMechanics.Bots.Route.PadYd
        float routeCellYd = 1.f;                                // BossMechanics.Bots.Route.CellYd
        uint32 routeMaxCells = 4096;                            // BossMechanics.Bots.Route.MaxCells
        uint32 routeMaxExpansions = 4096;                       // BossMechanics.Bots.Route.MaxExpansions
        uint32 routeMaxLegs = 8;                                // BossMechanics.Bots.Route.MaxLegs
        uint32 routeRetryMs = 1000;                             // BossMechanics.Bots.Route.RetryMs
    };

    struct ModuleConfig
    {
        bool enable = true;                                     // BossMechanics.Enable
        bool debug = false;                                     // BossMechanics.Debug
        std::string referenceHealthText = "20:1000:1500 60:5000:8000";  // BossMechanics.ReferenceHealth.Rows
        std::vector<ReferenceHealthRow> referenceHealthRows;
        FollowMode followMode = FollowMode::SplineStep;         // BossMechanics.Telegraph.FollowMode
        uint32 followIntervalMs = 200;                          // BossMechanics.Telegraph.FollowIntervalMs
        float lift = 0.3f;                                      // BossMechanics.Telegraph.Lift
        bool groundSnap = false;                                // BossMechanics.Telegraph.GroundSnap
        float groundSampleYd = 2.f;                             // BossMechanics.Telegraph.GroundSampleYd
        float maxPieceSpreadYd = 0.75f;                         // BossMechanics.Telegraph.MaxPieceSpreadYd
        bool shrinkProjectileLines = true;                      // BossMechanics.Telegraph.ShrinkProjectileLines
        bool fill = true;                                       // BossMechanics.Telegraph.Fill
        uint32 fillStepMs = 150;                                // BossMechanics.Telegraph.FillStepMs (longest step)
        float fillStepYd = 0.4f;                                // BossMechanics.Telegraph.FillStepYd (0 = fixed step)
        uint32 fillLeadMs = 150;                                // BossMechanics.Telegraph.FillLeadMs
        uint32 maxActivePerMap = 256;                           // BossMechanics.MaxActivePerMap
        float openWorldCandidateRadius = 100.f;                 // BossMechanics.OpenWorld.CandidateRadius (0 = map)
        BotConfig bots;                                         // BossMechanics.Bots.*

        ModuleConfig();
    };

    ModuleConfig const& GetConfig();
    void LoadConfig(bool reload);
}

#endif
