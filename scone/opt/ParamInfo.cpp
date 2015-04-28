#include "stdafx.h"
#include "ParamInfo.h"

#include "Rng/GlobalRng.h"
#include "../core/InitFromPropNode.h"

namespace scone
{
	namespace opt
	{

		ParamInfo::ParamInfo( const String& i_name, double i_init_mean, double i_init_std, double i_init_min, double i_init_max, double i_min, double i_max ) :
		name( i_name ),
		init_mean( i_init_mean ), init_std( i_init_std ),
		init_min( i_init_min ),	init_max( i_init_max ),
		min( i_min ), max( i_max ),	is_free( true )
		{
		}

		ParamInfo::ParamInfo( const String& i_name, const PropNode& props ) :
		name( i_name )
		{
			INIT_FROM_PROP( props, init_min, 0.0 );
			INIT_FROM_PROP( props, init_max, 0.0 );
			INIT_FROM_PROP( props, init_mean, 0.0 );
			INIT_FROM_PROP( props, init_std, 0.0 );
			INIT_FROM_PROP( props, min, REAL_MIN );
			INIT_FROM_PROP( props, max, REAL_MAX );
			INIT_FROM_PROP( props, is_free, true ); /// TODO: some smarter default
			props.Touch();
		}

		double ParamInfo::GetInitialValue() const
		{
			if ( is_free )
			{
				if ( init_mean != 0.0 || init_std != 0.0 )
					return GetRestrained( Rng::gauss( init_mean, Square( init_std ) ), min, max );
				else
					return Rng::uni( init_min, init_max );
			}
			else return init_mean;
		}
	}
}
