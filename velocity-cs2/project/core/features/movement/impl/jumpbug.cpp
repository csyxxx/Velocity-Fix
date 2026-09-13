#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <utilities/logging/logging.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <core/settings.hpp>

#include "../movement.hpp"
#include <protection/game_addresses.hpp>

namespace features::movement {

	void jumpbug::on_create_move( systems::input::usercmd* cmd )
	{
		this->m_active_this_tick = false;

		if ( !settings::g_movement.jumpbug.value )
		{
			return;
		}

		if ( features::movement::g_edgebug.active_this_tick( ) )
		{
			return;
		}

		auto& buttons = cmd->buttons;
		if ( !( buttons.value & cstypes::command_buttons::in_jump ) )
		{
			return;
		}

		const auto local = systems::g_local.get( );
		if ( !local.pawn )
		{
			return;
		}

		const auto move_type = memory::read<std::uint8_t>( local.pawn + SCHEMA( "C_BaseEntity", "m_nActualMoveType"_hash ) );
		if ( move_type == cstypes::move_type::ladder || move_type == cstypes::move_type::noclip )
		{
			return;
		}

		const auto& prestate = systems::g_prediction.pre( );
		if ( prestate.flags & cstypes::entity_flags::on_ground )
		{
			return;
		}

		if ( prestate.velocity.z > 0.0f || prestate.networked_velocity.z > 0.0f )
		{
			return;
		}

		// Calibrate the subtick contact bias from the previous attempt. A missed
		// takeoff is moved later; a takeoff that loses horizontal speed is moved
		// earlier. This absorbs small prediction/server contact differences.
		const auto current_tick = features::combat::g_shared.ctx( ).current_tick;
		if ( this->m_last_tick != 0 )
		{
			const auto gap = std::max( current_tick - this->m_last_tick, 1 );
			this->m_avg_tick_gap = this->m_avg_tick_gap * 0.9f + static_cast< float >( gap ) * 0.1f;
		}
		this->m_last_tick = current_tick;
		const auto dropping_frames = this->m_avg_tick_gap > 1.05f;
		if ( this->m_jump_fired_prev )
		{
			if ( ( prestate.flags & cstypes::entity_flags::on_ground ) && prestate.velocity.z <= 0.0f )
				this->m_when_bias = std::min( this->m_when_bias + 0.02f, 0.10f );
			else if ( prestate.velocity.z > 0.0f )
			{
				const auto speed_2d = prestate.velocity.length_2d( );
				if ( this->m_pre_takeoff_speed_2d > 0.0f && speed_2d < this->m_pre_takeoff_speed_2d * 0.93f )
					this->m_when_bias = std::max( this->m_when_bias - 0.02f, 0.0f );
				else
					this->m_when_bias = std::max( this->m_when_bias - 0.005f, 0.0f );
			}
			this->m_jump_fired_prev = false;
		}

		const auto movement_services = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BasePlayerPawn", "m_pMovementServices"_hash ) );
		if ( !movement_services )
		{
			return;
		}

		const auto duck_amount = memory::read<float>( movement_services + SCHEMA( "CCSPlayer_MovementServices", "m_flDuckAmount"_hash ) );
		const auto holding_duck = ( buttons.value & cstypes::command_buttons::in_duck ) != 0;
		const auto mins = memory::read<math::vector3>( local.pawn + SCHEMA( "C_BaseModelEntity", "m_Collision"_hash ) + SCHEMA( "CCollisionProperty", "m_vecMins"_hash ) );
		auto maxs = memory::read<math::vector3>( local.pawn + SCHEMA( "C_BaseModelEntity", "m_Collision"_hash ) + SCHEMA( "CCollisionProperty", "m_vecMaxs"_hash ) );

		auto trace_origin = prestate.networked_origin;
		{
			if ( holding_duck && duck_amount > 0.0f )
			{
				const auto standing_height{ 72.0f };
				const auto duck_hull_diff = standing_height - maxs.z;
				trace_origin.z -= duck_hull_diff * 0.5f;
				maxs.z = standing_height;
			}
		}

		auto trace_mask{ 0ull };
		{
			const auto pawn_ptr = memory::safe_read<std::uintptr_t>( movement_services + 56 ).value_or( 0 );
			if ( pawn_ptr )
			{
				trace_mask = memory::safe_read<std::uintptr_t>( pawn_ptr + 0xd48 ).value_or( 0 );
				if ( memory::safe_read<std::uint32_t>( pawn_ptr + 0x3f8 ).value_or( 0 ) & 0x10 )
					trace_mask |= 0x20;
			}
			else
			{
				trace_mask |= 0x20;
			}
		}

		const auto filter = systems::g_tracing.make_player_movement_filter( local.pawn, trace_mask, 11 );
		const auto sv_gravity = CONVAR ("sv_gravity")->get<float>( );
		const auto sv_standable_normal = CONVAR ("sv_standable_normal")->get<float>( );
		const auto gravity_scale = memory::read<float>( local.pawn + SCHEMA( "C_BaseEntity", "m_flGravityScale"_hash ) );

		// Movement applies gravity before integrating the air step. Using the
		// local predicted velocity and a half-gravity correction shifts the
		// contact point enough to miss the jump edge at high speed.
		auto velocity = prestate.velocity;
		velocity.z -= gravity_scale * sv_gravity * cstypes::tick_interval;

		math::vector3 trace_start = trace_origin;
		math::vector3 trace_end{};

		trace_end.x = trace_origin.x + velocity.x * cstypes::tick_interval;
		trace_end.y = trace_origin.y + velocity.y * cstypes::tick_interval;
		trace_end.z = trace_origin.z + velocity.z * cstypes::tick_interval;

		trace_end.z -= 2.0f;

		const auto bbox = systems::tracing::bbox_collision{ mins, maxs };
		const auto result = systems::g_tracing.trace_player_bbox( trace_start, trace_end, bbox, filter, movement_services );
		const auto valid_trace = ( result.fraction > 0.0f && result.fraction < 1.0f );

		if ( !valid_trace || result.normal.z < sv_standable_normal )
		{
			return;
		}

		// Do not create a subtick sequence for marginal or sloped contacts. The
		// server accepts only a monotonic button timeline; emitting a duck/jump
		// pulse on an invalid landing was the source of intermittent disconnects.
		if ( result.normal.z < 0.985f )
		{
			return;
		}

		// Refine the hull contact instead of using the coarse first trace.
		// The server evaluates the movement step before the subtick jump edge,
		// so even a few percent of error can turn a jumpbug into a normal fall.
		const auto trace_delta = trace_end - trace_start;
		auto contact_fraction = result.fraction;
		auto lo = 0.0f;
		auto hi = result.fraction;
		for ( int i = 0; i < 4; ++i )
		{
			const auto mid = ( lo + hi ) * 0.5f;
			const auto probe_end = trace_start + trace_delta * mid;
			const auto probe = systems::g_tracing.trace_player_bbox( trace_start, probe_end, bbox, filter, movement_services );
			if ( probe.fraction < 1.0f )
				hi = mid;
			else
				lo = mid;
		}
		contact_fraction = hi;

		// Contact very close to a command boundary is not representable as a
		// reliable press/release pair. Keep the whole-tick jump fallback so the
		// landing still produces a takeoff instead of silently dropping.
		if ( contact_fraction < 0.03f || contact_fraction > 0.90f )
		{
			buttons.value |= cstypes::command_buttons::in_jump;
			buttons.value_changed |= cstypes::command_buttons::in_jump;
			buttons.value_scroll &= ~cstypes::command_buttons::in_jump;
			this->m_landing_fraction = std::clamp( contact_fraction, 0.0f, 1.0f );
			this->m_active_this_tick = true;
			return;
		}

		auto when = std::clamp( contact_fraction + this->m_when_bias, 0.03f, 0.90f );

		this->m_landing_fraction = when;
		this->m_active_this_tick = true;

		const auto base = cmd->csgo_user_cmd.mutable_base( );
		if ( !base )
		{
			return;
		}

		const auto subtick_moves = base->mutable_subtick_moves( );
		if ( !subtick_moves )
		{
			return;
		}

		if ( const auto duck_down = systems::g_input.acquire_subtick_step( subtick_moves ) )
		{
			duck_down->set_button( cstypes::command_buttons::in_duck );
			duck_down->set_pressed( true );
			duck_down->set_when( 0.0f );
			duck_down->set_analog_forward_delta( 0.0f );
			duck_down->set_analog_left_delta( 0.0f );
		}

		if ( const auto duck_up = systems::g_input.acquire_subtick_step( subtick_moves ) )
		{
			duck_up->set_button( cstypes::command_buttons::in_duck );
			duck_up->set_pressed( false );
			duck_up->set_when( std::max( when - 0.01f, 0.0f ) );
			duck_up->set_analog_forward_delta( 0.0f );
			duck_up->set_analog_left_delta( 0.0f );
		}

		// Release and press jump at the same contact fraction. Splitting these
		// edges by a fixed amount lets the server register the landing before the
		// jump press, which is the intermittent speed-loss case.
		if ( const auto jump_up = systems::g_input.acquire_subtick_step( subtick_moves ) )
		{
			jump_up->set_button( cstypes::command_buttons::in_jump );
			jump_up->set_pressed( false );
			jump_up->set_when( when );
			jump_up->set_analog_forward_delta( 0.0f );
			jump_up->set_analog_left_delta( 0.0f );
		}

		if ( const auto jump_down = systems::g_input.acquire_subtick_step( subtick_moves ) )
		{
			jump_down->set_button( cstypes::command_buttons::in_jump );
			jump_down->set_pressed( true );
			jump_down->set_when( when );
			jump_down->set_analog_forward_delta( 0.0f );
			jump_down->set_analog_left_delta( 0.0f );
		}

		// Remove the whole-tick bit. The subtick release/press pair below must
		// be the only jump edge, otherwise the held bit suppresses the next
		// release and makes subsequent jumpbugs intermittent.
		buttons.value &= ~cstypes::command_buttons::in_jump;
		buttons.value_changed &= ~cstypes::command_buttons::in_jump;
		buttons.value_scroll &= ~cstypes::command_buttons::in_jump;

		// If the trace is off by a small amount, later edges are harmless while
		// airborne and give the server another chance to catch the landing.
		const float retry_edges[ 4 ]{ when + 0.03f, when + 0.06f, when + 0.09f, when + 0.12f };
		for ( const auto retry_when : retry_edges )
		{
			const auto clamped_when = std::min( retry_when, 0.97f );
			if ( const auto retry_up = systems::g_input.acquire_subtick_step( subtick_moves ) )
			{
				retry_up->set_button( cstypes::command_buttons::in_jump );
				retry_up->set_pressed( false );
				retry_up->set_when( std::max( clamped_when - 0.01f, 0.0f ) );
				retry_up->set_analog_forward_delta( 0.0f );
				retry_up->set_analog_left_delta( 0.0f );
			}
			if ( const auto retry_down = systems::g_input.acquire_subtick_step( subtick_moves ) )
			{
				retry_down->set_button( cstypes::command_buttons::in_jump );
				retry_down->set_pressed( true );
				retry_down->set_when( clamped_when );
				retry_down->set_analog_forward_delta( 0.0f );
				retry_down->set_analog_left_delta( 0.0f );
			}
		}

		this->m_jump_fired_prev = true;
		this->m_pre_takeoff_speed_2d = prestate.velocity.length_2d( );
		this->m_active_this_tick = true;
	}

	float jumpbug::get_impulse_mul( std::uintptr_t local_pawn ) const
	{
		const auto movement_services = memory::read<std::uintptr_t>( local_pawn + SCHEMA( "C_BasePlayerPawn", "m_pMovementServices"_hash ) );
		if ( !movement_services )
		{
			return 0.0f;
		}

		const auto stamina = memory::read<float>( movement_services + SCHEMA( "CCSPlayer_MovementServices", "m_flStamina"_hash ) );

		if (CONVAR ("sv_legacy_jump")->get<bool>( ) )
		{
			if ( stamina <= 0.0f )
			{
				return 1.0f;
			}

			return std::clamp( 1.0f - ( stamina / 100.0f ), 0.0f, 1.0f );
		}

		const auto current_tick = memory::read<int>( memory::read<std::uintptr_t>( addresses::globals::global_vars ) + 0x44 );
		const auto modern_jump = movement_services + SCHEMA( "CCSPlayer_MovementServices", "m_ModernJump"_hash );
		const auto landing_vel_z = memory::read<float>( modern_jump + SCHEMA( "CCSPlayerModernJump", "m_flLastLandedVelocityZ"_hash ) );
		const auto landed_tick = memory::read<std::uint32_t>( modern_jump + SCHEMA( "CCSPlayerModernJump", "m_nLastLandedTick"_hash ) );
		const auto base = std::clamp( ( landing_vel_z * 0.0005f ) + 1.0f, 0.02f, 1.0f );
		const auto ticks_since_landing = static_cast< float >( current_tick - landed_tick );

		auto result = std::clamp( base + ( ticks_since_landing * 0.6f ), 0.0f, 1.0f );

		if ( stamina > 0.0f )
		{
			result *= std::clamp( 1.0f - ( stamina / 100.0f ), 0.0f, 1.0f );
		}

		return result;
	}

} // namespace features::movement
