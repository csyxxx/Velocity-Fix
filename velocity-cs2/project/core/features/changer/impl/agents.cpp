#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <core/settings.hpp>
#include <protection/game_addresses.hpp>
#include <utilities/logging/logging.hpp>

namespace features::changer {
	namespace {
		constexpr std::size_t post_data_update_index{ 10 };

		bool prepare_player_model( const std::string& path )
		{
			struct resource_path_buffer
			{
				std::uint32_t unknown1{};
				std::uint32_t unknown2{ 0xc00000c8 };

				union
				{
					std::uintptr_t string_ptr;
					std::uint8_t data[ 0xc8 ];
				};

				std::uintptr_t unknown3{};
				std::uintptr_t resource_type{};
			} buffer{};

			const auto init_path = PATTERN (patterns::init_particle_path_buffer);
			const auto precache = PATTERN (patterns::resource_system_precache);
			const auto load = PATTERN (patterns::resource_system_load);
			if ( !init_path || !addresses::globals::resource_system || ( !precache && !load ) )
			{
				return false;
			}

			memory::call<void>( init_path, &buffer, path.c_str( ) );
			buffer.resource_type = 'ldmv';
			if ( precache )
			{
				memory::call<void>( precache, addresses::globals::resource_system, &buffer, "" );
			}

			if ( load )
			{
				memory::call<void>( init_path, &buffer, path.c_str( ) );
				buffer.resource_type = 'ldmv';
				memory::call<void>( load, addresses::globals::resource_system, &buffer, "" );
			}

			return true;
		}

		void refresh_animation_graph( std::uintptr_t pawn )
		{
			const auto game_scene_node_offset = SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash );
			const auto populate_history_offset = SCHEMA( "CBaseAnimGraph", "m_bInitiallyPopulateInterpHistory"_hash );
			const auto update_enabled_offset = SCHEMA( "CBaseAnimGraph", "m_bAnimGraphUpdateEnabled"_hash );
			const auto update_scheduled_offset = SCHEMA( "CBaseAnimGraph", "m_bAnimationUpdateScheduled"_hash );
			const auto set_skeleton = PATTERN( patterns::game_scene_node_set_skeleton );
			if ( !pawn || !game_scene_node_offset || !populate_history_offset || !update_enabled_offset || !update_scheduled_offset )
			{
				return;
			}

			// Server-side SetModel reaches this callback through a network state
			// update. A local SetModel call does not, so explicitly run the entity's
			// model-change lifecycle before rebuilding graph and skeleton state.
			memory::call_vfunc<void>( pawn, post_data_update_index, 1 );
			logging::console::print( "[model changer] post data update pawn={:#x}", pawn );

			const auto rebuild_skeleton = [ & ]( )
			{
				if ( !set_skeleton )
				{
					return false;
				}

				const auto game_scene_node = memory::safe_read<std::uintptr_t>( pawn + game_scene_node_offset );
				if ( !game_scene_node || !*game_scene_node )
				{
					return false;
				}

				memory::call<void>( set_skeleton, *game_scene_node, 0x100 );
				return true;
			};

			// SetModel changes the resource handle, but the scene node can retain the
			// previous model's bone cache. Rebuild it before rebinding the anim graph.
			rebuild_skeleton( );
			memory::write<bool>( pawn + populate_history_offset, true );
			memory::write<bool>( pawn + update_enabled_offset, true );
			memory::write<bool>( pawn + update_scheduled_offset, true );

			if ( rebuild_skeleton( ) )
			{
				logging::console::print( "[model changer] skeleton refresh pawn={:#x}", pawn );
			}
		}

		void schedule_model_animation( std::uintptr_t pawn )
		{
			const auto game_scene_node_offset = SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash );
			const auto update_enabled_offset = SCHEMA( "CBaseAnimGraph", "m_bAnimGraphUpdateEnabled"_hash );
			const auto update_scheduled_offset = SCHEMA( "CBaseAnimGraph", "m_bAnimationUpdateScheduled"_hash );
			const auto set_skeleton = PATTERN( patterns::game_scene_node_set_skeleton );
			if ( !pawn || !game_scene_node_offset || !update_enabled_offset || !update_scheduled_offset )
			{
				return;
			}

			memory::write<bool>( pawn + update_enabled_offset, true );
			memory::write<bool>( pawn + update_scheduled_offset, true );

			if ( !set_skeleton )
			{
				return;
			}

			const auto game_scene_node = memory::safe_read<std::uintptr_t>( pawn + game_scene_node_offset );
			if ( game_scene_node && *game_scene_node )
			{
				memory::call<void>( set_skeleton, *game_scene_node, 0x100 );
			}
		}

	} // namespace

	void agents::apply_custom_model( std::string model_path )
	{
		if ( model_path.empty( ) )
		{
			return;
		}

		logging::console::print( "[model changer] requested model={}", model_path );

		std::lock_guard lock{ this->m_custom_model_mutex };
		this->m_requested_custom_model = std::move( model_path );
		++this->m_custom_model_revision;
	}

	void agents::disable_custom_model( )
	{
		std::lock_guard lock{ this->m_custom_model_mutex };
		this->m_requested_custom_model.clear( );
		++this->m_custom_model_revision;
	}

	std::string agents::custom_model( ) const
	{
		std::lock_guard lock{ this->m_custom_model_mutex };
		return this->m_requested_custom_model;
	}

	void agents::on_frame_stage_notify( )
	{
		const auto local = systems::g_local.get( );
		if ( !local.is_alive || systems::g_local.is_in_cinematic( ) || !local.pawn )
		{
			return;
		}

		const auto team = memory::read<int>( local.pawn + SCHEMA( "C_BaseEntity", "m_iTeamNum"_hash ) );
		const auto selected_def_index = ( team == 3 ) ? settings::g_changer.agents.ct_def : ( team == 2 ) ? settings::g_changer.agents.t_def : static_cast< std::int16_t >( 0 );
		std::string custom_model{};
		std::uint64_t custom_model_revision{};
		{
			std::lock_guard lock{ this->m_custom_model_mutex };
			custom_model = this->m_requested_custom_model;
			custom_model_revision = this->m_custom_model_revision;
		}

		const econ_item_system::item_def* selected{ nullptr };
		if ( custom_model.empty( ) && selected_def_index != 0 )
		{
			selected = g_econ_item_system.find_def( selected_def_index );
		}

		const auto desired_model = !custom_model.empty( )
			? custom_model
			: ( selected && !selected->model_player.empty( ) ? selected->model_player : std::string{} );

		if ( this->m_tracked_pawn != local.pawn )
		{
			this->m_original_model.clear( );
			this->m_applied_model.clear( );
			this->m_overridden = false;
			this->m_animation_refresh_pending = false;
			this->m_applied_handle = 0;
			this->m_tracked_team = 0;
			this->m_tracked_pawn = local.pawn;
		}

		const auto game_scene_node = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
		if ( !game_scene_node )
		{
			return;
		}

		const auto model_state = game_scene_node + SCHEMA( "CSkeletonInstance", "m_modelState"_hash );
		const auto custom_model_changed = custom_model_revision != this->m_processed_custom_model_revision;
		this->m_processed_custom_model_revision = custom_model_revision;

		if ( custom_model_changed && this->m_overridden )
		{
			if ( !this->m_original_model.empty( ) )
			{
				memory::call<void>( PATTERN (patterns::set_player_model), local.pawn, this->m_original_model.c_str( ) );
				refresh_animation_graph( local.pawn );
				this->cycle_weapon_owners( local.pawn );
			}

			this->m_applied_model.clear( );
			this->m_applied_handle = 0;
			this->m_animation_refresh_pending = false;
			this->m_tracked_team = 0;
			this->m_overridden = false;
		}

		if ( desired_model.empty( ) )
		{
			if ( this->m_overridden && !this->m_original_model.empty( ) )
			{
				memory::call<void>( PATTERN (patterns::set_player_model), local.pawn, this->m_original_model.c_str( ) );
				refresh_animation_graph( local.pawn );
				this->cycle_weapon_owners( local.pawn );

				this->m_applied_handle = 0;
				this->m_applied_model.clear( );
				this->m_animation_refresh_pending = false;
				this->m_tracked_team = 0;
				this->m_overridden = false;
				this->m_original_model.clear( );
			}
			return;
		}

		if ( !this->m_overridden && this->m_original_model.empty( ) )
		{
			const auto model_name_ptr = memory::read<std::uintptr_t>( model_state + SCHEMA( "CModelState", "m_ModelName"_hash ) );
			if ( model_name_ptr )
			{
				this->m_original_model = memory::read_string( model_name_ptr );
			}
		}

		const auto current_handle = memory::read<std::uintptr_t>( model_state + SCHEMA( "CModelState", "m_hModel"_hash ) );
		const auto selection_matches = ( this->m_applied_model == desired_model );
		const auto team_matches = ( this->m_tracked_team == team );
		const auto handle_matches = ( this->m_applied_handle != 0 && current_handle == this->m_applied_handle );
		const auto model_name_ptr = memory::read<std::uintptr_t>( model_state + SCHEMA( "CModelState", "m_ModelName"_hash ) );
		const auto current_model_name = model_name_ptr ? memory::read_string( model_name_ptr ) : std::string{};
		const auto model_matches = current_model_name == desired_model;

		// SetModel may finish loading the resource after this stage. Wait until
		// the model name is updated, then bind the graph against the new skeleton.
		if ( this->m_overridden && selection_matches && team_matches && this->m_animation_refresh_pending )
		{
			if ( model_matches )
			{
				logging::console::print( "[model changer] animation rebind model={} handle={:#x}", current_model_name, current_handle );
				refresh_animation_graph( local.pawn );
				this->m_applied_handle = current_handle;
				this->m_animation_refresh_pending = false;
			}
			return;
		}

		if ( this->m_overridden && selection_matches && team_matches && !this->m_animation_refresh_pending && handle_matches )
		{
			schedule_model_animation( local.pawn );
			return;
		}

		if ( this->m_overridden && !team_matches )
		{
			this->m_original_model.clear( );
			this->m_applied_model.clear( );
			this->m_applied_handle = 0;
			this->m_animation_refresh_pending = false;
			this->m_overridden = false;

			const auto model_name_ptr = memory::read<std::uintptr_t>( model_state + SCHEMA( "CModelState", "m_ModelName"_hash ) );
			if ( model_name_ptr )
			{
				this->m_original_model = memory::read_string( model_name_ptr );
			}
		}
		else if ( this->m_overridden && !selection_matches )
		{
			if ( !this->m_original_model.empty( ) )
			{
				memory::call<void>( PATTERN (patterns::set_player_model), local.pawn, this->m_original_model.c_str( ) );
				refresh_animation_graph( local.pawn );
				this->cycle_weapon_owners( local.pawn );
			}

			this->m_applied_model.clear( );
			this->m_applied_handle = 0;
			this->m_animation_refresh_pending = false;
			this->m_overridden = false;
		}

		if ( this->m_prepared_model != desired_model )
		{
			prepare_player_model( desired_model );
			this->m_prepared_model = desired_model;
			return;
		}

		memory::call<void>( PATTERN (patterns::set_player_model), local.pawn, desired_model.c_str( ) );
		const auto collision = local.pawn + SCHEMA( "C_BaseModelEntity", "m_Collision"_hash );
		memory::write<math::vector3>( collision + SCHEMA( "CCollisionProperty", "m_vecMins"_hash ), math::vector3( -16.0f, -16.0f, 0.0f ) );
		memory::write<math::vector3>( collision + SCHEMA( "CCollisionProperty", "m_vecMaxs"_hash ), math::vector3( 16.0f, 16.0f, 72.0f ) );

		this->cycle_weapon_owners( local.pawn );

		this->m_applied_handle = memory::read<std::uintptr_t>( model_state + SCHEMA( "CModelState", "m_hModel"_hash ) );
		this->m_animation_refresh_pending = true;
		this->m_applied_model = desired_model;
		this->m_tracked_team = team;
		this->m_overridden = true;
	}

	void agents::cycle_weapon_owners( std::uintptr_t pawn )
	{
		const auto weapon_services = memory::read<std::uintptr_t>( pawn + SCHEMA( "C_BasePlayerPawn", "m_pWeaponServices"_hash ) );
		if ( !weapon_services )
		{
			return;
		}

		const auto weapons_base = weapon_services + SCHEMA( "CPlayer_WeaponServices", "m_hMyWeapons"_hash );
		const auto weapons_size = memory::read<int>( weapons_base );
		const auto weapons_data = memory::read<std::uintptr_t>( weapons_base + 0x8 );

		if ( !weapons_data || weapons_size <= 0 )
		{
			return;
		}

		for ( auto i = 0; i < weapons_size; ++i )
		{
			const auto handle = memory::read<std::uint32_t>( weapons_data + i * sizeof( std::uint32_t ) );
			const auto weapon = systems::g_entities.lookup( handle );

			if ( !weapon )
			{
				continue;
			}

			const auto owner_off = SCHEMA( "C_BaseEntity", "m_hOwnerEntity"_hash );
			const auto saved_owner = memory::read<std::uint32_t>( weapon + owner_off );

			memory::write<std::uint32_t>( weapon + owner_off, 0xffffffff );
			memory::write<std::uint32_t>( weapon + owner_off, saved_owner );
		}
	}

} // namespace features::changer
