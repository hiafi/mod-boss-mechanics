/*
 * mod-boss-mechanics: configuration loading (STAGE-M1-HANDOFF §3.11).
 */

#include "BossMechanicsConfig.h"
#include "Config.h"
#include "Log.h"
#include "MechanicProviders.h"
#include "Random.h"
#include <algorithm>
#include <cmath>

namespace BossMechanics
{
    namespace
    {
        constexpr char const* kDefaultRows = "20:1000:1500 60:5000:8000";

        ModuleConfig sConfig;

        // Reads an unsigned key and clamps it into [lo, hi], warning when the configured value was out of range.
        uint32 GetClampedU32(char const* name, uint32 def, uint32 lo, uint32 hi)
        {
            uint32 const v = sConfigMgr->GetOption<uint32>(name, def);
            uint32 const c = std::clamp(v, lo, hi);
            if (c != v)
                LOG_WARN("module.bossmechanics", "{} = {} is out of range {}-{}; using {}", name, v, lo, hi, c);
            return c;
        }

        float GetClampedFloat(char const* name, float def, float lo, float hi)
        {
            float const v = sConfigMgr->GetOption<float>(name, def);
            float const c = std::isfinite(v) ? std::clamp(v, lo, hi) : def;
            if (c != v)
                LOG_WARN("module.bossmechanics", "{} = {} is out of range {}-{}; using {}", name, v, lo, hi, c);
            return c;
        }

        BotConfig LoadBotConfig()
        {
            BotConfig b;
            b.enable = sConfigMgr->GetOption<bool>("BossMechanics.Bots.Enable", true);
            b.avoidAoeInInstances = sConfigMgr->GetOption<bool>("BossMechanics.Bots.AvoidAoeInInstances", true);
            b.openWorldTestScope = sConfigMgr->GetOption<bool>("BossMechanics.Bots.OpenWorldTestScope", true);
            b.gateSweepMs = GetClampedU32("BossMechanics.Bots.GateSweepMs", 1000, 100, 60000);
            b.fastTickMs = GetClampedU32("BossMechanics.Bots.FastTickMs", 100, 0, 1000);
            b.startDelayMs = GetClampedU32("BossMechanics.Bots.StartDelayMs", 150, 0, 2000);
            b.safetyMarginYd = GetClampedFloat("BossMechanics.Bots.SafetyMarginYd", 0.75f, 0.25f, 5.f);
            b.safetyMarginMs = GetClampedU32("BossMechanics.Bots.SafetyMarginMs", 300, 0, 2000);
            b.postResolveHoldMs = GetClampedU32("BossMechanics.Bots.PostResolveHoldMs", 500, 0, 5000);
            b.graceMarginMs = GetClampedU32("BossMechanics.Bots.GraceMarginMs", 250, 0, 5000);
            b.horizonMs = GetClampedU32("BossMechanics.Bots.HorizonMs", 10000, 1000, 60000);
            b.holdCapMs = GetClampedU32("BossMechanics.Bots.HoldCapMs", 3000, 0, 60000);
            b.maxCandidates = GetClampedU32("BossMechanics.Bots.MaxCandidates", 400, 16, 4000);
            b.maxPathChecks = GetClampedU32("BossMechanics.Bots.MaxPathChecks", 8, 1, 64);
            b.gridStepYd = GetClampedFloat("BossMechanics.Bots.GridStepYd", 2.f, 0.5f, 10.f);
            b.minSolveIntervalMs = GetClampedU32("BossMechanics.Bots.MinSolveIntervalMs", 150, 0, 5000);
            b.movingAnchorRecheckMs = GetClampedU32("BossMechanics.Bots.MovingAnchorRecheckMs", 250, 50, 5000);
            b.roleSlackMs = GetClampedU32("BossMechanics.Bots.RoleSlackMs", 1000, 0, 60000);
            b.rangedDistanceYd = GetClampedFloat("BossMechanics.Bots.RangedDistanceYd", 22.f, 8.f, 40.f);
            b.facingSlackDeg = GetClampedFloat("BossMechanics.Bots.FacingSlackDeg", 15.f, 0.f, 90.f);
            b.debug = sConfigMgr->GetOption<bool>("BossMechanics.Bots.Debug", false);

            // M3
            b.humanSlackYd = GetClampedFloat("BossMechanics.Bots.HumanSlackYd", 1.f, 0.f, 5.f);
            b.regionComfortWeight = GetClampedFloat("BossMechanics.Bots.RegionComfortWeight", 1.5f, 0.f, 20.f);
            b.trackSlackYd = GetClampedFloat("BossMechanics.Bots.TrackSlackYd", 0.75f, 0.f, 5.f);
            b.lookAwayMarginDeg = GetClampedFloat("BossMechanics.Bots.LookAway.MarginDeg", 10.f, 0.f, 45.f);
            b.tankFacingEnable = sConfigMgr->GetOption<bool>("BossMechanics.Bots.TankFacing.Enable", true);
            b.parkEnable = sConfigMgr->GetOption<bool>("BossMechanics.Bots.Park.Enable", true);
            b.parkDepthYd = GetClampedFloat("BossMechanics.Bots.Park.DepthYd", 1.f, 0.f, 10.f);
            b.parkHoldCapMs = GetClampedU32("BossMechanics.Bots.Park.HoldCapMs", 8000, 0, 60000);
            b.freezeSettleMs = GetClampedU32("BossMechanics.Bots.Freeze.SettleMs", 250, 0, 2000);
            b.healerSightWeight = GetClampedFloat("BossMechanics.Bots.Healer.SightWeight", 10.f, 0.f, 50.f);
            b.healerMaxSightChecks = GetClampedU32("BossMechanics.Bots.Healer.MaxSightChecks", 8, 0, 32);
            b.routeEnable = sConfigMgr->GetOption<bool>("BossMechanics.Bots.Route.Enable", true);
            b.routePadYd = GetClampedFloat("BossMechanics.Bots.Route.PadYd", 8.f, 0.f, 40.f);
            b.routeCellYd = GetClampedFloat("BossMechanics.Bots.Route.CellYd", 1.f, 0.25f, 5.f);
            b.routeMaxCells = GetClampedU32("BossMechanics.Bots.Route.MaxCells", 4096, 64, 16384);
            b.routeMaxExpansions = GetClampedU32("BossMechanics.Bots.Route.MaxExpansions", 4096, 64, 16384);
            b.routeMaxLegs = GetClampedU32("BossMechanics.Bots.Route.MaxLegs", 8, 1, 32);
            b.routeRetryMs = GetClampedU32("BossMechanics.Bots.Route.RetryMs", 1000, 100, 60000);
            b.forceLeaks = GetClampedU32("BossMechanics.Bots.ForceLeaks", 0, 0, 100);

            BotFallibilityConfig& f = b.fallibility;
            f.enable = sConfigMgr->GetOption<bool>("BossMechanics.Bots.Fallibility.Enable", false);
            f.reactionMs = GetClampedU32("BossMechanics.Bots.Fallibility.ReactionMs", 350, 0, 5000);
            f.jitterMs = GetClampedU32("BossMechanics.Bots.Fallibility.JitterMs", 250, 0, 5000);
            f.lateChancePct = GetClampedFloat("BossMechanics.Bots.Fallibility.LateChancePct", 5.f, 0.f, 100.f);
            f.lateExtraMs = GetClampedU32("BossMechanics.Bots.Fallibility.LateExtraMs", 700, 0, 5000);
            f.wrongSideChancePct = GetClampedFloat("BossMechanics.Bots.Fallibility.WrongSideChancePct", 3.f, 0.f,
                100.f);
            f.realiseMs = GetClampedU32("BossMechanics.Bots.Fallibility.RealiseMs", 700, 0, 10000);
            f.scaleNormal = GetClampedFloat("BossMechanics.Bots.Fallibility.ScaleNormal", 1.f, 0.f, 10.f);
            f.scaleHeroic = GetClampedFloat("BossMechanics.Bots.Fallibility.ScaleHeroic", 1.f, 0.f, 10.f);
            f.seed = sConfigMgr->GetOption<uint64>("BossMechanics.Bots.Fallibility.Seed", 0);
            f.effectiveSeed = f.seed;
            if (!f.effectiveSeed)
            {
                // One random seed per load (two urand draws), never 0.
                f.effectiveSeed = (uint64(urand(0, 0xFFFFFFFF)) << 32) | urand(0, 0xFFFFFFFF);
                if (!f.effectiveSeed)
                    f.effectiveSeed = 1;
            }
            return b;
        }
    }

    ModuleConfig::ModuleConfig()
    {
        ParseReferenceHealthRows(referenceHealthText, referenceHealthRows);
    }

    ModuleConfig const& GetConfig()
    {
        return sConfig;
    }

    void LoadConfig(bool reload)
    {
        ModuleConfig c;
        c.enable = sConfigMgr->GetOption<bool>("BossMechanics.Enable", true);
        c.debug = sConfigMgr->GetOption<bool>("BossMechanics.Debug", false);

        c.referenceHealthText = sConfigMgr->GetOption<std::string>("BossMechanics.ReferenceHealth.Rows", kDefaultRows);
        if (!ParseReferenceHealthRows(c.referenceHealthText, c.referenceHealthRows))
        {
            LOG_WARN("module.bossmechanics", "BossMechanics.ReferenceHealth.Rows '{}' is malformed (want "
                "'level:nonTank:tank ...'); using '{}'", c.referenceHealthText, kDefaultRows);
            c.referenceHealthText = kDefaultRows;
            ParseReferenceHealthRows(c.referenceHealthText, c.referenceHealthRows);
        }

        uint32 const mode = sConfigMgr->GetOption<uint32>("BossMechanics.Telegraph.FollowMode", 0);
        if (mode > static_cast<uint32>(FollowMode::Teleport))
        {
            LOG_WARN("module.bossmechanics", "BossMechanics.Telegraph.FollowMode {} is out of range 0-2; using 0",
                mode);
            c.followMode = FollowMode::SplineStep;
        }
        else
            c.followMode = static_cast<FollowMode>(mode);

        c.followIntervalMs = std::max<uint32>(50, sConfigMgr->GetOption<uint32>(
            "BossMechanics.Telegraph.FollowIntervalMs", 200));
        c.lift = sConfigMgr->GetOption<float>("BossMechanics.Telegraph.Lift", 0.3f);
        c.groundSnap = sConfigMgr->GetOption<bool>("BossMechanics.Telegraph.GroundSnap", false);
        c.groundSampleYd = std::max(0.5f, sConfigMgr->GetOption<float>("BossMechanics.Telegraph.GroundSampleYd", 2.f));
        c.maxPieceSpreadYd = std::max(0.f, sConfigMgr->GetOption<float>("BossMechanics.Telegraph.MaxPieceSpreadYd",
            0.75f));
        c.shrinkProjectileLines = sConfigMgr->GetOption<bool>("BossMechanics.Telegraph.ShrinkProjectileLines", true);
        c.fill = sConfigMgr->GetOption<bool>("BossMechanics.Telegraph.Fill", true);
        c.fillStepMs = std::max<uint32>(50, sConfigMgr->GetOption<uint32>("BossMechanics.Telegraph.FillStepMs", 150));
        c.fillStepYd = std::max(0.f, sConfigMgr->GetOption<float>("BossMechanics.Telegraph.FillStepYd", 0.4f));
        c.fillLeadMs = std::min<uint32>(2000, sConfigMgr->GetOption<uint32>("BossMechanics.Telegraph.FillLeadMs", 150));
        c.maxActivePerMap = sConfigMgr->GetOption<uint32>("BossMechanics.MaxActivePerMap", 256);
        c.openWorldCandidateRadius = std::max(0.f, sConfigMgr->GetOption<float>(
            "BossMechanics.OpenWorld.CandidateRadius", 100.f));
        c.bots = LoadBotConfig();

        sConfig = std::move(c);

        if (sConfig.bots.forceLeaks)
            LOG_WARN("module.bossmechanics.bots", "mod-boss-mechanics: BossMechanics.Bots.ForceLeaks = {}: the bots' "
                "lock lets blocked moves, casts and turns through until each leak detector has counted {} (a debug "
                "check; never on a live realm)", sConfig.bots.forceLeaks, sConfig.bots.forceLeaks);

        if (sConfig.bots.fallibility.enable)
            LOG_INFO("module.bossmechanics.bots", "mod-boss-mechanics: bot fallibility on, seed {}{}",
                sConfig.bots.fallibility.effectiveSeed, sConfig.bots.fallibility.seed ? "" : " (random)");

        if (!reload && IsReferenceHealthStubActive())
            LOG_INFO("module.bossmechanics", "mod-boss-mechanics: reference health stub active ('{}'); placeholder "
                "numbers until the difficulties work installs measured ones", sConfig.referenceHealthText);
    }
}
