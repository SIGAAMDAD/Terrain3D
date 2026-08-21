#pragma once

#ifndef TERRAIN3D_EASTL_CONFIG_H
#define TERRAIN3D_EASTL_CONFIG_H
#define TERRAIN3D_EASTL_CONFIGURED 1

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>

#include "core/terrain_3d_compiler.h"
#include "memory/terrain_3d_memory.h"
#include "memory/terrain_3d_zone_allocator.h"

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

typedef struct t3dEastlAllocationStats_s {
	size_t nLiveBytes = 0u;
	size_t nPeakBytes = 0u;
	size_t nLiveAllocations = 0u;
	size_t nTotalAllocations = 0u;
	size_t nFailedAllocations = 0u;
	size_t nOverflowAllocations = 0u;
	size_t nOverflowLiveBytes = 0u;
} t3dEastlAllocationStats_t;

typedef struct t3dEastlAllocationHeader_s {
	void *pRaw = nullptr;
	T3DMemoryZone *pZone = nullptr;
	size_t nSize = 0u;
	uint8_t nOrigin = TERRAIN3D_ALLOCATION_BOOTSTRAP;
} t3dEastlAllocationHeader_t;

inline constexpr size_t TERRAIN3D_EASTL_ALLOCATION_HEADER_SIZE = sizeof( t3dEastlAllocationHeader_t );

class T3DEastlAllocator {
	using AllocationHeader = t3dEastlAllocationHeader_t;

	inline static std::atomic<size_t> s_nLiveBytes{0u};
	inline static std::atomic<size_t> s_nPeakBytes{0u};
	inline static std::atomic<size_t> s_nLiveAllocations{0u};
	inline static std::atomic<size_t> s_nTotalAllocations{0u};
	inline static std::atomic<size_t> s_nFailedAllocations{0u};
	inline static std::atomic<size_t> s_nOverflowAllocations{0u};
	inline static std::atomic<size_t> s_nOverflowLiveBytes{0u};

	static void recordAllocation( const size_t nSize, const uint8_t nOrigin ) {
		const size_t live = s_nLiveBytes.fetch_add( nSize, std::memory_order_relaxed ) + nSize;
		s_nLiveAllocations.fetch_add( 1u, std::memory_order_relaxed );
		s_nTotalAllocations.fetch_add( 1u, std::memory_order_relaxed );
		if ( nOrigin == TERRAIN3D_ALLOCATION_OVERFLOW ) {
			s_nOverflowAllocations.fetch_add( 1u, std::memory_order_relaxed );
			s_nOverflowLiveBytes.fetch_add( nSize, std::memory_order_relaxed );
		}
		size_t peak = s_nPeakBytes.load( std::memory_order_relaxed );
		while ( live > peak && !s_nPeakBytes.compare_exchange_weak( peak, live, std::memory_order_relaxed, std::memory_order_relaxed ) ) {}
	}

	EA_FORCE_INLINE static void recordFailure( void ) { s_nFailedAllocations.fetch_add( 1u, std::memory_order_relaxed ); }

public:
	EA_FORCE_INLINE T3DEastlAllocator( const char * = EASTL_ALLOCATOR_DEFAULT_NAME ) {}
	T3DEastlAllocator( const T3DEastlAllocator & ) = default;
	EA_FORCE_INLINE T3DEastlAllocator( const T3DEastlAllocator &, const char * ) {}
	T3DEastlAllocator &operator=( const T3DEastlAllocator & ) = default;
	EA_FORCE_INLINE const char *getName( void ) const { return EASTL_ALLOCATOR_DEFAULT_NAME; }
	EA_FORCE_INLINE void setName( const char * ) {}

	EA_FORCE_INLINE void *allocate( const size_t n, int = 0 ) { return allocate( n, alignof( std::max_align_t ), 0, 0 ); }
	void *allocate( const size_t n, size_t alignment, const size_t alignment_offset, int = 0 ) {
		if ( alignment < alignof( void * ) ) alignment = alignof( void * );
		if ( n == 0u ) return nullptr;
		if ( ( alignment & ( alignment - 1u ) ) != 0u ) { recordFailure(); return nullptr; }
		const size_t header_size = TERRAIN3D_EASTL_ALLOCATION_HEADER_SIZE;
		const size_t alignment_slack = alignment - 1u;
		if ( n > std::numeric_limits<size_t>::max() - header_size - alignment_slack ) { recordFailure(); return nullptr; }
		const size_t total = n + header_size + alignment_slack;
		T3DMemoryZone *const pZone = T3DZoneAllocatorRouter::zone();
		void *pRaw = pZone ? pZone->allocate( total, alignment ) : terrain3d_memory::allocate( total );
		uint8_t origin = pZone ? TERRAIN3D_ALLOCATION_ZONE : TERRAIN3D_ALLOCATION_BOOTSTRAP;
		if ( !pRaw && pZone && T3DZoneAllocatorRouter::overflowPolicy() == TERRAIN3D_ALLOCATOR_OVERFLOW_HEAP ) {
			pRaw = terrain3d_memory::allocate( total );
			origin = TERRAIN3D_ALLOCATION_OVERFLOW;
		}
		if ( !pRaw ) { recordFailure(); return nullptr; }
		const uintptr_t begin = ( uintptr_t ) pRaw + header_size;
		const uintptr_t effective_offset = ( uintptr_t ) ( alignment_offset & alignment_slack );
		uintptr_t aligned = ( begin + effective_offset + alignment_slack ) & ~( uintptr_t ) alignment_slack;
		aligned -= effective_offset;
		AllocationHeader header;
		header.pRaw = pRaw;
		header.pZone = pZone;
		header.nSize = n;
		header.nOrigin = origin;
		std::memcpy( ( void * ) ( aligned - header_size ), &header, header_size );
		recordAllocation( n, origin );
		return ( void * ) aligned;
	}

	void deallocate( void *const p, size_t = 0 ) {
		if ( !p ) return;
		AllocationHeader header;
		std::memcpy( &header, ( unsigned char * ) p - sizeof( AllocationHeader ), sizeof( AllocationHeader ) );
		s_nLiveBytes.fetch_sub( header.nSize, std::memory_order_relaxed );
		s_nLiveAllocations.fetch_sub( 1u, std::memory_order_relaxed );
		if ( header.nOrigin == TERRAIN3D_ALLOCATION_OVERFLOW ) s_nOverflowLiveBytes.fetch_sub( header.nSize, std::memory_order_relaxed );
		if ( header.nOrigin == TERRAIN3D_ALLOCATION_ZONE && header.pZone ) header.pZone->deallocate( header.pRaw );
		else terrain3d_memory::release( header.pRaw );
	}

	static t3dEastlAllocationStats_t getAllocationStats( void ) {
		t3dEastlAllocationStats_t stats;
		stats.nLiveBytes = s_nLiveBytes.load( std::memory_order_relaxed );
		stats.nPeakBytes = s_nPeakBytes.load( std::memory_order_relaxed );
		stats.nLiveAllocations = s_nLiveAllocations.load( std::memory_order_relaxed );
		stats.nTotalAllocations = s_nTotalAllocations.load( std::memory_order_relaxed );
		stats.nFailedAllocations = s_nFailedAllocations.load( std::memory_order_relaxed );
		stats.nOverflowAllocations = s_nOverflowAllocations.load( std::memory_order_relaxed );
		stats.nOverflowLiveBytes = s_nOverflowLiveBytes.load( std::memory_order_relaxed );
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
EA_FORCE_INLINE static T3DEastlAllocator *T3DGetDefaultEastlAllocator( void ) { static T3DEastlAllocator s_allocator; return &s_allocator; }
#define EASTLAllocatorDefault T3DGetDefaultEastlAllocator

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
#define EA_GET_INSTRUCTION_POINTER() _ReturnAddress()
#else
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
