#ifndef TERRAIN3D_BINDING_H
#define TERRAIN3D_BINDING_H

#include <godot_cpp/core/class_db.hpp>
#include <initializer_list>

// Keep method names, argument names and PropertyInfo explicit: they are public
// Godot contracts. This macro only removes the repeated registration sequence.
#define T3D_BIND_PROPERTY( cls, setter, getter, argument, info, setter_name, getter_name )                             \
	do {                                                                                                               \
		godot::ClassDB::bind_method( godot::D_METHOD( setter_name, argument ), &cls::setter );                         \
		godot::ClassDB::bind_method( godot::D_METHOD( getter_name ), &cls::getter );                                   \
		ADD_PROPERTY( info, setter_name, getter_name );                                                                \
	} while ( 0 )

struct t3dPropertyUsageRule_t
{
	const char *m_pName;
	bool		m_bApply;
	uint32_t	m_nUsage;
	bool		m_bMerge = false;
};

// First applicable exact-name rule wins. Merge preserves existing usage flags.
inline bool t3dApplyPropertyUsageRules(
	godot::PropertyInfo							   &rProperty,
	std::initializer_list< t3dPropertyUsageRule_t > rules ) {
	for ( const auto &rule : rules ) {
		if ( rule.m_bApply && rProperty.name == godot::StringName( rule.m_pName ) ) {
			rProperty.usage = rule.m_bMerge ? rProperty.usage | rule.m_nUsage : rule.m_nUsage;
			return true;
		}
	}
	return false;
}

inline bool t3dApplyPropertyPrefixUsage(
	godot::PropertyInfo &rProperty,
	const char			*pPrefix,
	const char			*pToggle,
	bool				 bApply,
	uint32_t			 nUsage = godot::PROPERTY_USAGE_NO_EDITOR ) {
	if ( bApply && rProperty.name != godot::StringName( pToggle ) && rProperty.name.begins_with( pPrefix ) ) {
		rProperty.usage = nUsage;
		return true;
	}
	return false;
}

#endif // TERRAIN3D_BINDING_H
