/*
 * mod-boss-mechanics: configuration loading (STAGE-M1-HANDOFF §3.11).
 */

#include "BossMechanicsConfig.h"
#include "Config.h"
#include "Log.h"
#include "MechanicProviders.h"

namespace BossMechanics
{
    namespace
    {
        constexpr char const* kDefaultRows = "20:1000:1500 60:5000:8000";

        ModuleConfig sConfig;
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
        c.lift = sConfigMgr->GetOption<float>("BossMechanics.Telegraph.Lift", 0.1f);
        c.shrinkProjectileLines = sConfigMgr->GetOption<bool>("BossMechanics.Telegraph.ShrinkProjectileLines", true);
        c.fill = sConfigMgr->GetOption<bool>("BossMechanics.Telegraph.Fill", true);
        c.fillStepMs = std::max<uint32>(50, sConfigMgr->GetOption<uint32>("BossMechanics.Telegraph.FillStepMs", 150));
        c.fillStepYd = std::max(0.f, sConfigMgr->GetOption<float>("BossMechanics.Telegraph.FillStepYd", 0.4f));
        c.fillLeadMs = std::min<uint32>(2000, sConfigMgr->GetOption<uint32>("BossMechanics.Telegraph.FillLeadMs", 150));
        c.maxActivePerMap = sConfigMgr->GetOption<uint32>("BossMechanics.MaxActivePerMap", 256);
        c.openWorldCandidateRadius = std::max(0.f, sConfigMgr->GetOption<float>(
            "BossMechanics.OpenWorld.CandidateRadius", 100.f));

        sConfig = std::move(c);

        if (!reload && IsReferenceHealthStubActive())
            LOG_INFO("module.bossmechanics", "mod-boss-mechanics: reference health stub active ('{}'); placeholder "
                "numbers until the difficulties work installs measured ones", sConfig.referenceHealthText);
    }
}
