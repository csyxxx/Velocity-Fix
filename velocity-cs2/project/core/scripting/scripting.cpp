#include <pch/pch.hpp>

#define SOL_NO_EXCEPTIONS 1
#include <sol/sol.hpp>

#include <core/scripting/scripting.hpp>
#include <core/settings.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/logging/logging.hpp>
#include <protection/game_addresses.hpp>

#include <fstream>
#include <mutex>

namespace scripting {

	namespace {

		struct entity_snapshot
		{
			int index{ -1 };
			std::string type{ "unknown" };
			std::string class_name{};
			int health{};
			int team{};
			bool alive{};
			bool enemy{};
			math::vector3 origin{};
			math::vector3 velocity{};
			math::vector3 eye_position{};
			int armor{};
			std::uint32_t flags{};
			float simulation_time{};
			bool scoped{};
		};

		struct bone_snapshot
		{
			int id{ -1 };
			math::vector3 position{};
			math::quaternion rotation{};
			float scale{};
			float fraction{ 1.0f };
			float fov{};
			int hitbox_index{ -1 };
			int hitgroup{ -1 };
			bool visible{};
			bool valid{};
		};

		struct hitbox_snapshot
		{
			int index{ -1 };
			int bone{ -1 };
			math::vector3 mins{};
			math::vector3 maxs{};
			float radius{};
			int hitgroup{};
			std::string hitgroup_name{};
			std::uint8_t shape_type{};
			bool translation_only{};
		};

		struct projection_snapshot
		{
			math::vector2 screen{};
			bool valid{};
			bool on_screen{};
		};

		struct trace_snapshot
		{
			float fraction{ 1.0f };
			math::vector3 start{};
			math::vector3 end{};
			math::vector3 position{};
			math::vector3 normal{};
			int hit_index{ -1 };
			std::uint32_t contents{};
			int hitbox_index{ -1 };
			int bone{ -1 };
			int hitgroup{ -1 };
			bool visible{};
			bool hit{};
			bool all_solid{};
		};

		struct game_event_context
		{
			std::string name{};
			std::uintptr_t event{};

			[[nodiscard]] int get_int( const std::string& key, const int fallback = 0 ) const
			{
				return event ? memory::call<int>( PATTERN( patterns::game_event_get_int ), event, key.c_str( ), fallback ) : fallback;
			}

			[[nodiscard]] float get_float( const std::string& key, const float fallback = 0.0f ) const
			{
				return event ? memory::call<float>( PATTERN( patterns::game_event_get_float ), event, key.c_str( ), fallback ) : fallback;
			}

			[[nodiscard]] math::vector3 get_vector( const std::string& x_key = "x", const std::string& y_key = "y", const std::string& z_key = "z" ) const
			{
				return { get_float( x_key ), get_float( y_key ), get_float( z_key ) };
			}

			[[nodiscard]] int pawn_index( const std::string& key = "userid" ) const;
			[[nodiscard]] bool is_local( const std::string& key = "userid" ) const;
		};

		struct command_context
		{
			systems::input::usercmd* command{};

			[[nodiscard]] proto::base_usercmd_pb* base( ) const
			{
				return command ? command->csgo_user_cmd.mutable_base( ) : nullptr;
			}

			[[nodiscard]] std::int64_t number( ) const { return command ? command->command_number : -1; }
			[[nodiscard]] int tick( ) const { return base( ) ? base( )->client_tick( ) : -1; }
			[[nodiscard]] float forward_move( ) const { return base( ) ? base( )->forwardmove( ) : 0.0f; }
			[[nodiscard]] float side_move( ) const { return base( ) ? base( )->leftmove( ) : 0.0f; }
			[[nodiscard]] float up_move( ) const { return base( ) ? base( )->upmove( ) : 0.0f; }

			[[nodiscard]] math::vector3 view_angles( ) const
			{
				const auto* view = base( ) ? base( )->viewangles( ) : nullptr;
				return view ? math::vector3{ view->x( ), view->y( ), view->z( ) } : systems::g_input.get_view_angles( );
			}

			void set_movement( const float forward, const float side, const float up = 0.0f ) const
			{
				if ( auto* value = base( ) )
				{
					value->set_forwardmove( std::clamp( forward, -450.0f, 450.0f ) );
					value->set_leftmove( std::clamp( side, -450.0f, 450.0f ) );
					value->set_upmove( std::clamp( up, -320.0f, 320.0f ) );
				}
			}

			void set_view_angles( math::vector3 angles ) const
			{
				angles.x = std::clamp( angles.x, -89.0f, 89.0f );
				angles.y = math::helpers::normalize_yaw( angles.y );
				angles.z = 0.0f;
				if ( auto* value = base( ) )
				{
					if ( auto* view = value->mutable_viewangles( ) )
					{
						view->set_x( angles.x );
						view->set_y( angles.y );
						view->set_z( angles.z );
					}
				}
			}

			void set_silent_angles( const math::vector3& angles ) const
			{
				const auto old_angles = view_angles( );
				const auto forward = forward_move( );
				const auto side = side_move( );
				set_view_angles( angles );

				math::vector3 old_forward{}, old_left{}, new_forward{}, new_left{};
				math::helpers::angle_vectors_left( old_angles, &old_forward, &old_left );
				math::helpers::angle_vectors_left( view_angles( ), &new_forward, &new_left );
				old_forward.z = old_left.z = new_forward.z = new_left.z = 0.0f;
				old_forward.normalize( ); old_left.normalize( ); new_forward.normalize( ); new_left.normalize( );
				const auto intent = old_forward * forward + old_left * -side;
				set_movement( new_forward.dot( intent ), -new_left.dot( intent ), up_move( ) );
			}
			void set_camera_angles( math::vector3 angles ) const
			{
				angles.x = std::clamp( angles.x, -89.0f, 89.0f );
				angles.y = math::helpers::normalize_yaw( angles.y );
				angles.z = 0.0f;
				systems::g_input.set_view_angles( angles );
			}
			void fire( const bool secondary = false ) const { set_button( secondary ? "attack2" : "attack", true ); }
			void stop_firing( ) const { set_button( "attack", false ); set_button( "attack2", false ); }

			[[nodiscard]] bool has_button( const std::string& name ) const;
			void set_button( const std::string& name, bool enabled ) const;
		};

		struct ui_value
		{
			enum class kind { none, boolean, decimal, integer, string, color };

			kind type{ kind::none };
			bool boolean{};
			float decimal{};
			int integer{};
			std::string text{};
			xdraw::color color{};
			std::unique_ptr<xui::setting> checkbox{};
		};

		[[nodiscard]] std::string lower_copy( std::string value )
		{
			std::ranges::transform( value, value.begin( ), [ ]( const unsigned char c )
				{
					return static_cast<char>( std::tolower( c ) );
				} );
			return value;
		}

		[[nodiscard]] std::string lua_arguments_text( sol::variadic_args arguments )
		{
			std::string result{};
			for ( const auto argument : arguments )
			{
				if ( !result.empty( ) ) result.push_back( '\t' );
				const sol::object value = argument;
				switch ( value.get_type( ) )
				{
				case sol::type::nil: result += "nil"; break;
				case sol::type::boolean: result += value.as<bool>( ) ? "true" : "false"; break;
				case sol::type::number: result += std::to_string( value.as<double>( ) ); break;
				case sol::type::string: result += value.as<std::string>( ); break;
				case sol::type::table: result += "<table>"; break;
				case sol::type::function: result += "<function>"; break;
				case sol::type::userdata: result += "<userdata>"; break;
				case sol::type::lightuserdata: result += "<lightuserdata>"; break;
				case sol::type::thread: result += "<thread>"; break;
				default: result += "<value>"; break;
				}
			}
			return result;
		}

		[[nodiscard]] const char* entity_type_name( const systems::entities::type type )
		{
			switch ( type )
			{
			case systems::entities::type::player: return "player";
			case systems::entities::type::item: return "item";
			case systems::entities::type::projectile: return "projectile";
			default: return "unknown";
			}
		}

		[[nodiscard]] entity_snapshot snapshot_entity( const systems::entities::cached& cached )
		{
			entity_snapshot out{};
			out.index = cached.index;
			out.type = entity_type_name( cached.type );
			if ( !cached.ptr )
			{
				return out;
			}

			out.health = memory::safe_read<int>( cached.ptr + SCHEMA( "C_BaseEntity", "m_iHealth"_hash ) ).value_or( 0 );
			out.team = memory::safe_read<int>( cached.ptr + SCHEMA( "C_BaseEntity", "m_iTeamNum"_hash ) ).value_or( 0 );
			out.alive = out.health > 0;
			out.velocity = memory::safe_read<math::vector3>( cached.ptr + SCHEMA( "C_BaseEntity", "m_vecVelocity"_hash ) ).value_or( math::vector3{} );
			out.flags = memory::safe_read<std::uint32_t>( cached.ptr + SCHEMA( "C_BaseEntity", "m_fFlags"_hash ) ).value_or( 0 );
			out.simulation_time = memory::safe_read<float>( cached.ptr + SCHEMA( "C_BaseEntity", "m_flSimulationTime"_hash ) ).value_or( 0.0f );
			if ( cached.type == systems::entities::type::player )
			{
				out.armor = memory::safe_read<int>( cached.ptr + SCHEMA( "C_CSPlayerPawn", "m_ArmorValue"_hash ) ).value_or( 0 );
				out.scoped = memory::safe_read<bool>( cached.ptr + SCHEMA( "C_CSPlayerPawn", "m_bIsScoped"_hash ) ).value_or( false );
			}

			const auto scene_node = memory::safe_read<std::uintptr_t>( cached.ptr + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) ).value_or( 0 );
			if ( scene_node )
			{
				out.origin = memory::safe_read<math::vector3>( scene_node + SCHEMA( "CGameSceneNode", "m_vecAbsOrigin"_hash ) ).value_or( math::vector3{} );
			}
			out.eye_position = out.origin + memory::safe_read<math::vector3>( cached.ptr + SCHEMA( "C_BaseModelEntity", "m_vecViewOffset"_hash ) ).value_or( math::vector3{} );

			const auto local = systems::g_local.get( );
			out.enemy = cached.type == systems::entities::type::player && local.is_valid( ) && local.is_this_other_team( out.team );
			if ( const auto* class_name = systems::g_entities.get_schema_name( cached.ptr ) )
			{
				out.class_name = class_name;
			}

			return out;
		}

		[[nodiscard]] std::optional<systems::entities::cached> cached_entity( const int requested_index )
		{
			for ( const auto type : { systems::entities::type::player, systems::entities::type::item, systems::entities::type::projectile } )
			{
				for ( const auto& cached : systems::g_entities.get_by_type( type ) )
				{
					if ( cached.index == requested_index ) return cached;
				}
			}
			return std::nullopt;
		}

		[[nodiscard]] std::uintptr_t entity_ptr( const int requested_index )
		{
			if ( requested_index == -1 ) return systems::g_local.get( ).pawn;
			if ( const auto cached = cached_entity( requested_index ) ) return cached->ptr;
			return systems::g_entities.get_by_index( requested_index );
		}

		[[nodiscard]] int entity_index_from_ptr( const std::uintptr_t entity )
		{
			if ( !entity ) return -1;
			for ( const auto type : { systems::entities::type::player, systems::entities::type::item, systems::entities::type::projectile } )
			{
				for ( const auto& cached : systems::g_entities.get_by_type( type ) )
				{
					if ( cached.ptr == entity ) return cached.index;
				}
			}
			return -1;
		}

		int game_event_context::pawn_index( const std::string& key ) const
		{
			if ( !event ) return -1;
			const auto hash = cstypes::event_hash{ 0, key.c_str( ) };
			return entity_index_from_ptr( memory::call<std::uintptr_t>( PATTERN( patterns::game_event_get_pawn ), event, &hash ) );
		}

		bool game_event_context::is_local( const std::string& key ) const
		{
			if ( !event ) return false;
			const auto hash = cstypes::event_hash{ 0, key.c_str( ) };
			return memory::call<std::uintptr_t>( PATTERN( patterns::game_event_get_controller ), event, &hash ) == systems::g_local.get( ).controller;
		}

		[[nodiscard]] trace_snapshot snapshot_trace( const systems::tracing::result& result )
		{
			return {
				.fraction = result.fraction,
				.start = result.start_pos,
				.end = result.end_pos,
				.position = result.position,
				.normal = result.normal,
				.hit_index = entity_index_from_ptr( result.hit_entity ),
				.contents = result.contents,
				.hit = result.fraction < 1.0f || result.hit_entity != 0,
				.all_solid = result.all_solid
			};
		}

		struct resolved_hitbox
		{
			systems::hitboxes::entry entry{};
			systems::bones::data bone{};
			math::vector3 center{};
		};

		[[nodiscard]] std::optional<resolved_hitbox> resolve_hitbox( const int entity_index, const int hitbox_index )
		{
			const auto entity = entity_ptr( entity_index );
			if ( !entity ) return std::nullopt;
			const auto node = memory::safe_read<std::uintptr_t>( entity + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) ).value_or( 0 );
			if ( !node ) return std::nullopt;
			const auto hitboxes = systems::g_hitboxes.query( node, true );
			for ( const auto& entry : hitboxes )
			{
				if ( entry.index != hitbox_index || entry.bone < 0 ) continue;
				const auto bone = systems::g_bones.get( entity, static_cast<std::uint32_t>( entry.bone ) );
				if ( bone.position.length_sqr( ) <= 0.0001f ) return std::nullopt;
				return resolved_hitbox{
					.entry = entry,
					.bone = bone,
					.center = bone.rotation.rotate_vector( ( entry.mins + entry.maxs ) * 0.5f ) + bone.position
				};
			}
			return std::nullopt;
		}

		[[nodiscard]] sol::table readonly_table( sol::this_state state, const sol::table& source )
		{
			sol::state_view lua_view{ state };
			const auto proxy = lua_view.create_table_with(
				sol::meta_function::new_index, sol::detail::fail_on_newindex,
				sol::meta_function::index, source,
				sol::meta_function::pairs, sol::stack::stack_detail::readonly_pairs );
			return lua_view.create_table_with( sol::metatable_key, proxy );
		}

		[[nodiscard]] std::uintptr_t button_mask( const std::string& name )
		{
			const auto lowered = lower_copy( name );
			if ( lowered == "attack" || lowered == "attack1" ) return cstypes::command_buttons::in_attack;
			if ( lowered == "attack2" || lowered == "secondary_attack" ) return cstypes::command_buttons::in_second_attack;
			if ( lowered == "jump" ) return cstypes::command_buttons::in_jump;
			if ( lowered == "duck" ) return cstypes::command_buttons::in_duck;
			if ( lowered == "use" ) return cstypes::command_buttons::in_use;
			if ( lowered == "reload" ) return cstypes::command_buttons::in_reload;
			if ( lowered == "forward" ) return cstypes::command_buttons::in_forward;
			if ( lowered == "back" ) return cstypes::command_buttons::in_back;
			if ( lowered == "left" ) return cstypes::command_buttons::in_moveleft;
			if ( lowered == "right" ) return cstypes::command_buttons::in_moveright;
			if ( lowered == "sprint" ) return cstypes::command_buttons::in_sprint;
			if ( lowered == "zoom" ) return cstypes::command_buttons::in_zoom;
			return 0;
		}

		bool command_context::has_button( const std::string& name ) const
		{
			const auto mask = button_mask( name );
			return command && mask && ( command->buttons.value & mask ) != 0;
		}

		void command_context::set_button( const std::string& name, const bool enabled ) const
		{
			const auto mask = button_mask( name );
			if ( !command || !mask ) return;
			if ( enabled ) command->buttons.value |= mask;
			else command->buttons.value &= ~mask;
			command->buttons.value_changed |= mask;
		}

		[[nodiscard]] std::string result_error( sol::protected_function_result& result )
		{
			sol::error error = result;
			return error.what( );
		}

		enum class api_setting_kind : std::uint8_t { boolean, integer, decimal, color, string };

		struct api_setting
		{
			std::string name{};
			api_setting_kind kind{};
			void* value{};
			xui::setting* module{};
		};

		[[nodiscard]] const char* api_setting_type_name( const api_setting_kind kind )
		{
			switch ( kind )
			{
			case api_setting_kind::boolean: return "boolean";
			case api_setting_kind::integer: return "integer";
			case api_setting_kind::decimal: return "number";
			case api_setting_kind::color: return "color";
			case api_setting_kind::string: return "string";
			default: return "unknown";
			}
		}

		[[nodiscard]] std::vector<api_setting>& api_settings( )
		{
			static auto entries = [ ]
			{
				std::vector<api_setting> out{};
				out.reserve( 256 );

				const auto add_bool = [ &out ]( const std::string& name, xui::setting& value )
					{ out.push_back( { name, api_setting_kind::boolean, &value.value, &value } ); };
				const auto add_int = [ &out ]( const std::string& name, config::val<int>& value )
					{ out.push_back( { name, api_setting_kind::integer, &value.value, nullptr } ); };
				const auto add_float = [ &out ]( const std::string& name, config::val<float>& value )
					{ out.push_back( { name, api_setting_kind::decimal, &value.value, nullptr } ); };
				const auto add_color = [ &out ]( const std::string& name, config::col& value )
					{ out.push_back( { name, api_setting_kind::color, &value.value, nullptr } ); };
				const auto add_string = [ &out ]( const std::string& name, config::str& value )
					{ out.push_back( { name, api_setting_kind::string, &value.value, nullptr } ); };

				add_bool( "ragebot.enabled", settings::g_combat.m_ragebot.enabled );
				add_bool( "ragebot.vac_bypass", settings::g_combat.m_ragebot.vac_bypass );
				constexpr const char* weapon_groups[]{ "pistol", "smg", "rifle", "shotgun", "sniper", "lmg" };
				for ( auto i = 0u; i < settings::combat::ragebot::k_group_count; ++i )
				{
					auto& group = settings::g_combat.m_ragebot.groups[ i ];
					const auto prefix = std::string{ "ragebot." } + weapon_groups[ i ] + ".";
					add_bool( prefix + "silent", group.silent ); add_bool( prefix + "no_spread", group.no_spread );
					add_bool( prefix + "doubletap", group.doubletap ); add_bool( prefix + "body_aim", group.body_aim );
					add_bool( prefix + "force_shot", group.force_shot ); add_bool( prefix + "force_shot_air", group.force_shot_air );
					add_bool( prefix + "min_damage_override", group.min_damage_override ); add_bool( prefix + "hitchance_override", group.hitchance_override );
					add_bool( prefix + "dynamic_pointscale", group.dynamic_pointscale ); add_bool( prefix + "debug_multipoints", group.debug_multipoints );
					add_bool( prefix + "hittable_regions", group.hittable_regions );
					add_float( prefix + "max_fov", group.max_fov ); add_float( prefix + "pointscale", group.pointscale );
					add_float( prefix + "hittable_region_duration", group.hittable_region_duration );
					add_int( prefix + "hitchance", group.hitchance ); add_int( prefix + "min_damage", group.min_damage );
					add_int( prefix + "min_damage_override_value", group.min_damage_override_value );
					add_int( prefix + "hitchance_override_value", group.hitchance_override_value );
					add_color( prefix + "hittable_region_direct_color", group.hittable_region_direct_color );
					add_color( prefix + "hittable_region_penetrated_color", group.hittable_region_penetrated_color );
				}

				add_bool( "legitbot.enabled", settings::g_combat.m_legitbot.enabled );
				for ( auto i = 0u; i < settings::combat::legitbot::k_group_count; ++i )
				{
					auto& group = settings::g_combat.m_legitbot.groups[ i ];
					const auto prefix = std::string{ "legitbot." } + weapon_groups[ i ] + ".";
					add_bool( prefix + "aimbot", group.aimbot ); add_bool( prefix + "force_lock", group.force_lock ); add_bool( prefix + "rcs", group.rcs );
					add_bool( prefix + "standalone_rcs", group.standalone_rcs ); add_bool( prefix + "triggerbot", group.triggerbot );
					add_bool( prefix + "trigger_head_only", group.trigger_head_only ); add_bool( prefix + "autostop", group.autostop ); add_bool( prefix + "autowall", group.autowall );
					add_bool( prefix + "visualize_fov", group.visualize_fov );
					add_float( prefix + "fov", group.fov ); add_int( prefix + "smooth", group.smooth ); add_float( prefix + "aim_speed", group.aim_speed );
					add_int( prefix + "rcs_min", group.rcs_min ); add_int( prefix + "rcs_max", group.rcs_max );
					add_int( prefix + "trigger_delay", group.trigger_delay ); add_int( prefix + "trigger_hitchance", group.trigger_hitchance );
					add_int( prefix + "min_damage", group.min_damage ); add_color( prefix + "fov_color", group.fov_color );
				}

				auto& aa = settings::g_combat.m_antiaim;
				add_bool( "antiaim.enabled", aa.enabled );
				add_bool( "antiaim.auto_yaw_adjust", aa.auto_yaw_adjust ); add_bool( "antiaim.manual_left", aa.manual_left );
				add_bool( "antiaim.manual_right", aa.manual_right ); add_bool( "antiaim.hide_shots", aa.hide_shots );
				add_bool( "antiaim.avoid_backstab", aa.avoid_backstab ); add_bool( "antiaim.direction_indicator", aa.direction_indicator );
				add_bool( "antiaim.direction_indicator_glow", aa.direction_indicator_glow );
				add_float( "antiaim.spin_speed", aa.spin_speed ); add_float( "antiaim.jitter_range", aa.jitter_range );
				add_float( "antiaim.jitter_speed", aa.jitter_speed ); add_float( "antiaim.direction_indicator_glow_strength", aa.direction_indicator_glow_strength );
				add_color( "antiaim.direction_indicator_color", aa.direction_indicator_color );

				add_bool( "combat.quickpeek", settings::g_combat.m_quickpeek.enabled ); add_color( "combat.quickpeek_color", settings::g_combat.m_quickpeek.color );
				add_color( "combat.quickpeek_retrack_color", settings::g_combat.m_quickpeek.retrack_color );
				add_bool( "combat.duckpeek", settings::g_combat.m_duckpeek.enabled );
				add_bool( "combat.lagcomp_extrapolation", settings::g_combat.m_lagcomp.extrapolation );
				add_int( "combat.max_backtrack_ticks", settings::g_combat.m_lagcomp.max_backtrack_ticks );
				add_int( "combat.max_extrapolate_ticks", settings::g_combat.m_lagcomp.max_extrapolate_ticks );
				add_int( "combat.min_extrapolate_ticks", settings::g_combat.m_lagcomp.min_extrapolate_ticks );
				add_int( "combat.local_prediction_ticks", settings::g_combat.m_lagcomp.local_prediction_ticks );
				add_bool( "combat.zeusbot", settings::g_combat.m_zeusbot.enabled ); add_bool( "combat.zeusbot_drop_after", settings::g_combat.m_zeusbot.drop_after );
				add_bool( "combat.knifebot", settings::g_combat.m_knifebot.enabled ); add_bool( "combat.auto_revolver", settings::g_combat.m_autos.revolver );
				add_bool( "combat.auto_scope", settings::g_combat.m_autos.scope ); add_bool( "combat.penetration_crosshair", settings::g_combat.m_penetration_crosshair.enabled );

				auto& movement = settings::g_movement;
				add_bool( "movement.bhop", movement.bhop ); add_bool( "movement.airstrafe", movement.airstrafe );
				add_bool( "movement.airstrafe_fully_directional", movement.airstrafe_fully_directional ); add_bool( "movement.jumpbug", movement.jumpbug );
				add_bool( "movement.fastladder", movement.fastladder ); add_bool( "movement.edgejump", movement.edgejump ); add_bool( "movement.edgestop", movement.edgestop );
				add_bool( "movement.edgebug", movement.edgebug ); add_bool( "movement.edgebug_include_jump_steps", movement.edgebug_include_jump_steps );
				add_bool( "movement.slowwalk", movement.slowwalk ); add_bool( "movement.test_strafer", movement.m_test_strafer.enabled );
				add_bool( "movement.strafe_fix", movement.strafe_fix ); add_bool( "movement.velocity_debug", movement.m_velocity_debug.enabled );
				add_int( "movement.edgebug_mode", movement.edgebug_mode ); add_int( "movement.edgebug_passes", movement.edgebug_passes );
				add_float( "movement.slowwalk_speed", movement.slowwalk_speed );

				auto& enemy = settings::g_esp.m_player.m_overlay[ 0 ]; auto& team = settings::g_esp.m_player.m_overlay[ 1 ];
				for ( const auto& pair : { std::pair{ std::string{ "esp.enemy." }, &enemy }, std::pair{ std::string{ "esp.team." }, &team } } )
				{
					auto& overlay = *pair.second; const auto& prefix = pair.first;
					add_bool( prefix + "enabled", overlay.enabled ); add_bool( prefix + "box", overlay.m_box.enabled ); add_bool( prefix + "box_fill", overlay.m_box.fill );
					add_bool( prefix + "box_outline", overlay.m_box.outline ); add_bool( prefix + "skeleton", overlay.m_skeleton.enabled );
					add_bool( prefix + "health", overlay.m_health_bar.enabled ); add_bool( prefix + "ammo", overlay.m_ammo_bar.enabled );
					add_bool( prefix + "flags", overlay.m_info_flags.enabled ); add_bool( prefix + "name", overlay.m_name.enabled );
					add_bool( prefix + "weapon", overlay.m_weapon.enabled ); add_bool( prefix + "oof_arrow", overlay.m_oof_arrow.enabled );
				}
				add_bool( "chams.enemy", settings::g_esp.m_player.m_chams.enemy.enabled ); add_bool( "chams.team", settings::g_esp.m_player.m_chams.team.enabled );
				add_bool( "chams.local", settings::g_esp.m_player.m_chams.local.enabled ); add_bool( "chams.backtrack", settings::g_esp.m_player.m_chams.backtrack.enabled );
				add_bool( "chams.onshot", settings::g_esp.m_player.m_chams.onshot.enabled ); add_bool( "glow.enemy", settings::g_esp.m_player.m_glow.enemy.enabled );
				add_bool( "glow.team", settings::g_esp.m_player.m_glow.team.enabled ); add_bool( "glow.local", settings::g_esp.m_player.m_glow.local.enabled );
				add_bool( "esp.items", settings::g_esp.m_item.m_overlay.enabled ); add_bool( "chams.items", settings::g_esp.m_item.m_chams.enabled );
				add_bool( "glow.items", settings::g_esp.m_item.m_glow.enabled ); add_bool( "esp.projectiles", settings::g_esp.m_projectile.m_overlay.enabled );
				add_bool( "esp.bomb_timer", settings::g_esp.m_other.bomb_timer ); add_bool( "esp.spectator_list", settings::g_esp.m_other.spectator_list );

				auto& misc = settings::g_misc;
				add_bool( "misc.scoreboard_weapons", misc.m_scoreboard_weapons.enabled ); add_bool( "misc.projectile_trajectory", misc.m_projectile_trajectory.enabled );
				add_bool( "misc.projectile_straight_throw", misc.m_projectile_trajectory.straight_throw ); add_bool( "misc.preserve_killfeed", misc.preserve_killfeed );
				add_bool( "misc.reveal_radar", misc.reveal_radar ); add_bool( "misc.disable_game_logs", misc.disable_game_logs );
				add_bool( "camera.custom_fov", misc.m_camera.change_fov ); add_float( "camera.fov", misc.m_camera.fov );
				add_bool( "camera.scoped_fov_override", misc.m_camera.scoped_fov_override ); add_float( "camera.scoped_fov", misc.m_camera.scoped_fov );
				add_bool( "camera.thirdperson", misc.m_camera.thirdperson ); add_float( "camera.thirdperson_distance", misc.m_camera.thirdperson_distance );
				add_bool( "camera.custom_aspect_ratio", misc.m_camera.change_aspect_ratio ); add_float( "camera.aspect_ratio", misc.m_camera.aspect_ratio );
				add_bool( "viewmodel.enabled", misc.m_viewmodel_adjust.enabled ); add_float( "viewmodel.offset_x", misc.m_viewmodel_adjust.offset_x );
				add_float( "viewmodel.offset_y", misc.m_viewmodel_adjust.offset_y ); add_float( "viewmodel.offset_z", misc.m_viewmodel_adjust.offset_z ); add_float( "viewmodel.fov", misc.m_viewmodel_adjust.fov );
				add_bool( "hud.crosshair", misc.m_hud.m_crosshair.enabled ); add_bool( "hud.scope", misc.m_hud.m_scope.enabled );
				add_bool( "hud.velocity_counter", misc.m_hud.m_velocity.counter ); add_bool( "hud.velocity_chart", misc.m_hud.m_velocity.chart );
				add_bool( "misc.dynamic_light", misc.m_dlight.enabled ); add_float( "misc.dynamic_light_radius", misc.m_dlight.radius ); add_color( "misc.dynamic_light_color", misc.m_dlight.color );
				add_bool( "misc.watermark", misc.m_watermark.enabled ); add_bool( "misc.watermark_fps", misc.m_watermark.show_fps );
				add_bool( "misc.watermark_ping", misc.m_watermark.show_ping ); add_bool( "misc.watermark_velocity", misc.m_watermark.show_velocity );
				add_bool( "removals.crosshair", misc.m_removals.crosshair ); add_bool( "removals.scope", misc.m_removals.scope );
				add_bool( "removals.smoke", misc.m_removals.smoke ); add_bool( "removals.recoil", misc.m_removals.recoil ); add_bool( "removals.decals", misc.m_removals.decals );
				add_float( "removals.flash_alpha", misc.m_removals.flash_alpha );
				add_bool( "impacts.hit_log", misc.m_impacts.hit_log ); add_bool( "impacts.miss_log", misc.m_impacts.miss_log );
				add_bool( "impacts.hit_sound", misc.m_impacts.hit_sound ); add_bool( "impacts.hit_marker", misc.m_impacts.hit_marker );
				add_bool( "impacts.bullet_tracers", misc.m_impacts.bullet_tracers ); add_float( "impacts.hit_log_duration", misc.m_impacts.hit_log_duration );
				add_float( "impacts.hit_sound_volume", misc.m_impacts.hit_sound_volume ); add_color( "impacts.hit_marker_color", misc.m_impacts.hit_marker_color );
				add_string( "name_changer.name", misc.m_name_changer.name ); add_string( "impacts.custom_hit_sound", misc.m_impacts.custom_hit_sound );

				auto& weather = settings::g_world.m_weather; auto& scene = settings::g_world.m_scene;
				add_bool( "world.weather", weather.enabled ); add_color( "world.weather_color", weather.color ); add_bool( "world.weather_fog", weather.fog_enabled );
				add_float( "world.weather_fog_density", weather.fog_density ); add_bool( "world.weather_wetness", weather.wetness ); add_bool( "world.weather_wind", weather.wind );
				add_float( "world.weather_wind_strength", weather.wind_strength ); add_bool( "world.skybox", scene.skybox.custom_skybox );
				add_bool( "world.skybox_color_enabled", scene.skybox.custom_color ); add_color( "world.skybox_color", scene.skybox.skybox_color );
				add_bool( "world.lighting", scene.lighting ); add_color( "world.lighting_color", scene.lighting_color ); add_float( "world.lighting_intensity", scene.lighting_intensity );
				add_bool( "world.world_color_enabled", scene.world_setting ); add_color( "world.world_color", scene.world_color );
				add_bool( "world.bloom", scene.bloom ); add_float( "world.bloom_value", scene.bloom_value ); add_bool( "world.gamma", scene.gamma ); add_float( "world.gamma_value", scene.gamma_value );

				return out;
			}( );
			return entries;
		}

		[[nodiscard]] api_setting* find_api_setting( const std::string& requested )
		{
			const auto name = lower_copy( requested );
			for ( auto& entry : api_settings( ) ) if ( entry.name == name ) return &entry;
			return nullptr;
		}

		void set_module_value( xui::setting& module, const bool value )
		{
			module.value = value;
			if ( module.bind.mode == xui::bind_mode::toggle ) module.bind.active = value;
			if ( value && module.bind.excludes )
			{
				module.bind.excludes->value = false;
				module.bind.excludes->bind.active = false;
			}
			settings::finalize_binds( );
		}

	} // namespace

	struct engine::impl : config::custom_field
	{
		struct scheduled_task
		{
			std::uint64_t id{};
			std::chrono::steady_clock::time_point due{};
			sol::protected_function callback{};
		};

		struct script
		{
			std::filesystem::path path{};
			std::unique_ptr<sol::state> lua{};
			std::unordered_map<std::string, std::vector<sol::protected_function>> callbacks{};
			std::vector<scheduled_task> scheduled{};
			std::uint64_t next_schedule_id{ 1 };
			std::unordered_map<std::string, ui_value> ui_values{};
			std::string error{};
			bool loaded{};
			bool faulted{};
		};

		mutable std::recursive_mutex mutex{};
		std::filesystem::path root{ "C:\\velocity-pro" };
		std::filesystem::path scripts_directory{ root / "luas" };
		std::vector<script> scripts{};
		std::vector<console_entry> console{};
		xdraw::draw_list* draw_list{};
		bool reload_requested{};
		bool initialized{};
		bool has_pending_config{};
		nlohmann::json pending_config{};
		std::string current_map{};

		[[nodiscard]] static nlohmann::json serialize_ui_value( const ui_value& value )
		{
			auto out = nlohmann::json::object( );
			switch ( value.type )
			{
			case ui_value::kind::boolean:
				out[ "type" ] = "boolean";
				out[ "value" ] = value.checkbox ? value.checkbox->value : value.boolean;
				break;
			case ui_value::kind::decimal:
				out[ "type" ] = "number";
				out[ "value" ] = value.decimal;
				break;
			case ui_value::kind::integer:
				out[ "type" ] = "integer";
				out[ "value" ] = value.integer;
				break;
			case ui_value::kind::string:
				out[ "type" ] = "string";
				out[ "value" ] = value.text;
				break;
			case ui_value::kind::color:
				out[ "type" ] = "color";
				out[ "value" ] = nlohmann::json::array( { value.color.r, value.color.g, value.color.b, value.color.a } );
				break;
			default:
				return nullptr;
			}
			return out;
		}

		[[nodiscard]] static std::optional<ui_value> deserialize_ui_value( const nlohmann::json& value )
		{
			if ( !value.is_object( ) || !value.contains( "type" ) || !value.contains( "value" ) )
			{
				return std::nullopt;
			}

			ui_value out{};
			const auto type = value.value( "type", std::string{} );
			try
			{
				if ( type == "boolean" )
				{
					out.type = ui_value::kind::boolean;
					out.boolean = value[ "value" ].get<bool>( );
				}
				else if ( type == "number" )
				{
					out.type = ui_value::kind::decimal;
					out.decimal = value[ "value" ].get<float>( );
				}
				else if ( type == "integer" )
				{
					out.type = ui_value::kind::integer;
					out.integer = value[ "value" ].get<int>( );
				}
				else if ( type == "string" )
				{
					out.type = ui_value::kind::string;
					out.text = value[ "value" ].get<std::string>( ).substr( 0, 256 );
				}
				else if ( type == "color" )
				{
					const auto& color = value[ "value" ];
					if ( !color.is_array( ) || color.size( ) < 4 ) return std::nullopt;
					out.type = ui_value::kind::color;
					out.color = xdraw::color{
						static_cast<std::uint8_t>( std::clamp( color[ 0 ].get<int>( ), 0, 255 ) ),
						static_cast<std::uint8_t>( std::clamp( color[ 1 ].get<int>( ), 0, 255 ) ),
						static_cast<std::uint8_t>( std::clamp( color[ 2 ].get<int>( ), 0, 255 ) ),
						static_cast<std::uint8_t>( std::clamp( color[ 3 ].get<int>( ), 0, 255 ) ) };
				}
				else
				{
					return std::nullopt;
				}
			}
			catch ( ... )
			{
				return std::nullopt;
			}
			return out;
		}

		void append_console( const script& entry, const console_level level, std::string message )
		{
			constexpr std::size_t maximum_entries{ 1000 };
			if ( console.size( ) >= maximum_entries )
			{
				console.erase( console.begin( ), console.begin( ) + static_cast<std::ptrdiff_t>( console.size( ) - maximum_entries + 1 ) );
			}
			console.push_back( { entry.path.filename( ).string( ), std::move( message ), level } );
		}

		[[nodiscard]] script* find( const std::string& name )
		{
			const auto requested = lower_copy( name );
			for ( auto& entry : scripts )
			{
				if ( lower_copy( entry.path.filename( ).string( ) ) == requested ) return &entry;
			}
			return nullptr;
		}

		void refresh_scripts( )
		{
			std::error_code error{};
			std::filesystem::create_directories( scripts_directory, error );
			if ( error ) return;

			std::vector<std::filesystem::path> discovered{};
			for ( const auto& directory_entry : std::filesystem::directory_iterator( scripts_directory, error ) )
			{
				if ( error ) break;
				if ( directory_entry.is_regular_file( ) && lower_copy( directory_entry.path( ).extension( ).string( ) ) == ".lua" )
				{
					discovered.push_back( directory_entry.path( ) );
				}
			}
			std::ranges::sort( discovered, [ ]( const auto& left, const auto& right )
				{ return lower_copy( left.filename( ).string( ) ) < lower_copy( right.filename( ).string( ) ); } );

			for ( auto it = scripts.begin( ); it != scripts.end( ); )
			{
				if ( std::ranges::any_of( discovered, [ & ]( const auto& path ) { return path == it->path; } ) )
				{
					++it;
					continue;
				}
				unload( *it );
				it = scripts.erase( it );
			}

			for ( const auto& path : discovered )
			{
				if ( !std::ranges::any_of( scripts, [ & ]( const auto& entry ) { return entry.path == path; } ) )
				{
					scripts.push_back( { .path = path } );
				}
			}
		}

		[[nodiscard]] nlohmann::json serialize( ) const override
		{
			const std::scoped_lock lock{ mutex };
			auto scripts_json = nlohmann::json::object( );
			for ( const auto& entry : scripts )
			{
				auto ui_json = nlohmann::json::object( );
				for ( const auto& [ key, value ] : entry.ui_values )
				{
					const auto serialized = serialize_ui_value( value );
					if ( !serialized.is_null( ) ) ui_json[ key ] = serialized;
				}

				scripts_json[ lower_copy( entry.path.filename( ).string( ) ) ] = nlohmann::json
				{
					{ "loaded", entry.loaded && !entry.faulted },
					{ "ui", std::move( ui_json ) }
				};
			}
			return nlohmann::json{ { "version", 1 }, { "scripts", std::move( scripts_json ) } };
		}

		void deserialize( const nlohmann::json& value ) override
		{
			const std::scoped_lock lock{ mutex };
			pending_config = value;
			has_pending_config = true;
			if ( initialized ) apply_pending_config( );
		}

		void apply_pending_config( )
		{
			if ( !has_pending_config ) return;
			has_pending_config = false;

			const auto config = std::move( pending_config );
			pending_config = {};
			refresh_scripts( );

			const auto scripts_it = config.find( "scripts" );
			const auto has_scripts = scripts_it != config.end( ) && scripts_it->is_object( );
			for ( auto& entry : scripts )
			{
				const auto name = lower_copy( entry.path.filename( ).string( ) );
				const nlohmann::json* saved = nullptr;
				if ( has_scripts )
				{
					const auto saved_it = scripts_it->find( name );
					if ( saved_it != scripts_it->end( ) ) saved = &*saved_it;
				}
				const auto should_load = saved && saved->is_object( ) && saved->value( "loaded", false );

				if ( entry.loaded || entry.lua ) unload( entry );
				entry.error.clear( );
				entry.faulted = false;
				std::unordered_map<std::string, ui_value> restored_ui{};

				if ( saved && saved->is_object( ) )
				{
					const auto ui = saved->find( "ui" );
					if ( ui != saved->end( ) && ui->is_object( ) )
					{
						for ( auto it = ui->begin( ); it != ui->end( ); ++it )
						{
							if ( auto restored = deserialize_ui_value( it.value( ) ) ) restored_ui.emplace( it.key( ), std::move( *restored ) );
						}
					}
				}

				entry.ui_values = std::move( restored_ui );
				if ( should_load ) load( entry );
			}
		}

		void clear_ui( script& entry )
		{
			for ( auto& [ _, value ] : entry.ui_values )
			{
				if ( value.checkbox ) xui::binds::unregister_setting( value.checkbox.get( ) );
			}
			entry.ui_values.clear( );
		}

		void report_callback_error( script& entry, sol::protected_function_result& result )
		{
			entry.error = result_error( result );
			append_console( entry, console_level::error, entry.error );
			logging::console::print( "[lua] {}: {}", entry.path.filename( ).string( ), entry.error );
			entry.faulted = true;
			entry.loaded = false;
			entry.callbacks.clear( );
			entry.scheduled.clear( );
		}

		void pump_scheduled( script& entry )
		{
			if ( !entry.lua ) return;
			constexpr std::size_t maximum_callbacks_per_pump{ 1024 };
			std::size_t callbacks_run{};
			while ( callbacks_run < maximum_callbacks_per_pump )
			{
				const auto now = std::chrono::steady_clock::now( );
				auto it = std::ranges::find_if( entry.scheduled, [ now ]( const scheduled_task& task ) { return task.due <= now; } );
				if ( it == entry.scheduled.end( ) )
				{
					break;
				}

				auto callback = std::move( it->callback );
				entry.scheduled.erase( it );
				if ( !callback.valid( ) ) continue;
				++callbacks_run;
				auto result = callback( );
				if ( !result.valid( ) )
				{
					report_callback_error( entry, result );
					break;
				}
			}
		}

		void pump_scheduled_all( )
		{
			for ( auto& entry : scripts )
				if ( entry.loaded && entry.lua ) pump_scheduled( entry );
		}

		template<typename... arguments>
		void invoke( script& entry, const std::string_view event, arguments&&... args )
		{
			if ( !entry.lua ) return;
			const auto named_callback = "on_" + std::string( event );
			auto callback = ( *entry.lua )[ named_callback ].get<sol::protected_function>( );
			if ( callback.valid( ) )
			{
				auto result = callback( std::forward<arguments>( args )... );
				if ( !result.valid( ) )
				{
					report_callback_error( entry, result );
					return;
				}
			}

			const auto found = entry.callbacks.find( std::string( event ) );
			if ( found == entry.callbacks.end( ) ) return;
			for ( auto& registered : found->second )
			{
				auto result = registered( std::forward<arguments>( args )... );
				if ( !result.valid( ) )
				{
					report_callback_error( entry, result );
					return;
				}
			}
		}

		void bind_api( sol::state& lua, script& entry )
		{
			lua.new_usertype<math::vector2>( "vec2",
				sol::constructors<math::vector2( ), math::vector2( float, float )>( ),
				"x", &math::vector2::x, "y", &math::vector2::y,
				"length", &math::vector2::length, "normalized", &math::vector2::normalized,
				sol::meta_function::addition, [ ]( const math::vector2& left, const math::vector2& right ) { return left + right; },
				sol::meta_function::subtraction, [ ]( const math::vector2& left, const math::vector2& right ) { return left - right; },
				sol::meta_function::multiplication, [ ]( const math::vector2& value, const float scale ) { return value * scale; } );

			lua.new_usertype<math::vector3>( "vec3",
				sol::constructors<math::vector3( ), math::vector3( float, float, float )>( ),
				"x", &math::vector3::x, "y", &math::vector3::y, "z", &math::vector3::z,
				"length", &math::vector3::length, "length_2d", &math::vector3::length_2d,
				"distance", &math::vector3::distance, "dot", &math::vector3::dot, "cross", &math::vector3::cross,
				"normalized", &math::vector3::normalized,
				sol::meta_function::addition, [ ]( const math::vector3& left, const math::vector3& right ) { return left + right; },
				sol::meta_function::subtraction, [ ]( const math::vector3& left, const math::vector3& right ) { return left - right; },
				sol::meta_function::multiplication, [ ]( const math::vector3& value, const float scale ) { return value * scale; } );

			lua.new_usertype<xdraw::color>( "color",
				sol::constructors<xdraw::color( ), xdraw::color( std::uint8_t, std::uint8_t, std::uint8_t, std::uint8_t )>( ),
				"r", &xdraw::color::r, "g", &xdraw::color::g, "b", &xdraw::color::b, "a", &xdraw::color::a,
				"alpha", &xdraw::color::alpha );

			lua.new_usertype<math::quaternion>( "quaternion", "x", &math::quaternion::x, "y", &math::quaternion::y,
				"z", &math::quaternion::z, "w", &math::quaternion::w );
			lua.new_usertype<entity_snapshot>( "entity", "index", &entity_snapshot::index, "type", &entity_snapshot::type,
				"class_name", &entity_snapshot::class_name, "health", &entity_snapshot::health, "team", &entity_snapshot::team,
				"alive", &entity_snapshot::alive, "enemy", &entity_snapshot::enemy, "origin", &entity_snapshot::origin, "velocity", &entity_snapshot::velocity,
				"eye_position", &entity_snapshot::eye_position, "armor", &entity_snapshot::armor, "flags", &entity_snapshot::flags,
				"simulation_time", &entity_snapshot::simulation_time, "scoped", &entity_snapshot::scoped );
			lua.new_usertype<bone_snapshot>( "bone", "id", &bone_snapshot::id, "position", &bone_snapshot::position,
				"rotation", &bone_snapshot::rotation, "scale", &bone_snapshot::scale, "fraction", &bone_snapshot::fraction,
				"fov", &bone_snapshot::fov, "hitbox_index", &bone_snapshot::hitbox_index, "hitgroup", &bone_snapshot::hitgroup,
				"visible", &bone_snapshot::visible, "valid", &bone_snapshot::valid );
			lua.new_usertype<hitbox_snapshot>( "hitbox", "index", &hitbox_snapshot::index, "bone", &hitbox_snapshot::bone,
				"mins", &hitbox_snapshot::mins, "maxs", &hitbox_snapshot::maxs, "radius", &hitbox_snapshot::radius,
				"hitgroup", &hitbox_snapshot::hitgroup, "hitgroup_name", &hitbox_snapshot::hitgroup_name,
				"shape_type", &hitbox_snapshot::shape_type, "translation_only", &hitbox_snapshot::translation_only );
			lua.new_usertype<projection_snapshot>( "projection", "screen", &projection_snapshot::screen, "valid", &projection_snapshot::valid, "on_screen", &projection_snapshot::on_screen );
			lua.new_usertype<trace_snapshot>( "trace_result", "fraction", &trace_snapshot::fraction, "start", &trace_snapshot::start,
				"end", &trace_snapshot::end, "position", &trace_snapshot::position, "normal", &trace_snapshot::normal,
				"hit_index", &trace_snapshot::hit_index, "contents", &trace_snapshot::contents, "hitbox_index", &trace_snapshot::hitbox_index,
				"bone", &trace_snapshot::bone, "hitgroup", &trace_snapshot::hitgroup, "visible", &trace_snapshot::visible,
				"hit", &trace_snapshot::hit, "all_solid", &trace_snapshot::all_solid );
			lua.new_usertype<game_event_context>( "game_event", "name", &game_event_context::name,
				"get_int", sol::overload(
					[ ]( const game_event_context& value, const std::string& key ) { return value.get_int( key ); },
					[ ]( const game_event_context& value, const std::string& key, const int fallback ) { return value.get_int( key, fallback ); } ),
				"get_float", sol::overload(
					[ ]( const game_event_context& value, const std::string& key ) { return value.get_float( key ); },
					[ ]( const game_event_context& value, const std::string& key, const float fallback ) { return value.get_float( key, fallback ); } ),
				"get_vector", sol::overload(
					[ ]( const game_event_context& value ) { return value.get_vector( ); },
					[ ]( const game_event_context& value, const std::string& x, const std::string& y, const std::string& z ) { return value.get_vector( x, y, z ); } ),
				"pawn_index", sol::overload(
					[ ]( const game_event_context& value ) { return value.pawn_index( ); },
					[ ]( const game_event_context& value, const std::string& key ) { return value.pawn_index( key ); } ),
				"is_local", sol::overload(
					[ ]( const game_event_context& value ) { return value.is_local( ); },
					[ ]( const game_event_context& value, const std::string& key ) { return value.is_local( key ); } ) );
			lua.new_usertype<command_context>( "command", "number", &command_context::number, "tick", &command_context::tick,
				"view_angles", &command_context::view_angles, "forward_move", &command_context::forward_move, "side_move", &command_context::side_move,
				"up_move", &command_context::up_move, "has_button", &command_context::has_button, "set_button", &command_context::set_button,
				"set_movement", &command_context::set_movement, "set_view_angles", &command_context::set_view_angles,
				"set_silent_angles", &command_context::set_silent_angles, "set_camera_angles", &command_context::set_camera_angles,
				"fire", sol::overload(
					[ ]( const command_context& value ) { value.fire( false ); },
					[ ]( const command_context& value, const bool secondary ) { value.fire( secondary ); } ),
				"stop_firing", &command_context::stop_firing );

			auto velocity = lua.create_named_table( "velocity" );
			velocity[ "version" ] = 5;
			velocity.set_function( "vec2", [ ]( const float x, const float y ) { return math::vector2{ x, y }; } );
			velocity.set_function( "vec3", [ ]( const float x, const float y, const float z ) { return math::vector3{ x, y, z }; } );
			velocity.set_function( "color", [ ]( const std::uint8_t r, const std::uint8_t g, const std::uint8_t b, const std::uint8_t a = 255 ) { return xdraw::color{ r, g, b, a }; } );
			velocity.set_function( "on", [ &entry ]( const std::string& event, sol::protected_function callback )
				{
					static const std::unordered_set<std::string> supported{
						"paint", "menu", "create_move_pre", "create_move", "create_move_post", "game_event", "level_init", "level_shutdown",
						"weapon_fire", "weapon_reload", "item_purchase", "item_pickup", "player_spawn", "player_blind", "player_footstep",
						"bullet_impact", "player_hurt", "player_death", "round_prestart", "round_start", "round_freeze_end", "round_end",
						"bomb_beginplant", "bomb_abortplant", "bomb_planted", "bomb_begindefuse", "bomb_abortdefuse", "bomb_defused", "bomb_exploded",
						"grenade_thrown", "inferno_startburn", "inferno_expire"
					};
					if ( supported.contains( event ) && callback.valid( ) ) entry.callbacks[ event ].push_back( std::move( callback ) );
				} );
			velocity.set_function( "schedule", [ &entry ]( const float delay, sol::protected_function callback ) -> sol::optional<std::uint64_t>
				{
					if ( !callback.valid( ) || !std::isfinite( delay ) || delay < 0.0f ) return sol::nullopt;
					const auto id = entry.next_schedule_id++;
					const auto clamped = std::clamp( delay, 0.0f, 86400.0f );
					const auto duration = std::chrono::duration_cast<std::chrono::steady_clock::duration>( std::chrono::duration<float>{ clamped } );
					entry.scheduled.push_back( { id, std::chrono::steady_clock::now( ) + duration, std::move( callback ) } );
					return id;
				} );
			velocity.set_function( "cancel_schedule", [ &entry ]( const std::uint64_t id )
				{
					auto found = entry.scheduled.end( );
					for ( auto it = entry.scheduled.begin( ); it != entry.scheduled.end( ); ++it )
					{
						if ( it->id == id ) { found = it; break; }
					}
					if ( found == entry.scheduled.end( ) ) return false;
					entry.scheduled.erase( found );
					return true;
				} );
			velocity.set_function( "reload_all", [ this ] { reload_requested = true; } );

			const auto write_console = [ this, &entry ]( const console_level level, sol::variadic_args arguments )
				{
					auto message = lua_arguments_text( arguments );
					append_console( entry, level, message );
					logging::console::print( level == console_level::error ? "[lua error] {}" : level == console_level::warning ? "[lua warning] {}" : "[lua] {}", message );
				};
			lua.set_function( "print", [ write_console ]( sol::variadic_args arguments ) { write_console( console_level::info, arguments ); } );
			lua.set_function( "Debug", [ write_console ]( sol::variadic_args arguments ) { write_console( console_level::info, arguments ); } );
			velocity.set_function( "debug", [ write_console ]( sol::variadic_args arguments ) { write_console( console_level::info, arguments ); } );

			auto log = velocity.create_named( "log" );
			log.set_function( "info", [ write_console ]( sol::variadic_args arguments ) { write_console( console_level::info, arguments ); } );
			log.set_function( "warn", [ write_console ]( sol::variadic_args arguments ) { write_console( console_level::warning, arguments ); } );
			log.set_function( "error", [ write_console ]( sol::variadic_args arguments ) { write_console( console_level::error, arguments ); } );

			auto math_api = velocity.create_named( "math" );
			math_api.set_function( "clamp", [ ]( const float value, const float minimum, const float maximum ) { return std::clamp( value, minimum, maximum ); } );
			math_api.set_function( "normalize_yaw", [ ]( const float yaw ) { return math::helpers::normalize_yaw( yaw ); } );
			math_api.set_function( "angle_to", [ ]( const math::vector3& from, const math::vector3& to ) { return math::helpers::calculate_angle( from, to ); } );
			math_api.set_function( "angle_distance", [ ]( const float from, const float to ) { return std::abs( math::helpers::normalize_yaw( to - from ) ); } );

			auto entity = velocity.create_named( "entity" );
			entity.set_function( "by_index", [ ]( const int index ) -> sol::optional<entity_snapshot>
				{
					if ( const auto cached = cached_entity( index ) ) return snapshot_entity( *cached );
					return sol::nullopt;
				} );
			entity.set_function( "list", [ ]( sol::this_state state, const std::string& type )
				{
					sol::state_view lua_view{ state };
					auto table = lua_view.create_table( );
					int output_index{};
					const auto wanted = lower_copy( type );
					for ( const auto entity_type : { systems::entities::type::player, systems::entities::type::item, systems::entities::type::projectile } )
					{
						if ( !wanted.empty( ) && wanted != "all" && wanted != entity_type_name( entity_type ) ) continue;
						for ( const auto& cached : systems::g_entities.get_by_type( entity_type ) ) table[ ++output_index ] = snapshot_entity( cached );
					}
					return table;
				} );
			entity.set_function( "players", [ ]( sol::this_state state )
				{
					sol::state_view lua_view{ state }; auto table = lua_view.create_table( ); int index{};
					for ( const auto& cached : systems::g_entities.get_by_type( systems::entities::type::player ) ) table[ ++index ] = snapshot_entity( cached );
					return table;
				} );
			entity.set_function( "items", [ ]( sol::this_state state )
				{
					sol::state_view lua_view{ state }; auto table = lua_view.create_table( ); int index{};
					for ( const auto& cached : systems::g_entities.get_by_type( systems::entities::type::item ) ) table[ ++index ] = snapshot_entity( cached );
					return table;
				} );
			entity.set_function( "projectiles", [ ]( sol::this_state state )
				{
					sol::state_view lua_view{ state }; auto table = lua_view.create_table( ); int index{};
					for ( const auto& cached : systems::g_entities.get_by_type( systems::entities::type::projectile ) ) table[ ++index ] = snapshot_entity( cached );
					return table;
				} );
			entity.set_function( "bone", [ ]( const int entity_index, const int bone_id ) -> sol::optional<bone_snapshot>
				{
					if ( bone_id < 0 || bone_id > 255 ) return sol::nullopt;
					const auto ptr = entity_ptr( entity_index );
					if ( !ptr ) return sol::nullopt;
					const auto value = systems::g_bones.get( ptr, static_cast<std::uint32_t>( bone_id ) );
					const auto valid = value.position.length_sqr( ) > 0.0001f;
					return bone_snapshot{ .id = bone_id, .position = value.position, .rotation = value.rotation, .scale = value.scale, .valid = valid };
				} );
			entity.set_function( "skeleton", [ ]( sol::this_state state, const int entity_index )
				{
					sol::state_view lua_view{ state }; auto table = lua_view.create_table( );
					const auto ptr = entity_ptr( entity_index ); if ( !ptr ) return table;
					const auto skeleton = systems::g_bones.get_skeleton( ptr ); int index{};
					for ( auto bone_id = 0u; bone_id < skeleton.size( ); ++bone_id )
					{
						const auto& value = skeleton[ bone_id ];
						if ( value.position.length_sqr( ) <= 0.0001f ) continue;
						table[ ++index ] = bone_snapshot{ .id = static_cast<int>( bone_id ), .position = value.position, .rotation = value.rotation, .scale = value.scale, .valid = true };
					}
					return table;
				} );
			entity.set_function( "hitboxes", [ ]( sol::this_state state, const int entity_index )
				{
					sol::state_view lua_view{ state }; auto table = lua_view.create_table( );
					const auto ptr = entity_ptr( entity_index ); if ( !ptr ) return table;
					const auto node = memory::safe_read<std::uintptr_t>( ptr + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) ).value_or( 0 );
					if ( !node ) return table;
					const auto hitboxes = systems::g_hitboxes.query( node, true ); int index{};
					for ( const auto& value : hitboxes )
					{
						const auto hitgroup = systems::g_hitboxes.hitgroup_from_hitbox( value.index );
						table[ ++index ] = hitbox_snapshot{
							.index = value.index, .bone = value.bone, .mins = value.mins, .maxs = value.maxs, .radius = value.radius,
							.hitgroup = hitgroup, .hitgroup_name = systems::g_hitboxes.hitgroup_to_name( hitgroup ),
							.shape_type = value.shape_type, .translation_only = value.translation_only
						};
					}
					return table;
				} );

			auto player = velocity.create_named( "player" );
			player.set_function( "local", [ ]
				{
					const auto local = systems::g_local.get( );
					return local.pawn ? snapshot_entity( { .ptr = local.pawn, .index = -1, .type = systems::entities::type::player } ) : entity_snapshot{};
				} );
			player.set_function( "list", [ ]( sol::this_state state )
				{
					sol::state_view lua_view{ state }; auto table = lua_view.create_table( ); int index{};
					for ( const auto& cached : systems::g_entities.get_by_type( systems::entities::type::player ) ) table[ ++index ] = snapshot_entity( cached );
					return table;
				} );

			auto view = velocity.create_named( "view" );
			view.set_function( "origin", [ ] { return systems::g_view.origin( ); } );
			view.set_function( "angles", [ ] { return systems::g_view.angles( ); } );
			view.set_function( "fov", [ ] { return systems::g_view.fov( ); } );
			view.set_function( "project", [ ]( const math::vector3& world )
				{
					const auto full = systems::g_view.project_full( world );
					return projection_snapshot{ .screen = full.screen, .valid = systems::g_view.projection_valid( full.screen ), .on_screen = full.on_screen };
				} );

			auto trace = velocity.create_named( "trace" );
			trace.set_function( "line", [ ]( const math::vector3& start, const math::vector3& end, const sol::optional<int>& skip_index,
				const sol::optional<std::uintptr_t>& mask, const sol::optional<int>& layer )
				{
					const auto skip = skip_index ? entity_ptr( *skip_index ) : systems::g_local.get( ).pawn;
					return snapshot_trace( systems::g_tracing.trace( start, end, skip, mask.value_or( 0x1c3003 ), static_cast<std::uint8_t>( std::clamp( layer.value_or( 4 ), 0, 255 ) ) ) );
				} );
			trace.set_function( "hull", [ ]( const math::vector3& start, const math::vector3& end, const math::vector3& mins, const math::vector3& maxs,
				const sol::optional<int>& skip_index, const sol::optional<std::uintptr_t>& mask, const sol::optional<int>& layer )
				{
					const auto skip = skip_index ? entity_ptr( *skip_index ) : systems::g_local.get( ).pawn;
					return snapshot_trace( systems::g_tracing.trace_hull( start, end, mins, maxs, skip, mask.value_or( 0x1c3003 ), static_cast<std::uint8_t>( std::clamp( layer.value_or( 4 ), 0, 255 ) ) ) );
				} );
			trace.set_function( "sphere", [ ]( const math::vector3& start, const math::vector3& end, const float radius,
				const sol::optional<int>& skip_index, const sol::optional<std::uintptr_t>& mask, const sol::optional<int>& layer )
				{
					const auto skip = skip_index ? entity_ptr( *skip_index ) : systems::g_local.get( ).pawn;
					const auto filter = systems::g_tracing.make_filter( skip, mask.value_or( 0x1c3003 ), static_cast<std::uint8_t>( std::clamp( layer.value_or( 4 ), 0, 255 ) ) );
					return snapshot_trace( systems::g_tracing.trace_sphere( start, end, std::max( radius, 0.0f ), filter ) );
				} );
			trace.set_function( "to_entity", [ ]( const math::vector3& start, const math::vector3& end, const int target_index,
				const sol::optional<int>& skip_index, const sol::optional<std::uintptr_t>& mask, const sol::optional<int>& layer ) -> sol::optional<trace_snapshot>
				{
					const auto target = entity_ptr( target_index ); if ( !target ) return sol::nullopt;
					const auto skip = skip_index ? entity_ptr( *skip_index ) : systems::g_local.get( ).pawn;
					return snapshot_trace( systems::g_tracing.trace_to_entity( start, end, target, skip, mask.value_or( 0x1c3003 ), static_cast<std::uint8_t>( std::clamp( layer.value_or( 4 ), 0, 255 ) ) ) );
				} );
			trace.set_function( "visible", [ ]( const math::vector3& start, const math::vector3& end, const int target_index,
				const sol::optional<int>& skip_index, const sol::optional<std::uintptr_t>& mask )
				{
					const auto target = entity_ptr( target_index ); const auto skip = skip_index ? entity_ptr( *skip_index ) : systems::g_local.get( ).pawn;
					return target && systems::g_tracing.is_visible( start, end, target, skip, mask.value_or( 0x1c3003 ) );
				} );
			trace.set_function( "hitbox", [ ]( const math::vector3& start, const int target_index, const int hitbox_index,
				const sol::optional<int>& skip_index, const sol::optional<std::uintptr_t>& mask, const sol::optional<int>& layer ) -> sol::optional<trace_snapshot>
				{
					const auto resolved = resolve_hitbox( target_index, hitbox_index );
					if ( !resolved ) return sol::nullopt;
					const auto target = entity_ptr( target_index );
					const auto skip = skip_index ? entity_ptr( *skip_index ) : systems::g_local.get( ).pawn;
					const auto result = systems::g_tracing.trace( start, resolved->center, skip,
						mask.value_or( 0x1c3003 ), static_cast<std::uint8_t>( std::clamp( layer.value_or( 4 ), 0, 255 ) ) );
					auto snapshot = snapshot_trace( result );
					snapshot.hitbox_index = hitbox_index;
					snapshot.bone = resolved->entry.bone;
					snapshot.hitgroup = systems::g_hitboxes.hitgroup_from_hitbox( hitbox_index );
					snapshot.visible = result.hit_entity == target || result.fraction > 0.97f;
					return snapshot;
				} );
			trace.set_function( "most_visible_bone", [ ]( const math::vector3& start, const int target_index, const sol::optional<sol::table>& candidates,
				const sol::optional<int>& skip_index, const sol::optional<std::uintptr_t>& mask, const sol::optional<int>& layer ) -> sol::optional<bone_snapshot>
				{
					const auto target = entity_ptr( target_index );
					if ( !target ) return sol::nullopt;
					std::vector<int> bone_ids{};
					if ( candidates ) for ( std::size_t i = 1; i <= candidates->size( ); ++i )
					{
						if ( const auto id = candidates->get<sol::optional<int>>( i ); id && *id >= 0 && *id <= 255 ) bone_ids.push_back( *id );
					}
					if ( bone_ids.empty( ) ) bone_ids = { 7, 6, 2, 1 };
					const auto skip = skip_index ? entity_ptr( *skip_index ) : systems::g_local.get( ).pawn;
					const auto view_angles = systems::g_input.get_view_angles( );
					sol::optional<bone_snapshot> best{};
					for ( const auto bone_id : bone_ids )
					{
						const auto bone = systems::g_bones.get( target, static_cast<std::uint32_t>( bone_id ) );
						if ( bone.position.length_sqr( ) <= 0.0001f ) continue;
						const auto aim = math::helpers::calculate_angle( start, bone.position );
						const auto fov = math::helpers::angle_distance( view_angles, aim );
						const auto result = systems::g_tracing.trace( start, bone.position, skip,
							mask.value_or( 0x1c3003 ), static_cast<std::uint8_t>( std::clamp( layer.value_or( 4 ), 0, 255 ) ) );
						const auto visible = result.hit_entity == target || result.fraction > 0.97f;
						if ( !visible ) continue;
						if ( !best || fov < best->fov ) best = bone_snapshot{
							.id = bone_id, .position = bone.position, .rotation = bone.rotation, .scale = bone.scale,
							.fraction = result.fraction, .fov = fov, .visible = true, .valid = true
						};
					}
					return best;
				} );

			auto time = velocity.create_named( "time" );
			time.set_function( "delta_time", [ ] { return xdraw::delta_time( ); } );
			time.set_function( "tick_interval", [ ] { return cstypes::tick_interval; } );
			time.set_function( "current_time", [ ] { return memory::safe_read<float>( addresses::globals::global_vars + 0x30 ).value_or( 0.0f ); } );
			time.set_function( "tick", [ ] { return cstypes::time_to_ticks( memory::safe_read<float>( addresses::globals::global_vars + 0x30 ).value_or( 0.0f ) ); } );

			auto screen = velocity.create_named( "screen" );
			screen.set_function( "size", [ ] { const auto [ w, h ] = xdraw::viewport_size( ); return math::vector2{ static_cast<float>( w ), static_cast<float>( h ) }; } );
			screen.set_function( "center", [ ] { const auto [ w, h ] = xdraw::viewport_size( ); return math::vector2{ w * 0.5f, h * 0.5f }; } );

			auto input = velocity.create_named( "input" );
			input.set_function( "view_angles", [ ] { return systems::g_input.get_view_angles( ); } );
			input.set_function( "set_view_angles", [ ]( math::vector3 angles )
				{
					angles.x = std::clamp( angles.x, -89.0f, 89.0f ); angles.y = math::helpers::normalize_yaw( angles.y ); angles.z = 0.0f;
					systems::g_input.set_view_angles( angles );
				} );
			input.set_function( "set_silent_angles", [ ]( math::vector3 angles )
				{
					const command_context context{ systems::g_input.get( ) }; if ( !context.command ) return false; context.set_silent_angles( angles ); return true;
				} );
			input.set_function( "fire", [ ]( const sol::optional<bool>& secondary )
				{
					const command_context context{ systems::g_input.get( ) }; if ( !context.command ) return false; context.fire( secondary.value_or( false ) ); return true;
				} );
			input.set_function( "stop_firing", [ ]
				{
					const command_context context{ systems::g_input.get( ) }; if ( !context.command ) return false; context.stop_firing( ); return true;
				} );
			input.set_function( "is_key_down", [ ]( const int virtual_key ) { return xui::ctx( ).input.key_down( virtual_key ); } );

			auto config_api = velocity.create_named( "config" );
			config_api.set_function( "exists", [ ]( const std::string& name ) { return find_api_setting( name ) != nullptr; } );
			config_api.set_function( "type", [ ]( const std::string& name ) -> sol::optional<std::string>
				{
					if ( const auto* setting = find_api_setting( name ) ) return std::string{ api_setting_type_name( setting->kind ) };
					return sol::nullopt;
				} );
			config_api.set_function( "get", [ ]( const std::string& name, sol::this_state state ) -> sol::object
				{
					sol::state_view lua_view{ state }; const auto* setting = find_api_setting( name );
					if ( !setting ) return sol::make_object( lua_view, sol::nil );
					switch ( setting->kind )
					{
					case api_setting_kind::boolean: return sol::make_object( lua_view, *static_cast<bool*>( setting->value ) );
					case api_setting_kind::integer: return sol::make_object( lua_view, *static_cast<int*>( setting->value ) );
					case api_setting_kind::decimal: return sol::make_object( lua_view, *static_cast<float*>( setting->value ) );
					case api_setting_kind::color: return sol::make_object( lua_view, *static_cast<xdraw::color*>( setting->value ) );
					case api_setting_kind::string: return sol::make_object( lua_view, *static_cast<std::string*>( setting->value ) );
					default: return sol::make_object( lua_view, sol::nil );
					}
				} );
			config_api.set_function( "set", [ ]( const std::string& name, sol::object value )
				{
					auto* setting = find_api_setting( name ); if ( !setting || !value.valid( ) ) return false;
					switch ( setting->kind )
					{
					case api_setting_kind::boolean:
						if ( !value.is<bool>( ) ) return false;
						if ( setting->module ) set_module_value( *setting->module, value.as<bool>( ) ); else *static_cast<bool*>( setting->value ) = value.as<bool>( );
						return true;
					case api_setting_kind::integer:
						if ( !value.is<int>( ) ) return false; *static_cast<int*>( setting->value ) = value.as<int>( ); return true;
					case api_setting_kind::decimal:
						if ( !value.is<double>( ) && !value.is<float>( ) && !value.is<int>( ) ) return false;
						*static_cast<float*>( setting->value ) = value.as<float>( ); return true;
					case api_setting_kind::color:
						if ( !value.is<xdraw::color>( ) ) return false; *static_cast<xdraw::color*>( setting->value ) = value.as<xdraw::color>( ); return true;
					case api_setting_kind::string:
						if ( !value.is<std::string>( ) ) return false; *static_cast<std::string*>( setting->value ) = value.as<std::string>( ).substr( 0, 512 ); return true;
					default: return false;
					}
				} );
			config_api.set_function( "list", [ ]( sol::this_state state, const sol::optional<std::string>& prefix )
				{
					sol::state_view lua_view{ state }; auto table = lua_view.create_table( ); int index{};
					const auto wanted = prefix ? lower_copy( *prefix ) : std::string{};
					for ( const auto& setting : api_settings( ) ) if ( wanted.empty( ) || setting.name.starts_with( wanted ) ) table[ ++index ] = setting.name;
					return table;
				} );

			auto modules = velocity.create_named( "modules" );
			modules.set_function( "exists", [ ]( const std::string& name ) { const auto* setting = find_api_setting( name ); return setting && setting->module; } );
			modules.set_function( "get", [ ]( const std::string& name ) -> sol::optional<bool>
				{ if ( const auto* setting = find_api_setting( name ); setting && setting->module ) return setting->module->value; return sol::nullopt; } );
			modules.set_function( "state", [ ]( sol::this_state state, const std::string& name ) -> sol::object
				{
					sol::state_view lua_view{ state };
					const auto requested = lower_copy( name );
					const auto rage_state = requested == "ragebot" || requested == "ragebot.enabled";
					const auto legit_state = requested == "legitbot" || requested == "legitbot.enabled";
					const auto antiaim_state = requested == "antiaim" || requested == "antiaim.enabled";
					const auto quickpeek_state = requested == "combat.quickpeek" || requested == "quickpeek";
					const auto movement_setting = find_api_setting( requested );
					const auto movement_state = requested == "movement" || ( requested.starts_with( "movement." ) && movement_setting && movement_setting->module );
					const auto local_state = requested == "player.local" || requested == "local";
					if ( !rage_state && !legit_state && !antiaim_state && !quickpeek_state && !movement_state && !local_state )
						return sol::make_object( lua_view, sol::nil );

					const auto local = systems::g_local.get( );
					auto values = lua_view.create_table( );
					values[ "module" ] = requested;
					const auto enabled_key = rage_state ? "ragebot.enabled" : legit_state ? "legitbot.enabled" : antiaim_state ? "antiaim.enabled"
						: quickpeek_state ? "combat.quickpeek" : requested;
					if ( const auto* setting = find_api_setting( enabled_key ); setting && setting->module ) values[ "enabled" ] = setting->module->value;

					if ( rage_state )
					{
						values[ "has_target" ] = features::combat::g_rage.has_target( );
						values[ "target_index" ] = features::combat::g_rage.target_entity_index( );
						values[ "target_hitbox" ] = features::combat::g_rage.target_hitbox( );
						values[ "target_hitgroup" ] = features::combat::g_rage.target_hitgroup( );
						values[ "target_position" ] = features::combat::g_rage.target_position( );
						values[ "target_damage" ] = features::combat::g_rage.target_damage( );
						values[ "target_hitchance" ] = features::combat::g_rage.target_hitchance( );
						values[ "target_fov" ] = features::combat::g_rage.target_fov( );
						values[ "firing_this_tick" ] = features::combat::g_rage.is_firing_this_tick( );
						values[ "should_stop" ] = features::combat::g_rage.should_stop( );
						values[ "air_stop_requested" ] = features::combat::g_rage.air_stop_requested( );
						values[ "revolver_cocking" ] = features::combat::g_rage.is_cocking_revolver( );
						const auto dt = features::combat::g_rage.get_doubletap_state( );
						values[ "doubletap_state" ] = dt == features::combat::rage::doubletap_state::ready ? "ready" : dt == features::combat::rage::doubletap_state::charging ? "charging" : "uncharged";
						values[ "doubletap_progress" ] = features::combat::g_rage.dt( ).progress( );
						values[ "doubletap_claimed_tick" ] = features::combat::g_rage.dt( ).claimed_tick( );
						values[ "doubletap_last_fire_tick" ] = features::combat::g_rage.dt( ).last_fire_tick( );
					}
					else if ( legit_state )
					{
						values[ "has_target" ] = features::combat::g_legit.has_target( );
						values[ "target_index" ] = entity_index_from_ptr( features::combat::g_legit.target_pawn( ) );
						values[ "target_hitbox" ] = features::combat::g_legit.target_hitbox( );
						values[ "target_hitgroup" ] = features::combat::g_legit.target_hitgroup( );
						values[ "target_position" ] = features::combat::g_legit.target_position( );
						values[ "target_damage" ] = features::combat::g_legit.target_damage( );
						values[ "target_hitchance" ] = features::combat::g_legit.target_hitchance( );
						values[ "target_fov" ] = features::combat::g_legit.target_fov( );
					}
					else if ( antiaim_state )
					{
						const auto& aa = features::combat::g_misc.antiaim( );
						values[ "active" ] = aa.is_active( ); values[ "desync_choking" ] = aa.is_desync_choking( );
						values[ "modified_angles" ] = aa.get_modified_angles( ); values[ "visual_angles" ] = aa.get_visual_angles( );
						values[ "has_visual_angles" ] = aa.has_visual_angles( ); values[ "final_ground_movement" ] = aa.has_final_ground_movement( );
					}
					else if ( quickpeek_state )
					{
						const auto& quickpeek = features::combat::g_misc.quickpeek( );
						values[ "active" ] = quickpeek.is_active( ); values[ "retracking" ] = quickpeek.is_retracking( );
					}
					else if ( movement_state )
					{
						values[ "jumpbug_active" ] = features::movement::g_jumpbug.active_this_tick( );
						values[ "jumpbug_landing_fraction" ] = features::movement::g_jumpbug.landing_fraction( );
						values[ "edgebug_active" ] = features::movement::g_edgebug.active_this_tick( );
						values[ "test_strafer_active" ] = features::movement::g_test_strafer.is_active( );
						values[ "test_strafer_handled" ] = features::movement::g_test_strafer.handled_this_tick( );
					}
					else if ( local_state )
					{
						values[ "valid" ] = local.is_valid( ); values[ "alive" ] = local.is_alive; values[ "team" ] = local.team;
						values[ "controller" ] = static_cast<std::uint64_t>( local.controller ); values[ "pawn" ] = static_cast<std::uint64_t>( local.pawn );
					}
					return sol::make_object( lua_view, readonly_table( state, values ) );
				} );
			modules.set_function( "set", [ ]( const std::string& name, const bool value )
				{ if ( auto* setting = find_api_setting( name ); setting && setting->module ) { set_module_value( *setting->module, value ); return true; } return false; } );
			modules.set_function( "toggle", [ ]( const std::string& name ) -> sol::optional<bool>
				{
					if ( auto* setting = find_api_setting( name ); setting && setting->module )
					{
						set_module_value( *setting->module, !setting->module->value ); return setting->module->value;
					}
					return sol::nullopt;
				} );
			modules.set_function( "list", [ ]( sol::this_state state, const sol::optional<std::string>& prefix )
				{
					sol::state_view lua_view{ state }; auto table = lua_view.create_table( ); int index{};
					const auto wanted = prefix ? lower_copy( *prefix ) : std::string{};
					for ( const auto& setting : api_settings( ) ) if ( setting.module && ( wanted.empty( ) || setting.name.starts_with( wanted ) ) ) table[ ++index ] = setting.name;
					return table;
				} );
			modules.set_function( "bind", [ ]( const std::string& name, sol::this_state state ) -> sol::object
				{
					sol::state_view lua_view{ state }; const auto* setting = find_api_setting( name );
					if ( !setting || !setting->module ) return sol::make_object( lua_view, sol::nil );
					auto table = lua_view.create_table( ); table[ "key" ] = setting->module->bind.key;
					table[ "mode" ] = std::string{ xui::binds::mode_name( setting->module->bind.mode ) };
					table[ "active" ] = setting->module->bind.active; return sol::make_object( lua_view, table );
				} );
			modules.set_function( "set_bind", [ ]( const std::string& name, const int key, const std::string& mode )
				{
					auto* setting = find_api_setting( name ); if ( !setting || !setting->module || key < 0 || key > 255 ) return false;
					const auto lowered = lower_copy( mode );
					if ( lowered == "toggle" ) setting->module->bind.mode = xui::bind_mode::toggle;
					else if ( lowered == "hold_on" || lowered == "hold on" ) setting->module->bind.mode = xui::bind_mode::hold_on;
					else if ( lowered == "hold_off" || lowered == "hold off" ) setting->module->bind.mode = xui::bind_mode::hold_off;
					else return false;
					setting->module->bind.key = key; setting->module->bind.active = setting->module->bind.mode == xui::bind_mode::toggle && setting->module->value;
					return true;
				} );
			modules.set_function( "clear_bind", [ ]( const std::string& name )
				{
					auto* setting = find_api_setting( name ); if ( !setting || !setting->module ) return false;
					setting->module->bind.key = 0; setting->module->bind.active = false; return true;
				} );

			// Backward-compatible boolean feature API, now backed by the complete module registry.
			auto features = velocity.create_named( "features" );
			features.set_function( "get", [ ]( const std::string& name ) -> sol::optional<bool> { const auto* setting = find_api_setting( name ); if ( setting && setting->module ) return setting->module->value; return sol::nullopt; } );
			features.set_function( "set", [ ]( const std::string& name, const bool value ) { if ( auto* setting = find_api_setting( name ); setting && setting->module ) set_module_value( *setting->module, value ); } );
			features.set_function( "list", [ ]( sol::this_state state )
				{
					sol::state_view lua_view{ state }; auto table = lua_view.create_table( ); int index{};
					for ( const auto& setting : api_settings( ) ) if ( setting.module ) table[ ++index ] = setting.name;
					return table;
				} );

			auto draw = velocity.create_named( "draw" );
			draw.set_function( "line", [ this ]( const math::vector2& start, const math::vector2& end, const xdraw::color color, const float thickness = 1.0f ) { if ( draw_list ) draw_list->line( start.x, start.y, end.x, end.y, color, std::max( thickness, 0.1f ) ); } );
			draw.set_function( "rect", [ this ]( const math::vector2& position, const math::vector2& size, const xdraw::color color, const float thickness = 1.0f ) { if ( draw_list ) draw_list->rect( position.x, position.y, size.x, size.y, color, std::max( thickness, 0.1f ) ); } );
			draw.set_function( "filled_rect", [ this ]( const math::vector2& position, const math::vector2& size, const xdraw::color color ) { if ( draw_list ) draw_list->rect_filled( position.x, position.y, size.x, size.y, color ); } );
			draw.set_function( "circle", [ this ]( const math::vector2& center, const float radius, const xdraw::color color, const float thickness = 1.0f ) { if ( draw_list ) draw_list->circle( center.x, center.y, std::max( radius, 0.0f ), color, std::max( thickness, 0.1f ) ); } );
			draw.set_function( "filled_circle", [ this ]( const math::vector2& center, const float radius, const xdraw::color color ) { if ( draw_list ) draw_list->circle_filled( center.x, center.y, std::max( radius, 0.0f ), color ); } );
			draw.set_function( "triangle", [ this ]( const math::vector2& a, const math::vector2& b, const math::vector2& c, const xdraw::color color ) { if ( draw_list ) draw_list->triangle_filled( a.x, a.y, b.x, b.y, c.x, c.y, color ); } );
			draw.set_function( "text", [ this ]( const math::vector2& position, const std::string& text, const xdraw::color color ) { if ( draw_list ) draw_list->text( position.x, position.y, text, color, xdraw::text_style::outlined ); } );
			draw.set_function( "measure_text", [ ]( const std::string& text ) { const auto [ w, h ] = xdraw::measure_text( text ); return math::vector2{ w, h }; } );
			draw.set_function( "world_line", [ this ]( const math::vector3& start, const math::vector3& end, const xdraw::color color, const float thickness = 1.0f )
				{
					if ( !draw_list ) return; const auto a = systems::g_view.project_full( start ); const auto b = systems::g_view.project_full( end );
					if ( a.on_screen && b.on_screen ) draw_list->line( a.screen.x, a.screen.y, b.screen.x, b.screen.y, color, std::max( thickness, 0.1f ) );
				} );

			auto ui = velocity.create_named( "ui" );
			ui.set_function( "text", [ &entry ]( const std::string& text ) { xui::push_id( entry.path.filename( ).string( ) ); xui::text( text, tokens::col_text ); xui::pop_id( ); } );
			ui.set_function( "separator", [ ] { xui::layout::separator( ); } );
			ui.set_function( "checkbox", [ &entry ]( const std::string& key, const bool default_value )
				{
					auto& value = entry.ui_values[ key ];
					if ( !value.checkbox )
					{
						const auto initial = value.type == ui_value::kind::boolean ? value.boolean : default_value;
						value.checkbox = std::make_unique<xui::setting>( initial );
						value.boolean = initial;
						value.type = ui_value::kind::boolean;
					}
					xui::push_id( entry.path.filename( ).string( ) ); xui::checkbox( key, *value.checkbox ); xui::pop_id( );
					return value.checkbox->value;
				} );
			ui.set_function( "slider_float", [ &entry ]( const std::string& key, const float default_value, const float minimum, const float maximum )
				{
				auto [ it, inserted ] = entry.ui_values.try_emplace( key ); if ( inserted || it->second.type != ui_value::kind::decimal ) { it->second.decimal = default_value; it->second.type = ui_value::kind::decimal; }
					xui::push_id( entry.path.filename( ).string( ) ); xui::slider_float( key, it->second.decimal, minimum, maximum ); xui::pop_id( ); return it->second.decimal;
				} );
			ui.set_function( "slider_int", [ &entry ]( const std::string& key, const int default_value, const int minimum, const int maximum )
				{
				auto [ it, inserted ] = entry.ui_values.try_emplace( key ); if ( inserted || it->second.type != ui_value::kind::integer ) { it->second.integer = default_value; it->second.type = ui_value::kind::integer; }
					xui::push_id( entry.path.filename( ).string( ) ); xui::slider_int( key, it->second.integer, minimum, maximum ); xui::pop_id( ); return it->second.integer;
				} );
			ui.set_function( "combo", [ &entry ]( const std::string& key, const int default_value, sol::table options )
				{
					std::vector<std::string> labels{}; std::vector<const char*> items{};
					for ( std::size_t i = 1; i <= options.size( ); ++i ) if ( auto label = options.get<sol::optional<std::string>>( i ) ) labels.push_back( *label );
					for ( const auto& label : labels ) items.push_back( label.c_str( ) );
				auto [ it, inserted ] = entry.ui_values.try_emplace( key ); if ( inserted || it->second.type != ui_value::kind::integer ) { it->second.integer = default_value; it->second.type = ui_value::kind::integer; }
					if ( !items.empty( ) ) { it->second.integer = std::clamp( it->second.integer, 0, static_cast<int>( items.size( ) - 1 ) ); xui::push_id( entry.path.filename( ).string( ) ); xui::combo( key, it->second.integer, items.data( ), static_cast<int>( items.size( ) ) ); xui::pop_id( ); }
					return it->second.integer;
				} );
			ui.set_function( "color_picker", [ &entry ]( const std::string& key, const xdraw::color default_value )
				{
				auto [ it, inserted ] = entry.ui_values.try_emplace( key ); if ( inserted || it->second.type != ui_value::kind::color ) { it->second.color = default_value; it->second.type = ui_value::kind::color; }
					xui::push_id( entry.path.filename( ).string( ) ); xui::color_picker( key, it->second.color ); xui::pop_id( ); return it->second.color;
				} );
			ui.set_function( "input_text", [ &entry ]( const std::string& key, const std::string& default_value, const int max_length = 128 )
				{
				auto [ it, inserted ] = entry.ui_values.try_emplace( key ); if ( inserted || it->second.type != ui_value::kind::string ) { it->second.text = default_value; it->second.type = ui_value::kind::string; }
					xui::push_id( entry.path.filename( ).string( ) ); xui::text_input( key, it->second.text, static_cast<std::size_t>( std::clamp( max_length, 1, 256 ) ) ); xui::pop_id( ); return it->second.text;
				} );
			ui.set_function( "button", [ &entry ]( const std::string& key ) { xui::push_id( entry.path.filename( ).string( ) ); const auto clicked = xui::button( key, xui::layout::item_width( ), 24.0f ); xui::pop_id( ); return clicked; } );
			ui.set_function( "get", [ &entry ]( const std::string& key, sol::this_state state ) -> sol::object
				{
					sol::state_view lua_view{ state }; const auto found = entry.ui_values.find( key ); if ( found == entry.ui_values.end( ) ) return sol::make_object( lua_view, sol::nil );
					const auto& value = found->second;
					switch ( value.type )
					{
					case ui_value::kind::boolean: return sol::make_object( lua_view, value.checkbox && value.checkbox->value );
					case ui_value::kind::decimal: return sol::make_object( lua_view, value.decimal );
					case ui_value::kind::integer: return sol::make_object( lua_view, value.integer );
					case ui_value::kind::string: return sol::make_object( lua_view, value.text );
					case ui_value::kind::color: return sol::make_object( lua_view, value.color );
					default: return sol::make_object( lua_view, sol::nil );
					}
				} );
			ui.set_function( "reset", [ this, &entry ]( const std::string& key ) { const auto found = entry.ui_values.find( key ); if ( found != entry.ui_values.end( ) ) { if ( found->second.checkbox ) xui::binds::unregister_setting( found->second.checkbox.get( ) ); entry.ui_values.erase( found ); } } );
		}

		void unload( script& entry )
		{
			clear_ui( entry );
			entry.callbacks.clear( );
			entry.scheduled.clear( );
			entry.next_schedule_id = 1;
			entry.lua.reset( );
			entry.loaded = false;
		}

		[[nodiscard]] bool load( script& entry )
		{
			std::unordered_map<std::string, ui_value> preserved_ui{};
			for ( const auto& [ key, value ] : entry.ui_values )
			{
				const auto serialized = serialize_ui_value( value );
				if ( auto restored = deserialize_ui_value( serialized ) ) preserved_ui.emplace( key, std::move( *restored ) );
			}
			unload( entry );
			entry.ui_values = std::move( preserved_ui );
			entry.error.clear( );
			entry.faulted = false;
			entry.lua = std::make_unique<sol::state>( );
			entry.lua->open_libraries( sol::lib::base, sol::lib::coroutine, sol::lib::math, sol::lib::string, sol::lib::table, sol::lib::utf8 );
			bind_api( *entry.lua, entry );

			auto result = entry.lua->safe_script_file( entry.path.string( ), sol::script_pass_on_error );
			if ( !result.valid( ) )
			{
				entry.error = result_error( result ); append_console( entry, console_level::error, entry.error ); entry.lua.reset( );
				logging::console::print( "[lua] failed to load {}: {}", entry.path.filename( ).string( ), entry.error );
				return false;
			}

			entry.loaded = true;
			auto on_load = ( *entry.lua )[ "on_load" ].get<sol::protected_function>( );
			if ( on_load.valid( ) )
			{
				auto callback_result = on_load( );
				if ( !callback_result.valid( ) ) { entry.error = result_error( callback_result ); append_console( entry, console_level::error, entry.error ); unload( entry ); return false; }
			}
			logging::console::print( "[lua] loaded {}", entry.path.filename( ).string( ) );
			return true;
		}
	};

	engine::engine( ) : m_impl{ std::make_unique<impl>( ) }
	{
		config::detail::register_field( {
			.key = config::detail::make_key( "lua", "scripts" ),
			.type = config::field_type::custom,
			.ptr = m_impl.get( ),
			.count = 1 } );
	}
	engine::~engine( ) = default;

	void engine::initialize( )
	{
		const std::scoped_lock lock{ m_impl->mutex };
		m_impl->initialized = true;
		m_impl->refresh_scripts( );
		logging::console::print( "[lua] scripts directory: {}", m_impl->scripts_directory.string( ) );
		m_impl->apply_pending_config( );
	}

	void engine::shutdown( )
	{
		const std::scoped_lock lock{ m_impl->mutex };
		for ( auto& entry : m_impl->scripts ) m_impl->unload( entry );
		m_impl->draw_list = nullptr;
	}

	void engine::refresh( )
	{
		const std::scoped_lock lock{ m_impl->mutex };
		m_impl->refresh_scripts( );
		m_impl->apply_pending_config( );
	}

	void engine::reload_all( )
	{
		const std::scoped_lock lock{ m_impl->mutex };
		this->refresh( );
		for ( auto& entry : m_impl->scripts ) m_impl->load( entry );
	}

	void engine::on_render( xdraw::draw_list& draw_list )
	{
		const std::scoped_lock lock{ m_impl->mutex };
		m_impl->draw_list = &draw_list;
		m_impl->pump_scheduled_all( );
		for ( auto& entry : m_impl->scripts ) if ( entry.loaded && entry.lua ) m_impl->invoke( entry, "paint" );
		m_impl->draw_list = nullptr;
		if ( m_impl->reload_requested ) { m_impl->reload_requested = false; this->reload_all( ); }
	}

	void engine::on_menu( )
	{
		const std::scoped_lock lock{ m_impl->mutex };
		for ( auto& entry : m_impl->scripts )
		{
			if ( !entry.loaded || !entry.lua ) continue;
			xui::push_id( entry.path.filename( ).string( ) );
			m_impl->invoke( entry, "menu" );
			xui::pop_id( );
		}
	}

	void engine::on_create_move_pre( void* raw_command )
	{
		const std::scoped_lock lock{ m_impl->mutex };
		const auto command = static_cast<systems::input::usercmd*>( raw_command );
		if ( !command ) return;
		const command_context context{ command };
		for ( auto& entry : m_impl->scripts ) if ( entry.loaded && entry.lua ) m_impl->invoke( entry, "create_move_pre", context );
	}

	void engine::on_create_move( void* raw_command )
	{
		const std::scoped_lock lock{ m_impl->mutex };
		const auto command = static_cast<systems::input::usercmd*>( raw_command );
		if ( !command ) return;
		const command_context context{ command };
		for ( auto& entry : m_impl->scripts ) if ( entry.loaded && entry.lua ) m_impl->invoke( entry, "create_move", context );
	}

	void engine::on_create_move_post( void* raw_command )
	{
		const std::scoped_lock lock{ m_impl->mutex };
		const auto command = static_cast<systems::input::usercmd*>( raw_command );
		if ( !command ) return;
		const command_context context{ command };
		for ( auto& entry : m_impl->scripts ) if ( entry.loaded && entry.lua ) m_impl->invoke( entry, "create_move_post", context );
	}

	void engine::on_game_event( const char* event_name, void* raw_event )
	{
		const std::scoped_lock lock{ m_impl->mutex };
		if ( !event_name || !raw_event ) return;
		const game_event_context context{ event_name, reinterpret_cast<std::uintptr_t>( raw_event ) };
		for ( auto& entry : m_impl->scripts )
		{
			if ( !entry.loaded || !entry.lua ) continue;
			m_impl->invoke( entry, "game_event", context );
			m_impl->invoke( entry, event_name, context );
		}
	}

	void engine::on_level_init( const char* map_name )
	{
		const std::scoped_lock lock{ m_impl->mutex };
		m_impl->current_map = map_name ? map_name : "";
		for ( auto& entry : m_impl->scripts ) if ( entry.loaded && entry.lua ) m_impl->invoke( entry, "level_init", m_impl->current_map );
	}

	void engine::on_level_shutdown( )
	{
		const std::scoped_lock lock{ m_impl->mutex };
		for ( auto& entry : m_impl->scripts ) if ( entry.loaded && entry.lua ) m_impl->invoke( entry, "level_shutdown" );
		m_impl->current_map.clear( );
	}

	bool engine::load( const std::string& name ) { const std::scoped_lock lock{ m_impl->mutex }; if ( auto* entry = m_impl->find( name ) ) return m_impl->load( *entry ); return false; }
	void engine::unload( const std::string& name ) { const std::scoped_lock lock{ m_impl->mutex }; if ( auto* entry = m_impl->find( name ) ) m_impl->unload( *entry ); }

	std::vector<script_info> engine::scripts( ) const
	{
		const std::scoped_lock lock{ m_impl->mutex };
		std::vector<script_info> result{}; result.reserve( m_impl->scripts.size( ) );
		for ( const auto& entry : m_impl->scripts ) result.push_back( { .name = entry.path.filename( ).string( ), .loaded = entry.loaded, .error = entry.error } );
		return result;
	}

	std::vector<console_entry> engine::console_entries( ) const { const std::scoped_lock lock{ m_impl->mutex }; return m_impl->console; }
	void engine::clear_console( ) { const std::scoped_lock lock{ m_impl->mutex }; m_impl->console.clear( ); }
	bool engine::export_console( ) const
	{
		const std::scoped_lock lock{ m_impl->mutex };
		const auto path = m_impl->root / "lua-console.log";
		std::ofstream file( path, std::ios::out | std::ios::trunc );
		if ( !file ) return false;
		for ( const auto& entry : m_impl->console )
		{
			const auto level = entry.level == console_level::error ? "ERROR" : entry.level == console_level::warning ? "WARN" : "INFO";
			file << '[' << level << "] [" << entry.script << "] " << entry.message << '\n';
		}
		return file.good( );
	}

	std::string engine::directory( ) const { const std::scoped_lock lock{ m_impl->mutex }; return m_impl->scripts_directory.string( ); }

} // namespace scripting
