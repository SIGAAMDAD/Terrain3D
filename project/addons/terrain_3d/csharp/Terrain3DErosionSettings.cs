#pragma warning disable CS0109
using System;
using System.Diagnostics;
using System.Linq;
using System.Reflection;
using Godot;

namespace TokisanGames;

[Tool]
public partial class Terrain3DErosionSettings : Resource
{

    public enum BoundaryMode
    {
        Closed = 0,
        Open = 1,
    }

    public new class GDExtensionPropertyName : Resource.PropertyName
    {
        public new static readonly StringName SimulationYears = "simulation_years";
        public new static readonly StringName SimulationSteps = "simulation_steps";
        public new static readonly StringName ResolutionScale = "resolution_scale";
        public new static readonly StringName AutoAdjustResolution = "auto_adjust_resolution";
        public new static readonly StringName MemoryBudgetMb = "memory_budget_mb";
        public new static readonly StringName ThreadCount = "thread_count";
        public new static readonly StringName Seed = "seed";
        public new static readonly StringName BoundaryModeValue = "boundary_mode";
        public new static readonly StringName MaxHeightChangePerStep = "max_height_change_per_step";
        public new static readonly StringName PreserveHoles = "preserve_holes";
        public new static readonly StringName ApplyHeight = "apply_height";
        public new static readonly StringName HeightOutputStrength = "height_output_strength";
        public new static readonly StringName GlobalIntensity = "global_intensity";
        public new static readonly StringName SimulationMask = "simulation_mask";
        public new static readonly StringName SimulationMaskWorldRect = "simulation_mask_world_rect";
        public new static readonly StringName InvertSimulationMask = "invert_simulation_mask";
        public new static readonly StringName EdgeFadeDistanceM = "edge_fade_distance_m";
        public new static readonly StringName HeightRangeEnabled = "height_range_enabled";
        public new static readonly StringName MinimumHeightM = "minimum_height_m";
        public new static readonly StringName MaximumHeightM = "maximum_height_m";
        public new static readonly StringName HeightRangeFalloffM = "height_range_falloff_m";
        public new static readonly StringName HydraulicEnabled = "hydraulic_enabled";
        public new static readonly StringName RainfallMPerYear = "rainfall_m_per_year";
        public new static readonly StringName RainfallVariation = "rainfall_variation";
        public new static readonly StringName EvaporationFraction = "evaporation_fraction";
        public new static readonly StringName InfiltrationFraction = "infiltration_fraction";
        public new static readonly StringName FlowRate = "flow_rate";
        public new static readonly StringName SedimentCapacity = "sediment_capacity";
        public new static readonly StringName HydraulicErosionMPerYear = "hydraulic_erosion_m_per_year";
        public new static readonly StringName HydraulicDepositionRate = "hydraulic_deposition_rate";
        public new static readonly StringName MinimumSlope = "minimum_slope";
        public new static readonly StringName StormFrequency = "storm_frequency";
        public new static readonly StringName StormIntensity = "storm_intensity";
        public new static readonly StringName ChannelIncisionStrength = "channel_incision_strength";
        public new static readonly StringName StandingWaterDepositionRate = "standing_water_deposition_rate";
        public new static readonly StringName WeatheringEnabled = "weathering_enabled";
        public new static readonly StringName PhysicalWeatheringMPerYear = "physical_weathering_m_per_year";
        public new static readonly StringName ChemicalWeatheringMPerYear = "chemical_weathering_m_per_year";
        public new static readonly StringName FreezeThawStrength = "freeze_thaw_strength";
        public new static readonly StringName TalusAngleDegrees = "talus_angle_degrees";
        public new static readonly StringName ThermalCreepRate = "thermal_creep_rate";
        public new static readonly StringName BaseHardness = "base_hardness";
        public new static readonly StringName HardnessVariation = "hardness_variation";
        public new static readonly StringName OxidationEnabled = "oxidation_enabled";
        public new static readonly StringName OxidationRatePerYear = "oxidation_rate_per_year";
        public new static readonly StringName OxidationWeakening = "oxidation_weakening";
        public new static readonly StringName WeatheringSlopeBias = "weathering_slope_bias";
        public new static readonly StringName ChemicalMoistureExponent = "chemical_moisture_exponent";
        public new static readonly StringName FreezePoint = "freeze_point";
        public new static readonly StringName WindEnabled = "wind_enabled";
        public new static readonly StringName WindDirection = "wind_direction";
        public new static readonly StringName WindStrength = "wind_strength";
        public new static readonly StringName WindVariation = "wind_variation";
        public new static readonly StringName WindErosionMPerYear = "wind_erosion_m_per_year";
        public new static readonly StringName WindTransportRate = "wind_transport_rate";
        public new static readonly StringName WindDepositionRate = "wind_deposition_rate";
        public new static readonly StringName WindShelterStrength = "wind_shelter_strength";
        public new static readonly StringName WindThreshold = "wind_threshold";
        public new static readonly StringName WindTransportDistanceCells = "wind_transport_distance_cells";
        public new static readonly StringName WindDepositionSlopeBias = "wind_deposition_slope_bias";
        public new static readonly StringName WaveEnabled = "wave_enabled";
        public new static readonly StringName SeaLevel = "sea_level";
        public new static readonly StringName TidalRange = "tidal_range";
        public new static readonly StringName WaveDirection = "wave_direction";
        public new static readonly StringName WaveStrength = "wave_strength";
        public new static readonly StringName WaveErosionMPerYear = "wave_erosion_m_per_year";
        public new static readonly StringName WaveDepositionRate = "wave_deposition_rate";
        public new static readonly StringName CoastlineBandM = "coastline_band_m";
        public new static readonly StringName WaveFetchDistanceM = "wave_fetch_distance_m";
        public new static readonly StringName WaveUndercutBias = "wave_undercut_bias";
        public new static readonly StringName BeachSmoothing = "beach_smoothing";
        public new static readonly StringName TrafficEnabled = "traffic_enabled";
        public new static readonly StringName TrafficMask = "traffic_mask";
        public new static readonly StringName TrafficWorldRect = "traffic_world_rect";
        public new static readonly StringName TrafficErosionMPerYear = "traffic_erosion_m_per_year";
        public new static readonly StringName TrafficCompaction = "traffic_compaction";
        public new static readonly StringName TrafficSmoothing = "traffic_smoothing";
        public new static readonly StringName TrafficRunoffMultiplier = "traffic_runoff_multiplier";
        public new static readonly StringName TrafficThreshold = "traffic_threshold";
        public new static readonly StringName TrafficWideningCells = "traffic_widening_cells";
        public new static readonly StringName MaterialOutputEnabled = "material_output_enabled";
        public new static readonly StringName PreserveControlFlags = "preserve_control_flags";
        public new static readonly StringName ResolveAutoMaterials = "resolve_auto_materials";
        public new static readonly StringName MaterialTransitionStrength = "material_transition_strength";
        public new static readonly StringName MaterialTransitionThreshold = "material_transition_threshold";
        public new static readonly StringName MaterialTransitionDepthM = "material_transition_depth_m";
        public new static readonly StringName OxidationColorEnabled = "oxidation_color_enabled";
        public new static readonly StringName OxidationTint = "oxidation_tint";
        public new static readonly StringName OxidationTintStrength = "oxidation_tint_strength";
        public new static readonly StringName MasterIntensityCurve = "master_intensity_curve";
        public new static readonly StringName RainfallCurve = "rainfall_curve";
        public new static readonly StringName WeatheringCurve = "weathering_curve";
        public new static readonly StringName OxidationCurve = "oxidation_curve";
        public new static readonly StringName WindCurve = "wind_curve";
        public new static readonly StringName WaveCurve = "wave_curve";
        public new static readonly StringName TrafficCurve = "traffic_curve";
        public new static readonly StringName TemperatureCurve = "temperature_curve";
        public new static readonly StringName SeaLevelCurve = "sea_level_curve";
        public new static readonly StringName SalinityMask = "salinity_mask";
        public new static readonly StringName SurfaceLayerDepthMap = "surface_layer_depth_map";
        public new static readonly StringName BedrockHardnessMap = "bedrock_hardness_map";
        public new static readonly StringName SedimentSortingEnabled = "sediment_sorting_enabled";
        public new static readonly StringName AlluvialFanEnabled = "alluvial_fan_enabled";
        public new static readonly StringName DebrisFlowEnabled = "debris_flow_enabled";
        public new static readonly StringName WetDryEnabled = "wet_dry_enabled";
        public new static readonly StringName SaltWeatheringEnabled = "salt_weathering_enabled";
        public new static readonly StringName SalinityWorldRect = "salinity_world_rect";
        public new static readonly StringName ExposureAgeEnabled = "exposure_age_enabled";
        public new static readonly StringName SnowmeltEnabled = "snowmelt_enabled";
        public new static readonly StringName DuneEnabled = "dune_enabled";
        public new static readonly StringName DesertPavementEnabled = "desert_pavement_enabled";
        public new static readonly StringName StratigraphyEnabled = "stratigraphy_enabled";
        public new static readonly StringName SurfaceLayerDepthWorldRect = "surface_layer_depth_world_rect";
        public new static readonly StringName BedrockHardnessWorldRect = "bedrock_hardness_world_rect";
        public new static readonly StringName CoarseTransportMultiplier = "coarse_transport_multiplier";
        public new static readonly StringName SandTransportMultiplier = "sand_transport_multiplier";
        public new static readonly StringName FineTransportMultiplier = "fine_transport_multiplier";
        public new static readonly StringName CoarseSettlingRate = "coarse_settling_rate";
        public new static readonly StringName SandSettlingRate = "sand_settling_rate";
        public new static readonly StringName FineSettlingRate = "fine_settling_rate";
        public new static readonly StringName AlluvialFanStrength = "alluvial_fan_strength";
        public new static readonly StringName AlluvialFanSlopeDrop = "alluvial_fan_slope_drop";
        public new static readonly StringName AlluvialFanSpread = "alluvial_fan_spread";
        public new static readonly StringName DebrisFlowConcentrationThreshold = "debris_flow_concentration_threshold";
        public new static readonly StringName DebrisFlowSlopeThreshold = "debris_flow_slope_threshold";
        public new static readonly StringName DebrisFlowRate = "debris_flow_rate";
        public new static readonly StringName DebrisFlowDepositionRate = "debris_flow_deposition_rate";
        public new static readonly StringName DebrisFlowRunoutCells = "debris_flow_runout_cells";
        public new static readonly StringName WetDryWeatheringMPerCycle = "wet_dry_weathering_m_per_cycle";
        public new static readonly StringName WetThreshold = "wet_threshold";
        public new static readonly StringName DryThreshold = "dry_threshold";
        public new static readonly StringName BackgroundSalinity = "background_salinity";
        public new static readonly StringName SaltWeatheringMPerYear = "salt_weathering_m_per_year";
        public new static readonly StringName SaltCrystallizationStrength = "salt_crystallization_strength";
        public new static readonly StringName SaltEvaporationBias = "salt_evaporation_bias";
        public new static readonly StringName SaltCohesionLoss = "salt_cohesion_loss";
        public new static readonly StringName VarnishMaturationYears = "varnish_maturation_years";
        public new static readonly StringName ExposureResetDepthM = "exposure_reset_depth_m";
        public new static readonly StringName VarnishStabilitySlope = "varnish_stability_slope";
        public new static readonly StringName VarnishOxidationMultiplier = "varnish_oxidation_multiplier";
        public new static readonly StringName SnowfallFraction = "snowfall_fraction";
        public new static readonly StringName SnowAccumulationTemperature = "snow_accumulation_temperature";
        public new static readonly StringName SnowMeltTemperature = "snow_melt_temperature";
        public new static readonly StringName SnowMeltFractionPerStep = "snow_melt_fraction_per_step";
        public new static readonly StringName SnowSublimationFraction = "snow_sublimation_fraction";
        public new static readonly StringName SandReposeAngleDegrees = "sand_repose_angle_degrees";
        public new static readonly StringName DuneAvalancheRate = "dune_avalanche_rate";
        public new static readonly StringName DuneRelaxationIterations = "dune_relaxation_iterations";
        public new static readonly StringName DuneMinSandDepthM = "dune_min_sand_depth_m";
        public new static readonly StringName PavementFormationRate = "pavement_formation_rate";
        public new static readonly StringName PavementFineRemovalThreshold = "pavement_fine_removal_threshold";
        public new static readonly StringName PavementArmorStrength = "pavement_armor_strength";
        public new static readonly StringName PavementInfiltrationReduction = "pavement_infiltration_reduction";
        public new static readonly StringName SurfaceLayerDepthM = "surface_layer_depth_m";
        public new static readonly StringName BedrockHardness = "bedrock_hardness";
        public new static readonly StringName StratigraphyTransitionWidthM = "stratigraphy_transition_width_m";
        public new static readonly StringName SnowmeltCurve = "snowmelt_curve";
        public new static readonly StringName SaltWeatheringCurve = "salt_weathering_curve";
        public new static readonly StringName DebrisFlowCurve = "debris_flow_curve";
    }

    public new double SimulationYears
    {
        get => Get(GDExtensionPropertyName.SimulationYears).As<double>();
        set => Set(GDExtensionPropertyName.SimulationYears, value);
    }

    public new long SimulationSteps
    {
        get => Get(GDExtensionPropertyName.SimulationSteps).As<long>();
        set => Set(GDExtensionPropertyName.SimulationSteps, value);
    }

    public new long ResolutionScale
    {
        get => Get(GDExtensionPropertyName.ResolutionScale).As<long>();
        set => Set(GDExtensionPropertyName.ResolutionScale, value);
    }

    public new bool AutoAdjustResolution
    {
        get => Get(GDExtensionPropertyName.AutoAdjustResolution).As<bool>();
        set => Set(GDExtensionPropertyName.AutoAdjustResolution, value);
    }

    public new long MemoryBudgetMb
    {
        get => Get(GDExtensionPropertyName.MemoryBudgetMb).As<long>();
        set => Set(GDExtensionPropertyName.MemoryBudgetMb, value);
    }

    public new long ThreadCount
    {
        get => Get(GDExtensionPropertyName.ThreadCount).As<long>();
        set => Set(GDExtensionPropertyName.ThreadCount, value);
    }

    public new long Seed
    {
        get => Get(GDExtensionPropertyName.Seed).As<long>();
        set => Set(GDExtensionPropertyName.Seed, value);
    }

    public new BoundaryMode BoundaryModeValue
    {
        get => (BoundaryMode)Get(GDExtensionPropertyName.BoundaryModeValue).As<long>();
        set => Set(GDExtensionPropertyName.BoundaryModeValue, Variant.From(value));
    }

    public new double MaxHeightChangePerStep
    {
        get => Get(GDExtensionPropertyName.MaxHeightChangePerStep).As<double>();
        set => Set(GDExtensionPropertyName.MaxHeightChangePerStep, value);
    }

    public new bool PreserveHoles
    {
        get => Get(GDExtensionPropertyName.PreserveHoles).As<bool>();
        set => Set(GDExtensionPropertyName.PreserveHoles, value);
    }

    public new bool ApplyHeight
    {
        get => Get(GDExtensionPropertyName.ApplyHeight).As<bool>();
        set => Set(GDExtensionPropertyName.ApplyHeight, value);
    }

    public new double HeightOutputStrength
    {
        get => Get(GDExtensionPropertyName.HeightOutputStrength).As<double>();
        set => Set(GDExtensionPropertyName.HeightOutputStrength, value);
    }

    public new double GlobalIntensity
    {
        get => Get(GDExtensionPropertyName.GlobalIntensity).As<double>();
        set => Set(GDExtensionPropertyName.GlobalIntensity, value);
    }

    public new Image SimulationMask
    {
        get => Get(GDExtensionPropertyName.SimulationMask).As<Image>();
        set => Set(GDExtensionPropertyName.SimulationMask, value);
    }

    public new Rect2 SimulationMaskWorldRect
    {
        get => Get(GDExtensionPropertyName.SimulationMaskWorldRect).As<Rect2>();
        set => Set(GDExtensionPropertyName.SimulationMaskWorldRect, value);
    }

    public new bool InvertSimulationMask
    {
        get => Get(GDExtensionPropertyName.InvertSimulationMask).As<bool>();
        set => Set(GDExtensionPropertyName.InvertSimulationMask, value);
    }

    public new double EdgeFadeDistanceM
    {
        get => Get(GDExtensionPropertyName.EdgeFadeDistanceM).As<double>();
        set => Set(GDExtensionPropertyName.EdgeFadeDistanceM, value);
    }

    public new bool HeightRangeEnabled
    {
        get => Get(GDExtensionPropertyName.HeightRangeEnabled).As<bool>();
        set => Set(GDExtensionPropertyName.HeightRangeEnabled, value);
    }

    public new double MinimumHeightM
    {
        get => Get(GDExtensionPropertyName.MinimumHeightM).As<double>();
        set => Set(GDExtensionPropertyName.MinimumHeightM, value);
    }

    public new double MaximumHeightM
    {
        get => Get(GDExtensionPropertyName.MaximumHeightM).As<double>();
        set => Set(GDExtensionPropertyName.MaximumHeightM, value);
    }

    public new double HeightRangeFalloffM
    {
        get => Get(GDExtensionPropertyName.HeightRangeFalloffM).As<double>();
        set => Set(GDExtensionPropertyName.HeightRangeFalloffM, value);
    }

    public new bool HydraulicEnabled
    {
        get => Get(GDExtensionPropertyName.HydraulicEnabled).As<bool>();
        set => Set(GDExtensionPropertyName.HydraulicEnabled, value);
    }

    public new double RainfallMPerYear
    {
        get => Get(GDExtensionPropertyName.RainfallMPerYear).As<double>();
        set => Set(GDExtensionPropertyName.RainfallMPerYear, value);
    }

    public new double RainfallVariation
    {
        get => Get(GDExtensionPropertyName.RainfallVariation).As<double>();
        set => Set(GDExtensionPropertyName.RainfallVariation, value);
    }

    public new double EvaporationFraction
    {
        get => Get(GDExtensionPropertyName.EvaporationFraction).As<double>();
        set => Set(GDExtensionPropertyName.EvaporationFraction, value);
    }

    public new double InfiltrationFraction
    {
        get => Get(GDExtensionPropertyName.InfiltrationFraction).As<double>();
        set => Set(GDExtensionPropertyName.InfiltrationFraction, value);
    }

    public new double FlowRate
    {
        get => Get(GDExtensionPropertyName.FlowRate).As<double>();
        set => Set(GDExtensionPropertyName.FlowRate, value);
    }

    public new double SedimentCapacity
    {
        get => Get(GDExtensionPropertyName.SedimentCapacity).As<double>();
        set => Set(GDExtensionPropertyName.SedimentCapacity, value);
    }

    public new double HydraulicErosionMPerYear
    {
        get => Get(GDExtensionPropertyName.HydraulicErosionMPerYear).As<double>();
        set => Set(GDExtensionPropertyName.HydraulicErosionMPerYear, value);
    }

    public new double HydraulicDepositionRate
    {
        get => Get(GDExtensionPropertyName.HydraulicDepositionRate).As<double>();
        set => Set(GDExtensionPropertyName.HydraulicDepositionRate, value);
    }

    public new double MinimumSlope
    {
        get => Get(GDExtensionPropertyName.MinimumSlope).As<double>();
        set => Set(GDExtensionPropertyName.MinimumSlope, value);
    }

    public new double StormFrequency
    {
        get => Get(GDExtensionPropertyName.StormFrequency).As<double>();
        set => Set(GDExtensionPropertyName.StormFrequency, value);
    }

    public new double StormIntensity
    {
        get => Get(GDExtensionPropertyName.StormIntensity).As<double>();
        set => Set(GDExtensionPropertyName.StormIntensity, value);
    }

    public new double ChannelIncisionStrength
    {
        get => Get(GDExtensionPropertyName.ChannelIncisionStrength).As<double>();
        set => Set(GDExtensionPropertyName.ChannelIncisionStrength, value);
    }

    public new double StandingWaterDepositionRate
    {
        get => Get(GDExtensionPropertyName.StandingWaterDepositionRate).As<double>();
        set => Set(GDExtensionPropertyName.StandingWaterDepositionRate, value);
    }

    public new bool WeatheringEnabled
    {
        get => Get(GDExtensionPropertyName.WeatheringEnabled).As<bool>();
        set => Set(GDExtensionPropertyName.WeatheringEnabled, value);
    }

    public new double PhysicalWeatheringMPerYear
    {
        get => Get(GDExtensionPropertyName.PhysicalWeatheringMPerYear).As<double>();
        set => Set(GDExtensionPropertyName.PhysicalWeatheringMPerYear, value);
    }

    public new double ChemicalWeatheringMPerYear
    {
        get => Get(GDExtensionPropertyName.ChemicalWeatheringMPerYear).As<double>();
        set => Set(GDExtensionPropertyName.ChemicalWeatheringMPerYear, value);
    }

    public new double FreezeThawStrength
    {
        get => Get(GDExtensionPropertyName.FreezeThawStrength).As<double>();
        set => Set(GDExtensionPropertyName.FreezeThawStrength, value);
    }

    public new double TalusAngleDegrees
    {
        get => Get(GDExtensionPropertyName.TalusAngleDegrees).As<double>();
        set => Set(GDExtensionPropertyName.TalusAngleDegrees, value);
    }

    public new double ThermalCreepRate
    {
        get => Get(GDExtensionPropertyName.ThermalCreepRate).As<double>();
        set => Set(GDExtensionPropertyName.ThermalCreepRate, value);
    }

    public new double BaseHardness
    {
        get => Get(GDExtensionPropertyName.BaseHardness).As<double>();
        set => Set(GDExtensionPropertyName.BaseHardness, value);
    }

    public new double HardnessVariation
    {
        get => Get(GDExtensionPropertyName.HardnessVariation).As<double>();
        set => Set(GDExtensionPropertyName.HardnessVariation, value);
    }

    public new bool OxidationEnabled
    {
        get => Get(GDExtensionPropertyName.OxidationEnabled).As<bool>();
        set => Set(GDExtensionPropertyName.OxidationEnabled, value);
    }

    public new double OxidationRatePerYear
    {
        get => Get(GDExtensionPropertyName.OxidationRatePerYear).As<double>();
        set => Set(GDExtensionPropertyName.OxidationRatePerYear, value);
    }

    public new double OxidationWeakening
    {
        get => Get(GDExtensionPropertyName.OxidationWeakening).As<double>();
        set => Set(GDExtensionPropertyName.OxidationWeakening, value);
    }

    public new double WeatheringSlopeBias
    {
        get => Get(GDExtensionPropertyName.WeatheringSlopeBias).As<double>();
        set => Set(GDExtensionPropertyName.WeatheringSlopeBias, value);
    }

    public new double ChemicalMoistureExponent
    {
        get => Get(GDExtensionPropertyName.ChemicalMoistureExponent).As<double>();
        set => Set(GDExtensionPropertyName.ChemicalMoistureExponent, value);
    }

    public new double FreezePoint
    {
        get => Get(GDExtensionPropertyName.FreezePoint).As<double>();
        set => Set(GDExtensionPropertyName.FreezePoint, value);
    }

    public new bool WindEnabled
    {
        get => Get(GDExtensionPropertyName.WindEnabled).As<bool>();
        set => Set(GDExtensionPropertyName.WindEnabled, value);
    }

    public new Vector2 WindDirection
    {
        get => Get(GDExtensionPropertyName.WindDirection).As<Vector2>();
        set => Set(GDExtensionPropertyName.WindDirection, value);
    }

    public new double WindStrength
    {
        get => Get(GDExtensionPropertyName.WindStrength).As<double>();
        set => Set(GDExtensionPropertyName.WindStrength, value);
    }

    public new double WindVariation
    {
        get => Get(GDExtensionPropertyName.WindVariation).As<double>();
        set => Set(GDExtensionPropertyName.WindVariation, value);
    }

    public new double WindErosionMPerYear
    {
        get => Get(GDExtensionPropertyName.WindErosionMPerYear).As<double>();
        set => Set(GDExtensionPropertyName.WindErosionMPerYear, value);
    }

    public new double WindTransportRate
    {
        get => Get(GDExtensionPropertyName.WindTransportRate).As<double>();
        set => Set(GDExtensionPropertyName.WindTransportRate, value);
    }

    public new double WindDepositionRate
    {
        get => Get(GDExtensionPropertyName.WindDepositionRate).As<double>();
        set => Set(GDExtensionPropertyName.WindDepositionRate, value);
    }

    public new double WindShelterStrength
    {
        get => Get(GDExtensionPropertyName.WindShelterStrength).As<double>();
        set => Set(GDExtensionPropertyName.WindShelterStrength, value);
    }

    public new double WindThreshold
    {
        get => Get(GDExtensionPropertyName.WindThreshold).As<double>();
        set => Set(GDExtensionPropertyName.WindThreshold, value);
    }

    public new long WindTransportDistanceCells
    {
        get => Get(GDExtensionPropertyName.WindTransportDistanceCells).As<long>();
        set => Set(GDExtensionPropertyName.WindTransportDistanceCells, value);
    }

    public new double WindDepositionSlopeBias
    {
        get => Get(GDExtensionPropertyName.WindDepositionSlopeBias).As<double>();
        set => Set(GDExtensionPropertyName.WindDepositionSlopeBias, value);
    }

    public new bool WaveEnabled
    {
        get => Get(GDExtensionPropertyName.WaveEnabled).As<bool>();
        set => Set(GDExtensionPropertyName.WaveEnabled, value);
    }

    public new double SeaLevel
    {
        get => Get(GDExtensionPropertyName.SeaLevel).As<double>();
        set => Set(GDExtensionPropertyName.SeaLevel, value);
    }

    public new double TidalRange
    {
        get => Get(GDExtensionPropertyName.TidalRange).As<double>();
        set => Set(GDExtensionPropertyName.TidalRange, value);
    }

    public new Vector2 WaveDirection
    {
        get => Get(GDExtensionPropertyName.WaveDirection).As<Vector2>();
        set => Set(GDExtensionPropertyName.WaveDirection, value);
    }

    public new double WaveStrength
    {
        get => Get(GDExtensionPropertyName.WaveStrength).As<double>();
        set => Set(GDExtensionPropertyName.WaveStrength, value);
    }

    public new double WaveErosionMPerYear
    {
        get => Get(GDExtensionPropertyName.WaveErosionMPerYear).As<double>();
        set => Set(GDExtensionPropertyName.WaveErosionMPerYear, value);
    }

    public new double WaveDepositionRate
    {
        get => Get(GDExtensionPropertyName.WaveDepositionRate).As<double>();
        set => Set(GDExtensionPropertyName.WaveDepositionRate, value);
    }

    public new double CoastlineBandM
    {
        get => Get(GDExtensionPropertyName.CoastlineBandM).As<double>();
        set => Set(GDExtensionPropertyName.CoastlineBandM, value);
    }

    public new double WaveFetchDistanceM
    {
        get => Get(GDExtensionPropertyName.WaveFetchDistanceM).As<double>();
        set => Set(GDExtensionPropertyName.WaveFetchDistanceM, value);
    }

    public new double WaveUndercutBias
    {
        get => Get(GDExtensionPropertyName.WaveUndercutBias).As<double>();
        set => Set(GDExtensionPropertyName.WaveUndercutBias, value);
    }

    public new double BeachSmoothing
    {
        get => Get(GDExtensionPropertyName.BeachSmoothing).As<double>();
        set => Set(GDExtensionPropertyName.BeachSmoothing, value);
    }

    public new bool TrafficEnabled
    {
        get => Get(GDExtensionPropertyName.TrafficEnabled).As<bool>();
        set => Set(GDExtensionPropertyName.TrafficEnabled, value);
    }

    public new Image TrafficMask
    {
        get => Get(GDExtensionPropertyName.TrafficMask).As<Image>();
        set => Set(GDExtensionPropertyName.TrafficMask, value);
    }

    public new Rect2 TrafficWorldRect
    {
        get => Get(GDExtensionPropertyName.TrafficWorldRect).As<Rect2>();
        set => Set(GDExtensionPropertyName.TrafficWorldRect, value);
    }

    public new double TrafficErosionMPerYear
    {
        get => Get(GDExtensionPropertyName.TrafficErosionMPerYear).As<double>();
        set => Set(GDExtensionPropertyName.TrafficErosionMPerYear, value);
    }

    public new double TrafficCompaction
    {
        get => Get(GDExtensionPropertyName.TrafficCompaction).As<double>();
        set => Set(GDExtensionPropertyName.TrafficCompaction, value);
    }

    public new double TrafficSmoothing
    {
        get => Get(GDExtensionPropertyName.TrafficSmoothing).As<double>();
        set => Set(GDExtensionPropertyName.TrafficSmoothing, value);
    }

    public new double TrafficRunoffMultiplier
    {
        get => Get(GDExtensionPropertyName.TrafficRunoffMultiplier).As<double>();
        set => Set(GDExtensionPropertyName.TrafficRunoffMultiplier, value);
    }

    public new double TrafficThreshold
    {
        get => Get(GDExtensionPropertyName.TrafficThreshold).As<double>();
        set => Set(GDExtensionPropertyName.TrafficThreshold, value);
    }

    public new long TrafficWideningCells
    {
        get => Get(GDExtensionPropertyName.TrafficWideningCells).As<long>();
        set => Set(GDExtensionPropertyName.TrafficWideningCells, value);
    }

    public new bool MaterialOutputEnabled
    {
        get => Get(GDExtensionPropertyName.MaterialOutputEnabled).As<bool>();
        set => Set(GDExtensionPropertyName.MaterialOutputEnabled, value);
    }

    public new bool PreserveControlFlags
    {
        get => Get(GDExtensionPropertyName.PreserveControlFlags).As<bool>();
        set => Set(GDExtensionPropertyName.PreserveControlFlags, value);
    }

    public new bool ResolveAutoMaterials
    {
        get => Get(GDExtensionPropertyName.ResolveAutoMaterials).As<bool>();
        set => Set(GDExtensionPropertyName.ResolveAutoMaterials, value);
    }

    public new double MaterialTransitionStrength
    {
        get => Get(GDExtensionPropertyName.MaterialTransitionStrength).As<double>();
        set => Set(GDExtensionPropertyName.MaterialTransitionStrength, value);
    }

    public new double MaterialTransitionThreshold
    {
        get => Get(GDExtensionPropertyName.MaterialTransitionThreshold).As<double>();
        set => Set(GDExtensionPropertyName.MaterialTransitionThreshold, value);
    }

    public new double MaterialTransitionDepthM
    {
        get => Get(GDExtensionPropertyName.MaterialTransitionDepthM).As<double>();
        set => Set(GDExtensionPropertyName.MaterialTransitionDepthM, value);
    }

    public new bool OxidationColorEnabled
    {
        get => Get(GDExtensionPropertyName.OxidationColorEnabled).As<bool>();
        set => Set(GDExtensionPropertyName.OxidationColorEnabled, value);
    }

    public new Color OxidationTint
    {
        get => Get(GDExtensionPropertyName.OxidationTint).As<Color>();
        set => Set(GDExtensionPropertyName.OxidationTint, value);
    }

    public new double OxidationTintStrength
    {
        get => Get(GDExtensionPropertyName.OxidationTintStrength).As<double>();
        set => Set(GDExtensionPropertyName.OxidationTintStrength, value);
    }

    public new Curve MasterIntensityCurve
    {
        get => Get(GDExtensionPropertyName.MasterIntensityCurve).As<Curve>();
        set => Set(GDExtensionPropertyName.MasterIntensityCurve, value);
    }

    public new Curve RainfallCurve
    {
        get => Get(GDExtensionPropertyName.RainfallCurve).As<Curve>();
        set => Set(GDExtensionPropertyName.RainfallCurve, value);
    }

    public new Curve WeatheringCurve
    {
        get => Get(GDExtensionPropertyName.WeatheringCurve).As<Curve>();
        set => Set(GDExtensionPropertyName.WeatheringCurve, value);
    }

    public new Curve OxidationCurve
    {
        get => Get(GDExtensionPropertyName.OxidationCurve).As<Curve>();
        set => Set(GDExtensionPropertyName.OxidationCurve, value);
    }

    public new Curve WindCurve
    {
        get => Get(GDExtensionPropertyName.WindCurve).As<Curve>();
        set => Set(GDExtensionPropertyName.WindCurve, value);
    }

    public new Curve WaveCurve
    {
        get => Get(GDExtensionPropertyName.WaveCurve).As<Curve>();
        set => Set(GDExtensionPropertyName.WaveCurve, value);
    }

    public new Curve TrafficCurve
    {
        get => Get(GDExtensionPropertyName.TrafficCurve).As<Curve>();
        set => Set(GDExtensionPropertyName.TrafficCurve, value);
    }

    public new Curve TemperatureCurve
    {
        get => Get(GDExtensionPropertyName.TemperatureCurve).As<Curve>();
        set => Set(GDExtensionPropertyName.TemperatureCurve, value);
    }
    public new Curve SeaLevelCurve
    {
        get => Get(GDExtensionPropertyName.SeaLevelCurve).As<Curve>();
        set => Set(GDExtensionPropertyName.SeaLevelCurve, value);
    }

    public new Image SalinityMask
    {
        get => Get(GDExtensionPropertyName.SalinityMask).As<Image>();
        set => Set(GDExtensionPropertyName.SalinityMask, value);
    }

    public new Image SurfaceLayerDepthMap
    {
        get => Get(GDExtensionPropertyName.SurfaceLayerDepthMap).As<Image>();
        set => Set(GDExtensionPropertyName.SurfaceLayerDepthMap, value);
    }

    public new Image BedrockHardnessMap
    {
        get => Get(GDExtensionPropertyName.BedrockHardnessMap).As<Image>();
        set => Set(GDExtensionPropertyName.BedrockHardnessMap, value);
    }

    public new bool SedimentSortingEnabled
    {
        get => Get(GDExtensionPropertyName.SedimentSortingEnabled).As<bool>();
        set => Set(GDExtensionPropertyName.SedimentSortingEnabled, value);
    }

    public new bool AlluvialFanEnabled
    {
        get => Get(GDExtensionPropertyName.AlluvialFanEnabled).As<bool>();
        set => Set(GDExtensionPropertyName.AlluvialFanEnabled, value);
    }

    public new bool DebrisFlowEnabled
    {
        get => Get(GDExtensionPropertyName.DebrisFlowEnabled).As<bool>();
        set => Set(GDExtensionPropertyName.DebrisFlowEnabled, value);
    }

    public new bool WetDryEnabled
    {
        get => Get(GDExtensionPropertyName.WetDryEnabled).As<bool>();
        set => Set(GDExtensionPropertyName.WetDryEnabled, value);
    }

    public new bool SaltWeatheringEnabled
    {
        get => Get(GDExtensionPropertyName.SaltWeatheringEnabled).As<bool>();
        set => Set(GDExtensionPropertyName.SaltWeatheringEnabled, value);
    }

    public new Rect2 SalinityWorldRect
    {
        get => Get(GDExtensionPropertyName.SalinityWorldRect).As<Rect2>();
        set => Set(GDExtensionPropertyName.SalinityWorldRect, value);
    }

    public new bool ExposureAgeEnabled
    {
        get => Get(GDExtensionPropertyName.ExposureAgeEnabled).As<bool>();
        set => Set(GDExtensionPropertyName.ExposureAgeEnabled, value);
    }

    public new bool SnowmeltEnabled
    {
        get => Get(GDExtensionPropertyName.SnowmeltEnabled).As<bool>();
        set => Set(GDExtensionPropertyName.SnowmeltEnabled, value);
    }

    public new bool DuneEnabled
    {
        get => Get(GDExtensionPropertyName.DuneEnabled).As<bool>();
        set => Set(GDExtensionPropertyName.DuneEnabled, value);
    }

    public new bool DesertPavementEnabled
    {
        get => Get(GDExtensionPropertyName.DesertPavementEnabled).As<bool>();
        set => Set(GDExtensionPropertyName.DesertPavementEnabled, value);
    }

    public new bool StratigraphyEnabled
    {
        get => Get(GDExtensionPropertyName.StratigraphyEnabled).As<bool>();
        set => Set(GDExtensionPropertyName.StratigraphyEnabled, value);
    }

    public new Rect2 SurfaceLayerDepthWorldRect
    {
        get => Get(GDExtensionPropertyName.SurfaceLayerDepthWorldRect).As<Rect2>();
        set => Set(GDExtensionPropertyName.SurfaceLayerDepthWorldRect, value);
    }

    public new Rect2 BedrockHardnessWorldRect
    {
        get => Get(GDExtensionPropertyName.BedrockHardnessWorldRect).As<Rect2>();
        set => Set(GDExtensionPropertyName.BedrockHardnessWorldRect, value);
    }

    public new double CoarseTransportMultiplier
    {
        get => Get(GDExtensionPropertyName.CoarseTransportMultiplier).As<double>();
        set => Set(GDExtensionPropertyName.CoarseTransportMultiplier, value);
    }

    public new double SandTransportMultiplier
    {
        get => Get(GDExtensionPropertyName.SandTransportMultiplier).As<double>();
        set => Set(GDExtensionPropertyName.SandTransportMultiplier, value);
    }

    public new double FineTransportMultiplier
    {
        get => Get(GDExtensionPropertyName.FineTransportMultiplier).As<double>();
        set => Set(GDExtensionPropertyName.FineTransportMultiplier, value);
    }

    public new double CoarseSettlingRate
    {
        get => Get(GDExtensionPropertyName.CoarseSettlingRate).As<double>();
        set => Set(GDExtensionPropertyName.CoarseSettlingRate, value);
    }

    public new double SandSettlingRate
    {
        get => Get(GDExtensionPropertyName.SandSettlingRate).As<double>();
        set => Set(GDExtensionPropertyName.SandSettlingRate, value);
    }

    public new double FineSettlingRate
    {
        get => Get(GDExtensionPropertyName.FineSettlingRate).As<double>();
        set => Set(GDExtensionPropertyName.FineSettlingRate, value);
    }

    public new double AlluvialFanStrength
    {
        get => Get(GDExtensionPropertyName.AlluvialFanStrength).As<double>();
        set => Set(GDExtensionPropertyName.AlluvialFanStrength, value);
    }

    public new double AlluvialFanSlopeDrop
    {
        get => Get(GDExtensionPropertyName.AlluvialFanSlopeDrop).As<double>();
        set => Set(GDExtensionPropertyName.AlluvialFanSlopeDrop, value);
    }

    public new double AlluvialFanSpread
    {
        get => Get(GDExtensionPropertyName.AlluvialFanSpread).As<double>();
        set => Set(GDExtensionPropertyName.AlluvialFanSpread, value);
    }

    public new double DebrisFlowConcentrationThreshold
    {
        get => Get(GDExtensionPropertyName.DebrisFlowConcentrationThreshold).As<double>();
        set => Set(GDExtensionPropertyName.DebrisFlowConcentrationThreshold, value);
    }

    public new double DebrisFlowSlopeThreshold
    {
        get => Get(GDExtensionPropertyName.DebrisFlowSlopeThreshold).As<double>();
        set => Set(GDExtensionPropertyName.DebrisFlowSlopeThreshold, value);
    }

    public new double DebrisFlowRate
    {
        get => Get(GDExtensionPropertyName.DebrisFlowRate).As<double>();
        set => Set(GDExtensionPropertyName.DebrisFlowRate, value);
    }

    public new double DebrisFlowDepositionRate
    {
        get => Get(GDExtensionPropertyName.DebrisFlowDepositionRate).As<double>();
        set => Set(GDExtensionPropertyName.DebrisFlowDepositionRate, value);
    }

    public new long DebrisFlowRunoutCells
    {
        get => Get(GDExtensionPropertyName.DebrisFlowRunoutCells).As<long>();
        set => Set(GDExtensionPropertyName.DebrisFlowRunoutCells, value);
    }

    public new double WetDryWeatheringMPerCycle
    {
        get => Get(GDExtensionPropertyName.WetDryWeatheringMPerCycle).As<double>();
        set => Set(GDExtensionPropertyName.WetDryWeatheringMPerCycle, value);
    }

    public new double WetThreshold
    {
        get => Get(GDExtensionPropertyName.WetThreshold).As<double>();
        set => Set(GDExtensionPropertyName.WetThreshold, value);
    }

    public new double DryThreshold
    {
        get => Get(GDExtensionPropertyName.DryThreshold).As<double>();
        set => Set(GDExtensionPropertyName.DryThreshold, value);
    }

    public new double BackgroundSalinity
    {
        get => Get(GDExtensionPropertyName.BackgroundSalinity).As<double>();
        set => Set(GDExtensionPropertyName.BackgroundSalinity, value);
    }

    public new double SaltWeatheringMPerYear
    {
        get => Get(GDExtensionPropertyName.SaltWeatheringMPerYear).As<double>();
        set => Set(GDExtensionPropertyName.SaltWeatheringMPerYear, value);
    }

    public new double SaltCrystallizationStrength
    {
        get => Get(GDExtensionPropertyName.SaltCrystallizationStrength).As<double>();
        set => Set(GDExtensionPropertyName.SaltCrystallizationStrength, value);
    }

    public new double SaltEvaporationBias
    {
        get => Get(GDExtensionPropertyName.SaltEvaporationBias).As<double>();
        set => Set(GDExtensionPropertyName.SaltEvaporationBias, value);
    }

    public new double SaltCohesionLoss
    {
        get => Get(GDExtensionPropertyName.SaltCohesionLoss).As<double>();
        set => Set(GDExtensionPropertyName.SaltCohesionLoss, value);
    }

    public new double VarnishMaturationYears
    {
        get => Get(GDExtensionPropertyName.VarnishMaturationYears).As<double>();
        set => Set(GDExtensionPropertyName.VarnishMaturationYears, value);
    }

    public new double ExposureResetDepthM
    {
        get => Get(GDExtensionPropertyName.ExposureResetDepthM).As<double>();
        set => Set(GDExtensionPropertyName.ExposureResetDepthM, value);
    }

    public new double VarnishStabilitySlope
    {
        get => Get(GDExtensionPropertyName.VarnishStabilitySlope).As<double>();
        set => Set(GDExtensionPropertyName.VarnishStabilitySlope, value);
    }

    public new double VarnishOxidationMultiplier
    {
        get => Get(GDExtensionPropertyName.VarnishOxidationMultiplier).As<double>();
        set => Set(GDExtensionPropertyName.VarnishOxidationMultiplier, value);
    }

    public new double SnowfallFraction
    {
        get => Get(GDExtensionPropertyName.SnowfallFraction).As<double>();
        set => Set(GDExtensionPropertyName.SnowfallFraction, value);
    }

    public new double SnowAccumulationTemperature
    {
        get => Get(GDExtensionPropertyName.SnowAccumulationTemperature).As<double>();
        set => Set(GDExtensionPropertyName.SnowAccumulationTemperature, value);
    }

    public new double SnowMeltTemperature
    {
        get => Get(GDExtensionPropertyName.SnowMeltTemperature).As<double>();
        set => Set(GDExtensionPropertyName.SnowMeltTemperature, value);
    }

    public new double SnowMeltFractionPerStep
    {
        get => Get(GDExtensionPropertyName.SnowMeltFractionPerStep).As<double>();
        set => Set(GDExtensionPropertyName.SnowMeltFractionPerStep, value);
    }

    public new double SnowSublimationFraction
    {
        get => Get(GDExtensionPropertyName.SnowSublimationFraction).As<double>();
        set => Set(GDExtensionPropertyName.SnowSublimationFraction, value);
    }

    public new double SandReposeAngleDegrees
    {
        get => Get(GDExtensionPropertyName.SandReposeAngleDegrees).As<double>();
        set => Set(GDExtensionPropertyName.SandReposeAngleDegrees, value);
    }

    public new double DuneAvalancheRate
    {
        get => Get(GDExtensionPropertyName.DuneAvalancheRate).As<double>();
        set => Set(GDExtensionPropertyName.DuneAvalancheRate, value);
    }

    public new long DuneRelaxationIterations
    {
        get => Get(GDExtensionPropertyName.DuneRelaxationIterations).As<long>();
        set => Set(GDExtensionPropertyName.DuneRelaxationIterations, value);
    }

    public new double DuneMinSandDepthM
    {
        get => Get(GDExtensionPropertyName.DuneMinSandDepthM).As<double>();
        set => Set(GDExtensionPropertyName.DuneMinSandDepthM, value);
    }

    public new double PavementFormationRate
    {
        get => Get(GDExtensionPropertyName.PavementFormationRate).As<double>();
        set => Set(GDExtensionPropertyName.PavementFormationRate, value);
    }

    public new double PavementFineRemovalThreshold
    {
        get => Get(GDExtensionPropertyName.PavementFineRemovalThreshold).As<double>();
        set => Set(GDExtensionPropertyName.PavementFineRemovalThreshold, value);
    }

    public new double PavementArmorStrength
    {
        get => Get(GDExtensionPropertyName.PavementArmorStrength).As<double>();
        set => Set(GDExtensionPropertyName.PavementArmorStrength, value);
    }

    public new double PavementInfiltrationReduction
    {
        get => Get(GDExtensionPropertyName.PavementInfiltrationReduction).As<double>();
        set => Set(GDExtensionPropertyName.PavementInfiltrationReduction, value);
    }

    public new double SurfaceLayerDepthM
    {
        get => Get(GDExtensionPropertyName.SurfaceLayerDepthM).As<double>();
        set => Set(GDExtensionPropertyName.SurfaceLayerDepthM, value);
    }

    public new double BedrockHardness
    {
        get => Get(GDExtensionPropertyName.BedrockHardness).As<double>();
        set => Set(GDExtensionPropertyName.BedrockHardness, value);
    }

    public new double StratigraphyTransitionWidthM
    {
        get => Get(GDExtensionPropertyName.StratigraphyTransitionWidthM).As<double>();
        set => Set(GDExtensionPropertyName.StratigraphyTransitionWidthM, value);
    }

    public new Curve SnowmeltCurve
    {
        get => Get(GDExtensionPropertyName.SnowmeltCurve).As<Curve>();
        set => Set(GDExtensionPropertyName.SnowmeltCurve, value);
    }

    public new Curve SaltWeatheringCurve
    {
        get => Get(GDExtensionPropertyName.SaltWeatheringCurve).As<Curve>();
        set => Set(GDExtensionPropertyName.SaltWeatheringCurve, value);
    }

    public new Curve DebrisFlowCurve
    {
        get => Get(GDExtensionPropertyName.DebrisFlowCurve).As<Curve>();
        set => Set(GDExtensionPropertyName.DebrisFlowCurve, value);
    }

}
