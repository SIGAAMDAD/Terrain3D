// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_REGION_IMAGE_COOKER_H
#define TERRAIN3D_REGION_IMAGE_COOKER_H

#include "Terrain3DConstants.h"
#include "assets/catalog/terrain_3d_assets.h"
#include "assets/resources/terrain_3d_texture_asset.h"
#include "core/utility/terrain_3d_util.h"
#include "data/cooking/terrain_3d_control_map_cooker.h"
#include "data/model/terrain_3d_region.h"
#include "lighting/runtime/terrain_3d_region_lighting.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/texture2d.hpp>

Ref<Image> t3dRegionImageResolveMap( const Terrain3DRegion *pRegion,
        const t3dMapType_t eMapType,
        const Ref<Image> &rDefaultHeight,
        const Ref<Image> &rDefaultControl,
        const Ref<Image> &rDefaultColor );

Ref<Image> t3dRegionImageCookGpuColor(
        const Ref<Image> &rImage, const Ref<Image> &rDefaultImage, const int nResolutionDivisor );

Ref<Image> t3dRegionImageCookGpuDerived(
        const Ref<Image> &rImage, const Ref<Image> &rDefaultImage, const int nResolutionDivisor );

bool t3dRegionImagePrepareMiddleCompositeMaterials( const Ref<Terrain3DAssets> &rAssets,
        PackedColorArray &rColors,
        PackedFloat32Array &rScales,
        Ref<Image> *pImages,
        const size_t nImageCapacity );

Color t3dRegionImageSampleMiddleCompositeMaterial( const Ref<Image> *pMaterialImages,
        const size_t nMaterialImageCount,
        const PackedFloat32Array &rMaterialScales,
        const int nMaterialId,
        const Color &rTint,
        const float fCosAngle,
        const float fSinAngle,
        const float fWorldX,
        const float fWorldY,
        const float fControlScale );

Ref<Image> t3dRegionImageCookMiddleComposite( const Vector2i &rLocation,
        const Ref<Image> &rControl,
        const Ref<Image> &rColor,
        const PackedColorArray &rMaterialColors,
        const PackedFloat32Array &rMaterialScales,
        const Ref<Image> *pMaterialImages,
        const size_t nMaterialImageCount,
        PackedByteArray &rOutput );

Ref<Image> t3dRegionImageCookTerrainShadowMask( const Terrain3DRegion *pRegion, const int nRegionSize );

#endif // TERRAIN3D_REGION_IMAGE_COOKER_H
