#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <utilities/fnv1a.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <protection/game_addresses.hpp>

namespace features::combat {

	void vac_bypass::suppress_cheat_cvars( ) const
	{
		static std::uintptr_t showusercmd{};
		static std::uintptr_t pred_print{};
		static int retry_countdown{};

		if ( !showusercmd || !pred_print )
		{
			if ( retry_countdown > 0 )
			{
				--retry_countdown;
			}
			else
			{
				retry_countdown = 256;
				if ( const auto cvar = addresses::globals::cvar )
				{
					showusercmd = reinterpret_cast< std::uintptr_t >(
						cvar->find( fnv1a::runtime_hash( xs( "cl_showusercmd" ) ) ) );
					pred_print = reinterpret_cast< std::uintptr_t >(
						cvar->find( fnv1a::runtime_hash( xs( "cl_pred_print_every_cmd" ) ) ) );
				}
			}
		}

		if ( showusercmd )
		{
			memory::safe_write< int >( showusercmd + 0x58, 0 );
		}

		if ( pred_print )
		{
			memory::safe_write< int >( pred_print + 0x58, 0 );
		}
	}

	void vac_bypass::on_create_move( systems::input::usercmd* cmd )
	{
		static math::vector3 last_angles{};
		static bool have_last_angles{};

		if ( !settings::g_combat.m_ragebot.vac_bypass.value )
		{
			// Do not carry a stale angle delta across enable/disable transitions.
			// Otherwise the first enabled command can contain a synthetic mouse jump.
			last_angles = {};
			have_last_angles = false;
			return;
		}

		this->suppress_cheat_cvars( );

		const auto base = cmd ? cmd->csgo_user_cmd.mutable_base( ) : nullptr;
		if ( !base )
		{
			return;
		}

		const auto va = base->viewangles( );
		if ( !va )
		{
			return;
		}

		const math::vector3 current_angles{ va->x( ), va->y( ), va->z( ) };
		const bool last_valid = have_last_angles;
		const auto moved = last_valid
			? std::fabsf( current_angles.x - last_angles.x ) + std::fabsf( current_angles.y - last_angles.y )
			: 0.0f;

		if ( last_valid )
		{
			const auto sens = CONVAR( "sensitivity" )->get<float>( );
			if ( sens > 0.0f )
			{
				const auto deg_per_count = sens * 0.022f;
				base->set_mousedx( static_cast< int >( std::roundf( std::remainderf( current_angles.y - last_angles.y, 360.0f ) / deg_per_count ) ) );
				base->set_mousedy( static_cast< int >( std::roundf( -std::remainderf( current_angles.x - last_angles.x, 360.0f ) / deg_per_count ) ) );
			}
		}

		last_angles = current_angles;
		have_last_angles = true;

		// Rage owns the attack history entry. Appending another entry after a
		// shot can shadow the selected angle on the server.
		if ( features::combat::g_rage.is_firing_this_tick( ) )
		{
			return;
		}

		if ( !last_valid || moved < 0.05f )
		{
			return;
		}

		systems::input::input_history_params params{};
		params.view_angles = current_angles;
		params.render_tick = base->client_tick( );
		params.render_frac = 1.0f;
		params.frame_number = base->legacy_command_number( );
		params.player_tick = base->client_tick( );
		params.player_frac = 1.0f;

		static_cast< void >( systems::g_input.push_input_history( cmd, params ) );
	}

} // namespace features::combat
