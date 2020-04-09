/*
** StateComponent.h
**
** Copyright (C) 2013-2019 Thomas Geijtenbeek and contributors. All rights reserved.
**
** This file is part of SCONE. For more information, see http://scone.software.
*/

#pragma once

#include "scone/core/Exception.h"
#include "scone/core/platform.h"
#include "scone/core/types.h"
#include "scone/core/HasName.h"
#include "scone/optimization/Params.h"
#include "scone/core/PropNode.h"
#include <vector>

namespace scone
{
	/// The StateComponent is used to define components that contain
	/// differential equations. The user must define a function that
	/// returns the initial condition and the calculation of the state
	/// derivative.
	class SCONE_API StateComponent : public HasName
	{
	public:
		StateComponent( const PropNode& props, Params& par )
			: INIT_MEMBER_REQUIRED( props, name ) {};

		/// Name of the state component (required).
		String name;
		virtual const String& GetName() const override { return name; }

		/// interface

		/// Return the initial conditions of this component.
		virtual std::vector< Real > GetInitialCondition() const
		{ SCONE_THROW_NOT_IMPLEMENTED }
		/// Calculates the state derivative xdot = f(t, x).
		virtual std::vector< Real > CalcStateDerivatives( Real t, std::vector< Real > x0 ) const
		{ SCONE_THROW_NOT_IMPLEMENTED }
		/// If this component models a hybrid system (discrete events) then one
		/// should make this function true.
		virtual bool HasDiscreteEvent() const { return false; }
		/// If +1 then triggered on rising sign transition. If -1 triggered on
		/// falling sign transition. If 0 triggered on both transitions.
		virtual int TriggeredOnSign() const { return 0; }
		/// Implements a zero crossing function that signifies an event.
		virtual Real CheckForEvent( Real t, std::vector< Real > x ) const
		{ SCONE_THROW_NOT_IMPLEMENTED }
		/// A function that is called when an event is detected and returns a
		/// the new state. It is assumed that any event modifies only the state.
		virtual std::vector< Real > EventHandler( Real t, std::vector< Real > x ) const
		{ SCONE_THROW_NOT_IMPLEMENTED }
	};
}
