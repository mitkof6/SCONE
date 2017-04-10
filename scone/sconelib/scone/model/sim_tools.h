#pragma once

#include "scone/core/core.h"
#include "scone/core/Vec3.h"
#include "scone/core/math.h"

namespace scone
{
	SCONE_API Vec3 GetGroundCop( const Vec3& force, const Vec3& moment, Real min_force = REAL_WIDE_EPSILON );
}