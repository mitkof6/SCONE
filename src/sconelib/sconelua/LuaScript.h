#pragma once

#include "scone/optimization/Params.h"
#include "scone/model/Model.h"
#include "platform.h"

#include <sol/sol.hpp>

namespace scone
{
	class SCONE_LUA_API LuaScript
	{
	public:
		LuaScript( const PropNode& props, Params& par, Model& model );
		virtual ~LuaScript();

		bool Run();

		Params& par_;
		Model& model_;
		sol::state state_;
		sol::load_result script_;
		xo::path script_file_;
	};	
}