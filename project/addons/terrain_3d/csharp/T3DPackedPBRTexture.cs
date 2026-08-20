#pragma warning disable CS0109
using System;
using System.Diagnostics;
using System.Linq;
using System.Reflection;
using Godot;

namespace TokisanGames;

[Tool]
public partial class T3DPackedPBRTexture : Resource
{
	private new static readonly StringName NativeName = new("T3DPackedPBRTexture");

	[Obsolete("Wrapper types cannot be constructed directly; use Instantiate().")]
	protected T3DPackedPBRTexture() { }

	private static CSharpScript _wrapperScriptAsset;

	public new static T3DPackedPBRTexture Bind(GodotObject godotObject)
	{
		if (!IsInstanceValid(godotObject)) return null;
		if (godotObject is T3DPackedPBRTexture wrapper) return wrapper;
#if DEBUG
		var currentObjectClassName = godotObject.GetClass();
		if (!ClassDB.IsParentClass(nameof(T3DPackedPBRTexture), currentObjectClassName))
			throw new InvalidOperationException($"The supplied object ({currentObjectClassName}) is not T3DPackedPBRTexture.");
#endif
		if (_wrapperScriptAsset is null)
		{
			var path = typeof(T3DPackedPBRTexture).GetCustomAttributes<ScriptPathAttribute>().FirstOrDefault();
			if (path is null) throw new UnreachableException();
			_wrapperScriptAsset = ResourceLoader.Load<CSharpScript>(path.Path);
		}
		var instanceId = godotObject.GetInstanceId();
		godotObject.SetScript(_wrapperScriptAsset);
		return (T3DPackedPBRTexture)InstanceFromId(instanceId);
	}

	public new static T3DPackedPBRTexture Instantiate() => Bind(ClassDB.Instantiate(NativeName).As<GodotObject>());

	public enum SourceChannel : long { Red = 0, Green = 1, Blue = 2, Alpha = 3 }

	public new class GDExtensionPropertyName : Resource.PropertyName
	{
		public new static readonly StringName PackedTexture = "packed_texture";
		public new static readonly StringName DisplacementTexture = "displacement_texture";

		[Obsolete("Legacy serialized migration field. Use PackedTexture.")]
		public new static readonly StringName NormalSource = "normal_source";
		[Obsolete("Legacy serialized migration field. Use PackedTexture.")]
		public new static readonly StringName RoughnessSource = "roughness_source";
		[Obsolete("Legacy serialized migration field. Use DisplacementTexture.")]
		public new static readonly StringName DisplacementSource = "displacement_source";
		[Obsolete("Legacy serialized migration field. Use PackedTexture.")]
		public new static readonly StringName AoSource = "ao_source";
		[Obsolete("Legacy serialized migration field. Metallic is a T3DTextureAsset scalar.")]
		public new static readonly StringName MetallicSource = "metallic_source";
		[Obsolete("Legacy serialized migration field.")]
		public new static readonly StringName OutputSize = "output_size";
		[Obsolete("Legacy serialized migration field.")]
		public new static readonly StringName InvertNormalGreen = "invert_normal_green";
		[Obsolete("Legacy serialized migration field.")]
		public new static readonly StringName RoughnessChannel = "roughness_channel";
		[Obsolete("Legacy serialized migration field.")]
		public new static readonly StringName DisplacementChannel = "displacement_channel";
		[Obsolete("Legacy serialized migration field.")]
		public new static readonly StringName AoChannel = "ao_channel";
		[Obsolete("Legacy serialized migration field.")]
		public new static readonly StringName MetallicChannel = "metallic_channel";
		[Obsolete("Legacy serialized migration field.")]
		public new static readonly StringName GenerateMipmaps = "generate_mipmaps";
		[Obsolete("Legacy serialized migration field.")]
		public new static readonly StringName AutoRebuild = "auto_rebuild";
	}

	public Texture2D PackedTexture { get => Get(GDExtensionPropertyName.PackedTexture).As<Texture2D>(); set => Set(GDExtensionPropertyName.PackedTexture, value); }
	public Texture2D DisplacementTexture { get => Get(GDExtensionPropertyName.DisplacementTexture).As<Texture2D>(); set => Set(GDExtensionPropertyName.DisplacementTexture, value); }

	[Obsolete("Legacy migration property. Use PackedTexture.")]
	public Texture2D NormalSource { get => Get(GDExtensionPropertyName.NormalSource).As<Texture2D>(); set => Set(GDExtensionPropertyName.NormalSource, value); }
	[Obsolete("Legacy migration property. Use PackedTexture.")]
	public Texture2D RoughnessSource { get => Get(GDExtensionPropertyName.RoughnessSource).As<Texture2D>(); set => Set(GDExtensionPropertyName.RoughnessSource, value); }
	[Obsolete("Legacy migration property. Use DisplacementTexture.")]
	public Texture2D DisplacementSource { get => Get(GDExtensionPropertyName.DisplacementSource).As<Texture2D>(); set => Set(GDExtensionPropertyName.DisplacementSource, value); }
	[Obsolete("Legacy migration property. Use PackedTexture.")]
	public Texture2D AoSource { get => Get(GDExtensionPropertyName.AoSource).As<Texture2D>(); set => Set(GDExtensionPropertyName.AoSource, value); }
	[Obsolete("Legacy migration property. Metallic is a T3DTextureAsset scalar.")]
	public Texture2D MetallicSource { get => Get(GDExtensionPropertyName.MetallicSource).As<Texture2D>(); set => Set(GDExtensionPropertyName.MetallicSource, value); }
	[Obsolete("Legacy migration property.")]
	public Vector2I OutputSize { get => Get(GDExtensionPropertyName.OutputSize).As<Vector2I>(); set => Set(GDExtensionPropertyName.OutputSize, value); }
	[Obsolete("Legacy migration property.")]
	public bool InvertNormalGreen { get => Get(GDExtensionPropertyName.InvertNormalGreen).As<bool>(); set => Set(GDExtensionPropertyName.InvertNormalGreen, value); }
	[Obsolete("Legacy migration property.")]
	public SourceChannel RoughnessChannel { get => (SourceChannel)Get(GDExtensionPropertyName.RoughnessChannel).As<long>(); set => Set(GDExtensionPropertyName.RoughnessChannel, (long)value); }
	[Obsolete("Legacy migration property.")]
	public SourceChannel DisplacementChannel { get => (SourceChannel)Get(GDExtensionPropertyName.DisplacementChannel).As<long>(); set => Set(GDExtensionPropertyName.DisplacementChannel, (long)value); }
	[Obsolete("Legacy migration property.")]
	public SourceChannel AoChannel { get => (SourceChannel)Get(GDExtensionPropertyName.AoChannel).As<long>(); set => Set(GDExtensionPropertyName.AoChannel, (long)value); }
	[Obsolete("Legacy migration property.")]
	public SourceChannel MetallicChannel { get => (SourceChannel)Get(GDExtensionPropertyName.MetallicChannel).As<long>(); set => Set(GDExtensionPropertyName.MetallicChannel, (long)value); }
	[Obsolete("Legacy migration property.")]
	public bool GenerateMipmaps { get => Get(GDExtensionPropertyName.GenerateMipmaps).As<bool>(); set => Set(GDExtensionPropertyName.GenerateMipmaps, value); }
	[Obsolete("Legacy migration property.")]
	public bool AutoRebuild { get => Get(GDExtensionPropertyName.AutoRebuild).As<bool>(); set => Set(GDExtensionPropertyName.AutoRebuild, value); }

	public new class GDExtensionMethodName : Resource.MethodName
	{
		public new static readonly StringName Rebuild = "rebuild";
		public new static readonly StringName IsDirty = "is_dirty";
		public new static readonly StringName HasRequiredSources = "has_required_sources";
		public new static readonly StringName IsValidSurfaceTexture = "is_valid_surface_texture";
		public new static readonly StringName IsValidDisplacementTexture = "is_valid_displacement_texture";
		public new static readonly StringName IsValidPackedTexture = "is_valid_packed_texture";
		public new static readonly StringName GetLastError = "get_last_error";
		public new static readonly StringName GetSurfaceTexture = "get_surface_texture";
		public new static readonly StringName GetDisplacementTexture = "get_displacement_texture";
		public new static readonly StringName GetTexture = "get_texture";
		public new static readonly StringName GetGeneratedSurfaceTexture = "get_generated_surface_texture";
		public new static readonly StringName GetGeneratedDisplacementTexture = "get_generated_displacement_texture";
		public new static readonly StringName GetGeneratedTexture = "get_generated_texture";
		public new static readonly StringName SaveDisplacementDds = "save_displacement_dds";
		public new static readonly StringName GetChannelLayout = "get_channel_layout";
	}

	public bool Rebuild() => Call(GDExtensionMethodName.Rebuild, []).As<bool>();
	public bool IsDirty() => Call(GDExtensionMethodName.IsDirty, []).As<bool>();
	public bool HasRequiredSources() => Call(GDExtensionMethodName.HasRequiredSources, []).As<bool>();
	public bool IsValidSurfaceTexture() => Call(GDExtensionMethodName.IsValidSurfaceTexture, []).As<bool>();
	public bool IsValidDisplacementTexture() => Call(GDExtensionMethodName.IsValidDisplacementTexture, []).As<bool>();
	[Obsolete("Use IsValidSurfaceTexture and IsValidDisplacementTexture.")]
	public bool IsValidPackedTexture() => Call(GDExtensionMethodName.IsValidPackedTexture, []).As<bool>();
	public string GetLastError() => Call(GDExtensionMethodName.GetLastError, []).As<string>();
	public Texture2D GetSurfaceTexture() => Call(GDExtensionMethodName.GetSurfaceTexture, []).As<Texture2D>();
	public Texture2D GetDisplacementTexture() => Call(GDExtensionMethodName.GetDisplacementTexture, []).As<Texture2D>();
	[Obsolete("Use GetSurfaceTexture.")]
	public Texture2D GetTexture() => Call(GDExtensionMethodName.GetTexture, []).As<Texture2D>();
	public Texture2D GetGeneratedSurfaceTexture() => Call(GDExtensionMethodName.GetGeneratedSurfaceTexture, []).As<Texture2D>();
	public Texture2D GetGeneratedDisplacementTexture() => Call(GDExtensionMethodName.GetGeneratedDisplacementTexture, []).As<Texture2D>();
	[Obsolete("Use GetGeneratedSurfaceTexture.")]
	public Texture2D GetGeneratedTexture() => Call(GDExtensionMethodName.GetGeneratedTexture, []).As<Texture2D>();
	public Error SaveDisplacementDds(string path) => Call(GDExtensionMethodName.SaveDisplacementDds, [path]).As<Error>();
	public string GetChannelLayout() => Call(GDExtensionMethodName.GetChannelLayout, []).As<string>();
}
