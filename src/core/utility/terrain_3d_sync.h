// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_SYNC_H
#define TERRAIN3D_SYNC_H

#include <chrono>
#include <condition_variable>
#include <stdint.h>
#include <string.h>
#include <mutex>
#include <thread>
#include <utility>

#if defined( _MSC_VER )
#include <intrin.h>
#endif

// Synchronization owns its memory-order contract; it does not depend on engine records.
typedef enum t3dMemoryOrder_e
{
	T3D_MEMORY_ORDER_RELAXED = 0,
	T3D_MEMORY_ORDER_CONSUME,
	T3D_MEMORY_ORDER_ACQUIRE,
	T3D_MEMORY_ORDER_RELEASE,
	T3D_MEMORY_ORDER_ACQ_REL,
	T3D_MEMORY_ORDER_SEQ_CST
} t3dMemoryOrder_t;

// A failed compare-exchange is a load and cannot release prior writes.
static inline t3dMemoryOrder_t t3dCompareExchangeFailureOrder(
	const t3dMemoryOrder_t eOrder ) {
	if ( eOrder == T3D_MEMORY_ORDER_RELEASE ) {
		return T3D_MEMORY_ORDER_RELAXED;
	}
	if ( eOrder == T3D_MEMORY_ORDER_ACQ_REL ) {
		return T3D_MEMORY_ORDER_ACQUIRE;
	}
	return eOrder;
}

#if !defined( _MSC_VER )
static inline int t3dNativeMemoryOrder(
	const t3dMemoryOrder_t eOrder ) {
	switch ( eOrder ) {
	case T3D_MEMORY_ORDER_RELAXED:
		return __ATOMIC_RELAXED;
	case T3D_MEMORY_ORDER_CONSUME:
		return __ATOMIC_CONSUME;
	case T3D_MEMORY_ORDER_ACQUIRE:
		return __ATOMIC_ACQUIRE;
	case T3D_MEMORY_ORDER_RELEASE:
		return __ATOMIC_RELEASE;
	case T3D_MEMORY_ORDER_ACQ_REL:
		return __ATOMIC_ACQ_REL;
	default:
		return __ATOMIC_SEQ_CST;
	}
}
#endif

#if defined( _MSC_VER )
template< typename T >
static inline T t3dAtomicMsLoad(
	const volatile T *pValue ) noexcept {
	static_assert( sizeof( T ) == 1u || sizeof( T ) == 2u || sizeof( T ) == 4u || sizeof( T ) == 8u,
				   "Unsupported atomic width" );
	T value;
	if constexpr ( sizeof( T ) == 1u ) {
		const uint8_t nBits = ( uint8_t ) _InterlockedCompareExchange8( ( volatile char * ) pValue, 0, 0 );
		memcpy( &value, &nBits, sizeof( value ) );
	} else if constexpr ( sizeof( T ) == 2u ) {
		const uint16_t nBits = ( uint16_t ) _InterlockedCompareExchange16( ( volatile short * ) pValue, 0, 0 );
		memcpy( &value, &nBits, sizeof( value ) );
	} else if constexpr ( sizeof( T ) == 4u ) {
		const uint32_t nBits = ( uint32_t ) _InterlockedCompareExchange( ( volatile long * ) pValue, 0, 0 );
		memcpy( &value, &nBits, sizeof( value ) );
	} else {
		const uint64_t nBits = ( uint64_t ) _InterlockedCompareExchange64( ( volatile __int64 * ) pValue, 0, 0 );
		memcpy( &value, &nBits, sizeof( value ) );
	}
	return value;
}

template< typename T >
static inline T t3dAtomicMsExchange(
	volatile T *pValue,
	const T		value ) noexcept {
	T previous;
	if constexpr ( sizeof( T ) == 1u ) {
		uint8_t nValueBits;
		memcpy( &nValueBits, &value, sizeof( nValueBits ) );
		const uint8_t nPreviousBits =
			( uint8_t ) _InterlockedExchange8( ( volatile char * ) pValue, ( char ) nValueBits );
		memcpy( &previous, &nPreviousBits, sizeof( previous ) );
	} else if constexpr ( sizeof( T ) == 2u ) {
		uint16_t nValueBits;
		memcpy( &nValueBits, &value, sizeof( nValueBits ) );
		const uint16_t nPreviousBits =
			( uint16_t ) _InterlockedExchange16( ( volatile short * ) pValue, ( short ) nValueBits );
		memcpy( &previous, &nPreviousBits, sizeof( previous ) );
	} else if constexpr ( sizeof( T ) == 4u ) {
		uint32_t nValueBits;
		memcpy( &nValueBits, &value, sizeof( nValueBits ) );
		const uint32_t nPreviousBits =
			( uint32_t ) _InterlockedExchange( ( volatile long * ) pValue, ( long ) nValueBits );
		memcpy( &previous, &nPreviousBits, sizeof( previous ) );
	} else {
		uint64_t nValueBits;
		memcpy( &nValueBits, &value, sizeof( nValueBits ) );
		const uint64_t nPreviousBits =
			( uint64_t ) _InterlockedExchange64( ( volatile __int64 * ) pValue, ( __int64 ) nValueBits );
		memcpy( &previous, &nPreviousBits, sizeof( previous ) );
	}
	return previous;
}

template< typename T >
static inline bool t3dAtomicMsCompareExchange(
	volatile T *pValue,
	T		   &rExpected,
	const T		desired ) noexcept {
	T observed;
	if constexpr ( sizeof( T ) == 1u ) {
		uint8_t nExpectedBits;
		uint8_t nDesiredBits;
		memcpy( &nExpectedBits, &rExpected, sizeof( nExpectedBits ) );
		memcpy( &nDesiredBits, &desired, sizeof( nDesiredBits ) );
		const uint8_t nObservedBits = ( uint8_t ) _InterlockedCompareExchange8( ( volatile char * ) pValue,
																				( char ) nDesiredBits,
																				( char ) nExpectedBits );
		memcpy( &observed, &nObservedBits, sizeof( observed ) );
	} else if constexpr ( sizeof( T ) == 2u ) {
		uint16_t nExpectedBits;
		uint16_t nDesiredBits;
		memcpy( &nExpectedBits, &rExpected, sizeof( nExpectedBits ) );
		memcpy( &nDesiredBits, &desired, sizeof( nDesiredBits ) );
		const uint16_t nObservedBits = ( uint16_t ) _InterlockedCompareExchange16( ( volatile short * ) pValue,
																				   ( short ) nDesiredBits,
																				   ( short ) nExpectedBits );
		memcpy( &observed, &nObservedBits, sizeof( observed ) );
	} else if constexpr ( sizeof( T ) == 4u ) {
		uint32_t nExpectedBits;
		uint32_t nDesiredBits;
		memcpy( &nExpectedBits, &rExpected, sizeof( nExpectedBits ) );
		memcpy( &nDesiredBits, &desired, sizeof( nDesiredBits ) );
		const uint32_t nObservedBits = ( uint32_t ) _InterlockedCompareExchange( ( volatile long * ) pValue,
																				 ( long ) nDesiredBits,
																				 ( long ) nExpectedBits );
		memcpy( &observed, &nObservedBits, sizeof( observed ) );
	} else {
		uint64_t nExpectedBits;
		uint64_t nDesiredBits;
		memcpy( &nExpectedBits, &rExpected, sizeof( nExpectedBits ) );
		memcpy( &nDesiredBits, &desired, sizeof( nDesiredBits ) );
		const uint64_t nObservedBits = ( uint64_t ) _InterlockedCompareExchange64( ( volatile __int64 * ) pValue,
																				   ( __int64 ) nDesiredBits,
																				   ( __int64 ) nExpectedBits );
		memcpy( &observed, &nObservedBits, sizeof( observed ) );
	}
	if ( memcmp( &observed, &rExpected, sizeof( T ) ) == 0 ) {
		return true;
	}
	rExpected = observed;
	return false;
}

template< typename T >
static inline T t3dAtomicMsFetchAdd(
	volatile T *pValue,
	const T		value ) noexcept {
	if constexpr ( sizeof( T ) == 1u ) {
		return ( T ) _InterlockedExchangeAdd8( ( volatile char * ) pValue, ( char ) value );
	} else if constexpr ( sizeof( T ) == 2u ) {
		return ( T ) _InterlockedExchangeAdd16( ( volatile short * ) pValue, ( short ) value );
	} else if constexpr ( sizeof( T ) == 4u ) {
		return ( T ) _InterlockedExchangeAdd( ( volatile long * ) pValue, ( long ) value );
	} else {
		return ( T ) _InterlockedExchangeAdd64( ( volatile __int64 * ) pValue, ( __int64 ) value );
	}
}

template< typename T >
static inline T t3dAtomicMsFetchOr(
	volatile T *pValue,
	const T		value ) noexcept {
	if constexpr ( sizeof( T ) == 1u ) {
		return ( T ) _InterlockedOr8( ( volatile char * ) pValue, ( char ) value );
	} else if constexpr ( sizeof( T ) == 2u ) {
		return ( T ) _InterlockedOr16( ( volatile short * ) pValue, ( short ) value );
	} else if constexpr ( sizeof( T ) == 4u ) {
		return ( T ) _InterlockedOr( ( volatile long * ) pValue, ( long ) value );
	} else {
		return ( T ) _InterlockedOr64( ( volatile __int64 * ) pValue, ( __int64 ) value );
	}
}

template< typename T >
static inline T t3dAtomicMsFetchAnd(
	volatile T *pValue,
	const T		value ) noexcept {
	if constexpr ( sizeof( T ) == 1u ) {
		return ( T ) _InterlockedAnd8( ( volatile char * ) pValue, ( char ) value );
	} else if constexpr ( sizeof( T ) == 2u ) {
		return ( T ) _InterlockedAnd16( ( volatile short * ) pValue, ( short ) value );
	} else if constexpr ( sizeof( T ) == 4u ) {
		return ( T ) _InterlockedAnd( ( volatile long * ) pValue, ( long ) value );
	} else {
		return ( T ) _InterlockedAnd64( ( volatile __int64 * ) pValue, ( __int64 ) value );
	}
}
#endif

template< typename T > class Terrain3DAtomic
{
	volatile T m_value;

public:
	constexpr Terrain3DAtomic(
		void ) noexcept :
		m_value{} {
	}
	constexpr Terrain3DAtomic(
		const T value ) noexcept :
		m_value( value ) {
	}
	Terrain3DAtomic( const Terrain3DAtomic & ) = delete;
	Terrain3DAtomic &operator=( const Terrain3DAtomic & ) = delete;

	T load(
		const t3dMemoryOrder_t eOrder = T3D_MEMORY_ORDER_SEQ_CST ) const noexcept {
#if defined( _MSC_VER )
		( void ) eOrder;
		return t3dAtomicMsLoad( &m_value );
#else
		return __atomic_load_n( &m_value, t3dNativeMemoryOrder( eOrder ) );
#endif
	}

	void store(
		const T				   value,
		const t3dMemoryOrder_t eOrder = T3D_MEMORY_ORDER_SEQ_CST ) noexcept {
#if defined( _MSC_VER )
		( void ) eOrder;
		( void ) t3dAtomicMsExchange( &m_value, value );
#else
		__atomic_store_n( &m_value, value, t3dNativeMemoryOrder( eOrder ) );
#endif
	}

	T exchange(
		const T				   value,
		const t3dMemoryOrder_t eOrder = T3D_MEMORY_ORDER_SEQ_CST ) noexcept {
#if defined( _MSC_VER )
		( void ) eOrder;
		return t3dAtomicMsExchange( &m_value, value );
#else
		return __atomic_exchange_n( &m_value, value, t3dNativeMemoryOrder( eOrder ) );
#endif
	}

	bool compare_exchange_weak(
		T					  &rExpected,
		const T				   desired,
		const t3dMemoryOrder_t eSuccess,
		const t3dMemoryOrder_t eFailure ) noexcept {
#if defined( _MSC_VER )
		( void ) eSuccess;
		( void ) eFailure;
		return t3dAtomicMsCompareExchange( &m_value, rExpected, desired );
#else
		return __atomic_compare_exchange_n( &m_value,
											&rExpected,
											desired,
											true,
											t3dNativeMemoryOrder( eSuccess ),
											t3dNativeMemoryOrder( eFailure ) );
#endif
	}

	bool compare_exchange_weak(
		T					  &rExpected,
		const T				   desired,
		const t3dMemoryOrder_t eOrder = T3D_MEMORY_ORDER_SEQ_CST ) noexcept {
		return compare_exchange_weak( rExpected, desired, eOrder, t3dCompareExchangeFailureOrder( eOrder ) );
	}

	bool compare_exchange_strong(
		T					  &rExpected,
		const T				   desired,
		const t3dMemoryOrder_t eSuccess,
		const t3dMemoryOrder_t eFailure ) noexcept {
#if defined( _MSC_VER )
		( void ) eSuccess;
		( void ) eFailure;
		return t3dAtomicMsCompareExchange( &m_value, rExpected, desired );
#else
		return __atomic_compare_exchange_n( &m_value,
											&rExpected,
											desired,
											false,
											t3dNativeMemoryOrder( eSuccess ),
											t3dNativeMemoryOrder( eFailure ) );
#endif
	}

	bool compare_exchange_strong(
		T					  &rExpected,
		const T				   desired,
		const t3dMemoryOrder_t eOrder = T3D_MEMORY_ORDER_SEQ_CST ) noexcept {
		return compare_exchange_strong( rExpected, desired, eOrder, t3dCompareExchangeFailureOrder( eOrder ) );
	}

	T fetch_add(
		const T				   value,
		const t3dMemoryOrder_t eOrder = T3D_MEMORY_ORDER_SEQ_CST ) noexcept {
#if defined( _MSC_VER )
		( void ) eOrder;
		return t3dAtomicMsFetchAdd( &m_value, value );
#else
		return __atomic_fetch_add( &m_value, value, t3dNativeMemoryOrder( eOrder ) );
#endif
	}

	T fetch_sub(
		const T				   value,
		const t3dMemoryOrder_t eOrder = T3D_MEMORY_ORDER_SEQ_CST ) noexcept {
#if defined( _MSC_VER )
		const T delta = ( T ) ( ( T ) 0 - value );
		( void ) eOrder;
		return t3dAtomicMsFetchAdd( &m_value, delta );
#else
		return __atomic_fetch_sub( &m_value, value, t3dNativeMemoryOrder( eOrder ) );
#endif
	}

	T fetch_or(
		const T				   value,
		const t3dMemoryOrder_t eOrder = T3D_MEMORY_ORDER_SEQ_CST ) noexcept {
#if defined( _MSC_VER )
		( void ) eOrder;
		return t3dAtomicMsFetchOr( &m_value, value );
#else
		return __atomic_fetch_or( &m_value, value, t3dNativeMemoryOrder( eOrder ) );
#endif
	}

	T fetch_and(
		const T				   value,
		const t3dMemoryOrder_t eOrder = T3D_MEMORY_ORDER_SEQ_CST ) noexcept {
#if defined( _MSC_VER )
		( void ) eOrder;
		return t3dAtomicMsFetchAnd( &m_value, value );
#else
		return __atomic_fetch_and( &m_value, value, t3dNativeMemoryOrder( eOrder ) );
#endif
	}

	bool is_lock_free(
		void ) const noexcept {
#if defined( _MSC_VER )
		return true;
#else
		return __atomic_is_lock_free( sizeof( T ), ( const volatile void * ) &m_value );
#endif
	}

	T operator=(
		const T value ) noexcept {
		store( value );
		return value;
	}

	operator T(
		void ) const noexcept {
		return load();
	}

	T operator++(
		void ) noexcept {
		return fetch_add( ( T ) 1 ) + ( T ) 1;
	}
	T operator++(
		int ) noexcept {
		return fetch_add( ( T ) 1 );
	}
	T operator--(
		void ) noexcept {
		return fetch_sub( ( T ) 1 ) - ( T ) 1;
	}
	T operator--(
		int ) noexcept {
		return fetch_sub( ( T ) 1 );
	}
	T operator+=(
		const T value ) noexcept {
		return fetch_add( value ) + value;
	}
	T operator-=(
		const T value ) noexcept {
		return fetch_sub( value ) - value;
	}
};

template<> class Terrain3DAtomic< float >
{
	Terrain3DAtomic< uint32_t > m_nBits;

	static uint32_t toBits(
		const float fValue ) noexcept {
		uint32_t nBits;
		memcpy( &nBits, &fValue, sizeof( nBits ) );
		return nBits;
	}

	static float fromBits(
		const uint32_t nBits ) noexcept {
		float fValue;
		memcpy( &fValue, &nBits, sizeof( fValue ) );
		return fValue;
	}

public:
	constexpr Terrain3DAtomic(
		void ) noexcept :
		m_nBits( 0u ) {
	}
	Terrain3DAtomic(
		const float fValue ) noexcept :
		m_nBits( toBits( fValue ) ) {
	}
	Terrain3DAtomic( const Terrain3DAtomic & ) = delete;
	Terrain3DAtomic &operator=( const Terrain3DAtomic & ) = delete;

	float load(
		const t3dMemoryOrder_t eOrder = T3D_MEMORY_ORDER_SEQ_CST ) const noexcept {
		return fromBits( m_nBits.load( eOrder ) );
	}

	void store(
		const float			   fValue,
		const t3dMemoryOrder_t eOrder = T3D_MEMORY_ORDER_SEQ_CST ) noexcept {
		m_nBits.store( toBits( fValue ), eOrder );
	}

	float exchange(
		const float			   fValue,
		const t3dMemoryOrder_t eOrder = T3D_MEMORY_ORDER_SEQ_CST ) noexcept {
		return fromBits( m_nBits.exchange( toBits( fValue ), eOrder ) );
	}

	bool compare_exchange_weak(
		float				  &rExpected,
		const float			   fDesired,
		const t3dMemoryOrder_t eSuccess,
		const t3dMemoryOrder_t eFailure ) noexcept {
		uint32_t   nExpected = toBits( rExpected );
		const bool bExchanged = m_nBits.compare_exchange_weak( nExpected, toBits( fDesired ), eSuccess, eFailure );
		if ( !bExchanged ) {
			rExpected = fromBits( nExpected );
		}
		return bExchanged;
	}

	bool compare_exchange_strong(
		float				  &rExpected,
		const float			   fDesired,
		const t3dMemoryOrder_t eSuccess,
		const t3dMemoryOrder_t eFailure ) noexcept {
		uint32_t   nExpected = toBits( rExpected );
		const bool bExchanged = m_nBits.compare_exchange_strong( nExpected, toBits( fDesired ), eSuccess, eFailure );
		if ( !bExchanged ) {
			rExpected = fromBits( nExpected );
		}
		return bExchanged;
	}

	bool is_lock_free(
		void ) const noexcept {
		return m_nBits.is_lock_free();
	}

	float operator=(
		const float fValue ) noexcept {
		store( fValue );
		return fValue;
	}

	operator float(
		void ) const noexcept {
		return load();
	}
};

typedef Terrain3DAtomic< bool >		t3dAtomicBool_t;
typedef Terrain3DAtomic< uint8_t >	t3dAtomicU8_t;
typedef Terrain3DAtomic< uint16_t > t3dAtomicU16_t;
typedef Terrain3DAtomic< uint32_t > t3dAtomicU32_t;
typedef Terrain3DAtomic< uint64_t > t3dAtomicU64_t;

class Terrain3DMutex
{
	friend class Terrain3DUniqueLock;
	std::mutex m_mutex;

public:
	Terrain3DMutex( void ) = default;
	Terrain3DMutex( const Terrain3DMutex & ) = delete;
	Terrain3DMutex &operator=( const Terrain3DMutex & ) = delete;

	void lock(
		void ) {
		m_mutex.lock();
	}
	void unlock(
		void ) {
		m_mutex.unlock();
	}
	bool try_lock(
		void ) {
		return m_mutex.try_lock();
	}
};

class Terrain3DScopedLock
{
	Terrain3DMutex *m_pMutex;

public:
	explicit Terrain3DScopedLock(
		Terrain3DMutex &rMutex ) :
		m_pMutex( &rMutex ) {
		m_pMutex->lock();
	}
	~Terrain3DScopedLock(
		void ) {
		m_pMutex->unlock();
	}
	Terrain3DScopedLock( const Terrain3DScopedLock & ) = delete;
	Terrain3DScopedLock &operator=( const Terrain3DScopedLock & ) = delete;
};

class Terrain3DUniqueLock
{
	friend class Terrain3DCondition;
	std::unique_lock< std::mutex > m_lock;

public:
	explicit Terrain3DUniqueLock(
		Terrain3DMutex &rMutex ) :
		m_lock( rMutex.m_mutex ) {
	}
	Terrain3DUniqueLock( const Terrain3DUniqueLock & ) = delete;
	Terrain3DUniqueLock &operator=( const Terrain3DUniqueLock & ) = delete;
};

class Terrain3DCondition
{
	std::condition_variable m_condition;

public:
	void wait(
		Terrain3DUniqueLock &rLock ) {
		m_condition.wait( rLock.m_lock );
	}
	void notifyOne(
		void ) {
		m_condition.notify_one();
	}
	void notifyAll(
		void ) {
		m_condition.notify_all();
	}
};

class Terrain3DThread
{
	std::thread m_thread;

public:
	Terrain3DThread( void ) noexcept = default;
	Terrain3DThread( Terrain3DThread && ) noexcept = default;
	Terrain3DThread &operator=( Terrain3DThread && ) noexcept = default;
	Terrain3DThread( const Terrain3DThread & ) = delete;
	Terrain3DThread &operator=( const Terrain3DThread & ) = delete;

	template< typename Fn, typename... Args >
	explicit Terrain3DThread(
		Fn &&fn,
		Args &&...args ) :
		m_thread( std::forward< Fn >( fn ), std::forward< Args >( args )... ) {
	}

	bool joinable(
		void ) const noexcept {
		return m_thread.joinable();
	}
	void join(
		void ) {
		m_thread.join();
	}
};

typedef std::once_flag t3dOnceFlag_t;

template< typename Fn >
static inline void t3dCallOnce(
	t3dOnceFlag_t &rFlag,
	Fn			   fn ) {
	std::call_once( rFlag, fn );
}

static inline void t3dThreadYield(
	void ) {
	std::this_thread::yield();
}

static inline uint64_t t3dSteadyClockUsec(
	void ) {
	return ( uint64_t ) std::chrono::duration_cast< std::chrono::microseconds >(
			   std::chrono::steady_clock::now().time_since_epoch() )
		.count();
}

#endif // TERRAIN3D_SYNC_H
