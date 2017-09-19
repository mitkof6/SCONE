#include "PatternNeuron.h"
#include "NeuralController.h"
#include "..\model\Model.h"
#include <cmath>
#include "flut\math\math.hpp"
#include "spot\par_tools.h"

namespace scone
{
	PatternNeuron::PatternNeuron( const PropNode& pn, Params& par, NeuralController& nc ) :
	Neuron( pn, par, nc ),
	model_( nc.GetModel() )
	{
		INIT_PAR( pn, par, t0_, 0.0 );
		INIT_PAR( pn, par, sigma_, 0.5 );
		INIT_PAR( pn, par, period_, 1.0 );
	}

	scone::activation_t PatternNeuron::GetOutput() const
	{
		auto t = std::fmod( model_.GetTime() - t0_ + period_, period_ ) - 0.5 * period_;
		return exp( -( t * t ) / sigma_ * sigma_ );
	}
}
