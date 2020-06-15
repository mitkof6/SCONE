/*
** system_tools.cpp
**
** Copyright (C) 2013-2019 Thomas Geijtenbeek and contributors. All rights reserved.
**
** This file is part of SCONE. For more information, see http://scone.software.
*/

#include "system_tools.h"

#include "xo/system/system_tools.h"
#include "xo/filesystem/path.h"
#include "xo/container/prop_node_tools.h"
#include "xo/xo_types.h"
#include "xo/filesystem/filesystem.h"

#include "Settings.h"
#include "Log.h"
#include "Exception.h"

namespace scone
{
	String g_Version;
	path g_RootFolder;

	path GetInstallFolder()
	{
		if ( g_RootFolder.empty() )
		{
			for ( g_RootFolder = xo::get_application_dir(); !g_RootFolder.empty(); g_RootFolder = g_RootFolder.parent_path() )
			{
				if ( xo::exists( g_RootFolder / ".sconeroot" ) )
				{
					g_RootFolder = xo::load_string( g_RootFolder / ".sconeroot" );
					break;
				}
				else if ( xo::exists( g_RootFolder / ".version" ) )
					break;
			}
			SCONE_THROW_IF( g_RootFolder.empty(), "Could not detect installation root folder, please run .updateversion.bat or .updateversion.sh" );
			log::debug( "SCONE root folder: ", g_RootFolder );
		}
		return g_RootFolder;
	}

	path GetFolder( const String& folder )
	{
		return GetSconeSettings().get< path >( "folders." + folder );
	}

	path GetSettingsFolder()
	{
		return xo::get_config_dir() / "SCONE";
	}

	path GetDataFolder()
	{
		return xo::get_documents_dir() / "SCONE";
	}

	path GetFolder( SconeFolder folder )
	{
		switch ( folder )
		{
		case SCONE_ROOT_FOLDER: return GetInstallFolder();
		case SCONE_RESULTS_FOLDER: return GetFolder( "results" );
		case SCONE_SCENARIO_FOLDER: return GetFolder( "scenarios" );
		case SCONE_GEOMETRY_FOLDER: return GetFolder( "geometry" );
		case SCONE_RESOURCE_FOLDER: return GetInstallFolder() / "resources";
		case SCONE_UI_RESOURCE_FOLDER: return GetInstallFolder() / "resources/ui";
		default: SCONE_THROW( "Unknown folder type" );
		}
	}

	path FindFile( const path& p )
	{
		if ( xo::current_find_file_path().empty() )
			log::warning( "No current find file path set, resorting to global current path" );
		
		return xo::find_file( {
			p,
			p.filename(), // filename without rel path
			path( ".." ) / p.filename() // filename in parent folder
			} );
	}
}
