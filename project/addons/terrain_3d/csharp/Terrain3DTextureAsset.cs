#pragma warning disable CS0109
using System;
using System.Diagnostics;
using System.Linq;
using System.Reflection;
using Godot;
using Godot.Collections;

namespace TokisanGames;

[Tool]
public partial class Terrain3DTextureAsset : Resource
{

	public new class GDExtensionSignalName : Resource.SignalName
	{
		/// <summary>
		/// Cached name for the 'id_changed' member.
		/// </summary>
		public new static readonly StringName IdChanged = "id_changed";
		/// <summary>
		/// Cached name for the 'file_changed' member.
		/// </summary>
		public new static readonly StringName FileChanged = "file_changed";
		/// <summary>
		/// Cached name for the 'setting_changed' member.
		/// </summary>
		public new static readonly StringName SettingChanged = "setting_changed";
	}

	public new delegate void IdChangedSignalHandler();
	private IdChangedSignalHandler _idChangedSignal;
	private Callable _idChangedSignalCallable;
	public event IdChangedSignalHandler IdChangedSignal
	{
		add
		{
			if (_idChangedSignal is null)
			{
				_idChangedSignalCallable = Callable.From(() =>
					_idChangedSignal?.Invoke());
				Connect(GDExtensionSignalName.IdChanged, _idChangedSignalCallable);
			}
			_idChangedSignal += value;
		}
		remove
		{
			_idChangedSignal -= value;
			if (_idChangedSignal is not null) return;
			Disconnect(GDExtensionSignalName.IdChanged, _idChangedSignalCallable);
			_idChangedSignalCallable = default;
		}
	}

	public new delegate void FileChangedSignalHandler();
	private FileChangedSignalHandler _fileChangedSignal;
	private Callable _fileChangedSignalCallable;
	public event FileChangedSignalHandler FileChangedSignal
	{
		add
		{
			if (_fileChangedSignal is null)
			{
				_fileChangedSignalCallable = Callable.From(() =>
					_fileChangedSignal?.Invoke());
				Connect(GDExtensionSignalName.FileChanged, _fileChangedSignalCallable);
			}
			_fileChangedSignal += value;
		}
		remove
		{
			_fileChangedSignal -= value;
			if (_fileChangedSignal is not null) return;
			Disconnect(GDExtensionSignalName.FileChanged, _fileChangedSignalCallable);
			_fileChangedSignalCallable = default;
		}
	}

	public new delegate void SettingChangedSignalHandler();
	private SettingChangedSignalHandler _settingChangedSignal;
	private Callable _settingChangedSignalCallable;
	public event SettingChangedSignalHandler SettingChangedSignal
	{
		add
		{
			if (_settingChangedSignal is null)
			{
				_settingChangedSignalCallable = Callable.From(() =>
					_settingChangedSignal?.Invoke());
				Connect(GDExtensionSignalName.SettingChanged, _settingChangedSignalCallable);
			}
			_settingChangedSignal += value;
		}
		remove
		{
			_settingChangedSignal -= value;
			if (_settingChangedSignal is not null) return;
			Disconnect(GDExtensionSignalName.SettingChanged, _settingChangedSignalCallable);
			_settingChangedSignalCallable = default;
		}
	}

	public new class GDExtensionPropertyName : Resource.PropertyName
	{
		/// <summary>
		/// Cached name for the 'name' member.
		/// </summary>
		public new static readonly StringName Name = "name";
		/// <summary>
		/// Cached name for the 'id' member.
		/// </summary>
		public new static readonly StringName Id = "id";
		/// <summary>
		/// Cached name for the 'albedo_color' member.
		/// </summary>
		public new static readonly StringName AlbedoColor = "albedo_color";
		/// <summary>
		/// Cached name for the 'albedo_texture' member.
		/// </summary>
		public new static readonly StringName AlbedoTexture = "albedo_texture";
		/// <summary>
		/// Cached name for the 'normal_texture' member.
		/// </summary>
		public new static readonly StringName NormalTexture = "normal_texture";
		/// <summary>
		/// Cached name for the externally authored 'packed_pbr_texture' member.
		/// </summary>
		public new static readonly StringName PackedPbrTexture = "packed_pbr_texture";
		[Obsolete("Use PackedPbrTexture.")]
		public new static readonly StringName SurfaceTexture = "surface_texture";
		public new static readonly StringName DisplacementTexture = "displacement_texture";
		public new static readonly StringName PbrTexture = "pbr_texture";
		/// <summary>Legacy serialized packed-PBR compatibility resource.</summary>
		public new static readonly StringName PackedPbrResource = "packed_pbr_resource";
		/// <summary>
		/// Cached name for the 'normal_depth' member.
		/// </summary>
		public new static readonly StringName NormalDepth = "normal_depth";
		/// <summary>
		/// Cached name for the 'ao_strength' member.
		/// </summary>
		public new static readonly StringName AoStrength = "ao_strength";
		/// <summary>
		/// Cached name for the 'ao_light_affect' member.
		/// </summary>
		public new static readonly StringName AoLightAffect = "ao_light_affect";
		/// <summary>
		/// Cached name for the 'roughness' member.
		/// </summary>
		public new static readonly StringName Roughness = "roughness";
		public new static readonly StringName Metallic = "metallic";
		/// <summary>
		/// Cached name for the 'displacement_scale' member.
		/// </summary>
		public new static readonly StringName DisplacementScale = "displacement_scale";
		/// <summary>
		/// Cached name for the 'displacement_offset' member.
		/// </summary>
		public new static readonly StringName DisplacementOffset = "displacement_offset";
		/// <summary>
		/// Cached name for the 'uv_scale' member.
		/// </summary>
		public new static readonly StringName UvScale = "uv_scale";
		public new static readonly StringName ErosionEnabled = "erosion_enabled";
		public new static readonly StringName ErosionHardness = "erosion_hardness";
		public new static readonly StringName ErosionHydraulicErodibility = "erosion_hydraulic_erodibility";
		public new static readonly StringName ErosionWindErodibility = "erosion_wind_erodibility";
		public new static readonly StringName ErosionWaveErodibility = "erosion_wave_erodibility";
		public new static readonly StringName ErosionWeatheringSusceptibility = "erosion_weathering_susceptibility";
		public new static readonly StringName ErosionOxidationSusceptibility = "erosion_oxidation_susceptibility";
		public new static readonly StringName ErosionSaltSusceptibility = "erosion_salt_susceptibility";
		public new static readonly StringName ErosionWetDrySusceptibility = "erosion_wet_dry_susceptibility";
		public new static readonly StringName ErosionInfiltrationMultiplier = "erosion_infiltration_multiplier";
		public new static readonly StringName ErosionTrafficErodibility = "erosion_traffic_erodibility";
		public new static readonly StringName ErosionCohesion = "erosion_cohesion";
		public new static readonly StringName ErosionGrainSize = "erosion_grain_size";
		public new static readonly StringName ErosionExposedTextureId = "erosion_exposed_texture_id";
		public new static readonly StringName ErosionWeatheredTextureId = "erosion_weathered_texture_id";
		public new static readonly StringName ErosionOxidizedTextureId = "erosion_oxidized_texture_id";
		public new static readonly StringName ErosionCoarseDepositTextureId = "erosion_coarse_deposit_texture_id";
		public new static readonly StringName ErosionFineDepositTextureId = "erosion_fine_deposit_texture_id";
		public new static readonly StringName ErosionSaltWeatheredTextureId = "erosion_salt_weathered_texture_id";
		public new static readonly StringName ErosionPavementTextureId = "erosion_pavement_texture_id";
		public new static readonly StringName ErosionVarnishedTextureId = "erosion_varnished_texture_id";
		/// <summary>
		/// Cached name for the 'detiling_rotation' member.
		/// </summary>
		public new static readonly StringName DetilingRotation = "detiling_rotation";
		/// <summary>
		/// Cached name for the 'detiling_shift' member.
		/// </summary>
		public new static readonly StringName DetilingShift = "detiling_shift";
	}

	public new string Name
	{
		get => Get(GDExtensionPropertyName.Name).As<string>();
		set => Set(GDExtensionPropertyName.Name, value);
	}

	public new long Id
	{
		get => Get(GDExtensionPropertyName.Id).As<long>();
		set => Set(GDExtensionPropertyName.Id, value);
	}

	public new Color AlbedoColor
	{
		get => Get(GDExtensionPropertyName.AlbedoColor).As<Color>();
		set => Set(GDExtensionPropertyName.AlbedoColor, value);
	}

	public new Texture2D AlbedoTexture
	{
		get => Get(GDExtensionPropertyName.AlbedoTexture).As<Texture2D>();
		set => Set(GDExtensionPropertyName.AlbedoTexture, value);
	}

	public new Texture2D NormalTexture
	{
		get => Get(GDExtensionPropertyName.NormalTexture).As<Texture2D>();
		set => Set(GDExtensionPropertyName.NormalTexture, value);
	}

	public new Texture2D PackedPbrTexture
	{
		get => Get(GDExtensionPropertyName.PackedPbrTexture).As<Texture2D>();
		set => Set(GDExtensionPropertyName.PackedPbrTexture, value);
	}

	[Obsolete("Use PackedPbrTexture.")]
	public new Texture2D SurfaceTexture
	{
		get => Get(GDExtensionPropertyName.SurfaceTexture).As<Texture2D>();
		set => Set(GDExtensionPropertyName.SurfaceTexture, value);
	}

	public new Texture2D DisplacementTexture
	{
		get => Get(GDExtensionPropertyName.DisplacementTexture).As<Texture2D>();
		set => Set(GDExtensionPropertyName.DisplacementTexture, value);
	}

	[Obsolete("Use PackedPbrTexture and DisplacementTexture.")]
	public new Texture2D PbrTexture
	{
		get => Get(GDExtensionPropertyName.PbrTexture).As<Texture2D>();
		set => Set(GDExtensionPropertyName.PbrTexture, value);
	}

	[Obsolete("Use PackedPbrTexture and DisplacementTexture. This resource remains only for serialized compatibility.")]
	public new Terrain3DPackedPBRTexture PackedPbrResource
	{
		get => Terrain3DPackedPBRTexture.Bind(Get(GDExtensionPropertyName.PackedPbrResource).As<GodotObject>());
		set => Set(GDExtensionPropertyName.PackedPbrResource, value);
	}

	public new double NormalDepth
	{
		get => Get(GDExtensionPropertyName.NormalDepth).As<double>();
		set => Set(GDExtensionPropertyName.NormalDepth, value);
	}

	public new double AoStrength
	{
		get => Get(GDExtensionPropertyName.AoStrength).As<double>();
		set => Set(GDExtensionPropertyName.AoStrength, value);
	}

	public new double AoLightAffect
	{
		get => Get(GDExtensionPropertyName.AoLightAffect).As<double>();
		set => Set(GDExtensionPropertyName.AoLightAffect, value);
	}

	public new double Roughness
	{
		get => Get(GDExtensionPropertyName.Roughness).As<double>();
		set => Set(GDExtensionPropertyName.Roughness, value);
	}

	public new double Metallic
	{
		get => Get(GDExtensionPropertyName.Metallic).As<double>();
		set => Set(GDExtensionPropertyName.Metallic, value);
	}

	public new double DisplacementScale
	{
		get => Get(GDExtensionPropertyName.DisplacementScale).As<double>();
		set => Set(GDExtensionPropertyName.DisplacementScale, value);
	}

	public new double DisplacementOffset
	{
		get => Get(GDExtensionPropertyName.DisplacementOffset).As<double>();
		set => Set(GDExtensionPropertyName.DisplacementOffset, value);
	}

	public new double UvScale
	{
		get => Get(GDExtensionPropertyName.UvScale).As<double>();
		set => Set(GDExtensionPropertyName.UvScale, value);
	}

	public new bool ErosionEnabled
	{
		get => Get(GDExtensionPropertyName.ErosionEnabled).As<bool>();
		set => Set(GDExtensionPropertyName.ErosionEnabled, value);
	}

	public new double ErosionHardness
	{
		get => Get(GDExtensionPropertyName.ErosionHardness).As<double>();
		set => Set(GDExtensionPropertyName.ErosionHardness, value);
	}

	public new double ErosionHydraulicErodibility
	{
		get => Get(GDExtensionPropertyName.ErosionHydraulicErodibility).As<double>();
		set => Set(GDExtensionPropertyName.ErosionHydraulicErodibility, value);
	}

	public new double ErosionWindErodibility
	{
		get => Get(GDExtensionPropertyName.ErosionWindErodibility).As<double>();
		set => Set(GDExtensionPropertyName.ErosionWindErodibility, value);
	}

	public new double ErosionWaveErodibility
	{
		get => Get(GDExtensionPropertyName.ErosionWaveErodibility).As<double>();
		set => Set(GDExtensionPropertyName.ErosionWaveErodibility, value);
	}

	public new double ErosionWeatheringSusceptibility
	{
		get => Get(GDExtensionPropertyName.ErosionWeatheringSusceptibility).As<double>();
		set => Set(GDExtensionPropertyName.ErosionWeatheringSusceptibility, value);
	}

	public new double ErosionOxidationSusceptibility
	{
		get => Get(GDExtensionPropertyName.ErosionOxidationSusceptibility).As<double>();
		set => Set(GDExtensionPropertyName.ErosionOxidationSusceptibility, value);
	}

	public new double ErosionSaltSusceptibility
	{
		get => Get(GDExtensionPropertyName.ErosionSaltSusceptibility).As<double>();
		set => Set(GDExtensionPropertyName.ErosionSaltSusceptibility, value);
	}

	public new double ErosionWetDrySusceptibility
	{
		get => Get(GDExtensionPropertyName.ErosionWetDrySusceptibility).As<double>();
		set => Set(GDExtensionPropertyName.ErosionWetDrySusceptibility, value);
	}

	public new double ErosionInfiltrationMultiplier
	{
		get => Get(GDExtensionPropertyName.ErosionInfiltrationMultiplier).As<double>();
		set => Set(GDExtensionPropertyName.ErosionInfiltrationMultiplier, value);
	}

	public new double ErosionTrafficErodibility
	{
		get => Get(GDExtensionPropertyName.ErosionTrafficErodibility).As<double>();
		set => Set(GDExtensionPropertyName.ErosionTrafficErodibility, value);
	}

	public new double ErosionCohesion
	{
		get => Get(GDExtensionPropertyName.ErosionCohesion).As<double>();
		set => Set(GDExtensionPropertyName.ErosionCohesion, value);
	}

	public new double ErosionGrainSize
	{
		get => Get(GDExtensionPropertyName.ErosionGrainSize).As<double>();
		set => Set(GDExtensionPropertyName.ErosionGrainSize, value);
	}

	public new long ErosionExposedTextureId
	{
		get => Get(GDExtensionPropertyName.ErosionExposedTextureId).As<long>();
		set => Set(GDExtensionPropertyName.ErosionExposedTextureId, value);
	}

	public new long ErosionWeatheredTextureId
	{
		get => Get(GDExtensionPropertyName.ErosionWeatheredTextureId).As<long>();
		set => Set(GDExtensionPropertyName.ErosionWeatheredTextureId, value);
	}

	public new long ErosionOxidizedTextureId
	{
		get => Get(GDExtensionPropertyName.ErosionOxidizedTextureId).As<long>();
		set => Set(GDExtensionPropertyName.ErosionOxidizedTextureId, value);
	}

	public new long ErosionCoarseDepositTextureId
	{
		get => Get(GDExtensionPropertyName.ErosionCoarseDepositTextureId).As<long>();
		set => Set(GDExtensionPropertyName.ErosionCoarseDepositTextureId, value);
	}

	public new long ErosionFineDepositTextureId
	{
		get => Get(GDExtensionPropertyName.ErosionFineDepositTextureId).As<long>();
		set => Set(GDExtensionPropertyName.ErosionFineDepositTextureId, value);
	}

	public new long ErosionSaltWeatheredTextureId
	{
		get => Get(GDExtensionPropertyName.ErosionSaltWeatheredTextureId).As<long>();
		set => Set(GDExtensionPropertyName.ErosionSaltWeatheredTextureId, value);
	}

	public new long ErosionPavementTextureId
	{
		get => Get(GDExtensionPropertyName.ErosionPavementTextureId).As<long>();
		set => Set(GDExtensionPropertyName.ErosionPavementTextureId, value);
	}

	public new long ErosionVarnishedTextureId
	{
		get => Get(GDExtensionPropertyName.ErosionVarnishedTextureId).As<long>();
		set => Set(GDExtensionPropertyName.ErosionVarnishedTextureId, value);
	}

	public new double DetilingRotation
	{
		get => Get(GDExtensionPropertyName.DetilingRotation).As<double>();
		set => Set(GDExtensionPropertyName.DetilingRotation, value);
	}

	public new double DetilingShift
	{
		get => Get(GDExtensionPropertyName.DetilingShift).As<double>();
		set => Set(GDExtensionPropertyName.DetilingShift, value);
	}

	public new class GDExtensionMethodName : Resource.MethodName
	{
		/// <summary>
		/// Cached name for the 'clear' member.
		/// </summary>
		public new static readonly StringName Clear = "clear";
		/// <summary>
		/// Cached name for the 'set_highlighted' member.
		/// </summary>
		public new static readonly StringName SetHighlighted = "set_highlighted";
		/// <summary>
		/// Cached name for the 'is_highlighted' member.
		/// </summary>
		public new static readonly StringName IsHighlighted = "is_highlighted";
		/// <summary>
		/// Cached name for the 'get_highlight_color' member.
		/// </summary>
		public new static readonly StringName GetHighlightColor = "get_highlight_color";
		/// <summary>
		/// Cached name for the 'get_thumbnail' member.
		/// </summary>
		public new static readonly StringName GetThumbnail = "get_thumbnail";
		public new static readonly StringName GetEffectiveSurfaceTexture = "get_effective_surface_texture";
		public new static readonly StringName GetPackedPbrTexture = "get_packed_pbr_texture";
		public new static readonly StringName GetEffectiveDisplacementTexture = "get_effective_displacement_texture";
	}

	public new void Clear() =>
		Call(GDExtensionMethodName.Clear, []);

	public new void SetHighlighted(bool enabled) =>
		Call(GDExtensionMethodName.SetHighlighted, [enabled]);

	public new bool IsHighlighted() =>
		Call(GDExtensionMethodName.IsHighlighted, []).As<bool>();

	public new Color GetHighlightColor() =>
		Call(GDExtensionMethodName.GetHighlightColor, []).As<Color>();

	public new Texture2D GetThumbnail() =>
		Call(GDExtensionMethodName.GetThumbnail, []).As<Texture2D>();

	public new Texture2D GetEffectiveSurfaceTexture() =>
		Call(GDExtensionMethodName.GetEffectiveSurfaceTexture, []).As<Texture2D>();

	public new Texture2D GetEffectiveDisplacementTexture() =>
		Call(GDExtensionMethodName.GetEffectiveDisplacementTexture, []).As<Texture2D>();

}
