// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_ASSET_RESOURCE_CLASS_H
#define TERRAIN3D_ASSET_RESOURCE_CLASS_H

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/texture2d.hpp>

#include "core/terrain_3d_compiler.h"
#include "Terrain3DConstants.h"

class T3DAssets;

// Parent class of T3DMeshAsset and T3DTextureAsset
class T3DAssetResource : public Resource {
	friend class T3DAssets;

public:
	EA_FORCE_INLINE T3DAssetResource( void ) {}
	EA_FORCE_INLINE ~T3DAssetResource( void ) {}

	virtual void initialize( void ) = 0;
	virtual void clear( void ) = 0;
	virtual void setName( const String &rName ) = 0;
	virtual String getName( void ) const = 0;
	virtual void setId( const int nId ) = 0;
	virtual int getId( void ) const = 0;

	virtual void setHighlighted( const bool bHighlighted ) = 0;
	virtual bool isHighlighted( void ) const = 0;
	virtual Color getHighlightColor( void ) const = 0;
	virtual Ref<Texture2D> getThumbnail( void ) const = 0;

protected:
	String m_name;
	int m_nId = 0;
	bool m_bHighlighted = false;
	// Internal stable-slot sentinel. Placeholder resources must never escape the asset registry as authored assets.
	bool m_bPlaceholder = false;
	Ref<Texture2D> m_pThumbnail;
};

#endif // TERRAIN3D_ASSET_RESOURCE_CLASS_H
