#pragma once

#ifndef TERRAIN3D_EASTL_CONFIG_H
#define TERRAIN3D_EASTL_CONFIG_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>

#include "memory/terrain_3d_memory.h"

// EASTL is used only in Terrain3D internals. Public Godot APIs remain Godot types.
#define EASTL_EABASE_DISABLED 1
#define EASTL_USER_DEFINED_ALLOCATOR 1
#define EASTL_EXCEPTIONS_ENABLED 0
#define EASTL_RTTI_ENABLED 0
#if defined( NDEBUG )
#define EASTL_DEBUG 0
#define EASTL_ASSERT_ENABLED 0
#else
#define EASTL_DEBUG 1
#define EASTL_ASSERT_ENABLED 1
#endif
#define EASTL_NAME_ENABLED 0
#define EASTL_DLL 0
#define EASTL_API
#define EASTL_LOCAL
#define EASTL_DEBUGPARAMS_LEVEL 0
#define EASTL_ALLOCATOR_DEFAULT_NAME "Terrain3D EASTL"
#define EASTL_NAME( x ) x
#define EASTL_NAME_VAL( x ) x

typedef struct t3deastlAllocationStats_s {
	size_t nLiveBytes = 0;
	size_t nPeakBytes = 0;
	size_t nLiveAllocations = 0;
	size_t nTotalAllocations = 0;
} t3deastlAllocationStats_t;

typedef struct t3deastlAllocationHeader_s {
	void *pRaw = nullptr;
#if EASTL_ASSERT_ENABLED
	size_t nSize = 0;
#endif
} t3deastlAllocationHeader_t;

inline constexpr size_t TERRAIN3D_EASTL_ALLOCATION_HEADER_SIZE = sizeof( t3deastlAllocationHeader_t );

class T3DEastlAllocator {
	using AllocationHeader = t3deastlAllocationHeader_t;

#if EASTL_ASSERT_ENABLED
	inline static std::atomic<size_t> s_nLiveBytes{0};
	inline static std::atomic<size_t> s_nPeakBytes{0};
	inline static std::atomic<size_t> s_nLiveAllocations{0};
	inline static std::atomic<size_t> s_nTotalAllocations{0};

	static void recordAllocation( const size_t nSize ) {
		const size_t live = s_nLiveBytes.fetch_add( nSize, std::memory_order_relaxed ) + nSize;
		s_nLiveAllocations.fetch_add( 1, std::memory_order_relaxed );
		s_nTotalAllocations.fetch_add( 1, std::memory_order_relaxed );
		size_t peak = s_nPeakBytes.load( std::memory_order_relaxed );
		while ( live > peak &&
			   !s_nPeakBytes.compare_exchange_weak( peak, live, std::memory_order_relaxed, std::memory_order_relaxed ) ) {
		}
	}
#endif

public:
	T3DEastlAllocator( const char * = EASTL_ALLOCATOR_DEFAULT_NAME ) {}
	T3DEastlAllocator( const T3DEastlAllocator & ) = default;
	T3DEastlAllocator( const T3DEastlAllocator &, const char * ) {}
	T3DEastlAllocator &operator=( const T3DEastlAllocator & ) = default;
	const char *getName( void ) const {
		return EASTL_ALLOCATOR_DEFAULT_NAME;
	}
	void setName( const char * ) {}

	void *allocate( const size_t n, int = 0 ) {
		return allocate( n, alignof( std::max_align_t ), 0, 0 );
	}
	void *allocate( const size_t n, size_t alignment, const size_t alignment_offset, int = 0 ) {
		if ( alignment < alignof( void * ) ) {
			alignment = alignof( void * );
		}
		if ( ( alignment & ( alignment - 1 ) ) != 0 ) {
			return nullptr;
		}
		const size_t header_size = TERRAIN3D_EASTL_ALLOCATION_HEADER_SIZE;
		const size_t alignment_slack = alignment - 1;
		if ( n > std::numeric_limits<size_t>::max() - header_size - alignment_slack ) {
			return nullptr;
		}
		const size_t extra = header_size + alignment_slack;
		void *const pRaw = terrain3d_memory::allocate( n + extra );
		if ( !pRaw ) {
			return nullptr;
		}
		const uintptr_t begin = ( uintptr_t ) ( pRaw ) + header_size;
		const uintptr_t effective_offset = ( uintptr_t ) ( alignment_offset & alignment_slack );
		uintptr_t aligned = ( begin + effective_offset + alignment_slack ) & ~( uintptr_t ) ( alignment_slack );
		aligned -= effective_offset;
		AllocationHeader header;
		header.pRaw = pRaw;
#if EASTL_ASSERT_ENABLED
		header.nSize = n;
		recordAllocation( n );
#endif
		// A nonzero EASTL alignment offset can make the user pointer unsuitable
		// for an AllocationHeader. Store/load the metadata byte-wise instead of
		// dereferencing a potentially misaligned header pointer.
		std::memcpy( ( void * ) ( aligned - header_size ), &header, header_size );
		return ( void * ) ( aligned );
	}
	void deallocate( void *const p, size_t = 0 ) {
		if ( !p ) {
			return;
		}
		AllocationHeader header;
		std::memcpy( &header, ( unsigned char * ) ( p ) - sizeof( AllocationHeader ), sizeof( AllocationHeader ) );
#if EASTL_ASSERT_ENABLED
		s_nLiveBytes.fetch_sub( header.nSize, std::memory_order_relaxed );
		s_nLiveAllocations.fetch_sub( 1, std::memory_order_relaxed );
#endif
		terrain3d_memory::release( header.pRaw );
	}

	static t3deastlAllocationStats_t getAllocationStats( void ) {
		t3deastlAllocationStats_t stats;
#if EASTL_ASSERT_ENABLED
		stats.nLiveBytes = s_nLiveBytes.load( std::memory_order_relaxed );
		stats.nPeakBytes = s_nPeakBytes.load( std::memory_order_relaxed );
		stats.nLiveAllocations = s_nLiveAllocations.load( std::memory_order_relaxed );
		stats.nTotalAllocations = s_nTotalAllocations.load( std::memory_order_relaxed );
#endif
		return stats;
	}

	friend bool operator==( const T3DEastlAllocator &, const T3DEastlAllocator & ) {
		return true;
	}
	friend bool operator!=( const T3DEastlAllocator &, const T3DEastlAllocator & ) {
		return false;
	}
};
#define EASTLAllocatorType T3DEastlAllocator
static inline T3DEastlAllocator *Terrain3DGetDefaultEastlAllocator( void ) {
	static T3DEastlAllocator s_allocator;
	return &s_allocator;
}
#define EASTLAllocatorDefault Terrain3DGetDefaultEastlAllocator

#if defined( _MSC_VER )
#define EA_COMPILER_MSVC 1
#define EA_COMPILER_VERSION _MSC_VER
#elif defined( __clang__ )
#define EA_COMPILER_CLANG 1
#define EA_COMPILER_VERSION ( __clang_major__ * 100 + __clang_minor__ )
#elif defined( __GNUC__ )
#define EA_COMPILER_GNUC 1
#define EA_COMPILER_VERSION ( __GNUC__ * 1000 + __GNUC_MINOR__ )
#endif

#if defined( _WIN32 )
#define EA_PLATFORM_WINDOWS 1
#define EA_PLATFORM_MICROSOFT 1
#elif defined( __APPLE__ )
#define EA_PLATFORM_APPLE 1
#define EA_PLATFORM_OSX 1
#elif defined( __linux__ )
#define EA_PLATFORM_LINUX 1
#define EA_PLATFORM_UNIX 1
#endif

#if INTPTR_MAX == INT64_MAX
#define EA_PLATFORM_PTR_SIZE 8
#define EA_PLATFORM_WORD_SIZE 8
#define EA_PLATFORM_MIN_MALLOC_ALIGNMENT 16
#else
#define EA_PLATFORM_PTR_SIZE 4
#define EA_PLATFORM_WORD_SIZE 4
#define EA_PLATFORM_MIN_MALLOC_ALIGNMENT 8
#endif

#if __cplusplus >= 202002L
#define EA_COMPILER_CPP20_ENABLED 1
#endif
#if __cplusplus >= 201703L
#define EA_COMPILER_CPP17_ENABLED 1
#endif
#if __cplusplus >= 201402L
#define EA_COMPILER_CPP14_ENABLED 1
#endif
#if __cplusplus >= 201103L
#define EA_COMPILER_CPP11_ENABLED 1
#endif

#define EA_PRAGMA_ONCE_SUPPORTED 1
#define EA_TSAN_ENABLED 0
#define EA_NOEXCEPT noexcept
#define EA_NOEXCEPT_IF( x ) noexcept( x )
#define EA_NOEXCEPT_EXPR( x ) noexcept( x )
#define EA_CPP14_CONSTEXPR constexpr
#define EA_CPP17_CONSTEXPR constexpr
#define EA_CPP20_CONSTEXPR constexpr
#define EA_CONSTEXPR constexpr
#define EA_CONSTEXPR_IF if constexpr
#define EA_OVERRIDE override
#define EA_FINAL final
#define EA_CONSTEXPR_OR_CONST constexpr
#if defined( _MSC_VER )
#define EA_FORCE_INLINE __forceinline
#define EA_GET_INSTRUCTION_POINTER() _ReturnAddress()
#else
#define EA_FORCE_INLINE inline __attribute__( ( always_inline ) )
#define EA_GET_INSTRUCTION_POINTER() __builtin_return_address( 0 )
#endif
#define EA_ALIGN_OF( type ) alignof( type )
#if defined( _MSC_VER )
#define EA_ALIGN( n ) __declspec( align( n ) )
#define EA_PREFIX_ALIGN( n ) __declspec( align( n ) )
#define EA_POSTFIX_ALIGN( n )
#define EA_RESTRICT __restrict
#else
#define EA_ALIGN( n ) __attribute__( ( aligned( n ) ) )
#define EA_PREFIX_ALIGN( n )
#define EA_POSTFIX_ALIGN( n ) __attribute__( ( aligned( n ) ) )
#define EA_RESTRICT __restrict__
#endif
#define EA_PACKED
#define EA_MAY_ALIAS
#if defined( _MSC_VER )
#define EA_LIKELY( x ) ( x )
#define EA_UNLIKELY( x ) ( x )
#define EA_ASSUME( x ) __assume( x )
#else
#define EA_LIKELY( x ) __builtin_expect( !!( x ), 1 )
#define EA_UNLIKELY( x ) __builtin_expect( !!( x ), 0 )
#define EA_ASSUME( x )                 \
	do {                             \
		if ( !( x ) ) {                  \
			__builtin_unreachable(); \
		}                            \
	} while ( 0 )
#endif
#define EA_UNUSED( x ) ( void ) ( x )
#define EA_ANALYSIS_ASSUME( x ) EA_ASSUME( x )
#define EA_DISABLE_VC_WARNING( x )
#define EA_RESTORE_VC_WARNING()
#define EA_DISABLE_ALL_VC_WARNINGS()
#define EA_RESTORE_ALL_VC_WARNINGS()
#define EA_PUSH_VC_WARNING()
#define EA_POP_VC_WARNING()
#define EA_DISABLE_CLANG_WARNING( x )
#define EA_RESTORE_CLANG_WARNING()
#define EA_DISABLE_GCC_WARNING( x )
#define EA_RESTORE_GCC_WARNING()
#define EA_DEPRECATED
#define EA_REMOVE_AT_2025_OCT_MSG( msg )
#define EA_REMOVE_AT_2026_APRIL
#define EA_DEPRECATED_MESSAGE( msg )
#define EA_NON_COPYABLE( ClassName )         \
	ClassName( const ClassName & ) = delete; \
	ClassName &operator=( const ClassName & ) = delete
#ifndef EA_NODISCARD
#define EA_NODISCARD [[nodiscard]]
#endif
#define EA_COMPILER_NO_EXCEPTIONS 1
#define EA_COMPILER_NO_RTTI 1
#define EA_COMPILER_NO_UNWIND 1
#define EABASE_VERSION_N 99999
#if defined( __has_builtin )
#define EA_COMPILER_HAS_BUILTIN( x ) __has_builtin( x )
#else
#define EA_COMPILER_HAS_BUILTIN( x ) 0
#endif
#if defined( __has_feature )
#define EA_COMPILER_HAS_FEATURE( x ) __has_feature( x )
#else
#define EA_COMPILER_HAS_FEATURE( x ) 0
#endif
#define EA_IS_ENABLED( x ) 0
#define EA_HAVE_CPP11_INITIALIZER_LIST 1
#define EA_HAVE_CPP11_TUPLES 1
#define EA_HAVE_CPP11_TYPE_TRAITS 1
#define EA_HAVE_CPP11_ATOMIC 1

#endif
