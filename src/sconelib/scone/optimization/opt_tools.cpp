/*
** opt_tools.cpp
**
** Copyright (C) 2013-2019 Thomas Geijtenbeek and contributors. All rights reserved.
**
** This file is part of SCONE. For more information, see http://scone.software.
*/

#include "opt_tools.h"
#include "scone/core/types.h"
#include "scone/core/Factories.h"
#include "scone/optimization/SimulationObjective.h"
#include "scone/core/profiler_config.h"

#include "xo/time/timer.h"
#include "xo/container/prop_node_tools.h"
#include "xo/filesystem/filesystem.h"

using xo::timer;

namespace scone
{
	SCONE_API bool LogUnusedProperties( const PropNode& pn )
	{
		// report unused properties
		if ( pn.count_unaccessed() > 0 )
		{
			log::warning( "Warning, unused properties:" );
			xo::log_unaccessed( pn );
			return true;
		}
		else return false;
	}

	PropNode EvaluateScenario( const PropNode& scenario_pn, const path& par_file, const path& output_base )
	{
		bool store_data = !output_base.empty();

		auto optProp = FindFactoryProps( GetOptimizerFactory(), scenario_pn, "Optimizer" );
		auto objProp = FindFactoryProps( GetObjectiveFactory(), optProp.props(), "Objective" );
		ObjectiveUP obj = CreateObjective( objProp, par_file.parent_path() );
		ModelObjective& so = dynamic_cast<ModelObjective&>( *obj );

		// report unused properties
		LogUnusedProperties( objProp.props() );

		// create model
		ModelUP model;
		if ( par_file.empty() || par_file.extension_no_dot() == "scone" )
		{
			// no par file was given, try to use init_file
			// IMPORTANT: this uses the parameter MEAN of the init_file
			// as to be consistent with running a scenario from inside SCONE studio
			// #todo: combine this code with CreateModelObjective, since the same is happening there
			if ( auto init_file = optProp.props().try_get< path >( "init_file" ) )
				so.info().import_mean_std( *init_file, optProp.props().get< bool >( "use_init_file_std", true ) );
			SearchPoint searchPoint( so.info() );
			model = so.CreateModelFromParams( searchPoint );
		}
		else model = so.CreateModelFromParFile( par_file );

		// set data storage
		SCONE_ASSERT( model );
		model->SetStoreData( store_data );

		timer tmr;
		auto result = so.EvaluateModel( *model, xo::stop_token() );
		auto duration = tmr().seconds();

		// write results
		if ( store_data )
		{
			auto files = model->WriteResults( output_base );
			log::info( "Results written to " + output_base.str() + "*" );
		}

		// collect statistics
		PropNode statistics;
		statistics.set( "result", so.GetReport( *model ) );
		statistics.set( "simulation time", model->GetTime() );
		statistics.set( "performance (x real-time)", model->GetTime() / duration );

		return statistics;
	}

	path FindScenario( const path& file )
	{
		if ( file.extension_no_dot() == "scone" || file.extension_no_dot() == "xml" )
			return file;
		auto folder = file.parent_path();
		return xo::find_file( { path( file ).replace_extension( "scone" ), folder / "config.scone", folder / "config.xml" } );
	}
}
