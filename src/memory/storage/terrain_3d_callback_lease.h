#ifndef TERRAIN3D_CALLBACK_LEASE_H
#define TERRAIN3D_CALLBACK_LEASE_H

// Release state supplies its typed callback payload and release eligibility.
// Pools retain pinning, generation validation and shutdown synchronization.
template< typename TView, typename TReleaseState > class Terrain3DCallbackLease
{
	TReleaseState m_release;
	TView		  m_view;

	void disarm() {
		m_release = {};
		m_view = {};
	}

public:
	Terrain3DCallbackLease() = default;
	Terrain3DCallbackLease(
		const TReleaseState &rRelease,
		const TView			&rView ) :
		m_release( rRelease ),
		m_view( rView ) {
	}
	~Terrain3DCallbackLease() {
		reset();
	}
	Terrain3DCallbackLease( const Terrain3DCallbackLease & ) = delete;
	Terrain3DCallbackLease &operator=( const Terrain3DCallbackLease & ) = delete;
	Terrain3DCallbackLease(
		Terrain3DCallbackLease &&rOther ) noexcept :
		m_release( rOther.m_release ),
		m_view( rOther.m_view ) {
		rOther.disarm();
	}
	Terrain3DCallbackLease &operator=(
		Terrain3DCallbackLease &&rOther ) noexcept {
		if ( this != &rOther ) {
			reset();
			m_release = rOther.m_release;
			m_view = rOther.m_view;
			rOther.disarm();
		}
		return *this;
	}
	void reset() {
		m_release.release();
		disarm();
	}
	bool valid() const {
		return m_view.valid();
	}
	const TView &view() const {
		return m_view;
	}
};

#endif // TERRAIN3D_CALLBACK_LEASE_H
