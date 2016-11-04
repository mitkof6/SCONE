#include "Settings.h"

#include <QDialog>
#include "ui_SconeSettings.h"
#include "scone/core/PropNode.h"
#include "scone/core/system_tools.h"
#include "qt_tools.h"
#include "flut/system/path.hpp"

namespace scone
{
	int Settings::showDialog( QWidget* parent )
	{
		QDialog* dlg = new QDialog( parent );
		Ui::Settings settings;
		settings.setupUi( dlg );

		// init settings
		PropNode pn = GetSconeSettings();
		settings.scenariosFolder->setText( make_qt( flut::path( GetFolder( SCONE_SCENARIO_FOLDER ) ).make_preferred() ) );
		settings.modelsFolder->setText( make_qt( flut::path(GetFolder( SCONE_MODEL_FOLDER ) ).make_preferred() ) );
		settings.resultsFolder->setText( make_qt( flut::path(GetFolder( SCONE_OUTPUT_FOLDER ) ).make_preferred() ) );
		settings.geometryFolder->setText( make_qt( flut::path(GetFolder( SCONE_GEOMETRY_FOLDER ) ).make_preferred() ) );

		int ret = dlg->exec();
		if ( ret == QDialog::Accepted )
		{
			// update settings
			pn.Set( "folders.scenarios", settings.scenariosFolder->text().toStdString() );
			pn.Set( "folders.models", settings.modelsFolder->text().toStdString() );
			pn.Set( "folders.results", settings.resultsFolder->text().toStdString() );
			pn.Set( "folders.geometry", settings.geometryFolder->text().toStdString() );

			SaveSconeSettings( pn );
		}

		return ret;
	}
}