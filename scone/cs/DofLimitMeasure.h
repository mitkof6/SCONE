#pragma once

#include "Measure.h"
#include "../core/InitFromPropNode.h"
#include "../core/Range.h"
#include "../sim/Dof.h"
#include "../core/Statistic.h"

namespace scone
{
	namespace cs
	{
		class DofLimitMeasure : public Measure
		{
		public:
			DofLimitMeasure( const PropNode& props, opt::ParamSet& par, sim::Model& model, const sim::Area& area );
			virtual ~DofLimitMeasure();

			virtual void UpdateControls( sim::Model& model, double timestamp ) override;
			virtual double GetResult( sim::Model& model ) override;
			virtual PropNode GetReport();
			virtual String GetSignature() override;

		private:
			struct Limit
			{
				Limit( const PropNode& props, sim::Model& model );
				sim::Dof& dof;
				Range< Real > range;
				Real squared_range_penalty;
				Real squared_force_penalty;
				Statistic<> penalty;
			};

			PropNode m_Report;
			std::vector< Limit > m_Limits;
		};
	}
}