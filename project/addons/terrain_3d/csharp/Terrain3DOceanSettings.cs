#pragma warning disable CS0109
using Godot;

namespace TokisanGames;

[Tool]
public partial class Terrain3DOceanSettings : Resource
{
    public new class GDExtensionPropertyName : Resource.PropertyName
    {
        public new static readonly StringName SeaLevel = "sea_level";
        public new static readonly StringName WaterColor = "water_color";
        public new static readonly StringName VisibleDepth = "visible_depth";
        public new static readonly StringName DepthCurve = "depth_curve";
        public new static readonly StringName ShoreBlend = "shore_blend";
        public new static readonly StringName RefractionStrength = "refraction_strength";
        public new static readonly StringName TimeScale = "time_scale";
        public new static readonly StringName HeightScale = "height_scale";
        public new static readonly StringName OceanScale = "ocean_scale";
        public new static readonly StringName FresnelScale = "fresnel_scale";
        public new static readonly StringName FresnelPower = "fresnel_power";
        public new static readonly StringName Specular = "specular";
        public new static readonly StringName FoamHeight = "foam_height";
        public new static readonly StringName FoamTopIntensity = "foam_top_intensity";
        public new static readonly StringName FoamDistance = "foam_distance";
        public new static readonly StringName FoamShoreIntensity = "foam_shore_intensity";
        public new static readonly StringName FoamNoiseScale = "foam_noise_scale";
        public new static readonly StringName HeightScattering = "height_scattering";
        public new static readonly StringName HeightThreshold = "height_threshold";
        public new static readonly StringName HeightAngleThreshold = "height_angle_threshold";
        public new static readonly StringName FresnelScattering = "fresnel_scattering";
        public new static readonly StringName DirectScattering = "direct_scattering";
    }

    public double SeaLevel { get => Get(GDExtensionPropertyName.SeaLevel).AsDouble(); set => Set(GDExtensionPropertyName.SeaLevel, value); }
    public Color WaterColor { get => Get(GDExtensionPropertyName.WaterColor).AsColor(); set => Set(GDExtensionPropertyName.WaterColor, value); }
    public double VisibleDepth { get => Get(GDExtensionPropertyName.VisibleDepth).AsDouble(); set => Set(GDExtensionPropertyName.VisibleDepth, value); }
    public double DepthCurve { get => Get(GDExtensionPropertyName.DepthCurve).AsDouble(); set => Set(GDExtensionPropertyName.DepthCurve, value); }
    public double ShoreBlend { get => Get(GDExtensionPropertyName.ShoreBlend).AsDouble(); set => Set(GDExtensionPropertyName.ShoreBlend, value); }
    public double RefractionStrength { get => Get(GDExtensionPropertyName.RefractionStrength).AsDouble(); set => Set(GDExtensionPropertyName.RefractionStrength, value); }
    public double TimeScale { get => Get(GDExtensionPropertyName.TimeScale).AsDouble(); set => Set(GDExtensionPropertyName.TimeScale, value); }
    public double HeightScale { get => Get(GDExtensionPropertyName.HeightScale).AsDouble(); set => Set(GDExtensionPropertyName.HeightScale, value); }
    public double OceanScale { get => Get(GDExtensionPropertyName.OceanScale).AsDouble(); set => Set(GDExtensionPropertyName.OceanScale, value); }
    public double FresnelScale { get => Get(GDExtensionPropertyName.FresnelScale).AsDouble(); set => Set(GDExtensionPropertyName.FresnelScale, value); }
    public double FresnelPower { get => Get(GDExtensionPropertyName.FresnelPower).AsDouble(); set => Set(GDExtensionPropertyName.FresnelPower, value); }
    public double Specular { get => Get(GDExtensionPropertyName.Specular).AsDouble(); set => Set(GDExtensionPropertyName.Specular, value); }
    public double FoamHeight { get => Get(GDExtensionPropertyName.FoamHeight).AsDouble(); set => Set(GDExtensionPropertyName.FoamHeight, value); }
    public double FoamTopIntensity { get => Get(GDExtensionPropertyName.FoamTopIntensity).AsDouble(); set => Set(GDExtensionPropertyName.FoamTopIntensity, value); }
    public double FoamDistance { get => Get(GDExtensionPropertyName.FoamDistance).AsDouble(); set => Set(GDExtensionPropertyName.FoamDistance, value); }
    public double FoamShoreIntensity { get => Get(GDExtensionPropertyName.FoamShoreIntensity).AsDouble(); set => Set(GDExtensionPropertyName.FoamShoreIntensity, value); }
    public double FoamNoiseScale { get => Get(GDExtensionPropertyName.FoamNoiseScale).AsDouble(); set => Set(GDExtensionPropertyName.FoamNoiseScale, value); }
    public double HeightScattering { get => Get(GDExtensionPropertyName.HeightScattering).AsDouble(); set => Set(GDExtensionPropertyName.HeightScattering, value); }
    public double HeightThreshold { get => Get(GDExtensionPropertyName.HeightThreshold).AsDouble(); set => Set(GDExtensionPropertyName.HeightThreshold, value); }
    public double HeightAngleThreshold { get => Get(GDExtensionPropertyName.HeightAngleThreshold).AsDouble(); set => Set(GDExtensionPropertyName.HeightAngleThreshold, value); }
    public double FresnelScattering { get => Get(GDExtensionPropertyName.FresnelScattering).AsDouble(); set => Set(GDExtensionPropertyName.FresnelScattering, value); }
    public double DirectScattering { get => Get(GDExtensionPropertyName.DirectScattering).AsDouble(); set => Set(GDExtensionPropertyName.DirectScattering, value); }
}
