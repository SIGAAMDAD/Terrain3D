// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_HASH_H
#define TERRAIN3D_HASH_H

#include "eastl_config.h"
#include "Terrain3DConstants.h"
#include <EASTL/functional.h>
#include <EASTL/utility.h>

typedef struct t3dVector2iHash_s
{
	size_t operator()(
		const Vector2i &v ) const {
		size_t h1 = eastl::hash< int >()( v.x );
		size_t h2 = eastl::hash< int >()( v.y );
		return h1 ^ ( h2 << 1 );
	}
} t3dVector2iHash_t;

typedef struct t3dPairVector2iIntHash_s
{
	size_t operator()(
		const eastl::pair< Vector2i, int > &p ) const {
		size_t h1 = t3dVector2iHash_t{}( p.first );
		size_t h2 = eastl::hash< int >{}( p.second );
		return h1 ^ ( h2 << 1 );
	}
} t3dPairVector2iIntHash_t;

typedef struct t3dVector3Hash_s
{
	size_t operator()(
		const Vector3 &v ) const {
		size_t h1 = eastl::hash< float >()( v.x );
		size_t h2 = eastl::hash< float >()( v.y );
		size_t h3 = eastl::hash< float >()( v.z );
		return h1 ^ ( h2 << 1 ) ^ ( h3 << 2 );
	}
} t3dVector3Hash_t;

#endif // TERRAIN3D_HASH_H
