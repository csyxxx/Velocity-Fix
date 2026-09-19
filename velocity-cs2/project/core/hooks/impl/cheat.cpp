#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <utilities/diag.hpp>
#include <utilities/hooking/hooking.hpp>
#include <utilities/logging/logging.hpp>
#include <utilities/security/security.hpp>
#include <core/rendering/rendering.hpp>
#include <core/scripting/scripting.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <protection/game_addresses.hpp>
#include "../hooks.hpp"

namespace hooks {
	namespace {

		constexpr auto desync_choke_limit = 5u;
		constexpr auto enable_command_yaw_choke = false;

		constexpr auto horizontal_movement_buttons = static_cast<std::uintptr_t>(
			cstypes::command_buttons::in_forward |
			cstypes::command_buttons::in_back |
			cstypes::command_buttons::in_moveleft |
			cstypes::command_buttons::in_moveright );

		void clear_horizontal_movement( systems::input::usercmd* cmd )
		{
			if ( !cmd )
			{
				return;
			}

			const auto base = cmd->csgo_user_cmd.mutable_base( );
			if ( !base )
			{
				return;
			}

			base->set_forwardmove( 0.0f );
			base->set_leftmove( 0.0f );
			cmd->buttons.value &= ~horizontal_movement_buttons;
			cmd->buttons.value_changed |= horizontal_movement_buttons;
			cmd->buttons.value_scroll &= ~horizontal_movement_buttons;

			for ( auto i = 0; i < base->subtick_moves_size( ); ++i )
			{
				const auto step = base->mutable_subtick_moves( i );
				if ( !step )
				{
					continue;
				}

				step->m_has_bits.clear( 0x8u | 0x10u );
				step->m_analog_forward_delta = 0.0f;
				step->m_analog_left_delta = 0.0f;

				if ( step->button( ) & horizontal_movement_buttons )
				{
					const auto remaining = step->button( ) & ~horizontal_movement_buttons;
					step->set_button( remaining );
					if ( !remaining )
					{
						step->set_pressed( false );
					}
				}
			}
		}

		void sort_subtick_moves_by_time( proto::base_usercmd_pb* base )
		{
			if ( !base )
			{
				return;
			}

			auto* moves = base->mutable_subtick_moves( );
			if ( !moves || !moves->m_rep || moves->m_current_size < 2 )
			{
				return;
			}

			std::stable_sort( moves->m_rep->elements, moves->m_rep->elements + moves->m_current_size,
				[ ]( void* lhs, void* rhs )
				{
					const auto left = proto::impl_ptr<const proto::subtick_move_step>( lhs );
					const auto right = proto::impl_ptr<const proto::subtick_move_step>( rhs );
					if ( !left || !right )
					{
						return left != nullptr;
					}
					return left->when( ) < right->when( );
				} );
		}

		class local_visual_bone_guard
		{
		public:
			void apply( std::uintptr_t scene_object, float yaw_delta )
			{
				if ( !scene_object )
				{
					return;
				}

				// Every render of the local scene object must wait until the previous
				// temporary bone rotation has been restored, including zero-delta frames.
				this->m_lock = std::unique_lock<std::mutex>{ s_mutex };
				if ( std::fabsf( yaw_delta ) <= 0.01f )
				{
					return;
				}

				const auto count = memory::safe_read<int>( scene_object + 0xd0 ).value_or( 0 );
				const auto matrices = memory::safe_read<std::uintptr_t>( scene_object + 0xd8 ).value_or( 0 );
				if ( !matrices || count <= 0 || count > static_cast<int>( this->m_backup.size( ) ) )
				{
					return;
				}

				for ( auto i = 0; i < count; ++i )
				{
					const auto matrix = memory::safe_read<math::matrix3x4>(
						matrices + static_cast<std::size_t>( i ) * sizeof( math::matrix3x4 ) );
					if ( !matrix )
					{
						return;
					}

					this->m_backup[ i ] = *matrix;
				}

				const auto radians = yaw_delta * ( std::numbers::pi_v<float> / 180.0f );
				const auto sine = std::sinf( radians );
				const auto cosine = std::cosf( radians );
				const auto pivot_x = this->m_backup[ 0 ][ 0 ][ 3 ];
				const auto pivot_y = this->m_backup[ 0 ][ 1 ][ 3 ];
				const auto root_yaw = std::atan2f(
					this->m_backup[ 0 ][ 1 ][ 0 ],
					this->m_backup[ 0 ][ 0 ][ 0 ] ) * ( 180.0f / std::numbers::pi_v<float> );

				for ( auto i = 0; i < count; ++i )
				{
					auto rotated = this->m_backup[ i ];
					for ( auto column = 0; column < 3; ++column )
					{
						rotated[ 0 ][ column ] = cosine * this->m_backup[ i ][ 0 ][ column ] - sine * this->m_backup[ i ][ 1 ][ column ];
						rotated[ 1 ][ column ] = sine * this->m_backup[ i ][ 0 ][ column ] + cosine * this->m_backup[ i ][ 1 ][ column ];
					}

					const auto offset_x = this->m_backup[ i ][ 0 ][ 3 ] - pivot_x;
					const auto offset_y = this->m_backup[ i ][ 1 ][ 3 ] - pivot_y;
					rotated[ 0 ][ 3 ] = pivot_x + cosine * offset_x - sine * offset_y;
					rotated[ 1 ][ 3 ] = pivot_y + sine * offset_x + cosine * offset_y;
					static_cast<void>( memory::safe_write<math::matrix3x4>(
						matrices + static_cast<std::size_t>( i ) * sizeof( math::matrix3x4 ),
						rotated ) );
				}

				this->m_matrices = matrices;
				this->m_count = count;

#if defined( DEV )
				static std::atomic_uint32_t diagnostics{};
				const auto index = diagnostics.fetch_add( 1, std::memory_order_relaxed );
				if ( index < 128 )
				{
					diag::writef(
						diag::level::info,
						"[visual_bones] index=%u scene=%p count=%d root_yaw=%.2f delta=%.2f target=%.2f",
						index,
						reinterpret_cast<void*>( scene_object ),
						count,
						root_yaw,
						yaw_delta,
						root_yaw + yaw_delta );
				}
#endif
			}

			~local_visual_bone_guard( )
			{
				for ( auto i = 0; i < this->m_count; ++i )
				{
					static_cast<void>( memory::safe_write<math::matrix3x4>(
						this->m_matrices + static_cast<std::size_t>( i ) * sizeof( math::matrix3x4 ),
						this->m_backup[ i ] ) );
				}
			}

		private:
			inline static std::mutex s_mutex{};
			std::array<math::matrix3x4, 256> m_backup{};
			std::unique_lock<std::mutex> m_lock{};
			std::uintptr_t m_matrices{};
			int m_count{};
		};

	} // namespace

	bool cheat::initialize () {
		if (!hooking::manager::create ({
			{ &m_present, &present, xs ("present"), addresses::functions::present },
			{ &m_resize_buffers, &resize_buffers, xs ("resize_buffers"), addresses::functions::resize_buffers }
			})) {
			return false;
		}

		diag::writef(
			diag::level::info,
			"[send_hook_init] send_tick_packet=%p",
			reinterpret_cast<void*>( PATTERN( patterns::send_tick_packet ) ) );

		const hooking::manager::entry feature_hooks[] {
			{ &m_cmd_interpreter, &cmd_interpreter, xs ("cmd_interpreter"), PATTERN (patterns::cmd_interpreter) },
			{ &m_frame_stage_notify, &frame_stage_notify, xs ("frame_stage_notify"), PATTERN (patterns::frame_stage_notify) },
			{ &m_on_client_frame_simulate, &on_client_frame_simulate, xs ("on_client_frame_simulate"), PATTERN (patterns::on_client_frame_simulate) },
			{ &m_send_tick_packet, &send_tick_packet, xs ("send_tick_packet"), PATTERN (patterns::send_tick_packet) },
			{ &m_create_move, &create_move, xs ("create_move"), PATTERN (patterns::create_move) },
			{ &m_handle_view_angles, &handle_view_angles, xs ("handle_view_angles"), PATTERN (patterns::handle_view_angles) },
			{ &m_add_entity, &add_entity, xs ("add_entity"), PATTERN (patterns::add_entity) },
			{ &m_remove_entity, &remove_entity, xs ("remove_entity"), PATTERN (patterns::remove_entity) },
			{ &m_render_view, &render_view, xs ("render_view"), PATTERN (patterns::render_view) },
			{ &m_draw_skybox_array, &draw_skybox_array, xs ("draw_skybox_array"), PATTERN (patterns::draw_skybox_array) },
			{ &m_light_scene_object, &light_scene_object, xs ("light_scene_object"), PATTERN (patterns::light_scene_object) },
			{ &m_draw_scene_object_array, &draw_scene_object_array, xs ("draw_scene_object_array"), PATTERN (patterns::draw_scene_object_array) },
			{ &m_draw_scene_object, &draw_scene_object, xs ("draw_scene_object"), PATTERN (patterns::draw_scene_object) },
			{ &m_is_glowing, &is_glowing, xs ("is_glowing"), PATTERN (patterns::is_glowing) },
			{ &m_get_glow_color, &get_glow_color, xs ("get_glow_color"), PATTERN (patterns::get_glow_color) },
			{ &m_generate_primitives, &generate_primitives, xs ("generate_primitives"), PATTERN (patterns::generate_primitives) },
			// The old parse_report_hit signature is no longer valid on the current
			// client build.  Its former target now takes a 64-bit output pointer;
			// installing the deleting-destructor detour here truncates that pointer
			// and crashes in client.dll+0x1556F4.  Keep this hook disabled until a
			// verified signature and prototype are available.
			{ &m_setup_fog, &setup_fog, xs ("setup_fog"), PATTERN (patterns::setup_fog) },
			{ &m_set_shader_param, &set_shader_param, xs ("set_shader_param"), PATTERN (patterns::set_shader_param) },
			{ &m_set_postprocess_vec, &set_postprocess_vec, xs ("set_postprocess_vec"), PATTERN (patterns::set_postprocess_vec) },
			{ &m_override_view, &override_view, xs ("override_view"), PATTERN (patterns::override_view) },
			{ &m_update_fov_sensitivity, &update_fov_sensitivity, xs ("update_fov_sensitivity"), PATTERN (patterns::update_fov_sensitivity) },
			{ &m_render_scope, &render_scope, xs ("render_scope"), PATTERN (patterns::render_scope) },
			{ &m_render_crosshair, &render_crosshair, xs ("render_crosshair"), PATTERN (patterns::render_crosshair) },
			{ &m_prepare_scene_material, &prepare_scene_material, xs ("prepare_scene_material"), PATTERN (patterns::prepare_scene_material) },
			{ &m_post_network_data_received, &post_network_data_received, xs ("post_network_data_received"), PATTERN (patterns::post_network_data_received) },
			{ &m_draw_overhead, &draw_overhead, xs ("draw_overhead"), PATTERN (patterns::draw_overhead) },
			{ &m_draw_legs, &draw_legs, xs ("draw_legs"), PATTERN (patterns::draw_legs) },
			{ &m_get_transforms_for_hitbox_list, &get_transforms_for_hitbox_list, xs ("get_transforms_for_hitbox_list"), PATTERN (patterns::get_transforms_for_hitbox_list) },
			{ &m_sort_primitives, &sort_primitives, xs ("sort_primitives"), PATTERN (patterns::sort_primitives) },
			{ &m_get_interpolated_shoot_position, &get_interpolated_shoot_position, xs ("get_interpolated_shoot_position"), PATTERN (patterns::get_interpolated_shoot_position) },
			{ &m_level_initialization, &level_initialization, xs ("level_initialization"), PATTERN (patterns::level_initialization) },
			{ &m_level_shutdown, &level_shutdown, xs ("level_shutdown"), PATTERN (patterns::level_shutdown) },
			{ &m_read_frame_input, &read_frame_input, xs ("read_frame_input"), PATTERN (patterns::read_frame_input) },
			{ &m_process_input_event, &process_input_event, xs ("process_input_event"), PATTERN (patterns::process_input_event) },
			{ &m_render_decals, &render_decals, xs ("render_decals"), PATTERN (patterns::render_decals) },
			{ &m_render_smoke, &render_smoke, xs ("render_smoke"), PATTERN (patterns::render_smoke) },
			{ &m_draw_flash_effect, &draw_flash_effect, xs ("draw_flash_effect"), PATTERN (patterns::draw_flash_effect) },
			{ &m_set_info, &set_info, xs ("set_info"), PATTERN (patterns::set_info) }
		};

		auto unavailable_hooks = 0u;
		for (const auto& entry : feature_hooks) {
			if (!hooking::manager::create ({ entry })) {
				++unavailable_hooks;
				logging::console::print (xs ("skipping unavailable hook: {}"), entry.name);
			}
		}

		if (unavailable_hooks) {
			logging::console::print (
				xs ("feature hooks initialized with {} unavailable"),
				unavailable_hooks);
		}

		return true;
	}

	void cheat::shutdown( )
	{
		m_wnd_proc.reset( );
		m_om_set_render_targets.reset( );
		m_present.reset( );
		m_resize_buffers.reset( );
		m_cmd_interpreter.reset( );
		m_frame_stage_notify.reset( );
		m_on_client_frame_simulate.reset( );
		m_send_tick_packet.reset( );
		m_create_move.reset( );
		m_handle_view_angles.reset( );
		m_add_entity.reset( );
		m_remove_entity.reset( );
		m_render_view.reset( );
		m_draw_skybox_array.reset( );
		m_light_scene_object.reset( );
		m_draw_scene_object_array.reset( );
		m_draw_scene_object.reset( );
		m_is_glowing.reset( );
		m_get_glow_color.reset( );
		m_generate_primitives.reset( );
		m_parse_report_hit.reset( );
		m_setup_fog.reset( );
		m_set_shader_param.reset( );
		m_set_postprocess_vec.reset( );
		m_override_view.reset( );
		m_update_fov_sensitivity.reset( );
		m_render_scope.reset( );
		m_render_crosshair.reset( );
		m_prepare_scene_material.reset( );
		m_post_network_data_received.reset( );
		m_draw_overhead.reset( );
		m_draw_legs.reset( );
		m_get_transforms_for_hitbox_list.reset( );
		m_sort_primitives.reset( );
		m_get_inaccuracy.reset( );
		m_get_interpolated_shoot_position.reset( );
		m_level_initialization.reset( );
		m_read_frame_input.reset( );
		m_process_input_event.reset( );
		m_render_decals.reset( );
		m_render_smoke.reset( );
		m_render_smoke_map.reset( );
		m_render_smoke_unmap.reset( );
		m_draw_flash_effect.reset( );
		m_set_info.reset( );
	}

	HRESULT __fastcall cheat::present( IDXGISwapChain* thisptr, UINT sync_interval, UINT flags )
	{
		rendering::g_context.on_present( thisptr );

		if ( !m_wnd_proc.is_enabled( ) && rendering::g_context.get_window( ) )
		{
			if ( m_wnd_proc.create( reinterpret_cast< void* >( GetWindowLongPtrW( rendering::g_context.get_window( ), GWLP_WNDPROC ) ), &wnd_proc ) )
			{
				m_wnd_proc.enable( );
			}
		}

		if ( !m_om_set_render_targets.is_enabled( ) && rendering::g_context.is_initialized( ) )
		{
			const auto om_addr = memory::get_vfunc( reinterpret_cast< std::uintptr_t >( rendering::g_context.get_context( ) ), 33 );
			if ( m_om_set_render_targets.create( reinterpret_cast< void* >( om_addr ), &om_set_render_targets ) )
			{
				m_om_set_render_targets.enable( );
			}
		}

		return m_present.call<HRESULT>( thisptr, sync_interval, flags );
	}

	HRESULT __fastcall cheat::resize_buffers( IDXGISwapChain* thisptr, UINT buffer_count, UINT width, UINT height, DXGI_FORMAT new_format, UINT swap_chain_flags )
	{
		rendering::g_context.on_resize_buffers( );

		const auto result = m_resize_buffers.call<long>( thisptr, buffer_count, width, height, new_format, swap_chain_flags );
		if ( SUCCEEDED( result ) )
		{
			rendering::g_context.on_resize_buffers_post( thisptr );
		}

		return result;
	}

	LRESULT __stdcall cheat::wnd_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
	{
		if ( msg == WM_ACTIVATE && LOWORD( wparam ) != WA_INACTIVE && rendering::g_menu.is_open( ) && rendering::g_context.get_window( ) == hwnd )
		{
			if ( addresses::globals::input_system )
			{
				memory::call_vfunc<void>( addresses::globals::input_system, 76, false );
			}

			SetCursor( LoadCursor( nullptr, IDC_ARROW ) );
			rendering::g_menu.apply_saved_cursor( );
		}

		if ( msg == WM_KEYDOWN && !( lparam & ( 1 << 30 ) ) && static_cast< int >( wparam ) == settings::g_misc.menu_key )
		{
			rendering::g_menu.toggle( );
			return 0;
		}

		xui::wndproc( msg, wparam, lparam );

		if ( rendering::g_menu.is_open( ) )
		{
			switch ( msg )
			{
			case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
			case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
			case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
			case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
			case WM_MOUSEMOVE:
				return 0;
			default:
				break;
			}
		}

		return m_wnd_proc.call<LRESULT>( hwnd, msg, wparam, lparam );
	}

	void __stdcall cheat::om_set_render_targets( ID3D11DeviceContext* ctx, UINT num_views, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv )
	{
		m_om_set_render_targets.call<void>( ctx, num_views, rtvs, dsv );
	}

	void __fastcall cheat::cmd_interpreter( std::uintptr_t render_thread, std::uintptr_t item, std::uint8_t flag )
	{
		m_cmd_interpreter.call<void>( render_thread, item, flag );
	}

	void __fastcall cheat::frame_stage_notify( std::uintptr_t thisptr, int stage )
	{
		if ( systems::g_entities.is_empty( ) )
		{
			systems::g_entities.force_update( );
		}

		systems::g_local.update( );

		if ( systems::g_local.get( ).is_valid( ) && systems::g_view.has_camera( ) )
		{
			if ( stage == 6 )
			{
				features::changer::g_guns.on_frame_stage_notify( );
			}

			if ( stage == 7 )
			{
				features::changer::g_agents.on_frame_stage_notify( );
				features::changer::g_gloves.on_frame_stage_notify( );
				features::changer::g_knives.on_frame_stage_notify( );

				features::world::g_scene.on_frame_stage_notify( );
				features::world::g_weather.on_frame_stage_notify( );
				features::misc::g_other.on_frame_stage_notify( );
				features::misc::g_impacts.on_frame_stage_notify( );

				
			}
		}

		{
			static auto was_active{ false };
			const auto is_active = settings::g_misc.m_removals.skybox_3d.value;

			if ( is_active != was_active )
			{
				CONVAR ("r_draw3dskybox")->m_value.i1 = is_active;
				was_active = is_active;
			}
		}

		// Source 2 copies dynamic-light entries into scene objects during this stage.
		// Publish our entry first, while keeping all manager mutations on the game thread.
		if ( stage == 6 )
		{
			features::misc::g_dlight.on_frame_stage_notify( );
		}

		m_frame_stage_notify.call<void>( thisptr, stage );

		// The current frame's world-to-projection matrix is published by the
		// engine during render-start stage 12.
		if ( stage == 12 )
		{
			systems::g_view.update_matrix( );
			systems::g_frame_data.update( );
		}

		if (systems::g_local.get ().is_valid () && systems::g_view.has_camera ()) {
			if (stage == 6) {
				// Capture lag records only after Source 2 has committed this network update,
				// so the simulation timestamp, world origin and evaluated bones agree.
				features::combat::g_shared.lc( ).run( );
				features::esp::player::g_chams.bt( ).update( );
				features::esp::player::g_chams.os ().update ();

				features::misc::g_scoreboard_weapons.on_frame_stage_notify ();
				features::misc::g_other.do_kill_feed_preservation( );
			}
		}
	}

	void __fastcall cheat::on_client_frame_simulate( std::uintptr_t thisptr, std::uintptr_t event )
	{
		const auto previous = m_send_diagnostic;
		const auto previous_packet_control = m_packet_control;
		m_send_diagnostic = {};
		m_packet_control = packet_control::unmanaged;
		m_send_diagnostic.event = event;
		m_send_diagnostic.command_number = -1;
		m_send_diagnostic.scheduled_before = event
			? memory::safe_read<bool>( event + 0x30 ).value_or( false )
			: false;

		const auto local = systems::g_local.get( );
		const auto desync_frame =
			enable_command_yaw_choke
			&& event
			&& m_send_tick_packet.is_enabled( )
			&& settings::g_combat.m_antiaim.enabled.value
			&& settings::g_movement.strafe_fix.value
			&& local.pawn
			&& local.controller
			&& local.is_alive
			&& systems::g_view.has_camera( );
		if ( desync_frame )
		{
			m_packet_control = m_desync_choked_commands >= desync_choke_limit
				? packet_control::send
				: packet_control::choke;
		}

		m_on_client_frame_simulate.call<void>( thisptr, event );

		m_send_diagnostic.scheduled_after = event
			? memory::safe_read<bool>( event + 0x30 ).value_or( false )
			: false;

#if defined( DEV )
		if ( settings::g_combat.m_antiaim.enabled.value
			&& ( m_send_diagnostic.scheduled_before
				|| m_send_diagnostic.scheduled_after
				|| m_send_diagnostic.create_move_calls ) )
		{
			const auto index = m_send_diagnostic_frames.fetch_add( 1, std::memory_order_relaxed );
			if ( index < 256 )
			{
				diag::writef(
					diag::level::info,
					"[send_diag] frame=%u event=%p scheduled=%u->%u create_calls=%u send_tick_calls=%u reason=%d suppressed=%u control=%u cmd=%lld yaw=%.2f move=(%.3f,%.3f) buttons=0x%llX",
					index,
					reinterpret_cast<void*>( event ),
					m_send_diagnostic.scheduled_before ? 1u : 0u,
					m_send_diagnostic.scheduled_after ? 1u : 0u,
					m_send_diagnostic.create_move_calls,
					m_send_diagnostic.send_tick_calls,
					m_send_diagnostic.send_tick_reason,
					m_send_diagnostic.send_tick_suppressed ? 1u : 0u,
					static_cast<unsigned>( m_packet_control ),
					static_cast<long long>( m_send_diagnostic.command_number ),
					m_send_diagnostic.yaw,
					m_send_diagnostic.forward_move,
					m_send_diagnostic.left_move,
					static_cast<unsigned long long>( m_send_diagnostic.buttons ) );
			}
		}
#endif

		m_send_diagnostic = previous;
		m_packet_control = previous_packet_control;
	}

	void __fastcall cheat::send_tick_packet( std::uintptr_t thisptr, int reason )
	{
		const auto suppress =
			reason == 2
			&& m_send_diagnostic.event
			&& m_packet_control == packet_control::choke;

		if ( m_send_diagnostic.event )
		{
			++m_send_diagnostic.send_tick_calls;
			m_send_diagnostic.send_tick_reason = reason;
			m_send_diagnostic.send_tick_suppressed = suppress;
		}

#if defined( DEV )
		if ( m_packet_control != packet_control::unmanaged )
		{
			const auto index = m_packet_control_diagnostics.fetch_add( 1, std::memory_order_relaxed );
			if ( index < 256 )
			{
				diag::writef(
					diag::level::info,
					"[send_tick_control] index=%u control=%u reason=%d suppress=%u create_calls=%u",
					index,
					static_cast<unsigned>( m_packet_control ),
					reason,
					suppress ? 1u : 0u,
					m_send_diagnostic.create_move_calls );
			}
		}
#endif

		if ( !suppress )
		{
			m_send_tick_packet.call<void>( thisptr, reason );
		}
	}

	void __fastcall cheat::create_move( std::uintptr_t thisptr, int slot, bool active )
	{
		const auto local = systems::g_local.get( );

		if ( !local.pawn || !local.controller )
		{
			return m_create_move.call<void>( thisptr, slot, active );
		}

		const auto cmd = systems::g_input.get_current_cmd( local.controller );
		if ( cmd && systems::g_input.is_subtick_overwrite( cmd ) )
		{
			systems::g_input.set_weapon_select( cmd, thisptr );
			return;
		}

		m_create_move.call<void>( thisptr, slot, active );

		{
			features::combat::g_shared.invalidate_if_needed( );
			features::combat::g_legit.invalidate_if_needed( );
			features::combat::g_misc.quickpeek( ).reset_if_needed( );
		}

		if ( !local.is_alive || !systems::g_view.has_camera( ) )
		{
			return;
		}

		const auto movement_services = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BasePlayerPawn", "m_pMovementServices"_hash ) );
		if ( !movement_services )
		{
			return;
		}

		const auto last_cmd_processed = memory::read<std::uint32_t>( movement_services + SCHEMA( "CPlayer_MovementServices", "m_nLastCommandNumberProcessed"_hash ) );
		if ( !last_cmd_processed )
		{
			return;
		}

		systems::g_input.update( );
		{
			const auto current_cmd = systems::g_input.get( );
			if ( !current_cmd || !current_cmd->csgo_user_cmd.has_base( ) )
			{
				return;
			}

			const auto desync_available =
				enable_command_yaw_choke
				&& m_send_tick_packet.is_enabled( )
				&& m_send_diagnostic.event
				&& settings::g_combat.m_antiaim.enabled.value
				&& settings::g_movement.strafe_fix.value;
			const auto send_fake_command =
				desync_available && m_packet_control == packet_control::send;
			auto& antiaim = features::combat::g_misc.antiaim( );
			antiaim.set_desync_choke( desync_available && !send_fake_command );

			static std::atomic_bool first_create_move_traced{};
			const auto trace = !first_create_move_traced.exchange( true, std::memory_order_relaxed );
			diag::exception_scope exception_scope{ "create_move: desubtick" };
			if ( trace )
			{
				diag::step( "create_move: feature pipeline begin" );
			}

			systems::g_input.desubtick( current_cmd );
			systems::g_prediction.capture_prestate( local.pawn, movement_services );
			scripting::g_engine.on_create_move_pre( current_cmd );

			{
				diag::set_exception_phase( "create_move: shared update" );
				features::movement::g_bhop.store_input( current_cmd );
				features::movement::g_airstrafe.store_input( current_cmd );
				features::movement::g_test_strafer.store_input( current_cmd );
				features::combat::g_shared.update( );

				diag::set_exception_phase( "create_move: combat misc" );
				antiaim.on_create_move( current_cmd );
			}
			if ( trace )
			{
				diag::step( "create_move: shared and combat misc ready" );
			}

			{
				diag::set_exception_phase( "create_move: pre-combat movement" );
				features::movement::g_slowwalk.on_create_move( current_cmd );
				features::movement::g_edgebug.on_create_move( current_cmd );
				features::movement::g_edgejump.on_create_move( current_cmd );
				features::movement::g_edgestop.on_create_move( current_cmd );
				features::movement::g_jumpbug.on_create_move( current_cmd );
				features::movement::g_bhop.on_create_move( current_cmd );
				features::movement::g_fastladder.on_create_move( current_cmd );
				if ( trace )
				{
					diag::step( "create_move: pre-combat movement end" );
					diag::step( "create_move: rage begin" );
				}

				{
					 diag::set_exception_phase( "create_move: rage" );
					// 参考基础流程：上一命令已经请求急停时，先把同一反向
					// 制动输入带入本命令，再让 Rage 重新评估目标和开火窗口。
					// 这样减速命令不会在开火前被玩家原始方向抵消。
					if ( features::combat::g_rage.previous_stop_requested( ) )
					{
						features::combat::g_misc.autostop( ).on_create_move( current_cmd );
					}
					features::combat::g_rage.on_create_move( current_cmd );
					// Rage decides the current command's stop state. Apply the
					// counter-strafe immediately so the trigger tick cannot retain
					// the player's original horizontal input.
					if ( !features::combat::g_rage.is_firing_this_tick( )
						&& features::combat::g_rage.movement_stop_requested( ) )
					{
						features::combat::g_misc.autostop( ).on_create_move( current_cmd );
					}
					if ( trace )
					{
						diag::step( "create_move: rage end" );
						diag::step( "create_move: legit begin" );
					}
					diag::set_exception_phase( "create_move: legit" );
					features::combat::g_legit.on_create_move( current_cmd );
					if ( trace )
					{
						diag::step( "create_move: legit end" );
					}
				}

				// Duckpeek 的起身/重新下蹲是独立于移动急停的按钮事务。
				// 即使本 tick 已经开火，也必须先执行它，否则 Rage 设置的
				// release 标志不会落到 usercmd，玩家会一直保持蹲下状态。
				features::combat::g_misc.duckpeek( ).on_create_move( current_cmd );

				if ( !features::combat::g_rage.is_firing_this_tick( ) )
				{
					diag::set_exception_phase( "create_move: post-combat movement" );
					// Once fire_gun() serializes the predicted eye/history, changing
					// movement here makes the server reconstruct a different shoot origin.
					// 急停接管期间跳过后续会重新写入移动量的模块，避免
					// airstrafe、duckpeek 或 strafe-fix 抵消反向制动。
					if ( !features::combat::g_rage.movement_stop_requested( )
						&& !features::combat::g_legit.movement_stop_requested( ) )
					{
						diag::set_exception_phase( "create_move: post-combat movement" );
						features::movement::g_test_strafer.on_create_move( current_cmd );
						features::movement::g_airstrafe.on_create_move( current_cmd );
					}
				}
				features::misc::g_projectile_trajectory.on_create_move( current_cmd );
			}
			if ( trace )
			{
				diag::step( "create_move: post-combat movement end" );
			}

			diag::set_exception_phase( "create_move: quickpeek" );
			features::combat::g_misc.quickpeek( ).on_create_move( current_cmd );
			diag::set_exception_phase( "create_move: final movement correction" );
			const auto movement_is_command_relative =
				features::combat::g_rage.is_firing_this_tick( )
				|| features::combat::g_misc.quickpeek( ).is_retracking( )
				|| features::combat::g_rage.movement_stop_requested( )
				|| features::combat::g_legit.movement_stop_requested( )
				|| ( settings::g_movement.strafe_fix.value
					&& features::movement::g_test_strafer.handled_this_tick( ) );
			antiaim.finalize_movement(
				current_cmd,
				movement_is_command_relative );
			// Keep the movement serialized by the command that rage predicted.
			// Clearing it after fire_gun() changes the server-side firing origin.
			if ( trace )
			{
				diag::step( "create_move: quickpeek end" );
				diag::step( "create_move: final subtick begin" );
			}

			diag::set_exception_phase( "create_move: final subtick" );
			// Keep a failed rage validation from turning into a silent crosshair shot.
			// The engine command can retain an attack bit written by a previous writer;
			// only preserve it when it was present in the original user input or rage
			// committed this frame.
			const auto& combat_ctx = features::combat::g_shared.ctx( );
			const auto gun_weapon = combat_ctx.weapon_type >= cstypes::weapon_type::pistol &&
				combat_ctx.weapon_type <= cstypes::weapon_type::lmg;
			if ( settings::g_combat.m_ragebot.enabled.value && gun_weapon &&
				combat_ctx.item_def_idx != cstypes::item_definition_index::weapon_r8_revolver &&
				!features::combat::g_rage.is_firing_this_tick( ) &&
				!features::combat::g_legit.is_firing_this_tick( ) )
			{
				current_cmd->buttons.value &= ~cstypes::command_buttons::in_attack;
				current_cmd->buttons.value_changed &= ~cstypes::command_buttons::in_attack;
				current_cmd->buttons.value_scroll &= ~cstypes::command_buttons::in_attack;
				current_cmd->csgo_user_cmd.set_attack1_start_history_index( -1 );
			}
			const auto final_base = current_cmd->csgo_user_cmd.mutable_base( );
			auto has_analog_movement = false;
			if ( final_base )
			{
				for ( auto i = 0; i < final_base->subtick_moves_size( ); ++i )
				{
					const auto step = final_base->mutable_subtick_moves( i );
					if ( step && ( step->m_has_bits.test( 0x8u ) || step->m_has_bits.test( 0x10u ) ) )
					{
						has_analog_movement = true;
						break;
					}
				}
			}

			if ( final_base && has_analog_movement
				&& !features::combat::g_rage.is_firing_this_tick( )
				&& !features::movement::g_test_strafer.handled_this_tick( )
				&& !antiaim.has_final_ground_movement( ) )
			{
				final_base->set_forwardmove( 0.0f );
				final_base->set_leftmove( 0.0f );
			}
			if ( trace )
			{
				diag::step( "create_move: final subtick end" );
			}

			diag::set_exception_phase( "create_move: vac bypass" );
			features::combat::g_vac_bypass.on_create_move( current_cmd );

			const auto force_send =
				( current_cmd->buttons.value & static_cast<std::uintptr_t>(
					cstypes::command_buttons::in_attack |
					cstypes::command_buttons::in_second_attack |
					cstypes::command_buttons::in_use ) )
				|| features::combat::g_rage.is_firing_this_tick( )
				|| features::combat::g_misc.quickpeek( ).is_retracking( );

			if ( !desync_available || !antiaim.is_active( ) )
			{
				m_packet_control = packet_control::unmanaged;
				m_desync_choked_commands = 0;
			}
			else if ( force_send )
			{
				m_packet_control = packet_control::send;
				m_desync_choked_commands = 0;
				static_cast<void>( memory::safe_write<bool>( m_send_diagnostic.event + 0x30, true ) );
			}
			else if ( send_fake_command )
			{
				m_packet_control = packet_control::send;
				m_desync_choked_commands = 0;
				clear_horizontal_movement( current_cmd );
				static_cast<void>( memory::safe_write<bool>( m_send_diagnostic.event + 0x30, true ) );
			}
			else
			{
				m_packet_control = packet_control::choke;
				++m_desync_choked_commands;
				static_cast<void>( memory::safe_write<bool>( m_send_diagnostic.event + 0x30, false ) );
			}

			//systems::g_legit_input.on_create_move( current_cmd );
		}
		static std::atomic_bool first_input_apply_traced{};
		const auto trace_apply =
			!first_input_apply_traced.exchange( true, std::memory_order_relaxed );
		if ( trace_apply )
		{
			diag::step( "create_move: input apply begin" );
		}
		diag::exception_scope exception_scope{ "create_move: input apply" };
		scripting::g_engine.on_create_move( systems::g_input.get( ) );
		scripting::g_engine.on_create_move_post( systems::g_input.get( ) );
		// Restore the canonical shot after every command writer, immediately before
		// CRC generation. This also captures a same-tick damage angle change that
		// arrived after rage selected its target.
		features::combat::g_rage.finalize_shot_command( systems::g_input.get( ) );
		systems::g_input.apply( );

		if ( const auto diagnostic_cmd = systems::g_input.get( ) )
		{
			if ( const auto diagnostic_base = diagnostic_cmd->csgo_user_cmd.mutable_base( ) )
			{
				if ( m_send_diagnostic.event )
				{
					++m_send_diagnostic.create_move_calls;
					m_send_diagnostic.command_number = diagnostic_cmd->command_number;
					m_send_diagnostic.buttons = diagnostic_cmd->buttons.value;
					m_send_diagnostic.forward_move = diagnostic_base->forwardmove( );
					m_send_diagnostic.left_move = diagnostic_base->leftmove( );
					if ( diagnostic_base->viewangles( ) )
					{
						m_send_diagnostic.yaw = diagnostic_base->viewangles( )->y( );
					}
				}

#if defined( DEV )
				if ( settings::g_combat.m_antiaim.enabled.value )
				{
					auto yaw_sum = 0.0f;
					auto yaw_steps = 0;
					auto analog_steps = 0;
					for ( auto i = 0; i < diagnostic_base->subtick_moves_size( ); ++i )
					{
						if ( const auto step = diagnostic_base->mutable_subtick_moves( i ) )
						{
							if ( step->m_has_bits.test( 0x40u ) )
							{
								yaw_sum += step->yaw_delta( );
								++yaw_steps;
							}
							if ( step->m_has_bits.test( 0x8u ) || step->m_has_bits.test( 0x10u ) )
							{
								++analog_steps;
							}
						}
					}

					const auto movement_active =
						std::fabsf( diagnostic_base->forwardmove( ) ) > 0.0001f
						|| std::fabsf( diagnostic_base->leftmove( ) ) > 0.0001f
						|| ( diagnostic_cmd->buttons.value & horizontal_movement_buttons )
						|| yaw_steps > 0;
					if ( movement_active )
					{
						const auto index = m_send_diagnostic_commands.fetch_add( 1, std::memory_order_relaxed );
						if ( index < 512 )
						{
							const auto scheduled = m_send_diagnostic.event
							? memory::safe_read<bool>( m_send_diagnostic.event + 0x30 ).value_or( false )
							: false;
						const auto base_yaw = diagnostic_base->viewangles( )
							? diagnostic_base->viewangles( )->y( )
							: 0.0f;
						auto final_yaw = base_yaw + yaw_sum;
						math::helpers::normalize_angle( final_yaw );
						diag::writef(
							diag::level::info,
							"[cmd_send_diag] index=%u inside_event=%u scheduled=%u event=%p cmd=%lld yaw=%.2f final_yaw=%.2f yaw_sum=%.2f yaw_steps=%d analog_steps=%d move=(%.3f,%.3f) buttons=0x%llX",
							index,
							m_send_diagnostic.event ? 1u : 0u,
							scheduled ? 1u : 0u,
							reinterpret_cast<void*>( m_send_diagnostic.event ),
							static_cast<long long>( diagnostic_cmd->command_number ),
							base_yaw,
							final_yaw,
							yaw_sum,
							yaw_steps,
							analog_steps,
							diagnostic_base->forwardmove( ),
							diagnostic_base->leftmove( ),
							static_cast<unsigned long long>( diagnostic_cmd->buttons.value ) );
						}
					}
				}
#endif
			}
		}
		if ( trace_apply )
		{
			diag::step( "create_move: input apply end" );
		}
	}

	void __fastcall cheat::handle_view_angles( std::uintptr_t thisptr, int a2 )
	{
		const auto view_angles = systems::g_input.get_view_angles( );

		m_handle_view_angles.call<void>( thisptr, a2 );

		systems::g_input.set_view_angles( view_angles );
	}

	void __fastcall cheat::add_entity( std::uintptr_t thisptr, std::uintptr_t entity, std::uint32_t handle )
	{
		systems::g_entities.on_add_entity( entity, handle );

		m_add_entity.call<void>( thisptr, entity, handle );
	}

	void __fastcall cheat::remove_entity( std::uintptr_t thisptr, std::uintptr_t entity, std::uint32_t handle )
	{
		systems::g_entities.on_remove_entity( entity, handle );

		m_remove_entity.call<void>( thisptr, entity, handle );
	}

	void __fastcall cheat::render_view( std::uintptr_t thisptr )
	{
		m_render_view.call<void>( thisptr );

		systems::g_view.update( thisptr + 0x10 );
		systems::g_frame_data.update( );
	}

	void __fastcall cheat::draw_skybox_array( std::uintptr_t thisptr, std::uintptr_t a2, std::uintptr_t mesh_array, int mesh_count, int a5, std::uintptr_t a6, std::uintptr_t a7, std::uintptr_t a8 )
	{
		features::world::g_scene.on_draw_skybox_array_pre( mesh_array, mesh_count );

		m_draw_skybox_array.call<void>( thisptr, a2, mesh_array, mesh_count, a5, a6, a7, a8 );

		features::world::g_scene.on_draw_skybox_array_post( );
	}

	std::uintptr_t __fastcall cheat::light_scene_object( std::uintptr_t thisptr, std::uintptr_t object, std::uintptr_t a3 )
	{
		features::world::g_scene.on_light_scene_object_pre( object );
		features::misc::g_dlight.apply_scene_color( object );

		const auto result = m_light_scene_object.call<std::uintptr_t>( thisptr, object, a3 );

		features::world::g_scene.on_light_scene_object_post( object );

		return result;
	}

	void __fastcall cheat::draw_scene_object_array( std::uintptr_t thisptr, std::uintptr_t a2, std::uintptr_t object_array )
	{
		m_draw_scene_object_array.call<void>( thisptr, a2, object_array );

		diag::exception_scope exception_scope{ "world: aggregate records" };
		features::world::g_scene.on_draw_scene_object_array( object_array );
	}

	std::uintptr_t __fastcall cheat::draw_scene_object( std::uintptr_t a1, std::uintptr_t a2, std::uintptr_t batch, int batch_count, int a5, std::uintptr_t a6, std::uintptr_t a7, std::uintptr_t a8 )
	{
		{
			diag::exception_scope exception_scope{ "world: primitive tint" };
			features::world::g_scene.on_draw_scene_object( batch, batch_count );
		}

		return m_draw_scene_object.call<std::uintptr_t>( a1, a2, batch, batch_count, a5, a6, a7, a8 );
	}

	bool __fastcall cheat::is_glowing( std::uintptr_t glow_property )
	{
		if ( glow_property )
		{
			const auto owner_entity = memory::read<std::uintptr_t>( glow_property + 0x18 );
			if ( owner_entity )
			{
				const auto owner_hash = fnv1a::runtime_hash( systems::g_entities.get_schema_name( owner_entity ) );
				if ( owner_hash )
				{
					if ( features::esp::player::g_glow.on_is_glowing( owner_entity, owner_hash ) )
					{
						return true;
					}

					if ( features::esp::item::g_glow.on_is_glowing( owner_entity, owner_hash ) )
					{
						return true;
					}
				}
			}
		}

		return m_is_glowing.call<bool>( glow_property );
	}

	void __fastcall cheat::get_glow_color( std::uintptr_t glow_property, float* color )
	{
		if ( glow_property )
		{
			const auto owner_entity = memory::read<std::uintptr_t>( glow_property + 0x18 );
			if ( owner_entity )
			{
				const auto owner_hash = fnv1a::runtime_hash( systems::g_entities.get_schema_name( owner_entity ) );
				if ( owner_hash )
				{
					if ( features::esp::player::g_glow.on_get_glow_color( owner_entity, owner_hash, color ) )
					{
						return;
					}

					if ( features::esp::item::g_glow.on_get_glow_color( owner_entity, owner_hash, color ) )
					{
						return;
					}
				}
			}
		}

		m_get_glow_color.call<void>( glow_property, color );
	}

	void __fastcall cheat::generate_primitives( std::uintptr_t thisptr, std::uintptr_t scene_object, std::uintptr_t scene_view, std::uintptr_t primitive_buffer )
	{
		diag::exception_scope exception_scope{ "chams: generate primitives" };
		local_visual_bone_guard visual_bones;

		if ( scene_object )
		{
			if ( features::esp::player::g_chams.bt( ).is_active( scene_object ) )
			{
				return;
			}

			if ( features::esp::player::g_chams.os( ).is_active( scene_object ) ) 
			{
				return;
			}

			const auto owner_handle = memory::read<std::uint32_t>( scene_object + 0xc0 );
			if ( owner_handle )
			{
				const auto owner_entity = systems::g_entities.lookup( owner_handle );
				if ( owner_entity )
				{
					const auto owner_hash = fnv1a::runtime_hash( systems::g_entities.get_schema_name( owner_entity ) );
					if ( owner_hash )
					{
						const auto local = systems::g_local.get( );
						auto& antiaim = features::combat::g_misc.antiaim( );
						float command_yaw{};
						float visual_yaw{};
						if ( owner_entity == local.view_pawn( )
							&& owner_hash == "C_CSPlayerPawn"_hash
							&& settings::g_misc.m_camera.thirdperson.value
							&& settings::g_movement.strafe_fix.value
							&& antiaim.get_render_yaws( command_yaw, visual_yaw ) )
						{
							auto yaw_delta = visual_yaw - command_yaw;
							math::helpers::normalize_angle( yaw_delta );
							visual_bones.apply( scene_object, yaw_delta );
						}

						if ( features::esp::player::g_chams.on_generate_primitives( owner_entity, owner_hash, scene_object, primitive_buffer, m_generate_primitives.original<void( __fastcall* )( std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t )>( ), thisptr, scene_view ) )
						{
							return;
						}

						if ( features::esp::item::g_chams.on_generate_primitives( owner_entity, owner_hash, scene_object, primitive_buffer, m_generate_primitives.original<void( __fastcall* )( std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t )>( ), thisptr, scene_view ) )
						{
							return;
						}

						systems::g_model_preview.on_generate_primitives(
							owner_entity,
							owner_hash,
							scene_object,
							primitive_buffer,
							m_generate_primitives.original<void( __fastcall* )( std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t )>( ),
							thisptr,
							scene_view
						);
					}
				}
			}
		}

		m_generate_primitives.call<void>( thisptr, scene_object, scene_view, primitive_buffer );
	}

	std::uintptr_t __fastcall cheat::parse_report_hit( std::uintptr_t thisptr, std::uint8_t deleting )
	{
		// Capture the protobuf fields before the deleting destructor can free them.
		features::misc::g_impacts.on_report_hit( thisptr );

		return m_parse_report_hit.call<std::uintptr_t>( thisptr, deleting );
	}

	std::uintptr_t __fastcall cheat::setup_fog( __m128i* output, int* mode )
	{
		if ( features::world::g_scene.on_setup_fog( output, mode ) )
		{
			return 0;
		}

		return m_setup_fog.call<std::uintptr_t>( output, mode );
	}

	std::uintptr_t __fastcall cheat::set_shader_param( __m128i* map, std::uint32_t hash, __m128i* value )
	{
		features::world::g_scene.on_set_shader_param( value, hash );

		return m_set_shader_param.call<std::uintptr_t>( map, hash, value );
	}

	std::uintptr_t __fastcall cheat::set_postprocess_vec( __m128i* map, std::uint32_t hash, __m128i* value )
	{
		constexpr std::uint32_t dof_ranges{ 0x2ACAB07C };

		// The engine only publishes DofRanges when the active camera enables DOF.
		// Insert it alongside any post-process vector, matching Artisan's live path.
		if ( settings::g_world.m_scene.dof.value && hash != dof_ranges )
		{
			__m128i* dof_value{};
			features::world::g_scene.on_set_shader_param( dof_value, dof_ranges );
			if ( dof_value )
			{
				m_set_postprocess_vec.call<std::uintptr_t>( map, dof_ranges, dof_value );
			}
		}

		features::world::g_scene.on_set_shader_param( value, hash );
		return m_set_postprocess_vec.call<std::uintptr_t>( map, hash, value );
	}

	void __fastcall cheat::override_view( std::uintptr_t thisptr, std::uintptr_t view_setup )
	{
		m_override_view.call<void>( thisptr, view_setup );

		features::misc::g_camera.on_override_view( view_setup );
		features::misc::g_removals.on_override_view( view_setup );
		features::combat::g_misc.duckpeek( ).on_override_view( view_setup );
	}

	void __fastcall cheat::update_fov_sensitivity( std::uintptr_t thisptr )
	{
		m_update_fov_sensitivity.call<void>( thisptr );

		features::misc::g_camera.update_fov_sensitivity( thisptr );
	}

	void __fastcall cheat::render_scope( std::uintptr_t a1, std::uintptr_t a2 )
	{
		m_render_scope.call<void>( a1, a2 );

		if ( settings::g_misc.m_removals.scope.value )
		{
			memory::write<std::uint8_t>( a2 + 4, 0 );
		}
	}

	bool __fastcall cheat::render_crosshair( std::uintptr_t a1 )
	{
		if ( settings::g_misc.m_removals.crosshair.value )
		{
			return false;
		}

		return m_render_crosshair.call<bool>( a1 );
	}

	float __fastcall cheat::prepare_scene_material( std::uintptr_t material, void* a2, float a3 )
	{
		features::misc::g_removals.on_prepare_scene_material( material );

		return m_prepare_scene_material.call<float>( material, a2, a3 );
	}

	void __fastcall cheat::post_network_data_received( std::uintptr_t thisptr )
	{
		m_post_network_data_received.call<void>( thisptr );
	}

	bool __fastcall cheat::draw_overhead( std::uintptr_t pawn, std::uint32_t player_slot )
	{
		if ( settings::g_misc.m_removals.overhead.value && pawn == systems::g_local.get( ).pawn )
		{
			return false;
		}

		return m_draw_overhead.call<bool>( pawn, player_slot );
	}

	std::uintptr_t __fastcall cheat::draw_legs( std::uintptr_t a1, std::uintptr_t a2, std::uintptr_t a3, std::uintptr_t a4, std::uintptr_t a5 )
	{
		if ( settings::g_misc.m_removals.legs.value )
		{
			return 0;
		}

		return m_draw_legs.call<std::uintptr_t>( a1, a2, a3, a4, a5 );
	}

	bool __fastcall cheat::get_transforms_for_hitbox_list( std::uintptr_t a1, std::uintptr_t a2, int* a3 )
	{
		if ( !features::combat::g_shared.autowalling( ) )
		{
			return m_get_transforms_for_hitbox_list.call<bool>( a1, a2, a3 );
		}

		const auto record = features::combat::g_shared.current_autowall_record( );
		if ( !record || !record->valid )
		{
			return m_get_transforms_for_hitbox_list.call<bool>( a1, a2, a3 );
		}

		const auto count = memory::safe_read<int>( reinterpret_cast< std::uintptr_t >( a3 ) ).value_or( 0 );
		const auto shape_array = memory::safe_read<std::uintptr_t>( reinterpret_cast< std::uintptr_t >( a3 ) + 8 ).value_or( 0 );
		const auto entity_bone_cache = memory::safe_read<std::uintptr_t>( a1 + 0x1c0 ).value_or( 0 );
		const auto model_handle = memory::safe_read<std::uintptr_t>( a1 + 0x1e0 ).value_or( 0 );
		const auto model = model_handle ? memory::safe_read<std::uintptr_t>( model_handle ).value_or( 0 ) : 0;

		if ( count <= 0 || count > 256 || !shape_array || !entity_bone_cache || !model )
		{
			return false;
		}

		static const auto get_bone_index = PATTERN( patterns::get_bone_index );
		if ( !get_bone_index )
		{
			return false;
		}

		// The client dereferences every resolved transform without checking the
		// backing cache. Reject a stale scene node instead of faulting in it.
		for ( auto i = 0; i < count; ++i )
		{
			const auto shape_ptr = shape_array + 16ull * i;
			const auto bone_index = memory::call<int>( get_bone_index, model, shape_ptr );

			if ( bone_index >= 0 &&
				( bone_index >= 256 ||
					!memory::safe_read<systems::bones::data>( entity_bone_cache + sizeof( systems::bones::data ) * bone_index ) ) )
			{
				return false;
			}
		}

		const auto target_scene = record->game_scene_node ? record->game_scene_node : memory::read<std::uintptr_t>( record->pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
		const auto target_bone_cache = target_scene ? memory::safe_read<std::uintptr_t>( target_scene + SCHEMA( "CSkeletonInstance", "m_modelState"_hash ) + 0x80 ).value_or( 0 ) : 0;
		const auto result = m_get_transforms_for_hitbox_list.call<bool>( a1, a2, a3 );

		if ( !result )
		{
			return false;
		}

		const auto is_plausible = [ ]( std::uintptr_t ptr ) noexcept -> bool
			{
				return ptr >= 0x10000ull && ptr < 0x00007FFFFFFFFFFFull;
			};

		if ( !is_plausible( entity_bone_cache ) || !is_plausible( target_bone_cache ) || entity_bone_cache != target_bone_cache )
		{
			return result;
		}

		const auto output_array = memory::safe_read<std::uintptr_t>( a2 + 16 ).value_or( 0 );

		if ( !output_array || count <= 0 )
		{
			return result;
		}

		for ( auto i = 0; i < count; ++i )
		{
			const auto shape_ptr = shape_array + 16ull * i;
			const auto bone_index = memory::call<int>( get_bone_index, model, shape_ptr );

			if ( bone_index < 0 || bone_index >= record->bone_count )
			{
				continue;
			}

			const auto dst = output_array + 32ull * i;
			std::memcpy( reinterpret_cast< void* >( dst ), &record->bones[ bone_index ], 32 );
		}

		return true;
	}

	void __fastcall cheat::sort_primitives( std::uintptr_t thisptr, std::uintptr_t a2, std::uintptr_t a3, std::uint32_t a4 )
	{
		m_sort_primitives.call<void>( thisptr, a2, a3, a4 );

		diag::exception_scope exception_scope{ "chams: sort primitives" };
		features::esp::player::g_chams.on_sort_primitives( a3, a4 );
	}

	float __fastcall cheat::get_inaccuracy( std::uintptr_t thisptr, float* a2, float* a3 )
	{
		const auto result = m_get_inaccuracy.call<float>( thisptr, a2, a3 );

#if defined(__clang__) || defined(__GNUC__)
		const auto result_address = reinterpret_cast< std::uintptr_t >( __builtin_return_address( 0 ) );
#else
		const auto result_address = reinterpret_cast< std::uintptr_t >( _ReturnAddress( ) );
#endif

		static const auto base_fire_guns_get_inaccuracy = PATTERN( patterns::base_fire_guns_get_inaccuracy );
		if ( base_fire_guns_get_inaccuracy &&
			result_address > base_fire_guns_get_inaccuracy &&
			result_address < base_fire_guns_get_inaccuracy + 0x600 )
		{
			features::misc::g_impacts.on_base_fire_guns_get_inaccuracy( thisptr, result );
		}

		return result;
	}

	float* __fastcall cheat::get_interpolated_shoot_position( std::uintptr_t thisptr, float* out, int* tick_frac )
	{
		const auto result = m_get_interpolated_shoot_position.call<float*>( thisptr, out, tick_frac );

		// Prediction calls this helper while building the next command as well.
		// Only the call made for an actual rage shot is its authoritative origin.
		if ( features::combat::g_rage.is_firing_this_tick( ) )
		{
			// Preserve the engine's authoritative value before applying the local
			// command origin override.  Feeding the overridden value to impacts hid
			// real shoot-position mismatches and made tracers appear to agree when the
			// server had actually used a different origin.
			features::misc::g_impacts.on_get_interpolated_shoot_position( thisptr, out );

			const auto& dt = features::combat::g_rage.dt( );
			const auto& fire_position = dt.fire_position( );
			if ( dt.has_fire_position( ) && out &&
				std::isfinite( fire_position.x ) && std::isfinite( fire_position.y ) &&
				std::isfinite( fire_position.z ) && fire_position.length_sqr( ) >= 1.0f )
			{
				// 引擎回调可能根据本地下一 tick 重新计算原点；开火期间
				// 强制复用 Rage 已验证的当前命令原点。
				out[ 0 ] = fire_position.x;
				out[ 1 ] = fire_position.y;
				out[ 2 ] = fire_position.z;
			}
		}

		return result;
	}

	std::uintptr_t __fastcall cheat::level_initialization( std::uintptr_t a1, const char* new_map )
	{
		if ( new_map && new_map[ 0 ] )
		{
			const char* leaf = std::strrchr( new_map, '/' );
			rendering::g_widgets.s_map_name = leaf ? leaf + 1 : new_map;
		}
		else
		{
			rendering::g_widgets.s_map_name.clear( );
		}

		features::world::g_scene.reset_skybox_state( );
		features::misc::g_impacts.on_level_change( );
		features::misc::g_scoreboard_weapons.on_level_change( );
		scripting::g_engine.on_level_init( new_map );

		return m_level_initialization.call<std::uintptr_t>( a1, new_map );
	}

	std::uintptr_t __fastcall cheat::level_shutdown( std::uintptr_t a1 )
	{
		rendering::g_widgets.s_map_name.clear();

		// Release feature-owned scene objects before Source 2 tears their parents down.
		features::misc::g_dlight.on_level_shutdown( );

		// clear all local player data on level shutdown
		systems::g_local.reset();
		scripting::g_engine.on_level_shutdown( );

		return m_level_shutdown.call<std::uintptr_t>( a1 );
	}

	void __fastcall cheat::read_frame_input( std::uintptr_t a1, std::uint32_t a2 )
	{
		m_read_frame_input.call<void>( a1, a2 );
	}

	void __fastcall cheat::process_input_event( std::uintptr_t csgo_input, int slot, float frametime )
	{
		systems::g_legit_input.on_process_input_event( csgo_input, slot );
		m_process_input_event.call<void>( csgo_input, slot, frametime );
	}

	std::uintptr_t __fastcall cheat::render_decals( std::uintptr_t render_context, std::uintptr_t** render_view, bool pass_flag_a, bool pass_flag_b )
	{
		if ( settings::g_misc.m_removals.decals.value )
		{
			return 0;
		}

		return m_render_decals.call<std::uintptr_t>( render_context, render_view, pass_flag_a, pass_flag_b );
	}

	void __fastcall cheat::render_smoke( std::uintptr_t a1, std::uintptr_t a2, int a3, int a4, std::uintptr_t a5, std::uintptr_t a6 )
	{
		static std::once_flag alright;
		std::call_once( alright, [ & ]
			{
				if ( !a2 )
				{
					return;
				}

				if ( m_render_smoke_map.create( reinterpret_cast< void* >( memory::get_vfunc( a2, 32 ) ), &render_smoke_map ) )
				{
					m_render_smoke_map.enable( );
				}

				if ( m_render_smoke_unmap.create( reinterpret_cast< void* >( memory::get_vfunc( a2, 33 ) ), &render_smoke_unmap ) )
				{
					m_render_smoke_unmap.enable( );
				}
			} );

		if ( settings::g_misc.m_removals.smoke.value )
		{
			return;
		}

		m_render_smoke.call<void>( a1, a2, a3, a4, a5, a6 );
	}

	std::uintptr_t __fastcall cheat::render_smoke_map( std::uintptr_t thisptr, std::size_t size, std::uintptr_t* out_ptr )
	{
		const auto result = m_render_smoke_map.call<std::uintptr_t>( thisptr, size, out_ptr );

		features::world::g_smoke.on_map( result, size, out_ptr && *out_ptr ? *out_ptr : 0 );

		return result;
	}

	void __fastcall cheat::render_smoke_unmap( std::uintptr_t thisptr, std::uintptr_t ctx, std::size_t size )
	{
		features::world::g_smoke.on_unmap( ctx );
		m_render_smoke_unmap.call<void>( thisptr, ctx, size );
	}

	char __fastcall cheat::set_info( std::uintptr_t rcx, std::uintptr_t a2 )
	{
		const auto& cfg = settings::g_misc.m_name_changer;
		const auto should_override = cfg.clantag.value || cfg.override_name.value || features::misc::other::s_name_change_pending;
		if ( should_override && a2 )
		{
			const auto arg_list = memory::safe_read<std::uintptr_t>( a2 + 0x440 ).value_or( 0 );
			const auto key = arg_list
				? memory::safe_read<const char*>( arg_list + 0x8 ).value_or( nullptr )
				: nullptr;

			if ( key && _stricmp( key, "name" ) == 0 )
			{
				constexpr std::uint64_t fcvar_protected = 1ull << 5;
				constexpr std::uint64_t fcvar_userinfo = 1ull << 9;
				constexpr std::uint64_t fcvar_registry_restricted = 1ull << 10;

				if ( addresses::globals::cvar )
				{
					if ( const auto name_cvar = addresses::globals::cvar->find( "name"_hash ) )
					{
						const auto flags_address = reinterpret_cast<std::uintptr_t>( name_cvar ) + offsetof( c_convar, m_flags );
						if ( const auto flags = memory::safe_read<std::uint64_t>( flags_address ) )
						{
							static_cast<void>( memory::safe_write<std::uint64_t>( flags_address,
								( *flags | fcvar_userinfo ) & ~( fcvar_protected | fcvar_registry_restricted ) ) );
						}
					}
				}

				const auto& display = features::misc::other::s_display_name;
				if ( !display.empty( ) )
				{
					static_cast<void>( memory::safe_write<const char*>( arg_list + 0x10, display.c_str( ) ) );
				}
			}
		}

		return m_set_info.call<char>( rcx, a2 );
	}

	void __fastcall cheat::draw_flash_effect( std::uintptr_t a1, int a2, std::uintptr_t* a3, std::uintptr_t a4, __m128* a5 )
	{
		if ( settings::g_misc.m_removals.flash_alpha.value < 100.0f && settings::g_misc.m_removals.flash_alpha.value != 0.0f )
		{
			const auto view_pawn = systems::g_local.get( ).view_pawn( );
			if ( view_pawn )
			{
				const auto max = settings::g_misc.m_removals.flash_alpha.value / 100.0f * 255.0f;
				memory::write<float>( view_pawn + SCHEMA( "C_CSPlayerPawnBase", "m_flFlashMaxAlpha"_hash ), max );
			}
		}

		if ( settings::g_misc.m_removals.flash_alpha.value != 0.0f )
		{
			m_draw_flash_effect.call<void>( a1, a2, a3, a4, a5 );
		}
	}

} // namespace hooks
