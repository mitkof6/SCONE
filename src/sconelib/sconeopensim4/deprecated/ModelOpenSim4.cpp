/*
** ModelOpenSim4.cpp
**
** Copyright (C) 2013-2019 Thomas Geijtenbeek and contributors. All rights reserved.
**
** This file is part of SCONE. For more information, see http://scone.software.
*/

#include "scone/core/Exception.h"
#include "scone/core/Log.h"
#include "scone/core/Factories.h"
#include "scone/core/StorageIo.h"

#include "ModelOpenSim4.h"
#include "BodyOpenSim4.h"
#include "MuscleOpenSim4.h"
#include "SimulationOpenSim4.h"
#include "JointOpenSim4.h"
#include "DofOpenSim4.h"
#include "simbody_tools.h"

#include <OpenSim/OpenSim.h>
#include <OpenSim/Simulation/Model/Umberger2010MuscleMetabolicsProbe.h>
#include <OpenSim/Simulation/Model/Bhargava2004MuscleMetabolicsProbe.h>

#include "scone/core/system_tools.h"
#include "scone/core/Profiler.h"

#include "xo/string/string_tools.h"
#include "xo/string/pattern_matcher.h"
#include "xo/container/container_tools.h"
#include "xo/utility/file_resource_cache.h"

#include <thread>
#include <mutex>

using std::cout;
using std::endl;

namespace scone
{
	std::mutex g_SimBodyMutex;

	xo::file_resource_cache< OpenSim::Model > g_ModelCache( []( const path& p ) { return new OpenSim::Model( p.string() ); } );
	xo::file_resource_cache< OpenSim::Storage > g_StorageCache( []( const path& p ) { return new OpenSim::Storage( p.string() ); } );

	// OpenSim4 controller that calls scone controllers
	class ControllerDispatcher : public OpenSim::Controller
	{
	    OpenSim_DECLARE_CONCRETE_OBJECT(ControllerDispatcher, OpenSim::Controller);
	public:
		ControllerDispatcher( ModelOpenSim4& model ) : m_Model( model ) { };
		virtual void computeControls( const SimTK::State& s, SimTK::Vector &controls ) const override;
		// virtual ControllerDispatcher* clone() const override { return new ControllerDispatcher( *this ); }
		// virtual const std::string& getConcreteClassName() const override { return "ControllerDispatcher"/*TODO*/; }

	private:
		SimTK::ReferencePtr<ModelOpenSim4> m_Model;
	};

	// Constructor
	ModelOpenSim4::ModelOpenSim4( const PropNode& props, Params& par ) :
		Model( props, par ),
		m_pOsimModel( nullptr ),
		m_pTkState( nullptr ),
		m_pControllerDispatcher( nullptr ),
		m_PrevIntStep( -1 ),
		m_PrevTime( 0.0 ),
		m_pProbe( 0 ),
		m_Mass( 0.0 ),
		m_BW( 0.0 )
	{
		SCONE_PROFILE_FUNCTION;

		path model_file;
		path state_init_file;
		String probe_class;

		INIT_PROP( props, integration_accuracy, 0.001 );
		INIT_PROP( props, integration_method, String( "SemiExplicitEuler2" ) );

		INIT_PROP_REQUIRED( props, model_file );
		INIT_PROP( props, state_init_file, path() );
		INIT_PROP( props, probe_class, String() );

		// TODO: Must make more generic.
		INIT_PROP( props, initial_load_dof, "/jointset/ground_pelvis/pelvis_ty/value" );
		INIT_PROP( props, create_body_forces, false );

		// always set create_body_forces when there's a PerturbationController
		// TODO: think of a nicer, more generic way of dealing with this issue
		if ( auto* controller = props.try_get_child( "Controller" ) )
		{
			for ( auto& cprops : controller->select( "Controller" ) )
				create_body_forces |= cprops.second.get<string>( "type" ) == "PerturbationController";
		}

		// create new OpenSim Model using resource cache
		{
			SCONE_PROFILE_SCOPE( "CreateModel" );
			model_file = FindFile( model_file );
			m_pOsimModel = g_ModelCache( model_file );
			AddExternalResource( model_file );
		}

		// create torque and point actuators
		if ( create_body_forces )
		{
			SCONE_PROFILE_SCOPE( "SetupBodyForces" );
			for ( int idx = 0; idx < m_pOsimModel->getBodySet().getSize(); ++idx )
			{
				OpenSim::ConstantForce* cf = new OpenSim::ConstantForce( m_pOsimModel->getBodySet().get( idx ).getName() );
				cf->set_point_is_global( false );
				cf->set_force_is_global( true );
				cf->set_torque_is_global( false );
				m_BodyForces.push_back( cf );
				m_pOsimModel->addForce( cf );
			}
		}

		{
			SCONE_PROFILE_SCOPE( "SetupOpenSimParameters" );

			// change model properties
			if ( auto* model_pars = props.try_get_child( "OpenSimProperties" ) )
				SetOpenSimProperties( *model_pars, par );

			// create controller dispatcher (ownership is automatically passed to OpenSim::Model)
			m_pControllerDispatcher = new ControllerDispatcher( *this );
			m_pOsimModel->addController( m_pControllerDispatcher );

			// create probe (ownership is automatically passed to OpenSim::Model)
			// OpenSim: this doesn't work! It either crashes or gives inconsistent results
			if ( probe_class == "Umberger2010MuscleMetabolicsProbe" )
			{
				auto probe = new OpenSim::Umberger2010MuscleMetabolicsProbe( true, true, true, true );
				GetOsimModel().addProbe( probe );
				for ( int idx = 0; idx < GetOsimModel().getMuscles().getSize(); ++idx )
				{
					OpenSim::Muscle& mus = GetOsimModel().getMuscles().get( idx );
					double mass = ( mus.getMaxIsometricForce() / 0.25e6 ) * 1059.7 * mus.getOptimalFiberLength(); // Derived from OpenSim doxygen
					probe->addMuscle( mus.getName(), 0.5 );
				}
				probe->setInitialConditions( SimTK::Vector( 1, 0.0 ) );
				probe->setOperation( "integrate" );
				m_pProbe = probe;
			}
		}

		// Initialize the system
		// This is not thread-safe in case an exception is thrown, so we add a mutex guard
		{
			SCONE_PROFILE_SCOPE( "InitSystem" );
			g_SimBodyMutex.lock();
			m_pTkState = &m_pOsimModel->initSystem();
			g_SimBodyMutex.unlock();
		}

		// create model component wrappers and sensors
		{
			SCONE_PROFILE_SCOPE( "CreateWrappers" );
			CreateModelWrappers( props, par );
			SetModelProperties( props, par );
		}

		{
			SCONE_PROFILE_SCOPE( "InitVariables" );
			// initialize cached variables to save computation time
			m_Mass = m_pOsimModel->getMultibodySystem().getMatterSubsystem().calcSystemMass( m_pOsimModel->getWorkingState() );
			m_BW = GetGravity().length() * GetMass();
			ValidateDofAxes();
		}

		// Create the integrator for the simulation.
		{
			SCONE_PROFILE_SCOPE( "InitIntegrators" );

			using Integ = OpenSim::Manager::IntegratorMethod;
			if ( integration_method == "RungeKuttaMerson" ) {
				m_integratorMethod = static_cast<int>(Integ::RungeKuttaMerson);
				m_pTkIntegrator = std::unique_ptr< SimTK::Integrator >( new SimTK::RungeKuttaMersonIntegrator( m_pOsimModel->getMultibodySystem() ) );
			} else if ( integration_method == "RungeKutta2" ) {
				m_integratorMethod = static_cast<int>(Integ::RungeKutta2);
				m_pTkIntegrator = std::unique_ptr< SimTK::Integrator >( new SimTK::RungeKutta2Integrator( m_pOsimModel->getMultibodySystem() ) );
			} else if ( integration_method == "RungeKutta3" ) {
				m_integratorMethod = static_cast<int>(Integ::RungeKutta3);
				m_pTkIntegrator = std::unique_ptr< SimTK::Integrator >( new SimTK::RungeKutta3Integrator( m_pOsimModel->getMultibodySystem() ) );
			} else if ( integration_method == "SemiExplicitEuler2" ) {
				m_integratorMethod = static_cast<int>(Integ::SemiExplicitEuler2);
				m_pTkIntegrator = std::unique_ptr< SimTK::Integrator >( new SimTK::SemiExplicitEuler2Integrator( m_pOsimModel->getMultibodySystem() ) );
			} else {
				SCONE_THROW( "Invalid integration method: " + xo::quoted( integration_method ) );
			}

			m_pTkIntegrator->setAccuracy( integration_accuracy );
			m_pTkIntegrator->setMaximumStepSize( max_step_size );
			m_pTkIntegrator->resetAllStatistics();
		}

		// read initial state
		{
			SCONE_PROFILE_SCOPE( "InitState" );
			InitStateFromTk();
			if ( !state_init_file.empty() )
			{
				state_init_file = FindFile( state_init_file );
				ReadState( state_init_file );
				AddExternalResource( state_init_file );
			}

			// update state variables if they are being optimized
			auto sio = props.try_get_child( "state_init_optimization" );
			auto offset = sio ? sio->try_get_child( "offset" ) : props.try_get_child( "initial_state_offset" );
			if ( offset )
			{
				bool symmetric = sio ? sio->get( "symmetric", false ) : props.get( "initial_state_offset_symmetric", false );
				auto inc_pat = xo::pattern_matcher( sio ? sio->get< String >( "include_states", "*" ) : props.get< String >( "initial_state_offset_include", "*" ), ";" );
				auto ex_pat = xo::pattern_matcher(
					( sio ? sio->get< String >( "exclude_states", "" ) : props.get< String >( "initial_state_offset_exclude", "" ) ) + ";*.activation;*.fiber_length", ";" );
				for ( index_t i = 0; i < m_State.GetSize(); ++i )
				{
					const String& state_name = m_State.GetName( i );
					if ( inc_pat( state_name ) && !ex_pat( state_name ) )
					{
						auto par_name = symmetric ? GetNameNoSide( state_name ) : state_name;
						m_State[ i ] += par.get( par_name + ".offset", *offset );
					}
				}
			}

			// apply and fix state
			if ( !initial_load_dof.empty() && initial_load > 0 && !GetContactGeometries().empty() )
			{
				CopyStateToTk();
				FixTkState( initial_load * GetBW() );
				CopyStateFromTk();
			}
		}

		// Realize acceleration because controllers may need it and in this way the results are consistent
		{
			SCONE_PROFILE_SCOPE( "RealizeSystem" );
			// Create a manager to run the simulation. Can change manager options to save run time and memory or print more information
			m_pOsimManager = std::unique_ptr< OpenSim::Manager >( new OpenSim::Manager( *m_pOsimModel ) );
			m_pOsimManager->setWriteToStorage( false );
			m_pOsimManager->setPerformAnalyses( false );
			// m_pOsimManager->setInitialTime( 0.0 );
			// m_pOsimManager->setFinalTime( 0.0 );

			m_pOsimManager->setIntegratorMethod(OpenSim::Manager::IntegratorMethod(m_integratorMethod));
			m_pOsimManager->setIntegratorAccuracy( integration_accuracy );
			m_pOsimManager->setIntegratorMaximumStepSize( max_step_size );

			m_pOsimModel->getMultibodySystem().realize( GetTkState(), SimTK::Stage::Acceleration );
		}

		// create and initialize controllers
		CreateControllers( props, par );

		log::info( "Successfully constructed ", GetName(), "; dofs=", GetDofs().size(), " muscles=", GetMuscles().size(), " mass=", GetMass() );
	}

	ModelOpenSim4::~ModelOpenSim4() {}

	void ModelOpenSim4::CreateModelWrappers( const PropNode& pn, Params& par )
	{
		SCONE_ASSERT( m_pOsimModel && m_Bodies.empty() && m_Joints.empty() && m_Dofs.empty() && m_Actuators.empty() && m_Muscles.empty() );

		// Create wrappers for bodies
		m_Bodies.emplace_back( new BodyOpenSim4( *this, m_pOsimModel->getGround() ) );
		for ( int idx = 0; idx < m_pOsimModel->getBodySet().getSize(); ++idx )
			m_Bodies.emplace_back( new BodyOpenSim4( *this, m_pOsimModel->getBodySet().get( idx ) ) );

		// setup hierarchy and create wrappers
		m_RootLink = CreateLinkHierarchy( m_pOsimModel->updGround() );

		// create wrappers for dofs
		for ( int idx = 0; idx < m_pOsimModel->getCoordinateSet().getSize(); ++idx ) {
			m_Dofs.emplace_back( new DofOpenSim4( *this, m_pOsimModel->getCoordinateSet().get( idx ) ) );
		}

		// create contact geometries
		for ( int idx = 0; idx < m_pOsimModel->getContactGeometrySet().getSize(); ++idx )
		{
			if ( auto cg = dynamic_cast< OpenSim::ContactSphere* >( &m_pOsimModel->getContactGeometrySet().get( idx ) ) )
			{
				auto& body = *FindByName( m_Bodies, cg->getFrame().findBaseFrame().getName() );
				const auto& X_BF = cg->getFrame().findTransformInBaseFrame();
				const auto X_FP = cg->getTransform();
				const auto loc = (X_BF * X_FP).p();
				m_ContactGeometries.emplace_back( body, from_osim( loc ), cg->getRadius() );
			}
		}

		// Create wrappers for actuators
		for ( int idx = 0; idx < m_pOsimModel->getActuators().getSize(); ++idx )
		{
			// OpenSim: Set<T>::get( idx ) is const but returns non-const reference, is this a bug?
			OpenSim::Actuator& osAct = m_pOsimModel->getActuators().get( idx );
			if ( OpenSim::Muscle* osMus = dynamic_cast<OpenSim::Muscle*>( &osAct ) )
			{
				m_Muscles.emplace_back( new MuscleOpenSim4( *this, *osMus ) );
				m_Actuators.push_back( m_Muscles.back().get() );
			}
			else if ( OpenSim::CoordinateActuator* osCo = dynamic_cast< OpenSim::CoordinateActuator* >( &osAct ) )
			{
				// add corresponding dof to list of actuators
				auto& dof = dynamic_cast<DofOpenSim4&>( *FindByName( m_Dofs, osCo->getCoordinate()->getName() ) );
				dof.SetCoordinateActuator( osCo );
				m_Actuators.push_back( &dof );
			}
			else if ( OpenSim::PointActuator* osPa = dynamic_cast<OpenSim::PointActuator*>( &osAct ) )
			{
				// do something?
			}
		}

		// create BodySensor
		//m_BalanceSensor = BalanceSensorUP( new BalanceSensor( * this ) );

		// create legs and connect stance_contact forces
		if ( Link* left_femur = m_RootLink->FindLink( "femur_l" ) )
		{
			Link& left_foot = left_femur->GetChild( 0 ).GetChild( 0 );
			m_Legs.emplace_back( new Leg( *left_femur, left_foot, m_Legs.size(), LeftSide ) );
			dynamic_cast<BodyOpenSim4&>( left_foot.GetBody() ).ConnectContactForce( "foot_l" );
		}

		if ( Link* right_femur = m_RootLink->FindLink( "femur_r" ) )
		{
			Link& right_foot = right_femur->GetChild( 0 ).GetChild( 0 );
			m_Legs.emplace_back( new Leg( *right_femur, right_femur->GetChild( 0 ).GetChild( 0 ), m_Legs.size(), RightSide ) );
			dynamic_cast<BodyOpenSim4&>( right_foot.GetBody() ).ConnectContactForce( "foot_r" );
		}
	}

	void ModelOpenSim4::SetModelProperties( const PropNode &pn, Params& par )
	{
		if ( auto* model_props = pn.try_get_child( "ModelProperties" ) )
		{
			for ( auto& mp : *model_props )
			{
				int usage = 0;
				if ( mp.first == "Actuator" )
				{
					for ( auto act : xo::make_view_if( m_Actuators, xo::pattern_matcher( mp.second.get< String >( "name" ) ) ) )
					{
						SCONE_THROW_IF( !use_fixed_control_step_size, "Custom Actuator Delay only works with use_fixed_control_step_size" );
						act->SetActuatorDelay( mp.second.get< TimeInSeconds >( "delay", 0.0 ) * sensor_delay_scaling_factor, fixed_control_step_size );
						++usage;
					}
				}

				if ( usage == 0 )
					log::warning( "Unused model property: ", mp.second.get< String >( "name" ) );
			}
		}
	}

	void ModelOpenSim4::SetOpenSimProperties( const PropNode& osim_pars, Params& par )
	{
		for ( auto& param : osim_pars )
		{
			if ( param.first == "Force" )
			{
				auto& name = param.second[ "name" ].raw_value();
				xo::pattern_matcher pm( name );
				int count = 0;
				for ( int i = 0; i < m_pOsimModel->updForceSet().getSize(); ++i )
				{
					auto& force = m_pOsimModel->updForceSet().get( i );
					if ( pm( force.getName() ) )
						SetOpenSimProperty( force, param.second, par ), ++count;
				}
				if ( count == 0 )
					log::warning( "Could not find OpenSim Object that matches ", name );
			}
		}
	}

	void ModelOpenSim4::SetOpenSimProperty( OpenSim::Object& os, const PropNode& pn, Params& par )
	{
		// we have a match!
		String prop_str = pn.get< String >( "property" );
		ScopedParamSetPrefixer prefix( par, pn.get< String >( "name" ) + "." );
		double value = par.get( prop_str, pn.get_child( "value" ) );
		if ( os.hasProperty( prop_str ) )
		{
			auto& prop = os.updPropertyByName( prop_str ).updValue< double >();
			prop = pn.get( "factor", false ) ? prop * value : value;
		}
	}

	std::vector<path> ModelOpenSim4::WriteResults( const path& file ) const
	{
		std::vector<path> files;
		WriteStorageSto( m_Data, file + ".sto", ( file.parent_path().filename() / file.stem() ).string() );
		files.push_back( file + ".sto" );

		if ( GetController() ) xo::append( files, GetController()->WriteResults( file ) );
		if ( GetMeasure() ) xo::append( files, GetMeasure()->WriteResults( file ) );

		return files;
	}

	void ModelOpenSim4::RequestTermination()
	{
		Model::RequestTermination();
		m_pOsimManager->halt();
	}

	Vec3 ModelOpenSim4::GetComPos() const
	{
		return from_osim( m_pOsimModel->calcMassCenterPosition( GetTkState() ) );
	}

	Vec3 ModelOpenSim4::GetComVel() const
	{
		return from_osim( m_pOsimModel->calcMassCenterVelocity( GetTkState() ) );
	}

	Vec3 ModelOpenSim4::GetComAcc() const
	{
		return from_osim( m_pOsimModel->calcMassCenterAcceleration( GetTkState() ) );
	}

	scone::Vec3 ModelOpenSim4::GetGravity() const
	{
		return from_osim( m_pOsimModel->getGravity() );
	}

	bool is_body_equal( BodyUP& body, OpenSim::Body& osBody )
	{
		return dynamic_cast<BodyOpenSim4&>( *body ).m_osBody == osBody;
	}

	scone::LinkUP ModelOpenSim4::CreateLinkHierarchy( const OpenSim::PhysicalFrame& osBody, Link* parent )
	{
		LinkUP link;

		// find the Body
		auto itBody = std::find_if( m_Bodies.begin(), m_Bodies.end(), [&]( BodyUP& body )
		{ return dynamic_cast<BodyOpenSim4&>( *body ).m_osBody == osBody; } );
		SCONE_ASSERT( itBody != m_Bodies.end() );

		const SimTK::MobilizedBodyIndex thisMBI = osBody.getMobilizedBodyIndex();
		// Get parent MobilizedBodyIndex for osBody.
		const auto& MB = osBody.getMobilizedBody();


		// find the Joint (if any)
		if ( &osBody != &osBody.getComponent<OpenSim::Ground>("/ground") )
		{
			const auto& parentMBI =
					MB.getParentMobilizedBody().getMobilizedBodyIndex();

		   	OpenSim::Joint* osimJointForThisLink = nullptr;
			// Search OpenSim for the Body with the same MBI as osBody's MBI.
			for ( auto iter = m_Bodies.begin(); iter != m_Bodies.end(); ++iter )
			{
			    if (osimJointForThisLink) break;
				BodyOpenSim4& body = dynamic_cast<BodyOpenSim4&>( **iter );

				const auto& MBI = body.m_osBody.getMobilizedBodyIndex();
				if (MBI == parentMBI) {
					// Search all OpenSim Joints for (parent=thisMBI && child=parentMBI) ||
					// 	(parent=parentMBI && child=thisMBI).
					auto& osModel =
							const_cast<OpenSim::Component&>(osBody.getRoot());
					auto osJoints = osModel.updComponentList<OpenSim::Joint>();
					for (auto& osJoint : osJoints) {
						const auto* parentBase = &osJoint.getParentFrame().findBaseFrame();
						const auto* childBase = &osJoint.getChildFrame().findBaseFrame();
						if ((parentBase == &body.m_osBody && childBase == &osBody) ||
								(parentBase == &osBody && childBase == &body.m_osBody)) {
							osimJointForThisLink = &osJoint;
							break;
						}
					}
				}
			}
			SCONE_ASSERT( osimJointForThisLink );
			// create a joint
			m_Joints.emplace_back( // scone joint
					new JointOpenSim4(
							/* scone body */ **itBody,
							/* parent scone joint */ parent ? &parent->GetJoint() : nullptr,
							/* model simbody */ *this,
							/* opensim joint */ *osimJointForThisLink ) );
			link = LinkUP( new Link( **itBody, *m_Joints.back(), parent ) );
		}
		else
		{
			// this is the root Link
			link = LinkUP( new Link( **itBody ) );
		}

		// add children
		for ( auto iter = m_Bodies.begin(); iter != m_Bodies.end(); ++iter )
		{
			BodyOpenSim4& childBody = dynamic_cast<BodyOpenSim4&>( **iter );

			const auto& childMB = childBody.m_osBody.getMobilizedBody();
			if ( childMB.getMobilizedBodyIndex() > 0 && childMB.getParentMobilizedBody().getMobilizedBodyIndex() == thisMBI )
			{
				// create child link
				link->GetChildren().push_back( CreateLinkHierarchy( childBody.m_osBody, link.get() ) );
			}
		}

		return link;
	}/*
	{
		LinkUP link;

		// find the Body
		auto itBody = std::find_if( m_Bodies.begin(), m_Bodies.end(), [&]( BodyUP& body )
		{ return dynamic_cast<BodyOpenSim4&>( *body ).m_osBody == osBody; } );
		SCONE_ASSERT( itBody != m_Bodies.end() );

		if (osBody.getName() == "ground") {
			link = LinkUP( new Link( **itBody ) );
		} else {
			m_Joints.emplace_back( new JointOpenSim4( **itBody, parent ? &parent->GetJoint() : nullptr, *this, osBody.getJoint() ) );
			// TODO use the OpenSim4 Matter Subsystem and map between MobilizedBodyIndices.
		}

		const auto& model = dynamic_cast<OpenSim::Model&>(osBody.getRoot());
		const auto& ground = model.getGround();
		for (auto& osJoint : osBody.getRoot().updComponentList<Joint>()) {
			if (&osJoint.getParentFrame().getBaseFrame() == &osJoint.getGround()) {


			}
		}

		// find the Joint (if any)
		if ( osBody.hasJoint() )
		{
			// create a joint
			m_Joints.emplace_back( new JointOpenSim4( **itBody, parent ? &parent->GetJoint() : nullptr, *this, osBody.getJoint() ) );
			link = LinkUP( new Link( **itBody, *m_Joints.back(), parent ) );
		}
		else
		{
			// this is the root Link
			link = LinkUP( new Link( **itBody ) );
		}

		// add children
		for ( auto iter = m_Bodies.begin(); iter != m_Bodies.end(); ++iter )
		{
			BodyOpenSim4& childBody = dynamic_cast<BodyOpenSim4&>( **iter );
			if ( childBody.m_osBody.hasJoint() && childBody.m_osBody.getJoint().getParentBody() == osBody )
			{
				// create child link
				link->GetChildren().push_back( CreateLinkHierarchy( childBody.m_osBody, link.get() ) );
			}
		}

		return link;
	}*/

	void ControllerDispatcher::computeControls( const SimTK::State& s, SimTK::Vector &controls ) const
	{
		SCONE_PROFILE_FUNCTION;

		// see 'catch' statement below for explanation try {} catch {} is needed
		try
		{
			if ( !m_Model->use_fixed_control_step_size )
			{
				// update current state (TODO: remove const cast)
				m_Model->SetTkState( const_cast<SimTK::State&>( s ) );

				// update SensorDelayAdapters at the beginning of each new step
				// TODO: move this to an analyzer object or some other point
				if ( m_Model->GetIntegrationStep() > m_Model->m_PrevIntStep && m_Model->GetIntegrationStep() > 0 )
				{
					m_Model->UpdateSensorDelayAdapters();
					m_Model->UpdateAnalyses();
				}

				// update actuator values
				m_Model->UpdateControlValues();

				// update previous integration step and time
				// OpenSim: do I need to keep this or is there are smarter way?
				if ( m_Model->GetIntegrationStep() > m_Model->m_PrevIntStep )
				{
					m_Model->m_PrevIntStep = m_Model->GetIntegrationStep();
					m_Model->m_PrevTime = m_Model->GetTime();
				}
			}

			// inject actuator values into controls
			{
				//SCONE_ASSERT_MSG( controls.size() == m_Model->GetMuscles().size(), "Only muscle actuators are supported in SCONE at this moment" );

				int idx = 0;
				for ( auto* act : m_Model->GetActuators() )
				{
					// This is an optimization that only works when there are only muscles
					// OpenSim: addInControls is rather inefficient, that's why we don't use it
					// TODO: fix this into a generic version (i.e. work with other actuators)
					controls[ idx++ ] += act->GetInput();
				}
			}
		}
		catch ( std::exception& e )
		{
			// exceptions are caught and reported here
			// otherwise they get lost in SimTK::AbstractIntegratorRep::attemptDAEStep()
			// OpenSim: please remove the catch(...) statement
			log::critical( e.what() );
			throw e;
		}
	}


	void ModelOpenSim4::StoreCurrentFrame()
	{
		SCONE_PROFILE_FUNCTION;

		// store scone data
		Model::StoreCurrentFrame();
	}

	void ModelOpenSim4::AdvanceSimulationTo( double time )
	{
		SCONE_PROFILE_FUNCTION;
		SCONE_ASSERT( m_pOsimManager );

		if ( use_fixed_control_step_size )
		{
			// initialize the time-stepper if this is the first step
			if ( !m_pTkTimeStepper )
			{
				// Integrate using time stepper
				m_pTkTimeStepper = std::unique_ptr< SimTK::TimeStepper >( new SimTK::TimeStepper( m_pOsimModel->getMultibodySystem(), *m_pTkIntegrator ) );
				m_pTkTimeStepper->initialize( GetTkState() );
				if ( GetStoreData() )
				{
					// store initial frame
					m_pOsimModel->getMultibodySystem().realize( GetTkState(), SimTK::Stage::Acceleration );
					CopyStateFromTk();
					StoreCurrentFrame();
				}
			}

			// start integration loop
			int number_of_steps = static_cast<int>( 0.5 + ( time - GetTime() ) / fixed_control_step_size );
			int thread_interuption_steps = static_cast<int>( std::max( 10.0, 0.02 / fixed_control_step_size ) );

			for ( int current_step = 0; current_step < number_of_steps; )
			{
				// update controls
				UpdateControlValues();

				// integrate
				m_PrevTime = GetTime();
				m_PrevIntStep = GetIntegrationStep();
				double target_time = GetTime() + fixed_control_step_size;
				SimTK::Integrator::SuccessfulStepStatus status;

				{
					SCONE_PROFILE_SCOPE( "SimTK::TimeStepper::stepTo" );
					status = m_pTkTimeStepper->stepTo( target_time );
				}

				SetTkState( m_pTkIntegrator->updAdvancedState() );
				CopyStateFromTk();

				++current_step;

				// Realize Acceleration, analysis components may need it
				// this way the results are always consistent
				m_pOsimModel->getMultibodySystem().realize( GetTkState(), SimTK::Stage::Acceleration );

				// update the sensor delays, analyses, and store data
				UpdateSensorDelayAdapters();
				UpdateAnalyses();

				if ( GetStoreData() )
					StoreCurrentFrame();

				// terminate when simulation has ended
				if ( HasSimulationEnded() )
				{
					log::DebugF( "Terminating simulation at %.3f", m_pTkTimeStepper->getTime() );
					break;
				}
			}
		}
		else
		{
		    SCONE_THROW("Using Manager is not supported currently.");
			// Integrate from initial time to final time (the old way)
			m_pOsimManager->integrate( time );
		}
	}

	double ModelOpenSim4::GetTime() const
	{
		return GetTkState().getTime();
	}

	int ModelOpenSim4::GetIntegrationStep() const
	{
		return GetTkIntegrator().getNumStepsTaken();
	}

	int ModelOpenSim4::GetPreviousIntegrationStep() const
	{
		return m_PrevIntStep;
	}

	double ModelOpenSim4::GetPreviousTime() const
	{
		return m_PrevTime;
	}

	std::ostream& ModelOpenSim4::ToStream( std::ostream& str ) const
	{
		Model::ToStream( str );

		GetOsimModel().getMultibodySystem().realize( *m_pTkState, SimTK::Stage::Dynamics );

		str << endl << "Forces:" << endl;
		const OpenSim::ForceSet& fset = GetOsimModel().getForceSet();
		for ( int i = 0; i < fset.getSize(); ++i )
		{
			OpenSim::Force& f = fset.get( i );
			str << f.getName() << endl;
			for ( int rec = 0; rec < f.getRecordLabels().size(); ++rec )
				str << "  " << f.getRecordLabels().get( rec ) << ": " << f.getRecordValues( *m_pTkState ).get( rec ) << endl;
		}

		return str;
	}

	scone::Real ModelOpenSim4::GetTotalEnergyConsumption() const
	{
		if ( m_pProbe )
			return m_pProbe->getProbeOutputs( GetTkState() )[ 0 ];
		else return 0.0;
	}

	double ModelOpenSim4::GetSimulationEndTime() const
	{
		return m_FinalTime;
	}

	void ModelOpenSim4::SetSimulationEndTime( double t )
	{
		m_FinalTime = t;
		m_pTkIntegrator->setFinalTime( t );
	}

	const String& ModelOpenSim4::GetName() const
	{
		return GetOsimModel().getName();
	}

	void ModelOpenSim4::ReadState( const path& file )
	{
		// create a copy of the storage
		auto store = g_StorageCache( file );
		OpenSim::Array< double > data = store->getStateVector( 0 )->getData();
		OpenSim::Array< std::string > storeLabels = store->getColumnLabels();

		// for all storage channels, check if there's a matching state
		for ( int i = 0; i < storeLabels.getSize(); i++ )
		{
			index_t idx = m_State.GetIndex( storeLabels[ i ] );
			if ( idx != NoIndex )
				m_State[ idx ] = data[ store->getStateIndex( storeLabels[ i ] ) ];
		}
	}

	void ModelOpenSim4::FixTkState( double force_threshold /*= 0.1*/, double fix_accuracy /*= 0.1 */ )
	{
		const Real step_size = 0.1;

		if ( GetState().GetIndex( initial_load_dof ) == NoIndex )
		{
			log::warning( "Ignoring initial load setting, could not find ", initial_load_dof );
			return;
		}

		// find top
		double top = GetOsimModel().getStateVariableValue( GetTkState(), initial_load_dof );
		while ( abs( GetTotalContactForce() ) > force_threshold )
		{
			top += step_size;
			GetOsimModel().setStateVariableValue( GetTkState(), initial_load_dof, top );
		}

		// find bottom
		double bottom = top;
		do
		{
			bottom -= step_size;
			GetOsimModel().setStateVariableValue( GetTkState(), initial_load_dof, bottom );
		}
		while ( abs( GetTotalContactForce() ) <= force_threshold );

		// find middle ground until we are close enough
		double force;
		double new_ty;
		for ( int i = 0; i < 100; ++i )
		{
			new_ty = ( top + bottom ) / 2;
			GetOsimModel().setStateVariableValue( GetTkState(), initial_load_dof, new_ty );
			force = abs( GetTotalContactForce() );

			// check if it's good enough
			if ( abs( force - force_threshold ) / force_threshold <= fix_accuracy )
				break;

			// update top / bottom
			if ( force > force_threshold ) bottom = new_ty; else top = new_ty;
		}

		if ( abs( force - force_threshold ) / force_threshold > fix_accuracy )
			log::WarningF( "Could not fix initial state, new_ty=%.6f top=%.6f bottom=%.6f force=%.6f (target=%.6f)", new_ty, top, bottom, force, force_threshold );
		else
			log::TraceF( "Fixed initial state, new_ty=%.6f top=%.6f bottom=%.6f force=%.6f (target=%.6f)", new_ty, top, bottom, force, force_threshold );
	}

	void ModelOpenSim4::InitStateFromTk()
	{
		SCONE_ASSERT( GetState().GetSize() == 0 );
		auto osnames = GetOsimModel().getStateVariableNames();
		auto osvalues = GetOsimModel().getStateVariableValues( GetTkState() );
		for ( int i = 0; i < osnames.size(); ++i )
			GetState().AddVariable( osnames[ i ], osvalues[ i ] );
	}

	void ModelOpenSim4::CopyStateFromTk()
	{
		SCONE_ASSERT( m_State.GetSize() >= GetOsimModel().getNumStateVariables() );
		auto osvalues = GetOsimModel().getStateVariableValues( GetTkState() );
		for ( int i = 0; i < osvalues.size(); ++i )
			m_State.SetValue( i, osvalues[ i ] );
	}

	void ModelOpenSim4::CopyStateToTk()
	{
		SCONE_ASSERT( m_State.GetSize() >= GetOsimModel().getNumStateVariables() );
		GetOsimModel().setStateVariableValues( GetTkState(),
				SimTK::Vector( m_State.GetSize(), &m_State.GetValues()[ 0 ] ) );

		// set locked coordinates
		auto& cs = GetOsimModel().updCoordinateSet();
		for ( int i = 0; i < cs.getSize(); ++i )
		{
			if ( cs.get( i ).getLocked( GetTkState() ) )
			{
				cs.get( i ).setLocked( GetTkState(), false );
				cs.get( i ).setLocked( GetTkState(), true );
			}
		}
	}

	void ModelOpenSim4::SetState( const State& state, TimeInSeconds timestamp )
	{
		m_State.SetValues( state.GetValues() );
		CopyStateToTk();
		GetTkState().setTime( timestamp );
		m_pOsimModel->getMultibodySystem().realize( GetTkState(), SimTK::Stage::Acceleration );
		if ( GetController() )
			UpdateControlValues();
	}

	void ModelOpenSim4::SetStateValues( const std::vector< Real >& state, TimeInSeconds timestamp )
	{
		m_State.SetValues( state );
		CopyStateToTk();
		GetTkState().setTime( timestamp );
		m_pOsimModel->getMultibodySystem().realize( GetTkState(), SimTK::Stage::Acceleration );
		if ( GetController() )
			UpdateControlValues();

		if ( GetStoreData() )
			StoreCurrentFrame();
	}

	TimeInSeconds ModelOpenSim4::GetSimulationStepSize()
	{
		SCONE_ASSERT( use_fixed_control_step_size );
		return fixed_control_step_size;
	}

	void ModelOpenSim4::ValidateDofAxes()
	{
		SimTK::Matrix jsmat;
		m_pOsimModel->getMatterSubsystem().calcSystemJacobian( GetTkState(), jsmat );

		// extract axes from system Jacobian
		for ( auto coIdx = 0u; coIdx < m_Dofs.size(); ++coIdx )
		{
			DofOpenSim4& dof = static_cast<DofOpenSim4&>( *m_Dofs[ coIdx ] );
			auto mbIdx = dof.GetOsCoordinate().getJoint().getParentFrame().getMobilizedBodyIndex();

			for ( auto j = 0; j < 3; ++j )
				dof.m_RotationAxis[ j ] = jsmat( mbIdx * 6 + j, coIdx );
		}
	}

	void ModelOpenSim4::UpdateOsimStorage()
	{
		auto stateValues = m_pOsimModel->getStateVariableValues( GetTkState() );

		OpenSim::StateVector vec;
		vec.setStates( GetTkState().getTime(), stateValues );
		m_pOsimManager->getStateStorage().append( vec );
	}

	void ModelOpenSim4::InitializeOpenSimMuscleActivations( double override_activation )
	{
		for ( auto iter = GetMuscles().begin(); iter != GetMuscles().end(); ++iter )
		{
			OpenSim::Muscle& osmus = dynamic_cast<MuscleOpenSim4*>( iter->get() )->GetOsMuscle();
			auto a = override_activation != 0.0 ? override_activation : ( *iter )->GetInput();
			osmus.setActivation( GetOsimModel().updWorkingState(), a );
		}

		m_pOsimModel->equilibrateMuscles( GetTkState() );
	}

	void ModelOpenSim4::SetController( ControllerUP c )
	{
		Model::SetController( std::move( c ) );

		// Initialize muscle dynamics STEP 1
		// equilibrate with initial small actuation so we can update the sensor delay adapters (needed for reflex controllers)
		InitializeOpenSimMuscleActivations( 0.05 );
		UpdateSensorDelayAdapters();

		// Initialize muscle dynamics STEP 2
		// compute actual initial control values and re-equilibrate muscles
		UpdateControlValues();
		InitializeOpenSimMuscleActivations();
	}
}
