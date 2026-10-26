#pragma warning disable CS0109
using System;
using System.Diagnostics;
using System.Linq;
using System.Reflection;
using Godot;
using Godot.Collections;

namespace TokisanGames;

[Tool]
public partial class Terrain3DAssets : Resource
{

	public enum AssetType
	{
		Texture = 0,
		Mesh = 1,
	}

	public enum TexturePackingMode
	{
		Legacy = 0,
		PackedPbrR8 = 1,
		[Obsolete("Use PackedPbrR8.")]
		SurfaceR16 = PackedPbrR8,
		[Obsolete("Use PackedPbrR8.")]
		PbrRgba16Ui = PackedPbrR8,
	}

	public new class GDExtensionSignalName : Resource.SignalName
	{
		/// <summary>
		/// Cached name for the 'meshes_changed' member.
		/// </summary>
		public new static readonly StringName MeshesChanged = "meshes_changed";
		/// <summary>
		/// Cached name for the 'textures_changed' member.
		/// </summary>
		public new static readonly StringName TexturesChanged = "textures_changed";
	}

	public new delegate void MeshesChangedSignalHandler();
	private MeshesChangedSignalHandler _meshesChangedSignal;
	private Callable _meshesChangedSignalCallable;
	public event MeshesChangedSignalHandler MeshesChangedSignal
	{
		add
		{
			if (_meshesChangedSignal is null)
			{
				_meshesChangedSignalCallable = Callable.From(() =>
					_meshesChangedSignal?.Invoke());
				Connect(GDExtensionSignalName.MeshesChanged, _meshesChangedSignalCallable);
			}
			_meshesChangedSignal += value;
		}
		remove
		{
			_meshesChangedSignal -= value;
			if (_meshesChangedSignal is not null) return;
			Disconnect(GDExtensionSignalName.MeshesChanged, _meshesChangedSignalCallable);
			_meshesChangedSignalCallable = default;
		}
	}

	public new delegate void TexturesChangedSignalHandler();
	private TexturesChangedSignalHandler _texturesChangedSignal;
	private Callable _texturesChangedSignalCallable;
	public event TexturesChangedSignalHandler TexturesChangedSignal
	{
		add
		{
			if (_texturesChangedSignal is null)
			{
				_texturesChangedSignalCallable = Callable.From(() =>
					_texturesChangedSignal?.Invoke());
				Connect(GDExtensionSignalName.TexturesChanged, _texturesChangedSignalCallable);
			}
			_texturesChangedSignal += value;
		}
		remove
		{
			_texturesChangedSignal -= value;
			if (_texturesChangedSignal is not null) return;
			Disconnect(GDExtensionSignalName.TexturesChanged, _texturesChangedSignalCallable);
			_texturesChangedSignalCallable = default;
		}
	}

	public new class GDExtensionPropertyName : Resource.PropertyName
	{
		/// <summary>
		/// Cached name for the 'mesh_list' member.
		/// </summary>
		public new static readonly StringName MeshList = "mesh_list";
		/// <summary>
		/// Cached name for the 'texture_list' member.
		/// </summary>
		public new static readonly StringName TextureList = "texture_list";
		/// <summary>
		/// Cached name for the 'texture_packing_mode' member.
		/// </summary>
		public new static readonly StringName TexturePackingModeValue = "texture_packing_mode";
	}

	public new Godot.Collections.Array MeshList
	{
		get => Get(GDExtensionPropertyName.MeshList).As<Godot.Collections.Array>();
		set => Set(GDExtensionPropertyName.MeshList, value);
	}

	public new Godot.Collections.Array TextureList
	{
		get => Get(GDExtensionPropertyName.TextureList).As<Godot.Collections.Array>();
		set => Set(GDExtensionPropertyName.TextureList, value);
	}

	public new TexturePackingMode TexturePackingModeValue
	{
		get => (TexturePackingMode)Get(GDExtensionPropertyName.TexturePackingModeValue).As<long>();
		set => Set(GDExtensionPropertyName.TexturePackingModeValue, (long)value);
	}

	public new class GDExtensionMethodName : Resource.MethodName
	{
		/// <summary>
		/// Cached name for the 'set_texture_asset' member.
		/// </summary>
		public new static readonly StringName SetTextureAsset = "set_texture_asset";
		/// <summary>
		/// Cached name for the 'get_texture_asset' member.
		/// </summary>
		public new static readonly StringName GetTextureAsset = "get_texture_asset";
		/// <summary>
		/// Cached name for the 'get_texture_count' member.
		/// </summary>
		public new static readonly StringName GetTextureCount = "get_texture_count";
		/// <summary>
		/// Cached name for the 'get_albedo_array_rid' member.
		/// </summary>
		public new static readonly StringName GetAlbedoArrayRid = "get_albedo_array_rid";
		/// <summary>
		/// Cached name for the 'get_normal_array_rid' member.
		/// </summary>
		public new static readonly StringName GetNormalArrayRid = "get_normal_array_rid";
		/// <summary>
		/// Cached name for the 'get_pbr_array_rid' member.
		/// </summary>
		public new static readonly StringName GetSurfaceArrayRid = "get_surface_array_rid";
		public new static readonly StringName GetDisplacementArrayRid = "get_displacement_array_rid";
		public new static readonly StringName GetPbrArrayRid = "get_pbr_array_rid";
		/// <summary>
		/// Cached name for the 'set_texture_packing_mode' member.
		/// </summary>
		public new static readonly StringName SetTexturePackingMode = "set_texture_packing_mode";
		/// <summary>
		/// Cached name for the 'get_texture_packing_mode' member.
		/// </summary>
		public new static readonly StringName GetTexturePackingMode = "get_texture_packing_mode";
		/// <summary>
		/// Cached name for the 'get_packed_pbr_mipmap_count' member.
		/// </summary>
		public new static readonly StringName GetPackedPbrMipmapCount = "get_packed_pbr_mipmap_count";
		/// <summary>
		/// Cached name for the 'get_texture_colors' member.
		/// </summary>
		public new static readonly StringName GetTextureColors = "get_texture_colors";
		/// <summary>
		/// Cached name for the 'get_texture_normal_depths' member.
		/// </summary>
		public new static readonly StringName GetTextureNormalDepths = "get_texture_normal_depths";
		/// <summary>
		/// Cached name for the 'get_texture_ao_strengths' member.
		/// </summary>
		public new static readonly StringName GetTextureAoStrengths = "get_texture_ao_strengths";
		/// <summary>
		/// Cached name for the 'get_texture_ao_light_affects' member.
		/// </summary>
		public new static readonly StringName GetTextureAoLightAffects = "get_texture_ao_light_affects";
		/// <summary>
		/// Cached name for the 'get_texture_roughness_mods' member.
		/// </summary>
		public new static readonly StringName GetTextureRoughnessMods = "get_texture_roughness_mods";
		public new static readonly StringName GetTextureMetallics = "get_texture_metallics";
		/// <summary>
		/// Cached name for the 'get_texture_uv_scales' member.
		/// </summary>
		public new static readonly StringName GetTextureUvScales = "get_texture_uv_scales";
		/// <summary>
		/// Cached name for the 'get_texture_detiles' member.
		/// </summary>
		public new static readonly StringName GetTextureDetiles = "get_texture_detiles";
		/// <summary>
		/// Cached name for the 'get_texture_displacements' member.
		/// </summary>
		public new static readonly StringName GetTextureDisplacements = "get_texture_displacements";
		/// <summary>
		/// Cached name for the 'clear_textures' member.
		/// </summary>
		public new static readonly StringName ClearTextures = "clear_textures";
		/// <summary>
		/// Cached name for the 'update_texture_list' member.
		/// </summary>
		public new static readonly StringName UpdateTextureList = "update_texture_list";
		/// <summary>
		/// Cached name for the 'set_mesh_asset' member.
		/// </summary>
		public new static readonly StringName SetMeshAsset = "set_mesh_asset";
		/// <summary>
		/// Cached name for the 'get_mesh_asset' member.
		/// </summary>
		public new static readonly StringName GetMeshAsset = "get_mesh_asset";
		/// <summary>
		/// Cached name for the 'get_mesh_count' member.
		/// </summary>
		public new static readonly StringName GetMeshCount = "get_mesh_count";
		/// <summary>
		/// Cached name for the 'create_mesh_thumbnails' member.
		/// </summary>
		public new static readonly StringName CreateMeshThumbnails = "create_mesh_thumbnails";
		/// <summary>
		/// Cached name for the 'update_mesh_list' member.
		/// </summary>
		public new static readonly StringName UpdateMeshList = "update_mesh_list";
		/// <summary>
		/// Cached name for the 'save' member.
		/// </summary>
		public new static readonly StringName Save = "save";
	}

	public new void SetTextureAsset(long id, Terrain3DTextureAsset texture) =>
		Call(GDExtensionMethodName.SetTextureAsset, [id, texture]);

	public new Terrain3DTextureAsset GetTextureAsset(long id) =>
		Terrain3DTextureAsset.Bind(Call(GDExtensionMethodName.GetTextureAsset, [id]).As<Resource>());

	public new long GetTextureCount() =>
		Call(GDExtensionMethodName.GetTextureCount, []).As<long>();

	public new Rid GetAlbedoArrayRid() =>
		Call(GDExtensionMethodName.GetAlbedoArrayRid, []).As<Rid>();

	public new Rid GetNormalArrayRid() =>
		Call(GDExtensionMethodName.GetNormalArrayRid, []).As<Rid>();

	public new Rid GetSurfaceArrayRid() =>
		Call(GDExtensionMethodName.GetSurfaceArrayRid, []).As<Rid>();

	public new Rid GetDisplacementArrayRid() =>
		Call(GDExtensionMethodName.GetDisplacementArrayRid, []).As<Rid>();

	[Obsolete("Use GetSurfaceArrayRid().")]
	public new Rid GetPbrArrayRid() =>
		Call(GDExtensionMethodName.GetPbrArrayRid, []).As<Rid>();

	public new void SetTexturePackingMode(TexturePackingMode mode) =>
		Call(GDExtensionMethodName.SetTexturePackingMode, [(long)mode]);

	public new TexturePackingMode GetTexturePackingMode() =>
		(TexturePackingMode)Call(GDExtensionMethodName.GetTexturePackingMode, []).As<long>();

	public new long GetPackedPbrMipmapCount() =>
		Call(GDExtensionMethodName.GetPackedPbrMipmapCount, []).As<long>();

	public new Color[] GetTextureColors() =>
		Call(GDExtensionMethodName.GetTextureColors, []).As<Color[]>();

	public new float[] GetTextureNormalDepths() =>
		Call(GDExtensionMethodName.GetTextureNormalDepths, []).As<float[]>();

	public new float[] GetTextureAoStrengths() =>
		Call(GDExtensionMethodName.GetTextureAoStrengths, []).As<float[]>();

	public new float[] GetTextureAoLightAffects() =>
		Call(GDExtensionMethodName.GetTextureAoLightAffects, []).As<float[]>();

	public new float[] GetTextureRoughnessMods() =>
		Call(GDExtensionMethodName.GetTextureRoughnessMods, []).As<float[]>();

	public new float[] GetTextureMetallics() =>
		Call(GDExtensionMethodName.GetTextureMetallics, []).As<float[]>();

	public new float[] GetTextureUvScales() =>
		Call(GDExtensionMethodName.GetTextureUvScales, []).As<float[]>();

	public new Vector2[] GetTextureDetiles() =>
		Call(GDExtensionMethodName.GetTextureDetiles, []).As<Vector2[]>();

	public new Vector2[] GetTextureDisplacements() =>
		Call(GDExtensionMethodName.GetTextureDisplacements, []).As<Vector2[]>();

	public new void ClearTextures(bool update = false) =>
		Call(GDExtensionMethodName.ClearTextures, [update]);

	public new void UpdateTextureList() =>
		Call(GDExtensionMethodName.UpdateTextureList, []);

	public new void SetMeshAsset(long id, Terrain3DMeshAsset mesh) =>
		Call(GDExtensionMethodName.SetMeshAsset, [id, mesh]);

	public new Terrain3DMeshAsset GetMeshAsset(long id) =>
		Terrain3DMeshAsset.Bind(Call(GDExtensionMethodName.GetMeshAsset, [id]).As<Resource>());

	public new long GetMeshCount() =>
		Call(GDExtensionMethodName.GetMeshCount, []).As<long>();

	public new void CreateMeshThumbnails(long id = -1, Vector2I size = default, bool force = false) =>
		Call(GDExtensionMethodName.CreateMeshThumbnails, [id, size, force]);

	public new void UpdateMeshList() =>
		Call(GDExtensionMethodName.UpdateMeshList, []);

	public new Error Save(string path = "") =>
		Call(GDExtensionMethodName.Save, [path]).As<Error>();

}

file static class AssetTypeExtensions
{
public static int SafeAsInt32(this Terrain3DAssets.AssetType enumValue) =>
Convert.ToInt32(enumValue);

public static int SafeAsInt32(this Terrain3DAssets.AssetType enumValue, int defaultValue) =>
Convert.ToInt32(enumValue);

public static int SafeAsInt32(this Terrain3DAssets.AssetType? enumValue, int defaultValue = 0) =>
enumValue.HasValue ? Convert.ToInt32(enumValue.Value) : defaultValue;
}

file static class TexturePackingModeExtensions
{
public static int SafeAsInt32(this Terrain3DAssets.TexturePackingMode enumValue) =>
Convert.ToInt32(enumValue);

public static int SafeAsInt32(this Terrain3DAssets.TexturePackingMode enumValue, int defaultValue) =>
Convert.ToInt32(enumValue);

public static int SafeAsInt32(this Terrain3DAssets.TexturePackingMode? enumValue, int defaultValue = 0) =>
enumValue.HasValue ? Convert.ToInt32(enumValue.Value) : defaultValue;
}
