#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <utilities/diag.hpp>
#include <utilities/logging/logging.hpp>
#include <utilities/random/random.hpp>
#include <utilities/threadpool/threadpool.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <protection/game_addresses.hpp>

namespace features::combat {

	bool rage::air_stop_enabled( ) const noexcept
	{
		const auto& group = settings::g_combat.m_ragebot.get_group( g_shared.ctx( ).weapon_type );
		return group.autostop.values[ 1 ] && !group.no_spread.value;
	}

	void rage::double_tap::reset( ) noexcept
	{
		this->m_state = charge_state::charging;
		this->m_next_attack = 0;
		this->m_shoot_tick = 0;
		this->m_charge_weapon_ready_tick = -1;
		this->m_charge_start_tick = -1;
		this->m_claimed_tick = 0;
		this->m_last_claimed_tick = -1;
		this->m_last_fire_tick = -1;
		this->m_prev_clip = -1;
		this->m_shot_count = 0;
		this->m_no_spread_claim_tick = 0;
		this->m_aim_tick = -1;
		this->m_has_aim = false;
		this->m_has_fire_position = false;
		this->m_has_fire_angle = false;
			// A pair is exactly two attack edges. A third predicted fire command
			// must only clear the rhythm, never become another immediate second.
		this->m_fired = false;
		this->m_second_fired = false;
		this->m_pair_armed = false;
		this->m_history_pair = false;
		this->m_progress = 0.0f;
	}

	void rage::double_tap::on_create_move( systems::input::usercmd* cmd, bool enabled )
	{
		// 每条命令读取武器冷却、弹匣和循环时间，更新 DT 的充能进度与 Ready 状态。
		const auto& ctx = g_shared.ctx( );
		if ( !enabled || !ctx.valid || !ctx.weapon )
		{
			this->reset( );
			return;
		}

		const auto local = systems::g_local.get( );
		if ( !local.controller || this->m_weapon != ctx.weapon )
		{
			this->m_weapon = ctx.weapon;
			this->reset( );
		}

		const auto tick_base = memory::read<int>( local.controller + SCHEMA( "CBasePlayerController", "m_nTickBase"_hash ) );
		const auto next_attack = memory::read<int>( ctx.weapon + SCHEMA( "C_BasePlayerWeapon", "m_nNextPrimaryAttackTick"_hash ) );
		const auto ratio = memory::read<float>( ctx.weapon + SCHEMA( "C_BasePlayerWeapon", "m_flNextPrimaryAttackTickRatio"_hash ) );
		const auto clip = memory::read<int>( ctx.weapon + SCHEMA( "C_BasePlayerWeapon", "m_iClip1"_hash ) );
		const auto reloading = memory::read<bool>( ctx.weapon + SCHEMA( "C_CSWeaponBase", "m_bInReload"_hash ) );

		this->m_next_attack = next_attack;
		this->m_ratio = std::isfinite( ratio ) ? ratio : 0.0f;
		this->m_wat_offset = 0.0f;
		if ( const auto wat_offset = SCHEMA( "C_BasePlayerWeapon", "m_flWatTickOffset"_hash ) )
		{
			const auto wat = memory::read<float>( ctx.weapon + wat_offset );
			if ( std::isfinite( wat ) && std::fabsf( wat ) < 1000.0f )
			{
				this->m_wat_offset = wat;
			}
		}
		auto cycle = 0.1f;
		if ( ctx.weapon_vdata )
		{
			cycle = memory::read<float>( ctx.weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_flCycleTime"_hash ) );
		}
		this->m_cycle_ticks = std::max( 1, static_cast< int >( std::ceil( cycle / cstypes::tick_interval ) ) );

		double offset_tick{};
		const auto wat_fraction = std::modf( static_cast< double >( this->m_wat_offset ), &offset_tick );
		const auto ratio_sum = static_cast< double >( this->m_ratio ) + wat_fraction;
		this->m_shoot_tick = next_attack + static_cast< int >( offset_tick );
		if ( ratio_sum >= 1.0 )
		{
			++this->m_shoot_tick;
		}
		else if ( ratio_sum < 0.0 )
		{
			--this->m_shoot_tick;
		}
		const auto initial_ready = this->m_last_fire_tick < 0 && tick_base >= this->m_shoot_tick;
		if ( initial_ready )
		{
			// 武器本身已可开火时，DT 初始状态直接 Ready，与 Selene 的初始充能规则一致。
			this->m_charge_weapon_ready_tick = this->m_shoot_tick;
			this->m_charge_start_tick = -1;
			this->m_state = charge_state::ready;
			this->m_progress = 1.0f;
		}
		else if ( this->m_charge_weapon_ready_tick < 0 || this->m_shoot_tick > this->m_charge_weapon_ready_tick )
		{
			this->m_charge_weapon_ready_tick = this->m_shoot_tick;
			this->m_charge_start_tick = std::max( this->m_shoot_tick, tick_base );
		}
		const auto dt_ready_tick = this->m_charge_start_tick + this->m_cycle_ticks;

		if ( this->m_prev_clip >= 0 && clip < this->m_prev_clip )
		{
			this->m_shot_count = std::min( this->m_shot_count + ( this->m_prev_clip - clip ), 2 );
		}
		this->m_prev_clip = clip;

		if ( reloading || clip <= 0 )
		{
			this->m_state = charge_state::failed;
			this->m_progress = 0.0f;
			this->m_charge_weapon_ready_tick = -1;
			this->m_charge_start_tick = -1;
			return;
		}

		if ( !initial_ready )
			this->m_state = tick_base >= dt_ready_tick ? charge_state::ready : charge_state::charging;
		if ( this->m_shot_count >= 2 )
		{
			this->m_shot_count = 0;
		}

		if ( !initial_ready )
		{
			const auto charge_elapsed = std::max( tick_base - this->m_charge_start_tick, 0 );
			this->m_progress = std::clamp(
				static_cast< float >( charge_elapsed ) / static_cast< float >( this->m_cycle_ticks ),
				0.0f,
				1.0f );
		}

		const auto since_last = tick_base - this->m_last_fire_tick;
		const auto predicted = this->m_next_attack + this->m_cycle_ticks;
		const auto predict_claim = since_last >= 1 && since_last <= 2 && this->m_next_attack == this->m_last_claimed_tick;
		this->m_claimed_tick = predict_claim ? predicted : this->m_shoot_tick;

		// 当前命令的 history 由引擎根据实时本地姿态生成。不能在预充能阶段
		// 把 player_tick 改成旧的 next_attack，否则双发提交会携带跨越数百
		// tick 的历史时间戳，服务器会按过期姿态重建射线。

		static_cast< void >( cmd );
	}

	void rage::double_tap::refresh_followup_timing( int next_attack, float ratio, float wat_offset, int tick_base ) noexcept
	{
		if ( next_attack < 0 || !std::isfinite( ratio ) )
		{
			return;
		}

		this->m_next_attack = next_attack;
		this->m_ratio = std::clamp( ratio, -0.999999f, 0.999999f );
		this->m_wat_offset = std::isfinite( wat_offset ) && std::fabsf( wat_offset ) < 1000.0f ? wat_offset : 0.0f;

		double offset_tick{};
		const auto wat_fraction = std::modf( static_cast< double >( this->m_wat_offset ), &offset_tick );
		const auto ratio_sum = static_cast< double >( this->m_ratio ) + wat_fraction;
		this->m_shoot_tick = this->m_next_attack + static_cast< int >( offset_tick );
		if ( ratio_sum >= 1.0 )
		{
			++this->m_shoot_tick;
		}
		else if ( ratio_sum < 0.0 )
		{
			--this->m_shoot_tick;
		}

		const auto since_last = tick_base - this->m_last_fire_tick;
		const auto predicted = this->m_next_attack + this->m_cycle_ticks;
		const auto predict_claim = since_last >= 1 && since_last <= 2
			&& this->m_next_attack == this->m_last_claimed_tick;
		this->m_claimed_tick = predict_claim ? predicted : this->m_shoot_tick;
	}

	bool rage::double_tap::on_fired( systems::input::usercmd* cmd, bool fired, bool enabled, bool no_spread )
	{
		static_cast< void >( no_spread );
		// Selene 风格提交：只保留引擎已经生成的 history[0]，用两个 subtick 边沿表达双发。
		if ( enabled && cmd && fired && this->m_pair_armed )
		{
			const auto local = systems::g_local.get( );
			const auto tick_base = local.controller
				? memory::read<int>( local.controller + SCHEMA( "CBasePlayerController", "m_nTickBase"_hash ) )
				: 0;
			const auto base = cmd->csgo_user_cmd.mutable_base( );
			auto* history = cmd->csgo_user_cmd.mutable_input_history( );
			const auto weapon = g_shared.ctx( ).weapon;
			const auto next_primary = this->m_next_attack;
			const auto clip = weapon ? memory::read<int>( weapon + SCHEMA( "C_BasePlayerWeapon", "m_iClip1"_hash ) ) : 0;
			const auto reloading = weapon && memory::read<bool>( weapon + SCHEMA( "C_CSWeaponBase", "m_bInReload"_hash ) );
			const auto valid_history = history && history->m_current_size >= 1 && history->m_current_size <= 16
				&& history->m_rep && history->m_rep->allocated_size >= history->m_current_size
				&& history->m_rep->elements[ 0 ];
			const auto can_commit = base && valid_history
				&& base->client_tick( ) >= next_primary
				&& tick_base >= this->m_last_fire_tick + 2
				&& !reloading && clip > 0;
			if ( !can_commit )
			{
				this->m_pair_armed = false;
				return false;
			}

			const auto subtick_moves = base->mutable_subtick_moves( );
			if ( !subtick_moves )
			{
				this->m_pair_armed = false;
				return false;
			}
			const auto old_size = subtick_moves->m_current_size;
			std::array< proto::subtick_move_step*, 16 > old_steps{};
			std::array< proto::subtick_move_step, 16 > old_values{};
			const auto captured_size = std::clamp( old_size, 0, 16 );
			for ( auto i = 0; i < captured_size; ++i )
			{
				old_steps[ static_cast< std::size_t >( i ) ] = base->mutable_subtick_moves( i );
				if ( old_steps[ static_cast< std::size_t >( i ) ] )
					old_values[ static_cast< std::size_t >( i ) ] = *old_steps[ static_cast< std::size_t >( i ) ];
			}
			subtick_moves->clear( );
			const auto press = systems::g_input.acquire_subtick_step( subtick_moves );
			const auto release = systems::g_input.acquire_subtick_step( subtick_moves );
			if ( !press || !release )
			{
				subtick_moves->m_current_size = old_size;
				for ( auto i = 0; i < captured_size; ++i )
					if ( old_steps[ static_cast< std::size_t >( i ) ] )
						*old_steps[ static_cast< std::size_t >( i ) ] = old_values[ static_cast< std::size_t >( i ) ];
				this->m_pair_armed = false;
				return false;
			}
			press->set_button( cstypes::command_buttons::in_attack );
			press->set_pressed( true );
			press->set_when( 0.0f );
			release->set_button( cstypes::command_buttons::in_attack );
			release->set_pressed( false );
			release->set_when( std::nextafter( 1.0f, 0.0f ) );

			cmd->buttons.value |= cstypes::command_buttons::in_attack;
			cmd->buttons.value_changed |= cstypes::command_buttons::in_attack;
			cmd->buttons.value_scroll |= cstypes::command_buttons::in_attack;
			// 双发必须共享同一条已验证的本地起点。清除基础移动量和方向键，
			// 防止两个 subtick 之间继续推进本地位置造成第二发偏移。
			constexpr auto horizontal_buttons = static_cast<std::uintptr_t>(
				cstypes::command_buttons::in_forward |
				cstypes::command_buttons::in_back |
				cstypes::command_buttons::in_moveleft |
				cstypes::command_buttons::in_moveright );
			base->set_forwardmove( 0.0f );
			base->set_leftmove( 0.0f );
			cmd->buttons.value &= ~horizontal_buttons;
			cmd->buttons.value_changed |= horizontal_buttons;
			cmd->buttons.value_scroll &= ~horizontal_buttons;
			cmd->csgo_user_cmd.set_attack1_start_history_index( 0 );
			cmd->csgo_user_cmd.set_attack2_start_history_index( -1 );
			this->m_history_pair = true;
			this->m_last_fire_tick = tick_base;
			this->m_last_claimed_tick = base->client_tick( );
			this->m_claimed_tick = base->client_tick( );
			this->m_pair_armed = false;
			this->m_fired = false;
			this->m_second_fired = false;
			this->m_state = charge_state::charging;
			this->m_progress = 0.0f;
			this->m_charge_start_tick = tick_base + 1;
			this->m_charge_weapon_ready_tick = tick_base + 1 + this->m_cycle_ticks;
			this->m_has_fire_position = false;
			this->m_has_fire_angle = false;
#if defined( DEV )
			const auto* committed_history = cmd->csgo_user_cmd.mutable_input_history( 0 );
			diag::writef( diag::level::info, "[dt_selene_commit] cmd=%lld client=%d tickbase=%d next=%d history=%d player=%d+%.3f attack1=0",
				static_cast< long long >( cmd->command_number ), base->client_tick( ), tick_base,
				this->m_next_attack, history->m_current_size,
				committed_history ? committed_history->player_tick_count( ) : -1,
				committed_history ? committed_history->player_tick_fraction( ) : 0.0f );
#endif
			return true;
		}
		if ( !enabled || !cmd )
		{
			return fired;
		}
		if ( !fired )
			return false;
		// 非配对开火沿用 Rage 已完成的单发命令，DT 不再改写其 history 或姿态。
		if ( !this->m_pair_armed )
		{
			this->m_fired = false;
			this->m_second_fired = false;
			return true;
		}
		return false;
	#if 0
		// 普通单发只记录开火；只有已配对的 Ready 状态才会写入两个攻击历史槽。
		if ( !enabled || !cmd )
		{
			return fired;
		}
		if ( !fired )
			return false;

		const auto local = systems::g_local.get( );
		const auto tick_base = local.controller
			? memory::read<int>( local.controller + SCHEMA( "CBasePlayerController", "m_nTickBase"_hash ) )
			: 0;

		if ( !this->m_pair_armed )
		{
			this->m_fired = false;
			this->m_second_fired = false;
			return true;
		}

		const auto server_tick = g_shared.sh( ).server_tick( );
		const auto first_tick = std::max(
			this->m_shoot_tick > 0 ? this->m_shoot_tick : ( this->m_next_attack > 0 ? this->m_next_attack : tick_base ),
			server_tick > 0 ? server_tick + 1 : tick_base + 1 );
		const auto second_tick = first_tick + 1;

		auto history = cmd->csgo_user_cmd.mutable_input_history( );
		if ( !history || history->m_current_size < 0 || history->m_current_size > 16 )
		{
			this->m_pair_armed = false;
			return false;
		}
		int live_history_tick = 0;
		if ( history->m_current_size > 0 )
		{
			if ( const auto* live_entry = cmd->csgo_user_cmd.mutable_input_history( history->m_current_size - 1 ) )
				live_history_tick = live_entry->player_tick_count( );
		}
		const auto transaction_tick = live_history_tick > 0 ? live_history_tick : std::max( tick_base, 1 );

		while ( history->m_current_size < 2 )
		{
			systems::input::input_history_params params{};
			params.view_angles = this->m_has_aim ? this->m_aim : systems::g_input.get_view_angles( );
			params.shoot_position = this->m_has_fire_position
				? this->m_fire_position
				: g_shared.get_shoot_position( );
			params.render_tick = transaction_tick;
			params.player_tick = transaction_tick;
			params.player_frac = 0.0f;
			if ( !systems::g_input.push_input_history( cmd, params ) )
			{
				this->m_pair_armed = false;
				return false;
			}
		}

		if ( history->m_current_size < 2 || !history->m_rep || history->m_rep->allocated_size < 2
			|| !history->m_rep->elements[ 0 ] || !history->m_rep->elements[ 1 ] )
		{
			this->m_pair_armed = false;
			return false;
		}

		const auto source_index = history->m_current_size - 1;
		const auto source = cmd->csgo_user_cmd.mutable_input_history( source_index );
		const auto first = cmd->csgo_user_cmd.mutable_input_history( 0 );
		const auto second = cmd->csgo_user_cmd.mutable_input_history( 1 );
		if ( !source || !first || !second )
		{
			this->m_pair_armed = false;
			return false;
		}
		const auto copy_target_state = [ source ]( auto* destination )
			{
				if ( source->has_render_tick_count( ) )
					destination->set_render_tick_count( source->render_tick_count( ) );
				if ( source->has_render_tick_fraction( ) )
					destination->set_render_tick_fraction( source->render_tick_fraction( ) );
				if ( source->has_target_ent_index( ) )
					destination->set_target_ent_index( source->target_ent_index( ) );

				const auto copy_vector = [ ]( const auto* from, auto* to )
					{
						if ( from && to )
						{
							to->set_x( from->x( ) );
							to->set_y( from->y( ) );
							to->set_z( from->z( ) );
						}
					};
				if ( source->has_target_head_pos_check( ) )
					copy_vector( source->target_head_pos_check( ), destination->mutable_target_head_pos_check( ) );
				if ( source->has_target_abs_pos_check( ) )
					copy_vector( source->target_abs_pos_check( ), destination->mutable_target_abs_pos_check( ) );
				if ( source->has_target_abs_ang_check( ) )
					copy_vector( source->target_abs_ang_check( ), destination->mutable_target_abs_ang_check( ) );
			};
		copy_target_state( first );
		copy_target_state( second );

		history->m_current_size = 2;

		first->set_player_tick_count( transaction_tick );
		first->set_player_tick_fraction( 0.0f );
		second->set_player_tick_count( transaction_tick );
		second->set_player_tick_fraction( 0.0f );
		const auto pair_angle = this->m_has_fire_angle
			? this->m_fire_angle
			: ( this->m_has_aim ? this->m_aim : systems::g_input.get_view_angles( ) );
		const auto pair_position = [ & ]( )
		{
			const auto valid = [ ]( const math::vector3& value )
			{
				return std::isfinite( value.x ) && std::isfinite( value.y ) &&
					std::isfinite( value.z ) && value.length_sqr( ) >= 1.0f;
			};
			if ( this->m_has_fire_position && valid( this->m_fire_position ) )
				return this->m_fire_position;
			return g_shared.get_shoot_position( );
		}( );
		if ( !std::isfinite( pair_position.x ) || !std::isfinite( pair_position.y )
			|| !std::isfinite( pair_position.z ) || pair_position.length_sqr( ) < 1.0f )
		{
			this->m_pair_armed = false;
			return false;
		}

		for ( const auto entry : { first, second } )
		{
			if ( const auto angles = entry->mutable_view_angles( ) )
			{
				angles->set_x( pair_angle.x );
				angles->set_y( pair_angle.y );
				angles->set_z( pair_angle.z );
			}
			if ( !entry->m_shoot_position && !systems::g_input.set_shoot_position( cmd, entry, pair_position ) )
			{
				this->m_pair_armed = false;
				return false;
			}
		}
		cmd->csgo_user_cmd.set_attack1_start_history_index( 0 );
		cmd->csgo_user_cmd.set_attack2_start_history_index( -1 );
		this->m_history_pair = true;

		if ( const auto base = cmd->csgo_user_cmd.mutable_base( ) )
		{
			cmd->buttons.value |= cstypes::command_buttons::in_attack;
			cmd->buttons.value_changed |= cstypes::command_buttons::in_attack;
			cmd->buttons.value_scroll |= cstypes::command_buttons::in_attack;
			const auto subtick_moves = base->mutable_subtick_moves( );
			if ( subtick_moves )
			{
				subtick_moves->clear( );
				if ( const auto step = systems::g_input.acquire_subtick_step( subtick_moves ) )
				{
					step->set_button( cstypes::command_buttons::in_attack );
					step->set_pressed( true );
					step->set_when( 0.0f );
				}
				if ( const auto step = systems::g_input.acquire_subtick_step( subtick_moves ) )
				{
					step->set_button( cstypes::command_buttons::in_attack );
					step->set_pressed( false );
					step->set_when( std::nextafter( 1.0f, 0.0f ) );
				}
			}
		}

#if defined( DEV )
		diag::writef( diag::level::info,
			"[dt_pair] cmd=%lld server=%d next=%d ratio=%.6f wat=%.6f first=%d+0.000000 second=%d+0.499999 history=%d attack=1",
			static_cast<long long>( cmd->command_number ), server_tick, this->m_next_attack,
			this->m_ratio, this->m_wat_offset, first_tick, second_tick, history->m_current_size );
#endif

		this->m_last_fire_tick = tick_base;
		this->m_last_claimed_tick = first_tick;
		this->m_claimed_tick = first_tick;
		this->m_pair_armed = false;
		this->m_fired = false;
		this->m_second_fired = false;
		this->m_state = charge_state::charging;
		this->m_progress = 0.0f;
		this->m_charge_start_tick = second_tick;
		this->m_charge_weapon_ready_tick = second_tick;
		this->m_has_fire_position = false;
		this->m_has_fire_angle = false;
		return true;
	#endif
	}

	void rage::double_tap::on_render( xdraw::draw_list&, bool ) const
	{
	}

	void rage::on_create_move( systems::input::usercmd* cmd )
	{
		// Rage 主循环在这里清理上一命令状态、建立预测上下文并选择本 tick 的目标。
		auto& ctx = g_shared.ctx( );
		const auto local = systems::g_local.get( );
		this->update_penetration_crosshair( local );
		this->m_should_stop = false;
		this->m_air_stop_requested = false;
		// 开火使用本命令捕获的本地原点，不再人为延迟一个 tick。
		this->m_deferred_fire_command = -1;
		this->m_has_deferred_fire_angle = false;
		const auto previous_rage_fire = this->m_firing_this_tick;
		this->m_firing_this_tick = false;
		this->m_double_tap.clear_history_pair( );
		if ( previous_rage_fire )
		{
			cmd->buttons.value &= ~( cstypes::command_buttons::in_attack | cstypes::command_buttons::in_second_attack );
			cmd->buttons.value_changed &= ~( cstypes::command_buttons::in_attack | cstypes::command_buttons::in_second_attack );
			cmd->buttons.value_scroll &= ~( cstypes::command_buttons::in_attack | cstypes::command_buttons::in_second_attack );
			cmd->csgo_user_cmd.set_attack1_start_history_index( -1 );
			cmd->csgo_user_cmd.set_attack2_start_history_index( -1 );
		}
		this->m_state_target = {};
		this->m_protected_shot_command = -1;
		this->m_protected_shot_history_index = -1;
		this->m_protected_consumed_server_angle_changes = 0;
		if ( !this->air_stop_enabled( )
			|| ( systems::g_prediction.pre( ).flags & cstypes::entity_flags::on_ground ) )
		{
			this->m_air_stop_settle_ticks = 0;
		}

		if ( !ctx.valid )
		{
			this->m_doubletap_sent_tick = -1;
			this->m_target_switch_pawn = 0;
			this->m_pending_target_switch_pawn = 0;
			this->m_target_switch_weapon = 0;
			this->m_pending_target_switch_tick = -1;
			this->m_revolver_cock_ticks = 0;
			this->m_doubletap_command_pending = false;
			this->m_double_tap.reset( );
			this->m_doubletap_weapon = 0;
			this->m_doubletap_pair_target_valid = false;
			this->m_doubletap_pair_target_tick = -1;
			this->cancel_doubletap_charge( );
			this->m_doubletap_state.store( doubletap_state::uncharged, std::memory_order_relaxed );
			return;
		}

		const auto& active_config = settings::g_combat.m_ragebot.get_group( ctx.weapon_type );
		if ( this->m_target_switch_weapon != ctx.weapon )
		{
			this->m_target_switch_pawn = 0;
			this->m_pending_target_switch_pawn = 0;
			this->m_pending_target_switch_tick = -1;
			this->m_target_switch_weapon = ctx.weapon;
		}
		if ( active_config.doubletap.value && this->m_doubletap_weapon != ctx.weapon )
		{
			this->m_doubletap_state.store( doubletap_state::uncharged, std::memory_order_relaxed );
			this->m_doubletap_pair_target_valid = false;
			this->m_doubletap_pair_target_tick = -1;
		}

		if ( !settings::g_combat.m_duckpeek.enabled.value )
		{
			this->m_release_duck_for_shot = false;
			this->m_duckpeek_reduck = false;
		}

		if ( this->m_zeus_fired )
		{
			this->m_zeus_fired = false;

			if ( settings::g_combat.m_zeusbot.drop_after && !systems::g_local.is_in_deathmatch( ) )
			{
				memory::call<void>(PATTERN (patterns::engine_client_cmd), addresses::globals::source2engine_to_client, 0, "drop", 0x7ffef001 );
			}

			return;
		}

		const auto is_knife = ctx.weapon_type == cstypes::weapon_type::knife;
		const auto is_taser = ctx.weapon_type == cstypes::weapon_type::taser;

		if ( !is_knife && !is_taser && ( ctx.weapon_type < cstypes::weapon_type::pistol || ctx.weapon_type > cstypes::weapon_type::lmg ) )
		{
			return;
		}

		const auto dt_enabled = !is_knife && !is_taser
			&& ctx.item_def_idx != cstypes::item_definition_index::weapon_r8_revolver
			&& active_config.doubletap.value;
		this->m_double_tap.on_create_move( cmd, dt_enabled );

		auto aim_ctx = this->build_context( cmd, local );

		if ( is_knife )
		{
			if ( !g_shared.can_shoot( cmd, local.controller ) )
			{
				return;
			}

			this->run_knife( cmd, aim_ctx, local );
		}
		else if ( is_taser )
		{
			if ( !g_shared.can_shoot( cmd, local.controller ) )
			{
				return;
			}

			this->run_taser( cmd, aim_ctx, local );
		}
		else if ( ctx.item_def_idx == cstypes::item_definition_index::weapon_r8_revolver )
		{
			this->auto_revolver( cmd, aim_ctx, local );
		}
		else
		{
			this->m_revolver_cock_ticks = 0;
			const auto tick_base = memory::read<int>( local.controller + SCHEMA( "CBasePlayerController", "m_nTickBase"_hash ) );
			const auto dt_second_attack = dt_enabled && this->m_double_tap.is_second_attack( tick_base );
			const auto manual_attack = ( cmd->buttons.value & cstypes::command_buttons::in_attack ) != 0
				|| cmd->csgo_user_cmd.attack1_start_history_index( ) >= 0;
			const auto can_fire = g_shared.can_shoot( cmd, local.controller, !dt_enabled );
			if ( manual_attack )
			{
#if defined( DEV )
				static int last_gate_tick = -1;
				static bool last_can_fire = false;
				if ( last_gate_tick < 0 || can_fire != last_can_fire || tick_base - last_gate_tick >= 8 )
				{
					const auto next_primary = memory::read<int>( ctx.weapon + SCHEMA( "C_BasePlayerWeapon", "m_nNextPrimaryAttackTick"_hash ) );
					const auto clip = memory::read<int>( ctx.weapon + SCHEMA( "C_BasePlayerWeapon", "m_iClip1"_hash ) );
					const auto reloading = memory::read<bool>( ctx.weapon + SCHEMA( "C_CSWeaponBase", "m_bInReload"_hash ) );
					diag::writef( diag::level::info,
						"[rage_gate] reason=%s tick=%d tickbase=%d next_primary=%d manual=%u clip=%d reload=%u dt=%u",
						manual_attack ? "manual_attack" : "can_shoot",
						tick_base,
						tick_base,
						next_primary,
						manual_attack ? 1u : 0u,
						clip,
						reloading ? 1u : 0u,
						dt_enabled ? 1u : 0u );
					last_gate_tick = tick_base;
					last_can_fire = can_fire;
				}
#endif
				this->m_double_tap.on_fired( cmd, false, dt_enabled, active_config.no_spread.value );
				return;
			}
			if ( !can_fire )
			{
				// between shot 模式需要在武器冷却期间继续扫描有效目标，
				// 否则 run_gun() 没有机会生成停步请求。
				if ( active_config.autostop.values[ 2 ] )
					this->run_gun( cmd, aim_ctx, local, false );
				this->m_double_tap.on_fired( cmd, false, dt_enabled, active_config.no_spread.value );
				return;
			}

			this->run_gun( cmd, aim_ctx, local );
			if ( dt_second_attack )
			{
				this->m_doubletap_pair_target_valid = false;
				this->m_doubletap_pair_target_tick = -1;
			}
			const auto rage_fired = this->m_firing_this_tick;
			const auto attack_committed = this->m_double_tap.on_fired( cmd, rage_fired, dt_enabled, active_config.no_spread.value );
			if ( rage_fired && dt_enabled && attack_committed )
				this->m_doubletap_sent_tick = tick_base;
			if ( rage_fired && dt_enabled && !attack_committed )
			{
				features::misc::g_impacts.discard_last_uncommitted_shot( );
			}
			if ( rage_fired && dt_enabled && attack_committed && this->m_double_tap.history_pair( ) )
			{
				this->m_doubletap_stored_client_tick = -1;
			}
		}
	}

	void rage::on_render( xdraw::draw_list& draw_list )
	{
		const auto& dt_config = settings::g_combat.m_ragebot.get_group( g_shared.ctx( ).weapon_type );
		if ( dt_config.doubletap.value )
		{
			const auto actual_state = this->m_doubletap_state.load( std::memory_order_relaxed );
			const auto local = systems::g_local.get( );
			const auto tick_base = local.controller
				? memory::read<int>( local.controller + SCHEMA( "CBasePlayerController", "m_nTickBase"_hash ) )
				: 0;
			const auto sent = this->m_doubletap_sent_tick >= 0 && tick_base >= this->m_doubletap_sent_tick
				&& tick_base - this->m_doubletap_sent_tick <= 3;
			const char* text = "DT UNCHARGED";
			xdraw::color color{ 235, 90, 90, 255 };
			if ( sent ) { text = "DT SENT"; color = { 93, 203, 255, 255 }; }
			else if ( actual_state == doubletap_state::charging ) { text = "DT CHARGING"; color = { 240, 190, 70, 255 }; }
			else if ( actual_state == doubletap_state::ready ) { text = "DT READY"; color = { 90, 220, 130, 255 }; }
			const auto [ screen_w, screen_h ] = xdraw::viewport_size( );
			const auto [ text_w, text_h ] = xdraw::measure_text( text );
			draw_list.text( std::floorf( static_cast< float >( screen_w ) * 0.5f - text_w * 0.5f ), std::floorf( static_cast< float >( screen_h ) * 0.58f ), text, color, xdraw::text_style::outlined );
		}

		this->draw_penetration_crosshair( draw_list );
		this->draw_hittable_regions( draw_list );
		this->draw_extrapolated_skeletons( draw_list );

		const auto& config = settings::g_combat.m_ragebot.get_group( g_shared.ctx( ).weapon_type );
		if ( !config.debug_multipoints.value )
		{
			return;
		}

		std::lock_guard lock( m_debug_mtx );

		for ( const auto& pt : m_debug_points )
		{
			const auto screen = systems::g_view.project( pt.position );
			if ( !systems::g_view.projection_valid( screen ) )
			{
				continue;
			}

			xdraw::color col{};
			switch ( pt.hitbox_index )
			{
			case 0:
				col = { 255, 80,  80  }; break; // head 鈥?red
			case 2: case 3:
				col = { 220, 220, 60  }; break; // stomach 鈥?yellow
			case 4: case 5: case 6:
				col = { 255, 160, 60  }; break; // chest 鈥?orange
			case 7: case 8: case 9: case 10: case 11: case 12:
				col = { 80,  160, 255 }; break; // legs 鈥?blue
			case 13: case 14: case 15: case 16: case 17: case 18:
				col = { 180, 80,  255 }; break; // arms 鈥?purple
			default:
				col = { 200, 200, 200 }; break;
			}

			const auto alpha  = pt.is_center ? std::uint8_t{ 255 } : std::uint8_t{ 160 };
			const auto radius = pt.is_center ? 3.5f : 2.0f;

			draw_list.circle_filled( screen.x, screen.y, radius, col.alpha( alpha ) );
		}
	}

	void rage::draw_extrapolated_skeletons( xdraw::draw_list& draw_list ) const
	{
		const auto& lagcomp = settings::g_combat.m_lagcomp;
		if ( !lagcomp.extrapolation_skeleton.value || !systems::g_view.has_camera( ) )
		{
			return;
		}

		std::vector<shared::lagcomp::record> records;
		{
			std::lock_guard lock( m_extrapolated_mtx );
			records = m_extrapolated_records;
		}

		constexpr auto none{ ~0u };
		constexpr std::array chains
		{
			std::array{ cstypes::bone_ids::head, cstypes::bone_ids::neck, cstypes::bone_ids::spine_4, cstypes::bone_ids::spine_3, cstypes::bone_ids::spine_2, cstypes::bone_ids::spine_1, cstypes::bone_ids::pelvis },
			std::array{ cstypes::bone_ids::left_hand, cstypes::bone_ids::left_elbow, cstypes::bone_ids::left_shoulder, cstypes::bone_ids::left_clavicle, cstypes::bone_ids::spine_4, none, none },
			std::array{ cstypes::bone_ids::right_hand, cstypes::bone_ids::right_elbow, cstypes::bone_ids::right_shoulder, cstypes::bone_ids::right_clavicle, cstypes::bone_ids::spine_4, none, none },
			std::array{ cstypes::bone_ids::left_foot, cstypes::bone_ids::left_knee, cstypes::bone_ids::left_hip, cstypes::bone_ids::pelvis, none, none, none },
			std::array{ cstypes::bone_ids::right_foot, cstypes::bone_ids::right_knee, cstypes::bone_ids::right_hip, cstypes::bone_ids::pelvis, none, none, none }
		};

		const auto outline = xdraw::color{ 0, 0, 0, 150 };
		const auto color = xdraw::color{ 105, 220, 255, 210 };
		for ( const auto& record : records )
		{
			if ( !record.extrapolated || !record.valid || record.bone_count <= 0 )
			{
				continue;
			}

			for ( const auto& chain : chains )
			{
				for ( std::size_t i = 1; i < chain.size( ); ++i )
				{
					const auto from = chain[ i - 1 ];
					const auto to = chain[ i ];
					if ( from == none || to == none || from >= 27u || to >= 27u ||
						from >= static_cast< unsigned >( record.bone_count ) || to >= static_cast< unsigned >( record.bone_count ) )
					{
						continue;
					}

					const auto a = systems::g_view.project( record.bones[ from ].position );
					const auto b = systems::g_view.project( record.bones[ to ].position );
					if ( !systems::g_view.projection_valid( a ) || !systems::g_view.projection_valid( b ) )
					{
						continue;
					}

					draw_list.line( a.x, a.y, b.x, b.y, outline, 2.6f );
					draw_list.line( a.x, a.y, b.x, b.y, color, 1.35f );
				}
			}
		}
	}

	void rage::finalize_shot_command( systems::input::usercmd* cmd )
	{
		if ( !cmd || cmd->command_number != this->m_protected_shot_command )
		{
			return;
		}

		const auto history_size = cmd->csgo_user_cmd.input_history_size( );
		if ( this->m_protected_shot_history_index < 0 || this->m_protected_shot_history_index >= history_size )
		{
			return;
		}

		const auto entry = cmd->csgo_user_cmd.mutable_input_history( this->m_protected_shot_history_index );
		const auto base = cmd->csgo_user_cmd.mutable_base( );
		if ( !entry || !base )
		{
			return;
		}

		if ( const auto angles = entry->mutable_view_angles( ) )
		{
			angles->set_x( this->m_protected_shot_angle.x );
			angles->set_y( this->m_protected_shot_angle.y );
			angles->set_z( this->m_protected_shot_angle.z );
		}

		const auto& config = settings::g_combat.m_ragebot.get_group( g_shared.ctx( ).weapon_type );
		if ( config.no_spread.value )
		{
			for ( auto i = 0; i < history_size; ++i )
			{
				const auto history_entry = cmd->csgo_user_cmd.mutable_input_history( i );
				if ( !history_entry || !history_entry->m_view_angles )
				{
					continue;
				}

				if ( const auto history_angles = history_entry->mutable_view_angles( ) )
				{
					history_angles->set_x( this->m_protected_shot_angle.x );
					history_angles->set_y( this->m_protected_shot_angle.y );
					history_angles->set_z( this->m_protected_shot_angle.z );
				}
			}
		}

		if ( const auto angles = base->mutable_viewangles( ) )
		{
			angles->set_x( this->m_protected_command_angle.x );
			angles->set_y( this->m_protected_command_angle.y );
			angles->set_z( this->m_protected_command_angle.z );
		}

		const auto consumed_server_angle_changes = std::max(
			this->m_protected_consumed_server_angle_changes,
			base->consumed_server_angle_changes( ) );
		base->set_consumed_server_angle_changes( consumed_server_angle_changes );

		if ( this->m_double_tap.history_pair( ) )
		{
			cmd->csgo_user_cmd.set_attack1_start_history_index( 1 );
		}
		else
		{
			cmd->csgo_user_cmd.set_attack1_start_history_index( this->m_protected_shot_history_index );
		}

#if defined( DEV )
		const auto final_shot = entry->view_angles( );
		const auto final_base = base->viewangles( );
		const auto final_eye = entry->shoot_position( );
		diag::writef(
			diag::level::info,
			"[rage_shot_final] cmd=%lld attack_history=%d history=%d/%d player_tick=%d+%.3f render_tick=%d+%.3f shoot_eye=(%.2f,%.2f,%.2f) consumed_angles=%u shot=(%.3f,%.3f,%.3f) base=(%.3f,%.3f,%.3f)",
			static_cast<long long>( cmd->command_number ),
			cmd->csgo_user_cmd.attack1_start_history_index( ),
			this->m_protected_shot_history_index,
			history_size,
			entry->player_tick_count( ),
			entry->player_tick_fraction( ),
			entry->render_tick_count( ),
			entry->render_tick_fraction( ),
			final_eye ? final_eye->x( ) : 0.0f,
			final_eye ? final_eye->y( ) : 0.0f,
			final_eye ? final_eye->z( ) : 0.0f,
			consumed_server_angle_changes,
			final_shot ? final_shot->x( ) : 0.0f,
			final_shot ? final_shot->y( ) : 0.0f,
			final_shot ? final_shot->z( ) : 0.0f,
			final_base ? final_base->x( ) : 0.0f,
			final_base ? final_base->y( ) : 0.0f,
			final_base ? final_base->z( ) : 0.0f );
#endif
	}

	rage::aim_context rage::build_context( systems::input::usercmd* cmd, const systems::local::snapshot& local ) const
	{
		auto& ctx = g_shared.ctx( );
		const auto& prestate = systems::g_prediction.pre( );

		aim_context out{};
		out.velocity = prestate.velocity;
		out.spread = g_shared.get_spread( );
		out.predicted_inaccuracy = g_shared.get_inaccuracy( true );
		g_shared.sh( ).snapshot( local.pawn, ctx.weapon_services );
		// 以 create-move 开始时捕获的网络原点作为当前姿态基准。
		// 武器服务中的 shoot history 可能已经写入下一 tick 位移，不能
		// 直接拿它作为本次扫描和穿透的起点。
		const auto valid_eye = [ ]( const math::vector3& value )
		{
			return std::isfinite( value.x ) && std::isfinite( value.y ) &&
				std::isfinite( value.z ) && value.length_sqr( ) >= 1.0f;
		};
		// Rage 始终使用武器服务返回的当前实时眼位，不使用 prestate 或预测回退。
		out.predicted_eye = g_shared.get_shoot_position( );
		#if defined( DEV )
		if ( valid_eye( out.predicted_eye ) )
			diag::writef( diag::level::info, "[rage_realtime_eye] eye=(%.2f,%.2f,%.2f)", out.predicted_eye.x, out.predicted_eye.y, out.predicted_eye.z );
		#endif

		// Rage 的扫描和开火必须绑定当前命令姿态。不要在这里执行完整的
		// 本地移动模拟，否则武器精度和后续停步链会被下一 tick 状态污染。
		// 停步预测由 predict_stop() 单独完成，不改变本次射击原点。
		const auto movement_simulated = false;

		const auto command_eye_valid = std::isfinite( out.predicted_eye.x ) &&
			std::isfinite( out.predicted_eye.y ) && std::isfinite( out.predicted_eye.z ) &&
			out.predicted_eye.length_sqr( ) >= 1.0f;
		out.local_prediction_applied = false;
#if defined( DEV )
		if ( command_eye_valid && !movement_simulated )
		{
			static auto last_log_tick = -64;
			if ( ctx.current_tick - last_log_tick >= 64 )
			{
				last_log_tick = ctx.current_tick;
				diag::writef( diag::level::info,
					"[local_prediction] movement simulation unavailable; using current command eye=(%.2f,%.2f,%.2f)",
					out.predicted_eye.x, out.predicted_eye.y, out.predicted_eye.z );
			}
		}
#else
		static_cast< void >( movement_simulated );
#endif

		ctx.spread = out.spread;
		ctx.inaccuracy = out.predicted_inaccuracy;

		out.view_angles = systems::g_input.get_view_angles( );
		out.on_ground = ( prestate.flags & cstypes::entity_flags::on_ground ) != 0;
		out.is_scoped = ctx.is_scoped;
		out.weapon_max_speed = ctx.weapon_max_speed;
		out.accurate_threshold = ctx.weapon_max_speed * 0.34f;

		return out;
	}

	rage::doubletap_action rage::process_doubletap( systems::input::usercmd* cmd, const systems::local::snapshot& local, bool charge_dt )
	{
		auto& shared_ctx = g_shared.ctx( );
		const auto& config = settings::g_combat.m_ragebot.get_group( shared_ctx.weapon_type );
		if ( !config.doubletap.value || shared_ctx.item_def_idx == cstypes::item_definition_index::weapon_r8_revolver )
		{
			this->m_doubletap_sent_tick = -1;
			this->m_double_tap.set_pair_armed( false );
			this->m_doubletap_command_pending = false;
			this->m_doubletap_weapon = 0;
			this->cancel_doubletap_charge( );
			this->m_doubletap_state.store( doubletap_state::uncharged, std::memory_order_relaxed );
			return doubletap_action::none;
		}

		const auto weapon_changed = this->m_doubletap_weapon != shared_ctx.weapon;
		if ( weapon_changed )
		{
			this->m_double_tap.set_pair_armed( false );
			this->m_doubletap_command_pending = false;
			this->m_double_tap.reset( );
			this->m_doubletap_weapon = shared_ctx.weapon;
			this->cancel_doubletap_charge( );
		}

		const auto base_cmd = cmd->csgo_user_cmd.mutable_base( );
		if ( !base_cmd )
		{
			return doubletap_action::none;
		}

		const auto client_tick = base_cmd->client_tick( );
		const auto reloading = memory::read<bool>( shared_ctx.weapon + SCHEMA( "C_CSWeaponBase", "m_bInReload"_hash ) );
		const auto clip = memory::read<int>( shared_ctx.weapon + SCHEMA( "C_BasePlayerWeapon", "m_iClip1"_hash ) );
		if ( reloading || clip < 2 )
		{
			this->m_double_tap.set_pair_armed( false );
			this->cancel_doubletap_charge( );
			this->m_doubletap_state.store( doubletap_state::uncharged, std::memory_order_relaxed );
			return doubletap_action::none;
		}

		const auto cycle_time = shared_ctx.weapon_vdata
			? memory::read<float>( shared_ctx.weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_flCycleTime"_hash ) )
			: 0.0f;
		const auto required_shift = static_cast<int>( std::ceil( cycle_time / cstypes::tick_interval ) );
		if ( required_shift < 1 )
		{
			this->m_double_tap.set_pair_armed( false );
			this->cancel_doubletap_charge( );
			this->m_doubletap_state.store( doubletap_state::uncharged, std::memory_order_relaxed );
			return doubletap_action::none;
		}

		this->m_doubletap_required_ticks = required_shift;
		const auto ready = this->m_double_tap.state( ) == double_tap::charge_state::ready;
		this->m_doubletap_state.store( ready ? doubletap_state::ready : doubletap_state::charging, std::memory_order_relaxed );
		if ( !ready )
		{
			this->m_double_tap.set_pair_armed( false );
			return doubletap_action::charging;
		}
		if ( !charge_dt )
		{
			this->m_double_tap.set_pair_armed( false );
			return doubletap_action::none;
		}

		// Ready 只代表能量已满；武器拉栓或上一发间隔未结束时不要 arm。
		const auto tick_base = local.controller
			? memory::read<int>( local.controller + SCHEMA( "CBasePlayerController", "m_nTickBase"_hash ) )
			: 0;
		const auto legal_next_attack = memory::read<int>( shared_ctx.weapon + SCHEMA( "C_BasePlayerWeapon", "m_nNextPrimaryAttackTick"_hash ) );
		if ( client_tick < legal_next_attack || tick_base < this->m_double_tap.last_fire_tick( ) + 2 )
		{
			this->m_double_tap.set_pair_armed( false );
			return doubletap_action::none;
		}

		this->m_double_tap.set_pair_armed( true );
		this->m_doubletap_release_client_tick = client_tick;
		return doubletap_action::command_pair;
	}

	void rage::cancel_doubletap_charge( ) noexcept
	{
		this->m_double_tap.set_pair_armed( false );
		this->m_doubletap_state.store( doubletap_state::uncharged, std::memory_order_relaxed );
		this->m_doubletap_stored_client_tick = -1;
		this->m_doubletap_release_client_tick = -1;
		this->m_doubletap_required_ticks = 0;
		this->m_doubletap_charge_command = -1;
		this->m_doubletap_release_pending = false;
		this->m_doubletap_followup_fire = false;
	}

	bool rage::write_doubletap_command( systems::input::usercmd* cmd, const systems::local::snapshot& local )
	{
		if ( !this->m_doubletap_command_pending )
		{
			return false;
		}

		auto target = this->m_doubletap_target;
		auto shoot_eye = this->m_doubletap_shoot_eye;
		const auto was_forced = this->m_doubletap_was_forced;
		this->m_doubletap_command_pending = false;
		this->m_doubletap_primary_command = -1;

		const auto base = cmd ? cmd->csgo_user_cmd.mutable_base( ) : nullptr;
		auto& shared_ctx = g_shared.ctx( );
		const auto invalid_pair = !cmd || !base || !local.controller || !target.hit.record || !target.hit.record->valid
			|| this->m_doubletap_weapon != g_shared.ctx( ).weapon || !target.hit.pawn
			|| memory::read<int>( target.hit.pawn + SCHEMA( "C_BaseEntity", "m_iHealth"_hash ) ) <= 0;
		if ( invalid_pair )
		{
#if defined( DEV )
			diag::writef( diag::level::warning, "[doubletap_followup_reject] reason=pair_invalid record=%u weapon_match=%u pawn=%u",
				target.hit.record && target.hit.record->valid ? 1u : 0u,
				this->m_doubletap_weapon == g_shared.ctx( ).weapon ? 1u : 0u,
				target.hit.pawn ? 1u : 0u );
#endif
			return false;
		}


		const auto next_attack = memory::read<int>( shared_ctx.weapon + SCHEMA( "C_BasePlayerWeapon", "m_nNextPrimaryAttackTick"_hash ) );
		const auto ratio = memory::read<float>( shared_ctx.weapon + SCHEMA( "C_BasePlayerWeapon", "m_flNextPrimaryAttackTickRatio"_hash ) );
		if ( next_attack >= 0 && std::isfinite( ratio ) )
		{
			auto wat_value = 0.0f;
			if ( const auto wat_offset = SCHEMA( "C_BasePlayerWeapon", "m_flWatTickOffset"_hash ) )
			{
				const auto wat = memory::read<float>( shared_ctx.weapon + wat_offset );
				if ( std::isfinite( wat ) && std::fabsf( wat ) < 1000.0f )
				{
					wat_value = wat;
				}
			}
			const auto tick_base = memory::read<int>( local.controller + SCHEMA( "CBasePlayerController", "m_nTickBase"_hash ) );
			this->m_double_tap.refresh_followup_timing( next_attack, ratio, wat_value, tick_base );
		}

		if ( !g_shared.can_shoot( cmd, local.controller, false ) )
		{
#if defined( DEV )
			diag::writef( diag::level::warning, "[doubletap_followup_reject] reason=can_shoot command=%lld last=%lld",
				static_cast< long long >( cmd->command_number ), static_cast< long long >( g_shared.last_shoot_tick( ) ) );
#endif
			return false;
		}

		shared_ctx.spread = g_shared.get_spread( );
		shared_ctx.inaccuracy = g_shared.get_inaccuracy( false );

		// DT 后续命令也必须使用当前命令的射击原点，不能读取可能处于
		// 临时预测状态的场景节点眼位。
		const auto current_eye = g_shared.get_shoot_position( );
		const auto finite_eye = [ ]( const math::vector3& v )
			{
				return std::isfinite( v.x ) && std::isfinite( v.y ) && std::isfinite( v.z )
					&& ( v.x * v.x + v.y * v.y + v.z * v.z ) > 1.0f;
			};
		if ( finite_eye( current_eye ) )
		{
			shoot_eye = current_eye;
			target.hit.source_eye.position = shoot_eye;
			target.hit.source_eye.player_tick = base->client_tick( );
			target.hit.source_eye.player_frac = 0.0f;
			target.hit.source_eye.lerp_ticks_int = 0;
			target.hit.source_eye.lerp_ticks_frac = 0.0f;
			target.hit.source_eye.is_uninterpolated = true;
			target.hit.aim_angle = math::helpers::calculate_angle( shoot_eye, target.hit.position );
		}
		else
		{
			const auto fallback_eye = g_shared.get_shoot_position( );
			if ( finite_eye( fallback_eye ) )
			{
				shoot_eye = fallback_eye;
				target.hit.source_eye.position = shoot_eye;
				target.hit.source_eye.is_uninterpolated = true;
				target.hit.source_eye.player_tick = base->client_tick( );
				target.hit.source_eye.player_frac = 0.0f;
				target.hit.source_eye.lerp_ticks_int = 0;
				target.hit.source_eye.lerp_ticks_frac = 0.0f;
				target.hit.aim_angle = math::helpers::calculate_angle( shoot_eye, target.hit.position );
			}
		}

		this->m_firing_this_tick = false;
		this->m_doubletap_followup_fire = true;
		if ( cmd->csgo_user_cmd.input_history_size( ) == 0 )
		{
			const auto view = base->viewangles( );
			systems::input::input_history_params params{};
			params.view_angles = view ? math::vector3{ view->x( ), view->y( ), view->z( ) } : systems::g_input.get_view_angles( );
			params.shoot_position = shoot_eye;
			const auto record_time = cstypes::tick_fraction::from_value( target.hit.record->simulation_time / cstypes::tick_interval );
			params.render_tick = record_time.tick + 1;
			params.player_tick = target.hit.source_eye.is_uninterpolated ? base->client_tick( ) : target.hit.source_eye.player_tick + target.hit.source_eye.lerp_ticks_int;
			params.player_frac = target.hit.source_eye.is_uninterpolated ? 0.0f : target.hit.source_eye.player_frac + target.hit.source_eye.lerp_ticks_frac;
			const auto player_carry = static_cast<int>( std::floor( params.player_frac ) );
			params.player_tick += player_carry;
			params.player_frac -= static_cast<float>( player_carry );
			params.target_ent_index = target.hit.entity_index;
			params.target_head_pos = target.hit.record->bones[ cstypes::bone_ids::head ].position;
			params.target_abs_pos = target.hit.record->origin;
			params.target_abs_ang = target.hit.record->rotation;
			params.fill_cheat_check_data = true;
			if ( !systems::g_input.push_input_history( cmd, params ) )
			{
#if defined( DEV )
				diag::writef( diag::level::warning, "[doubletap_followup_reject] reason=history_push command=%lld",
					static_cast< long long >( cmd->command_number ) );
#endif
				this->m_doubletap_followup_fire = false;
				return false;
			}
		}
		this->fire_gun( cmd, target, was_forced, shoot_eye, g_shared.get_aim_punch( local.pawn ), local, false );
		this->m_doubletap_followup_fire = false;
		if ( !this->m_firing_this_tick )
		{
#if defined( DEV )
			diag::writef( diag::level::warning, "[doubletap_followup_reject] reason=fire_gun command=%lld",
				static_cast< long long >( cmd->command_number ) );
#endif
			cmd->buttons.value &= ~cstypes::command_buttons::in_attack;
			cmd->buttons.value_changed |= cstypes::command_buttons::in_attack;
			cmd->buttons.value_scroll &= ~cstypes::command_buttons::in_attack;
			cmd->csgo_user_cmd.set_attack1_start_history_index( -1 );
		}

		const auto& config = settings::g_combat.m_ragebot.get_group( shared_ctx.weapon_type );
		const auto fired = this->m_firing_this_tick;
		static_cast< void >( this->m_double_tap.on_fired( cmd, fired, config.doubletap.value, config.no_spread.value ) );
		return fired;
	}

	std::optional<rage::stop_prediction> rage::predict_stop( const aim_context& ctx, const math::vector3& current_eye, const systems::local::snapshot& local ) const
	{
		const auto& prestate = systems::g_prediction.pre( );
		auto starting_velocity = ctx.velocity;
		if ( !std::isfinite( starting_velocity.x ) || !std::isfinite( starting_velocity.y ) || !std::isfinite( starting_velocity.z ) ||
			starting_velocity.length_2d( ) > 4096.0f )
		{
			starting_velocity = prestate.networked_velocity;
		}
		const auto speed = starting_velocity.length_2d( );
		const auto early_threshold = std::max( 1.0f, ctx.accurate_threshold * 0.88f );
		const auto will_stop = ctx.on_ground && ( speed > early_threshold || ( ctx.is_scoped && speed > 1.0f ) );

		if ( !will_stop )
		{
			return std::nullopt;
		}

		auto sim_vel = starting_velocity;
		sim_vel.z = 0.0f;
		auto predicted_origin = prestate.origin;
		const auto view_offset = memory::read<math::vector3>( local.pawn + SCHEMA( "C_BaseModelEntity", "m_vecViewOffset"_hash ) );
		if ( !std::isfinite( predicted_origin.x ) || !std::isfinite( predicted_origin.y ) || !std::isfinite( predicted_origin.z ) )
			predicted_origin = current_eye - view_offset;
		auto predicted_eye = predicted_origin + view_offset;

		const auto sv_friction = CONVAR("sv_friction")->get<float>( );
		const auto sv_stopspeed = CONVAR("sv_stopspeed")->get<float>( );
		const auto surface_friction = prestate.surface_friction;

		const auto movement_services = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BasePlayerPawn", "m_pMovementServices"_hash ) );
		if ( !movement_services )
		{
			return std::nullopt;
		}

		const auto collision = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BaseEntity", "m_pCollision"_hash ) );
		auto obb_mins = collision ? memory::read<math::vector3>( collision + SCHEMA( "CCollisionProperty", "m_vecMins"_hash ) ) : math::vector3{ -16.0f, -16.0f, 0.0f };
		auto obb_maxs = collision ? memory::read<math::vector3>( collision + SCHEMA( "CCollisionProperty", "m_vecMaxs"_hash ) ) : math::vector3{ 16.0f, 16.0f, 72.0f };
		const auto valid_bounds = [ ]( const math::vector3& mins, const math::vector3& maxs )
		{
			return std::isfinite( mins.x ) && std::isfinite( mins.y ) && std::isfinite( mins.z ) &&
				std::isfinite( maxs.x ) && std::isfinite( maxs.y ) && std::isfinite( maxs.z ) &&
				maxs.x > mins.x && maxs.y > mins.y && maxs.z > mins.z;
		};
		if ( !valid_bounds( obb_mins, obb_maxs ) )
		{
			obb_mins = { -16.0f, -16.0f, 0.0f };
			obb_maxs = { 16.0f, 16.0f, 72.0f };
		}
		const auto trace_available = addresses::globals::game_trace_manager != 0;

		const auto accel = CONVAR("sv_accelerate")->get<float>( );
		const auto accel_base = g_misc.autostop( ).get_effective_accel_base(
			local.pawn,
			movement_services,
			prestate.flags,
			g_shared.ctx( ).weapon_max_speed );

		auto ticks_to_stop{ 0 };
		for ( auto i = 1; i <= 32; ++i )
		{
			const auto sim_speed = sim_vel.length_2d( );
			if ( sim_speed <= ctx.accurate_threshold )
			{
				ticks_to_stop = i - 1;
				break;
			}

			const auto control = std::fmaxf( sim_speed, sv_stopspeed );
			const auto drop = sv_friction * surface_friction * control * cstypes::tick_interval;
			auto new_speed = std::fmaxf( sim_speed - drop, 0.0f );

			const auto accel_speed = std::fminf( accel * accel_base * surface_friction * cstypes::tick_interval, new_speed );
			new_speed = std::fmaxf( new_speed - accel_speed, 0.0f );

			if ( new_speed > 0.0f )
			{
				sim_vel *= ( new_speed / sim_speed );
			}
			else
			{
				sim_vel = {};
				ticks_to_stop = i;
				break;
			}

			if ( trace_available )
			{
				const auto start = predicted_origin;
				const auto trace = systems::g_tracing.trace_hull( start, start + sim_vel * cstypes::tick_interval,
					obb_mins, obb_maxs, local.pawn, 0x1c3003, 4 );
				predicted_origin = trace.end_pos;
				if ( trace.fraction < 1.0f )
				{
					const auto into_plane = sim_vel.dot( trace.normal );
					if ( into_plane < 0.0f )
						sim_vel -= trace.normal * into_plane;
					const auto remaining = ( 1.0f - trace.fraction ) * cstypes::tick_interval;
					if ( remaining > 0.0001f && sim_vel.length_sqr( ) > 0.01f )
					{
						const auto slide = systems::g_tracing.trace_hull( predicted_origin,
							predicted_origin + sim_vel * remaining, obb_mins, obb_maxs,
							local.pawn, 0x1c3003, 4 );
						predicted_origin = slide.end_pos;
					}
				}
			}
			else
			{
				predicted_origin += sim_vel * cstypes::tick_interval;
			}
			predicted_eye = predicted_origin + view_offset;
			if ( sim_vel.length_2d( ) <= ctx.accurate_threshold )
			{
				ticks_to_stop = i;
				break;
			}
		}

		if ( ticks_to_stop == 0 && sim_vel.length_2d( ) > ctx.accurate_threshold )
		{
			ticks_to_stop = 32;
		}

		return stop_prediction
		{
			.eye = predicted_eye,
			.inaccuracy = g_shared.get_inaccuracy_at_velocity( local.pawn, sim_vel ),
			.ticks_to_stop = ticks_to_stop,
			.time_to_stop = static_cast< float >( ticks_to_stop ) * cstypes::tick_interval
		};
	}

	std::vector<rage::candidate> rage::gather_candidates( const systems::local::snapshot& local, float max_distance_sq ) const
	{
		// 收集有效敌方记录；普通 Rage 只使用当前网络姿态或完整外推姿态，不混用过期记录。
		const auto& shared_ctx = g_shared.ctx( );
		const auto backtrack_enabled = settings::g_combat.m_lagcomp.backtrack.value;
		const auto extrapolation_enabled = settings::g_combat.m_lagcomp.extrapolation.value
			&& !backtrack_enabled;
		const auto players = systems::g_entities.get_by_type( systems::entities::type::player );
		const auto local_origin = systems::g_prediction.pre( ).origin;
		constexpr auto max_extrapolated_targets{ 3 };
		std::unordered_set<std::uintptr_t> extrapolation_targets;
		extrapolation_targets.reserve( max_extrapolated_targets );
		std::unordered_map<std::uintptr_t, std::vector<shared::lagcomp::record*>> cached_records;
		cached_records.reserve( players.size( ) );

		std::vector<candidate> out;
		out.reserve( players.size( ) );

		{
			std::lock_guard lock( const_cast<rage*>( this )->m_extrapolated_mtx );
			const_cast<rage*>( this )->m_extrapolated_records.clear( );
			const_cast<rage*>( this )->m_extrapolated_records.reserve( max_extrapolated_targets );
		}
		{
			std::lock_guard lock( const_cast<rage*>( this )->m_live_records_mtx );
			const_cast<rage*>( this )->m_live_records.clear( );
			const_cast<rage*>( this )->m_live_records.reserve( players.size( ) );
		}

		if ( extrapolation_enabled )
		{
			std::vector<std::pair<float, std::uintptr_t>> priority;
			priority.reserve( players.size( ) );

			for ( const auto& p : players )
			{
				if ( !p.ptr || p.ptr == local.controller ||
					!memory::read<bool>( p.ptr + SCHEMA( "CCSPlayerController", "m_bPawnIsAlive"_hash ) ) )
				{
					continue;
				}

				const auto pawn_handle = memory::read<std::uint32_t>( p.ptr + SCHEMA( "CBasePlayerController", "m_hPawn"_hash ) );
				const auto pawn = systems::g_entities.lookup( pawn_handle );
				if ( !pawn || pawn == local.pawn )
				{
					continue;
				}

				const auto team = memory::read<int>( pawn + SCHEMA( "C_BaseEntity", "m_iTeamNum"_hash ) );
				if ( !local.is_this_other_team( team ) ||
					memory::read<int>( pawn + SCHEMA( "C_BaseEntity", "m_iHealth"_hash ) ) <= 0 ||
					memory::read<bool>( pawn + SCHEMA( "C_CSPlayerPawn", "m_bGunGameImmunity"_hash ) ) )
				{
					continue;
				}

				auto records = g_shared.lc( ).get_valid_records( pawn );
				cached_records[ pawn ] = records;
				if ( records.empty( ) || !records.front( ) )
				{
					continue;
				}

				const auto delta = records.front( )->origin - local_origin;
				const auto distance_sq = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
				if ( ( shared_ctx.range > 1.0f && distance_sq > ( shared_ctx.range + 128.0f ) * ( shared_ctx.range + 128.0f ) ) ||
					( max_distance_sq > 0.0f && distance_sq > max_distance_sq ) )
				{
					continue;
				}

				const auto speed = records.front( )->velocity.length_2d( );
				const auto vertical_speed = std::fabsf( records.front( )->velocity.z );
				auto has_recent_motion = speed > 16.0f || vertical_speed > 16.0f;
				if ( records.size( ) > 1 && records[ 1 ] )
				{
					has_recent_motion = has_recent_motion ||
					( records.front( )->origin - records[ 1 ]->origin ).length_sqr( ) > 1.0f;
				}

				if ( has_recent_motion )
				{
					priority.emplace_back( distance_sq, pawn );
				}
			}

			std::sort( priority.begin( ), priority.end( ), [ ]( const auto& lhs, const auto& rhs )
				{
					return lhs.first < rhs.first;
				} );

			const auto budget = std::min<std::size_t>( max_extrapolated_targets, priority.size( ) );
			for ( auto i = std::size_t{}; i < budget; ++i )
			{
				extrapolation_targets.insert( priority[ i ].second );
			}
		}

		for ( const auto& p : players )
		{
			if ( !p.ptr || p.ptr == local.controller )
			{
				continue;
			}

			if ( !memory::read<bool>( p.ptr + SCHEMA( "CCSPlayerController", "m_bPawnIsAlive"_hash ) ) )
			{
				continue;
			}

			const auto pawn_handle = memory::read<std::uint32_t>( p.ptr + SCHEMA( "CBasePlayerController", "m_hPawn"_hash ) );
			const auto pawn = systems::g_entities.lookup( pawn_handle );

			if ( !pawn || pawn == local.pawn )
			{
				continue;
			}

			const auto team = memory::read<int>( pawn + SCHEMA( "C_BaseEntity", "m_iTeamNum"_hash ) );
			if ( !local.is_this_other_team( team ) )
			{
				continue;
			}

			const auto health = memory::read<int>( pawn + SCHEMA( "C_BaseEntity", "m_iHealth"_hash ) );
			if ( health <= 0 )
			{
				continue;
			}

			if ( memory::read<bool>( pawn + SCHEMA( "C_CSPlayerPawn", "m_bGunGameImmunity"_hash ) ) )
			{
				continue;
			}

			std::vector<shared::lagcomp::record*> records;
			if ( const auto cached = cached_records.find( pawn ); cached != cached_records.end( ) )
			{
				records = cached->second;
			}
			else
			{
				records = g_shared.lc( ).get_valid_records( pawn );
			}
			// 记录队列可能在玩家死亡后的一个帧内仍返回旧骨骼；
			// 在进入候选集前再次校验生命状态，禁止尸体记录参与扫描。
			std::erase_if( records, [ ]( auto* record )
			{
				return !record || !record->valid;
			} );
			const auto backtrack = settings::g_combat.m_lagcomp.backtrack.value;

			if ( !extrapolation_enabled )
			{
				std::erase_if( records, [ ]( const auto* record )
					{
						return !record || record->extrapolated;
					} );
			}

			if ( records.empty( ) )
			{
				shared::lagcomp::record live_record{};
				if ( live_record.setup( pawn ) && live_record.valid )
				{
					live_record.live_snapshot = true;
					std::lock_guard lock( const_cast<rage*>( this )->m_live_records_mtx );
					const_cast<rage*>( this )->m_live_records.push_back( std::move( live_record ) );
					records.push_back( &const_cast<rage*>( this )->m_live_records.back( ) );
				}
			}
			if ( records.empty( ) )
			{
				continue;
			}

			const auto& newest_record = *records.front( );
			const auto newest_delta = newest_record.origin - local_origin;
			const auto newest_distance_sq = newest_delta.x * newest_delta.x + newest_delta.y * newest_delta.y + newest_delta.z * newest_delta.z;
			if ( shared_ctx.range > 1.0f && newest_distance_sq > ( shared_ctx.range + 128.0f ) * ( shared_ctx.range + 128.0f ) )
			{
				continue;
			}

			if ( max_distance_sq > 0.0f )
			{
				const auto& origin = local_origin;
				const auto delta_front = records.front( )->origin - origin;
				auto closest_sq = delta_front.x * delta_front.x + delta_front.y * delta_front.y + delta_front.z * delta_front.z;

				if ( records.size( ) > 1 )
				{
					const auto delta_back = records.back( )->origin - origin;
					const auto back_sq = delta_back.x * delta_back.x + delta_back.y * delta_back.y + delta_back.z * delta_back.z;
					closest_sq = std::min( closest_sq, back_sq );
				}

				if ( closest_sq > max_distance_sq )
				{
					continue;
				}
			}

			const auto newest_speed = newest_record.velocity.length_2d( );
			auto has_recent_motion = newest_speed > 16.0f || std::fabsf( newest_record.velocity.z ) > 16.0f;
			if ( records.size( ) > 1 && records[ 1 ] )
			{
				const auto sample_delta = newest_record.origin - records[ 1 ]->origin;
				has_recent_motion = has_recent_motion || sample_delta.length_sqr( ) > 1.0f;
			}

			if ( has_recent_motion && extrapolation_targets.contains( pawn ) )
			{
				auto extrap = g_shared.lc( ).extrapolate( pawn );
				if ( extrap.has_value( ) )
				{
					std::lock_guard lock( const_cast<rage*>( this )->m_extrapolated_mtx );
					const_cast<rage*>( this )->m_extrapolated_records.push_back( std::move( *extrap ) );
					records.insert( records.begin( ), &const_cast<rage*>( this )->m_extrapolated_records.back( ) );
				}
			}

			candidate c{};
			c.pawn = pawn;
			c.entity_index = static_cast< int >( pawn_handle & 0x7fffu );
			c.health = health;
			c.armor = memory::read<int>( pawn + SCHEMA( "C_CSPlayerPawn", "m_ArmorValue"_hash ) );

			const auto min_extrapolate_ticks = std::clamp(
				settings::g_combat.m_lagcomp.min_extrapolate_ticks.value,
				0,
				18 );
			const auto live_target_velocity = memory::read<math::vector3>( pawn + SCHEMA( "C_BaseEntity", "m_vecVelocity"_hash ) );
			const auto target_is_stationary = live_target_velocity.length_2d( ) <= 1.0f;
			const auto pick_record_indices = [ &records, backtrack, min_extrapolate_ticks, target_is_stationary ]( std::array<int, k_max_scan_records>& out_indices ) -> int
				{
					const auto count = records.size( );
					if ( count == 0 )
					{
						return 0;
					}

					auto picked{ 0 };
					const auto add_index = [ & ]( int idx )
						{
							if ( picked >= k_max_scan_records )
							{
								return;
							}

							if ( idx < 0 || idx >= static_cast< int >( records.size( ) )
								|| !records[ static_cast< std::size_t >( idx ) ]
								|| ( !target_is_stationary
									&& records[ static_cast< std::size_t >( idx ) ]->extrapolated_ticks < min_extrapolate_ticks ) )
							{
								return;
							}

							for ( auto i = 0; i < picked; ++i )
							{
								if ( out_indices[ i ] == idx )
								{
									return;
								}
							}

							out_indices[ picked++ ] = idx;
						};

					for ( auto i = static_cast< int >( count ) - 1; i >= 0; --i )
					{
						if ( records[ static_cast< std::size_t >( i ) ] && records[ static_cast< std::size_t >( i ) ]->extrapolated )
						{
							add_index( i );
							break;
						}
					}

					add_index( 0 );
					if ( target_is_stationary && !settings::g_combat.m_lagcomp.extrapolation.value && !backtrack )
					{
						return picked;
					}
					if ( count > 0 && records[ 0 ] && records[ 0 ]->extrapolated && !backtrack )
					{
						if ( count > 1 && records[ 1 ] )
						{
							add_index( 1 );
						}
						return picked;
					}
					if ( !backtrack )
					{
						if ( count > 1 && records[ 0 ] && records[ 0 ]->extrapolated && records[ 1 ] )
						{
							const auto origin_delta = ( records[ 0 ]->origin - records[ 1 ]->origin ).length( );
							if ( records[ 0 ]->tick != records[ 1 ]->tick || origin_delta > 1.0f )
							{
								add_index( 1 );
							}
						}
						return picked;
					}

					const auto current_tick = g_shared.ctx( ).current_tick;
					const auto max_backtrack = std::clamp(
						settings::g_combat.m_lagcomp.max_backtrack_ticks.value,
						1,
						shared::lagcomp::k_max_history_ticks );
					const auto find_closest_age = [ & ]( int wanted_age )
					{
						auto best_index{ -1 };
						auto best_distance{ std::numeric_limits< int >::max( ) };
						for ( auto i = 0; i < static_cast< int >( count ); ++i )
						{
							const auto* record = records[ static_cast< std::size_t >( i ) ];
							if ( !record || record->extrapolated )
							{
								continue;
							}

							const auto age = current_tick > 0 ? current_tick - record->tick : 0;
							if ( age < 0 || age > max_backtrack )
							{
								continue;
							}

							const auto distance = std::abs( age - wanted_age );
							if ( distance < best_distance )
							{
								best_distance = distance;
								best_index = i;
							}
						}
						return best_index;
					};

					add_index( find_closest_age( 0 ) );
					if ( picked < k_max_scan_records )
						{
						add_index( find_closest_age( max_backtrack / 2 ) );
					}
					if ( picked < k_max_scan_records )
					{
						add_index( find_closest_age( max_backtrack ) );
					}

					return picked;
				};

			std::array<int, k_max_scan_records> record_indices{};
			const auto picked_count = pick_record_indices( record_indices );

			for ( auto i = 0; i < picked_count; ++i )
			{
				c.records[ i ] = records[ static_cast< std::size_t >( record_indices[ i ] ) ];
			}

			c.record_count = picked_count;

			if ( shared_ctx.weapon_type >= cstypes::weapon_type::pistol && shared_ctx.weapon_type <= cstypes::weapon_type::lmg )
			{
				const auto& config = settings::g_combat.m_ragebot.get_group( shared_ctx.weapon_type );
				c.min_damage = this->get_min_damage( config, health, config.min_damage_override.value );
			}

			out.push_back( c );
		}

		return out;
	}

	void rage::run_gun( systems::input::usercmd* cmd, const aim_context& ctx, const systems::local::snapshot& local, bool allow_fire )
	{
		if ( !settings::g_combat.m_ragebot.enabled )
		{
			return;
		}

		auto& shared_ctx = g_shared.ctx( );
		const auto& config = settings::g_combat.m_ragebot.get_group( shared_ctx.weapon_type );
		const auto backtrack_enabled = settings::g_combat.m_lagcomp.backtrack.value;
		const auto log_gate = [ & ]( const char* reason, std::size_t candidate_count = 0,
			std::size_t hit_count = 0, bool best_valid = false )
		{
#if defined( DEV )
			static const char* last_reason = nullptr;
			static int last_tick = -1;
			const auto tick = shared_ctx.current_tick;
			if ( last_reason == reason && last_tick >= 0 && tick - last_tick < 8 )
			{
				return;
			}
			last_reason = reason;
			last_tick = tick;
			const auto tick_base = local.controller
				? memory::read<int>( local.controller + SCHEMA( "CBasePlayerController", "m_nTickBase"_hash ) )
				: -1;
			const auto next_primary = shared_ctx.weapon
				? memory::read<int>( shared_ctx.weapon + SCHEMA( "C_BasePlayerWeapon", "m_nNextPrimaryAttackTick"_hash ) )
				: -1;
			diag::writef( diag::level::info,
				"[rage_gate] reason=%s tick=%d tickbase=%d next_primary=%d allow_fire=%u candidates=%zu hits=%zu best=%u inaccuracy=%.6f spread=%.6f tickrate=%d interval=%.6f",
				reason, tick, tick_base, next_primary, allow_fire ? 1u : 0u,
				candidate_count, hit_count, best_valid ? 1u : 0u,
				shared_ctx.inaccuracy, shared_ctx.spread, shared_ctx.tickrate, shared_ctx.tick_interval );
#else
			static_cast< void >( reason );
			static_cast< void >( candidate_count );
			static_cast< void >( hit_count );
			static_cast< void >( best_valid );
#endif
		};
		if ( config.no_spread.value && !g_shared.tickrate_normal( ) )
		{
			// Seed reconstruction is tied to the 64 tick interval used by the
			// command/history path.  Do not submit a shot while GlobalVars is
			// stale, invalid, or reporting another tickrate.
			this->m_firing_this_tick = false;
			log_gate( "nospread_tickrate_invalid" );
			return;
		}
		if ( config.no_spread.value && shared_ctx.item_def_idx == cstypes::item_definition_index::weapon_r8_revolver )
		{
			shared_ctx.weapon_mode = 1;
		}

		const auto dt_enabled = config.doubletap.value
			&& shared_ctx.item_def_idx != cstypes::item_definition_index::weapon_r8_revolver;
		const auto finish_doubletap = [ & ]( bool fire )
		{
			return this->process_doubletap( cmd, local, fire );
		};
		const auto can_commit_dt_shot = [ & ]( doubletap_action action )
		{
			// 开启 DT 时，只有已充满并成功配对的命令才允许进入开火路径。
			// charging 状态保持攻击键抑制，避免普通单发消耗掉 DT 的 Ready 节奏。
			return !dt_enabled || action == doubletap_action::command_pair;
		};

		// Keep both local states. No-spread uses the command state produced by
		// prediction; other backtrack paths retain their pre-simulation behavior.
		const auto latest_local_eye = g_shared.get_shoot_position( );
		auto predicted_command_eye = latest_local_eye;
		auto predicted_command_accuracy = shared::accuracy_sample{};
		auto predicted_command_accuracy_valid = false;
		auto predicted_command_aim_punch = g_shared.get_aim_punch( local.pawn );
		const auto prediction_ok = systems::g_prediction.simulate( cmd, local, [ & ]
		{
			predicted_command_eye = g_shared.get_shoot_position( );
			predicted_command_aim_punch = g_shared.get_aim_punch( local.pawn );
			predicted_command_accuracy = g_shared.get_accuracy_sample( true );
			predicted_command_accuracy_valid = std::isfinite( predicted_command_accuracy.inaccuracy )
				&& std::isfinite( predicted_command_accuracy.spread );
			if ( predicted_command_accuracy_valid )
			{
				shared_ctx.inaccuracy = predicted_command_accuracy.inaccuracy;
				shared_ctx.spread = predicted_command_accuracy.spread;
			}
			shared_ctx.recoil_index = memory::read<float>(
				shared_ctx.weapon + SCHEMA( "C_CSWeaponBase", "m_flRecoilIndex"_hash ) );
		} );
		const auto finite_latest_eye = std::isfinite( latest_local_eye.x )
			&& std::isfinite( latest_local_eye.y )
			&& std::isfinite( latest_local_eye.z )
			&& latest_local_eye.length_sqr( ) >= 1.0f;
		const auto finite_predicted_eye = std::isfinite( predicted_command_eye.x )
			&& std::isfinite( predicted_command_eye.y )
			&& std::isfinite( predicted_command_eye.z )
			&& predicted_command_eye.length_sqr( ) >= 1.0f;
		const auto predicted_command_aim_punch_valid = std::isfinite( predicted_command_aim_punch.x )
			&& std::isfinite( predicted_command_aim_punch.y )
			&& std::isfinite( predicted_command_aim_punch.z );
		const auto firing_eye_valid = config.no_spread.value
			? finite_predicted_eye
			: ( backtrack_enabled ? finite_latest_eye : finite_predicted_eye );
		if ( config.no_spread.value &&
			( !prediction_ok || !firing_eye_valid || !predicted_command_accuracy_valid ||
				!predicted_command_aim_punch_valid ) )
		{
			log_gate( "prediction_unavailable" );
			this->m_firing_this_tick = false;
			return;
		}
		if ( !finite_predicted_eye )
		{
			predicted_command_eye = g_shared.get_shoot_position( );
		}

		auto candidates = this->gather_candidates( local );

		{
			std::lock_guard lock( m_debug_mtx );
			m_debug_points.clear( );
		}

		if ( candidates.empty( ) )
		{
			log_gate( "no_candidates" );
			this->m_target_switch_pawn = 0;
			this->m_pending_target_switch_pawn = 0;
			this->m_pending_target_switch_tick = -1;
			finish_doubletap( false );
			return;
		}

		auto eye_candidates = g_shared.sh( ).get_candidates( );
		const auto finite_eye = [ ]( const math::vector3& value )
		{
			return std::isfinite( value.x ) && std::isfinite( value.y ) &&
				std::isfinite( value.z ) && value.length_sqr( ) >= 1.0f;
		};
		// 使用 build_context 在预测前捕获的当前命令射击原点。
		// 这里不能重新读取场景节点眼位，否则下一 tick 的临时预测状态
		// 会被误当成当前开火位置，导致扫描和真实弹道起点错位。
		const auto current_command_eye = config.no_spread.value
			? predicted_command_eye
			: ( backtrack_enabled ? latest_local_eye : predicted_command_eye );
		// Rewind only the target; no-spread keeps the local ray on the predicted
		// command state that the server will process.
		if ( !finite_eye( current_command_eye ) )
		{
			log_gate( "current_eye_invalid", candidates.size( ) );
			finish_doubletap( false );
			return;
		}
		eye_candidates.entries[ 0 ].position = current_command_eye;
		eye_candidates.entries[ 0 ].is_uninterpolated = true;
		eye_candidates.entries[ 0 ].player_tick = g_shared.sh( ).client_tick( );
		eye_candidates.entries[ 0 ].player_frac = g_shared.sh( ).client_tick_frac( );
		eye_candidates.entries[ 0 ].lerp_ticks_int = 0;
		eye_candidates.entries[ 0 ].lerp_ticks_frac = 0.0f;
		eye_candidates.count = 1;

		// early 模式先模拟当前反向制动后的停点，再用停点眼位和停点
		// 精度扫描。没有命中时仍回退到实时眼位，避免提前量导致漏目标。
		std::optional<stop_prediction> early_stop{};
		if ( config.autostop.values[ 0 ] && ctx.on_ground && !config.no_spread.value && !backtrack_enabled )
		{
			early_stop = this->predict_stop( ctx, eye_candidates.entries[ 0 ].position, local );
		}

		const auto scan_from_eye_candidates = [ & ]( const math::vector3& eye_offset, float inaccuracy )
		{
			std::vector<scan_hit> hits_out;
			const auto record_limit = config.no_spread.value && !backtrack_enabled ? 1 : 3;
			const auto scan_pass = [ & ]( bool use_stop_eye )
			{
				for ( auto i = 0; i < eye_candidates.count; ++i )
				{
					const auto use_stop = use_stop_eye && i == 0 && eye_offset.length_sqr( ) <= 0.001f;
					const auto eye = ( use_stop ? early_stop->eye : eye_candidates.entries[ i ].position ) + eye_offset;
					const auto scan_accuracy = use_stop ? early_stop->inaccuracy : inaccuracy;
					auto hits = this->scan_players( eye, scan_accuracy, ctx, candidates, local, 0, record_limit );

					for ( auto& hit : hits )
					{
						auto source_eye = eye_candidates.entries[ i ];
						if ( hit.source_eye.position.length_sqr( ) <= 0.0001f )
							hit.source_eye.position = eye;
						source_eye.position = hit.source_eye.position;
						hit.source_eye = source_eye;
						hits_out.push_back( std::move( hit ) );
					}

					if ( !hits_out.empty( ) )
						break;
				}
			};

			if ( early_stop && eye_offset.length_sqr( ) <= 0.001f )
				scan_pass( true );
			if ( hits_out.empty( ) )
				scan_pass( false );

			// 拉出掩体的首个命令可能还保留网络原点，而场景节点已经
			// 完成了本命令的实际位移。首轮无命中时，用当前眼位补扫，
			// 但限制在一个 tick 的位移范围内，避免引入未来姿态。
			if ( false )
			{
				const auto live_eye = g_shared.get_eye_position( local.pawn );
				const auto delta = live_eye - eye_candidates.entries[ 0 ].position;
				const auto velocity = systems::g_prediction.pre( ).velocity;
				const auto max_delta = std::clamp( velocity.length( ) * cstypes::tick_interval * 1.5f, 2.0f, 8.0f );
				if ( finite_eye( live_eye ) && delta.length( ) <= max_delta )
				{
					auto live_hits = this->scan_players( live_eye, inaccuracy, ctx, candidates, local, 0, record_limit );
					for ( auto& hit : live_hits )
					{
						hit.source_eye.position = live_eye;
						hit.source_eye.is_uninterpolated = true;
						hit.source_eye.player_tick = g_shared.sh( ).client_tick( );
						hit.source_eye.player_frac = 0.0f;
						hit.source_eye.lerp_ticks_int = 0;
						hit.source_eye.lerp_ticks_frac = 0.0f;
						hits_out.push_back( std::move( hit ) );
					}
				}
			}

			return hits_out;
		};

		const auto select_shot_target = [ & ]( const std::vector<scan_hit>& hits, float inaccuracy )
		{
			if ( config.no_spread.value )
			{
				return this->select_best( ctx, hits, inaccuracy );
			}

			if ( !this->air_stop_enabled( ) || ctx.on_ground )
			{
				const auto velocity = systems::g_prediction.pre( ).velocity;
				const auto moving = std::isfinite( velocity.x ) && std::isfinite( velocity.y ) &&
					std::isfinite( velocity.z ) && velocity.length_2d( ) > 20.0f;
				if ( moving )
				{
					std::vector<scan_hit> direct_hits;
					direct_hits.reserve( hits.size( ) );
					for ( const auto& hit : hits )
					{
						if ( !hit.penetrated )
							direct_hits.push_back( hit );
					}
					if ( !direct_hits.empty( ) )
						return this->select_best( ctx, direct_hits, inaccuracy );
				}

				return this->select_best( ctx, hits, inaccuracy );
			}

			std::vector<scan_hit> direct_hits;
			direct_hits.reserve( hits.size( ) );
			for ( const auto& hit : hits )
			{
				if ( !hit.penetrated )
				{
					direct_hits.push_back( hit );
				}
			}

			return this->select_best( ctx, direct_hits, inaccuracy );
		};

		if ( config.no_spread.value )
		{
			this->m_should_stop = false;
			this->m_air_stop_requested = false;
			this->m_air_stop_settle_ticks = 0;

			// Keep the exact command state captured before prediction was restored.
			shared_ctx.inaccuracy = predicted_command_accuracy.inaccuracy;
			shared_ctx.spread = predicted_command_accuracy.spread;
			if ( !std::isfinite( shared_ctx.inaccuracy ) || !std::isfinite( shared_ctx.spread ) )
			{
				log_gate( "invalid_accuracy", candidates.size( ) );
				finish_doubletap( false );
				return;
			}

			std::vector<scan_hit> legacy_hits;
			for ( auto i = 0; i < eye_candidates.count; ++i )
			{
				auto hits = this->scan_players(
					eye_candidates.entries[ i ].position,
					shared_ctx.inaccuracy,
					ctx,
					candidates,
					local,
					0,
					config.no_spread.value && !backtrack_enabled ? 1 : k_max_scan_records );
				for ( auto& hit : hits )
				{
					hit.source_eye = eye_candidates.entries[ i ];
					legacy_hits.push_back( std::move( hit ) );
				}
			}

			if ( legacy_hits.empty( ) )
			{
				log_gate( "no_scan_hits", candidates.size( ) );
				finish_doubletap( false );
				return;
			}

			auto legacy_best = this->select_best( ctx, legacy_hits, shared_ctx.inaccuracy );
			if ( !legacy_best.valid )
			{
				log_gate( "best_invalid", candidates.size( ), legacy_hits.size( ) );
				finish_doubletap( false );
				return;
			}

			legacy_best = this->apply_target_switch_delay( legacy_best, ctx, legacy_hits, shared_ctx.inaccuracy );
			this->update_state_target( legacy_best );
			if ( !legacy_best.valid || !allow_fire )
			{
				if ( !allow_fire )
					log_gate( "fire_not_allowed", candidates.size( ), legacy_hits.size( ), legacy_best.valid );
				return;
			}

			this->fire_gun( cmd, legacy_best, false, legacy_best.hit.source_eye.position, predicted_command_aim_punch, local, false );
			if ( !this->m_firing_this_tick )
			{
				cmd->buttons.value &= ~cstypes::command_buttons::in_attack;
				cmd->buttons.value_changed |= cstypes::command_buttons::in_attack;
				cmd->buttons.value_scroll &= ~cstypes::command_buttons::in_attack;
				cmd->csgo_user_cmd.set_attack1_start_history_index( -1 );
			}
			return;
		}

		const auto primary_eye = eye_candidates.entries[ 0 ].position;
		const auto& prestate = systems::g_prediction.pre( );
		const auto duckpeek_enabled = settings::g_combat.m_duckpeek.enabled.value && ctx.on_ground
			&& !backtrack_enabled;

		auto current_hits = scan_from_eye_candidates( {}, ctx.predicted_inaccuracy );
		std::vector<scan_hit> rise_hits;
		if ( duckpeek_enabled )
		{
			for ( const auto rise_height : {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f, 17.0f, 18.0f, 19.0f, 20.0f, 21.0f, 22.0f } )
			{
				auto points = scan_from_eye_candidates( { 0.0f, 0.0f, rise_height }, ctx.predicted_inaccuracy );
				rise_hits.insert( rise_hits.end( ), points.begin( ), points.end( ) );
				current_hits.insert(
					current_hits.end( ),
					std::make_move_iterator( points.begin( ) ),
					std::make_move_iterator( points.end( ) ) );
			}
		}
		this->update_hittable_regions( current_hits );
		auto best = select_shot_target( current_hits, ctx.predicted_inaccuracy );
		if ( duckpeek_enabled && !rise_hits.empty( ) )
		{
			const auto rise_best = select_shot_target( rise_hits, ctx.predicted_inaccuracy );
			if ( rise_best.valid && ( !best.valid || rise_best.score >= best.score ) )
			{
				best = rise_best;
			}
		}
		best = this->apply_target_switch_delay( best, ctx, current_hits, ctx.predicted_inaccuracy );
		this->update_state_target( best );

		const auto needed_hc = config.hitchance_override.value ? static_cast< float >( config.hitchance_override_value ) / 100.0f : static_cast< float >( config.hitchance ) / 100.0f;
		const auto duckpeek_active = duckpeek_enabled;
		auto duck_amount = 1.0f;
		auto ducking_transition = false;
		if ( const auto movement_services = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BasePlayerPawn", "m_pMovementServices"_hash ) ) )
		{
			duck_amount = std::clamp(
				memory::read<float>( movement_services + SCHEMA( "CCSPlayer_MovementServices", "m_flDuckAmount"_hash ) ),
				0.0f, 1.0f );
			ducking_transition = memory::read<bool>( movement_services + SCHEMA( "CCSPlayer_MovementServices", "m_bDucking"_hash ) );
		}
		const auto is_ducked = ( prestate.flags & cstypes::entity_flags::ducking ) != 0
			&& duck_amount > 0.94f
			&& !ducking_transition;

		const auto standing_inaccuracy = duckpeek_active ? this->get_standing_inaccuracy( local, ctx ) : ctx.predicted_inaccuracy;
		const auto standing_hc = best.valid
			? ( duckpeek_active ? this->evaluate_hitchance( best.hit, ctx, standing_inaccuracy ) : best.hitchance )
			: 0.0f;

		const auto accurate = best.valid && standing_hc >= needed_hc;
		const auto max_acc = g_shared.is_max_accuracy( standing_inaccuracy );
		const auto force = best.valid && ( ctx.on_ground ? ( config.force_shot.value && max_acc ) : ( config.force_shot_air.value && max_acc ) );
		const auto shot_viable = accurate || force;

		// 只用急停预测决定移动输入；最终命中与开火仍以当前眼位验证。
		if ( !best.valid )
		{
			log_gate( "best_invalid", candidates.size( ), current_hits.size( ) );
			finish_doubletap( false );
			return;
		}

		// Once a target is selected, autostop owns horizontal movement for the
		// entire attackable window. Speed is only an input-strength parameter;
		// it must never clear the stop request by itself.
		const auto can_attack = allow_fire && local.controller
			&& g_shared.can_shoot( cmd, local.controller, false );
		this->m_should_stop = ctx.on_ground && can_attack;

		if ( !this->prepare_air_stop_for_shot( ctx ) )
		{
			log_gate( "air_stop_pending", candidates.size( ), current_hits.size( ), best.valid );
			return;
		}

		const auto should_auto_scope =
			allow_fire
			&& shared_ctx.weapon_type == cstypes::weapon_type::sniper
			&& !ctx.is_scoped
			&& !config.no_spread.value
			&& settings::g_combat.m_autos.scope.value;

		if ( should_auto_scope )
		{
			finish_doubletap( false );
			cmd->buttons.value |= cstypes::command_buttons::in_second_attack;
			cmd->buttons.value_changed |= cstypes::command_buttons::in_second_attack;
			cmd->buttons.value_scroll |= cstypes::command_buttons::in_second_attack;

			const auto history_index = cmd->csgo_user_cmd.input_history_size( ) - 1;
			if ( history_index >= 0 )
			{
				cmd->csgo_user_cmd.set_attack2_start_history_index( history_index );
			}

			// 开镜命令本身不代表这一帧需要持续急停。只有武器的精准窗口
			// 确实要求减速时才保留 stop 请求，下一帧会重新评估。
			this->m_should_stop = ctx.on_ground && can_attack;
			return;
		}

		if ( duckpeek_active && allow_fire )
		{
			if ( this->m_duckpeek_reduck )
			{
				this->m_release_duck_for_shot = false;
			}
			else if ( shot_viable )
			{
				this->m_release_duck_for_shot = true;
			}
			else if ( !this->m_duckpeek_reduck )
			{
				this->m_release_duck_for_shot = false;
			}
		}

		const auto pre_speed = prestate.networked_velocity.length_2d( );
		const auto predicted_speed = ctx.velocity.length_2d( );
		const auto can_shoot_now = pre_speed <= ctx.accurate_threshold && predicted_speed <= ctx.accurate_threshold;
		// 有效目标存在且当前还不能稳定开火时急停；达到完整精准速度窗口后释放。
		// between-shot 选项继续覆盖武器冷却间隔。
		auto ready_to_fire = shot_viable && can_shoot_now;
		if ( duckpeek_active )
		{
			if ( is_ducked || this->m_duckpeek_reduck )
			{
				ready_to_fire = false;
			}
			else
			{
				ready_to_fire = ready_to_fire && this->m_release_duck_for_shot;
			}
		}

		if ( ready_to_fire && allow_fire )
		{
			const auto dt_action = finish_doubletap( true );
			if ( !can_commit_dt_shot( dt_action ) )
			{
				log_gate( "doubletap_pending", candidates.size( ), current_hits.size( ), best.valid );
				return;
			}
			this->m_double_tap.update_aim( best.hit.aim_angle - g_shared.get_aim_punch( local.pawn ), shared_ctx.current_tick );
			const auto was_forced = !accurate && force;
			this->fire_gun( cmd, best, was_forced, best.hit.source_eye.position, predicted_command_aim_punch, local, false );
			if ( !this->m_firing_this_tick )
			{
				log_gate( "fire_rejected", candidates.size( ), current_hits.size( ), best.valid );
				cmd->buttons.value &= ~cstypes::command_buttons::in_attack;
				cmd->buttons.value_changed |= cstypes::command_buttons::in_attack;
				cmd->buttons.value_scroll &= ~cstypes::command_buttons::in_attack;
				cmd->csgo_user_cmd.set_attack1_start_history_index( -1 );
			}
			if ( dt_action == doubletap_action::command_pair && !this->m_firing_this_tick )
			{
				this->cancel_doubletap_charge( );
			}
			// between shot 只在本次命令完成开火后保留，下一命令会按目标、
			// 冷却和当前速度重新计算，避免状态永久锁停。
			static_cast< void >( dt_action );

			if ( duckpeek_active )
			{
				this->m_duckpeek_reduck = true;
				this->m_release_duck_for_shot = false;
			}
		}
		else
		{
			log_gate( allow_fire ? "hitchance_pending" : "fire_not_allowed", candidates.size( ), current_hits.size( ), best.valid );
			finish_doubletap( false );
		}
	}

	void rage::run_taser( systems::input::usercmd* cmd, const aim_context& ctx, const systems::local::snapshot& local )
	{
		if ( !settings::g_combat.m_zeusbot.enabled )
		{
			return;
		}

		auto candidates = this->gather_candidates( local );
		if ( candidates.empty( ) )
		{
			this->m_target_switch_pawn = 0;
			this->m_pending_target_switch_pawn = 0;
			this->m_pending_target_switch_tick = -1;
			return;
		}

		auto eye_candidates = g_shared.sh( ).get_candidates( );
		if ( eye_candidates.count == 0 )
		{
			eye_candidates.entries[ 0 ].position = g_shared.get_shoot_position( );
			eye_candidates.entries[ 0 ].is_uninterpolated = true;
			eye_candidates.count = 1;
		}

		std::vector<scan_hit> all_hits;

		for ( auto i = 0; i < eye_candidates.count; ++i )
		{
			auto hits = this->scan_taser( eye_candidates.entries[ i ].position, ctx, candidates, local );

			for ( auto& h : hits )
			{
				h.source_eye = eye_candidates.entries[ i ];
				all_hits.push_back( std::move( h ) );
			}
		}

		if ( all_hits.empty( ) )
		{
			return;
		}

		target best{};

		for ( const auto& h : all_hits )
		{
			if ( !best.valid || h.score > best.score )
			{
				best.hit = h;
				best.hitchance = 1.0f;
				best.score = h.score;
				best.valid = true;
			}
		}

		best = this->apply_target_switch_delay( best, ctx, all_hits, 0.0f );
		if ( best.valid )
		{
			this->update_state_target( best );
			this->m_zeus_fired = true;
			this->fire_melee( cmd, best, local );
		}
	}

	void rage::run_knife( systems::input::usercmd* cmd, const aim_context& ctx, const systems::local::snapshot& local )
	{
		if ( !settings::g_combat.m_knifebot.enabled )
		{
			return;
		}

		const auto info = this->get_knife_info( local );
		if ( !info.can_slash && !info.can_stab )
		{
			return;
		}

		constexpr auto max_knife_dist_sq = 150.0f * 150.0f;
		auto candidates = this->gather_candidates( local, max_knife_dist_sq );
		if ( candidates.empty( ) )
		{
			this->m_target_switch_pawn = 0;
			this->m_pending_target_switch_pawn = 0;
			this->m_pending_target_switch_tick = -1;
			return;
		}

		auto eye_candidates = g_shared.sh( ).get_candidates( );
		if ( eye_candidates.count == 0 )
		{
			eye_candidates.entries[ 0 ].position = g_shared.get_shoot_position( );
			eye_candidates.entries[ 0 ].is_uninterpolated = true;
			eye_candidates.count = 1;
		}

		std::vector<scan_hit> all_hits;

		for ( auto i = 0; i < eye_candidates.count; ++i )
		{
			auto hits = this->scan_knife( eye_candidates.entries[ i ].position, ctx, info, candidates, local );

			for ( auto& h : hits )
			{
				h.source_eye = eye_candidates.entries[ i ];
				all_hits.push_back( std::move( h ) );
			}
		}

		if ( all_hits.empty( ) )
		{
			return;
		}

		target best{};
		target best_backstab{};

		for ( const auto& h : all_hits )
		{
			auto& dest = h.is_backstab ? best_backstab : best;

			if ( !dest.valid || h.score > dest.score )
			{
				dest.hit = h;
				dest.hitchance = 1.0f;
				dest.score = h.score;
				dest.valid = true;
			}
		}

		auto& chosen = best_backstab.valid ? best_backstab : best;
		if ( !chosen.valid )
		{
			return;
		}

		chosen = this->apply_target_switch_delay( chosen, ctx, all_hits, 0.0f );
		if ( !chosen.valid )
		{
			return;
		}

		this->m_knife_attack = static_cast< std::uint8_t >( chosen.hit.attack_type );
		this->update_state_target( chosen );
		this->fire_melee( cmd, chosen, local );
	}

	void rage::auto_revolver( systems::input::usercmd* cmd, const aim_context& ctx, const systems::local::snapshot& local )
	{
		if ( !settings::g_combat.m_ragebot.enabled )
		{
			this->m_revolver_cock_ticks = 0;
			return;
		}

		const auto& shared_ctx = g_shared.ctx( );
		const auto& config = settings::g_combat.m_ragebot.get_group( shared_ctx.weapon_type );
		if ( config.no_spread.value )
		{
			this->m_revolver_cock_ticks = 0;

			if ( !g_shared.can_shoot( cmd, local.controller, false ) )
			{
				return;
			}

			const auto tick_base = memory::read<int>( local.controller + SCHEMA( "CBasePlayerController", "m_nTickBase"_hash ) );
			const auto base = cmd->csgo_user_cmd.base( );
			const auto client_tick = base ? base->client_tick( ) : tick_base;
			const auto next_secondary = memory::read<int>( shared_ctx.weapon + SCHEMA( "C_BasePlayerWeapon", "m_nNextSecondaryAttackTick"_hash ) );
			if ( ( next_secondary > 0 && client_tick < next_secondary ) || g_shared.last_shoot_tick( ) == tick_base )
			{
				return;
			}

			this->run_gun( cmd, ctx, local );
			return;
		}

		const auto tick_base = memory::read<int>( local.controller + SCHEMA( "CBasePlayerController", "m_nTickBase"_hash ) );
		const auto ready_tick = memory::read<int>( shared_ctx.weapon + SCHEMA( "C_CSWeaponBase", "m_nPostponeFireReadyTicks"_hash ) );
		const auto hauled_back = memory::read<bool>( shared_ctx.weapon + SCHEMA( "C_CSWeaponBase", "m_bIsHauledBack"_hash ) );

		const auto starting_cock = this->m_revolver_cock_ticks == 0 && !hauled_back;
		if ( !g_shared.can_shoot( cmd, local.controller, starting_cock ) )
		{
			this->m_revolver_cock_ticks = 0;
			return;
		}

		if ( !settings::g_combat.m_autos.revolver.value )
		{
			this->m_revolver_cock_ticks = 0;
			return;
		}

		const auto& shoot_history = g_shared.sh( );
		const auto history_lead = std::clamp( shoot_history.client_tick( ) - shoot_history.server_tick( ), 0, 64 );
		const auto server_fire_tick = tick_base + history_lead + 1;
		const auto ready_to_fire = hauled_back && ready_tick > 0 && server_fire_tick >= ready_tick;

		if ( ready_to_fire )
		{
			this->run_gun( cmd, ctx, local );
			if ( !this->m_firing_this_tick )
			{
				cmd->buttons.value &= ~cstypes::command_buttons::in_attack;
				cmd->buttons.value_changed |= cstypes::command_buttons::in_attack;
				cmd->buttons.value_scroll &= ~cstypes::command_buttons::in_attack;
				cmd->csgo_user_cmd.set_attack1_start_history_index( -1 );
			}
			this->m_revolver_cock_ticks = 0;
			return;
		}

		this->run_gun( cmd, ctx, local, false );

		cmd->buttons.value |= cstypes::command_buttons::in_attack;
		cmd->buttons.value_changed |= cstypes::command_buttons::in_attack;
		cmd->buttons.value_scroll |= cstypes::command_buttons::in_attack;

		const auto history_index = cmd->csgo_user_cmd.input_history_size( ) - 1;
		if ( history_index >= 0 )
		{
			cmd->csgo_user_cmd.set_attack1_start_history_index( history_index );
		}

		this->m_revolver_cock_ticks = std::min( this->m_revolver_cock_ticks + 1, 64 );
	}

	std::vector<rage::scan_hit> rage::scan_players( const math::vector3& eye, float inaccuracy, const aim_context& ctx, std::vector<candidate>& candidates, const systems::local::snapshot& local, const int first_record, const int record_limit ) const
	{
		std::vector<std::vector<scan_hit>> per_candidate( candidates.size( ) );
		threadpool::parallel_for( 0, static_cast< int >( candidates.size( ) ), [ & ]( int begin, int end )
			{
				for ( auto ci = begin; ci < end; ++ci )
				{
					auto& cand = candidates[ ci ];
					auto& candidate_hits = per_candidate[ ci ];
					candidate_hits.reserve( 24 );

					const auto first = std::clamp( first_record, 0, cand.record_count );
					auto last = std::min( cand.record_count, first + std::max( record_limit, 0 ) );
					if ( record_limit == 1 && first == 0 && last == 1 && cand.record_count > 1
						&& cand.records[ 0 ] && cand.records[ 0 ]->extrapolated )
					{
						last = 2;
					}
					for ( auto ri = first; ri < last; ++ri )
					{
						if ( !cand.records[ ri ] || !cand.records[ ri ]->valid )
						{
							continue;
						}

						auto* record = cand.records[ ri ];
						auto scan_eye = eye;

						auto hits = this->scan_player( scan_eye, inaccuracy, ctx, cand, record, local );
						for ( auto& h : hits )
						{
							h.source_eye.position = scan_eye;
							candidate_hits.push_back( std::move( h ) );
						}
					}
				}
			}, 1 );

		std::vector<scan_hit> flat;
		auto total_hits{ std::size_t{} };
		for ( const auto& hits : per_candidate )
		{
			total_hits += hits.size( );
		}
		flat.reserve( total_hits );

		for ( auto& v : per_candidate )
		{
			for ( auto& h : v )
			{
				flat.push_back( std::move( h ) );
			}
		}

		return flat;
	}

	std::vector<rage::scan_hit> rage::scan_player( const math::vector3& eye, float inaccuracy, const aim_context& ctx, candidate& cand, shared::lagcomp::record* record, const systems::local::snapshot& local ) const
	{
		if ( !cand.pawn || !record || !record->valid || cand.record_count <= 0 || cand.health <= 0 )
			return {};
		if ( memory::read<int>( cand.pawn + SCHEMA( "C_BaseEntity", "m_iHealth"_hash ) ) <= 0 ||
			memory::read<std::uint8_t>( cand.pawn + SCHEMA( "C_BaseEntity", "m_lifeState"_hash ) ) != 0 )
			return {};

		const auto& shared_ctx = g_shared.ctx( );
		const auto& config = settings::g_combat.m_ragebot.get_group( shared_ctx.weapon_type );

		const auto game_scene_node = memory::read<std::uintptr_t>( cand.pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
		if ( !game_scene_node )
		{
			return {};
		}

		const auto hitbox_set = systems::g_hitboxes.query( game_scene_node );
		if ( hitbox_set.count <= 0 )
		{
			return {};
		}

		const auto skeleton = g_shared.lc( ).get_skeleton( *record );
		const auto debug_primary_record = record->live_snapshot ||
			( cand.record_count > 0 && cand.records[ 0 ] == record );
		const auto pen_ctx = g_shared.pen( ).prepare_target( cand.pawn, record, &hitbox_set );

		const auto force_body = config.body_aim.value;

		std::array<int, 19> scan_order{};
		auto scan_count{ 0 };

		if ( !force_body && config.hitboxes.values[ 0 ] )
		{
			scan_order[ scan_count++ ] = 0;
		}

		if ( config.hitboxes.values[ 1 ] )
		{
			scan_order[ scan_count++ ] = 4;
			scan_order[ scan_count++ ] = 5;
			scan_order[ scan_count++ ] = 6;
		}

		if ( config.hitboxes.values[ 2 ] )
		{
			scan_order[ scan_count++ ] = 3;
			scan_order[ scan_count++ ] = 2;
		}

		if ( config.hitboxes.values[ 3 ] )
		{
			for ( auto idx : { 13, 14, 15, 16, 17, 18 } )
			{
				scan_order[ scan_count++ ] = idx;
			}
		}

		if ( config.hitboxes.values[ 4 ] )
		{
			for ( auto idx : { 7, 8, 9, 10 } )
			{
				scan_order[ scan_count++ ] = idx;
			}
		}

		if ( config.hitboxes.values[ 5 ] )
		{
			for ( auto idx : { 11, 12 } )
			{
				scan_order[ scan_count++ ] = idx;
			}
		}

		if ( scan_count == 0 )
		{
			if ( !force_body )
			{
				scan_order[ scan_count++ ] = 0;
			}

			for ( auto idx : { 4, 5, 6, 3, 2 } )
			{
				scan_order[ scan_count++ ] = idx;
			}
		}

		struct trace_point
		{
			math::vector3 position;
			int hitbox_index;
			int bone_index;
			systems::hitboxes::entry hitbox;
			bool is_center;
		};

		constexpr std::size_t k_max_debug_points{ 1024 };
		const auto add_debug_point = [ & ]( const math::vector3& position, const int hitbox_index, const bool is_center )
		{
			if ( !config.debug_multipoints.value || !debug_primary_record )
			{
				return;
			}

			std::lock_guard lock( m_debug_mtx );
			if ( m_debug_points.size( ) < k_max_debug_points )
			{
				m_debug_points.push_back( { position, hitbox_index, is_center } );
			}
		};

		std::vector<trace_point> points;
		points.reserve( static_cast< std::size_t >( scan_count ) * 4 );

		for ( auto idx = 0; idx < scan_count; ++idx )
		{
			const auto hitbox_index = scan_order[ idx ];
			const systems::hitboxes::entry* hb{ nullptr };

			for ( const auto& entry : hitbox_set )
			{
				if ( entry.index == hitbox_index )
				{
					hb = &entry;
					break;
				}
			}

			if ( !hb || hb->bone < 0 || hb->bone >= static_cast< int >( skeleton.size( ) ) )
			{
				continue;
			}

			const auto& bone = skeleton[ hb->bone ];
			if ( bone.position.length_sqr( ) < 1.0f )
			{
				continue;
			}

			const auto hitbox_center = ( hb->mins + hb->maxs ) * 0.5f;
			const auto center = bone.rotation.rotate_vector( hitbox_center ) + bone.position;
			const auto center_is_first = g_shared.pen( ).first_hitbox( eye, center, pen_ctx ) == hitbox_index;

			trace_point cp{};
			cp.position = center;
			cp.hitbox_index = hitbox_index;
			cp.bone_index = hb->bone;
			cp.hitbox = *hb;
			cp.is_center = true;
			points.push_back( cp );

			if ( center_is_first )
			{
				add_debug_point( center, hitbox_index, true );
			}

		}

		if ( points.empty( ) )
		{
			return {};
		}

		std::vector<scan_hit> results;
		results.reserve( static_cast< std::size_t >( scan_count ) * 2 );
		const auto evaluate_point = [ & ]( const trace_point& tp, const bool accept_multipoint )
		{
			if ( !tp.is_center && ( !accept_multipoint || tp.hitbox_index < 0 || tp.hitbox_index >= 19 ) )
			{
				return;
			}

			const auto aim = math::helpers::calculate_angle( eye, tp.position );
			const auto fov = math::helpers::angle_distance( ctx.view_angles, aim );

			if ( !std::isfinite( aim.x ) || !std::isfinite( aim.y ) || !std::isfinite( aim.z ) || !std::isfinite( fov ) )
			{
				return;
			}

			if ( fov > config.max_fov )
			{
				return;
			}

			shared::penetration::result pen{};
			if ( !g_shared.pen( ).run( eye, tp.position, pen_ctx, local.pawn, local.team, pen, tp.hitbox_index ) )
			{
				return;
			}

			if ( pen.damage < cand.min_damage )
			{
				return;
			}
			// 穿透路径对入射角和墙体厚度非常敏感；扫描点刚好贴着
			// min damage 时，最终命令的浮点取整可能落到阈值以下。
			// 预留少量余量，避免命中但实际伤害不足。

			if ( pen.hitgroup <= 0 )
			{
				return;
			}

			const auto requested_hitgroup = systems::g_hitboxes.hitgroup_from_hitbox( tp.hitbox_index );
			const auto resolved_hitgroup = pen.hitbox >= 0
				? systems::g_hitboxes.hitgroup_from_hitbox( pen.hitbox )
				: pen.hitgroup;
			if ( requested_hitgroup > 0 && resolved_hitgroup > 0 && requested_hitgroup != resolved_hitgroup )
			{
				return;
			}

			auto resolved_hitbox = tp.hitbox;
			auto resolved_bone = tp.bone_index;
			if ( pen.hitbox >= 0 )
			{
				for ( const auto& entry : hitbox_set )
				{
					if ( entry.index == pen.hitbox )
					{
						resolved_hitbox = entry;
						resolved_bone = entry.bone;
						break;
					}
				}
			}

			scan_hit h{};
			h.position = tp.position;
			h.aim_angle = aim;
			h.damage = pen.damage;
			h.fov = fov;
			h.hitbox_index = pen.hitbox >= 0 ? pen.hitbox : tp.hitbox_index;
			h.hitgroup = pen.hitgroup;
			h.bone_index = resolved_bone;
			h.hitbox = resolved_hitbox;
			h.is_center = tp.is_center;
			h.penetrated = pen.penetrated;
			h.pawn = cand.pawn;
			h.entity_index = cand.entity_index;
			h.health = cand.health;
			h.record = record;

			results.push_back( h );
		};

		for ( const auto& point : points )
		{
			if ( point.is_center && g_shared.pen( ).first_hitbox( eye, point.position, pen_ctx ) == point.hitbox_index )
			{
				evaluate_point( point, false );
			}
		}

		if ( config.pointscale > 0.0f || config.head_pointscale > 0.0f )
		{
			const auto record_speed = record->velocity.length_2d( );
			const auto live_velocity = memory::read<math::vector3>( cand.pawn + SCHEMA( "C_BaseEntity", "m_vecVelocity"_hash ) );
			const auto live_velocity_valid = std::isfinite( live_velocity.x ) && std::isfinite( live_velocity.y ) &&
				std::isfinite( live_velocity.z ) && live_velocity.length_2d( ) <= 4096.0f;
			const auto motion_velocity = live_velocity_valid && live_velocity.length_2d( ) > 8.0f
				? live_velocity
				: record->velocity;
			const auto target_speed = std::max( record_speed, motion_velocity.length_2d( ) );
			const auto target_distance = ( eye - record->origin ).length( );
			const auto speed_bonus = target_speed > 8.0f
				? static_cast< std::size_t >( std::clamp( target_speed / 110.0f, 0.0f, 4.0f ) )
				: 0u;
			const auto range_bonus = target_distance < 900.0f ? std::size_t{ 2 } : 0u;
			const auto max_multipoint_traces = std::min< std::size_t >( 48u, 32u + speed_bonus * 2u + range_bonus * 4u );
			auto multipoint_traces{ std::size_t{} };

			for ( const auto& point : points )
			{
				const auto core_hitbox = point.hitbox_index == 0 || ( point.hitbox_index >= 2 && point.hitbox_index <= 6 );
				if ( !point.is_center || !core_hitbox )
				{
					continue;
				}
				const auto& bone = skeleton[ point.bone_index ];
				const auto pointscale = point.hitbox_index == 0 ? config.head_pointscale : config.pointscale;
				if ( pointscale <= 0.0f )
				{
					continue;
				}
				const auto motion_ticks = target_speed > 8.0f
					? std::max( record->extrapolated ? record->extrapolated_ticks : 1, 1 )
					: 0;
				const auto multipoints = this->generate_multipoints(
					point.hitbox,
					point.position,
					bone.rotation,
					pointscale,
					eye,
					inaccuracy,
					motion_velocity,
					motion_ticks );

				for ( const auto& multipoint : multipoints )
				{
					if ( multipoint_traces >= max_multipoint_traces )
					{
						break;
					}

					const auto multipoint_aim = math::helpers::calculate_angle( eye, multipoint );
					const auto multipoint_fov = math::helpers::angle_distance( ctx.view_angles, multipoint_aim );
					if ( !std::isfinite( multipoint_aim.x ) || !std::isfinite( multipoint_aim.y ) ||
						!std::isfinite( multipoint_aim.z ) || !std::isfinite( multipoint_fov ) ||
						multipoint_fov > config.max_fov )
					{
						continue;
					}

					if ( g_shared.pen( ).first_hitbox( eye, multipoint, pen_ctx ) != point.hitbox_index )
					{
						continue;
					}
					add_debug_point( multipoint, point.hitbox_index, false );

					++multipoint_traces;
					trace_point refined = point;
					refined.position = multipoint;
					refined.is_center = false;
					evaluate_point( refined, true );
				}

				if ( multipoint_traces >= max_multipoint_traces )
				{
					break;
				}
			}
		}

		return results;
	}

	rage::target rage::select_best( const aim_context& aim_ctx, const std::vector<scan_hit>& hits, float eval_inaccuracy ) const
	{
		auto hitgroup_priority = [ ]( int hitbox_index ) -> int
			{
				if ( hitbox_index == 0 ) { return 4; }
				if ( hitbox_index >= 1 && hitbox_index <= 6 ) { return 3; }
				if ( hitbox_index >= 13 && hitbox_index <= 18 ) { return 2; }
				if ( hitbox_index >= 7 && hitbox_index <= 12 ) { return 1; }
				return 0;
			};

		struct record_group
		{
			shared::lagcomp::record* record;
			std::vector<int> hit_indices;
		};

		std::vector<record_group> groups;
		groups.reserve( 16 );

		for ( auto i = 0; i < static_cast< int >( hits.size( ) ); ++i )
		{
			auto rec = hits[ i ].record;
			auto found{ false };

			for ( auto& g : groups )
			{
				if ( g.record == rec )
				{
					g.hit_indices.push_back( i );
					found = true;
					break;
				}
			}

			if ( !found )
			{
				record_group g{};
				g.record = rec;
				g.hit_indices.reserve( 16 );
				g.hit_indices.push_back( i );
				groups.push_back( std::move( g ) );
			}
		}

		constexpr auto top_k_per_record{ 6 };

		auto cheap_score = [ & ]( const scan_hit& h ) -> float
			{
				const auto lethal_bonus = h.damage >= static_cast< float >( h.health ) ? 100000.0f : 0.0f;
				const auto direct_bonus = h.penetrated ? 0.0f : 25.0f;
				const auto center_bonus = h.is_center ? 5.0f : 0.0f;

				return lethal_bonus + h.damage * 100.0f +
					static_cast< float >( hitgroup_priority( h.hitbox_index ) ) * 250.0f +
					direct_bonus + center_bonus - h.fov;
			};

		for ( auto& group : groups )
		{
			if ( static_cast< int >( group.hit_indices.size( ) ) <= top_k_per_record )
			{
				continue;
			}

			std::array<int, 19> best_by_hitbox{};
			best_by_hitbox.fill( -1 );
			std::vector<int> head_candidates;
			head_candidates.reserve( 5 );
			for ( const auto idx : group.hit_indices )
			{
				const auto hitbox_index = hits[ idx ].hitbox_index;
				if ( hitbox_index < 0 || hitbox_index >= static_cast< int >( best_by_hitbox.size( ) ) )
				{
					continue;
				}
				if ( hitbox_index == 0 )
				{
					head_candidates.push_back( idx );
					continue;
				}

				auto& best_idx = best_by_hitbox[ hitbox_index ];
				if ( best_idx < 0 || cheap_score( hits[ idx ] ) > cheap_score( hits[ best_idx ] ) )
				{
					best_idx = idx;
				}
			}

			std::vector<int> region_candidates;
			region_candidates.reserve( best_by_hitbox.size( ) + head_candidates.size( ) );
		region_candidates.insert( region_candidates.end( ), head_candidates.begin( ), head_candidates.end( ) );
			for ( const auto idx : best_by_hitbox )
			{
				if ( idx >= 0 )
				{
					region_candidates.push_back( idx );
				}
			}

			if ( region_candidates.size( ) <= top_k_per_record )
			{
				group.hit_indices = std::move( region_candidates );
				continue;
			}

			std::partial_sort
			(
				region_candidates.begin( ),
				region_candidates.begin( ) + top_k_per_record,
				region_candidates.end( ),
				[ & ]( int a, int b ) { return cheap_score( hits[ a ] ) > cheap_score( hits[ b ] ); }
			);

			region_candidates.resize( top_k_per_record );
			group.hit_indices = std::move( region_candidates );
		}

		struct evaluated_hit
		{
			int hit_index;
			float hitchance;
			float score;
		};

		std::vector<evaluated_hit> evaluated;
		evaluated.reserve( hits.size( ) );
		auto logged_historical_skip = false;

		std::unordered_map<std::uintptr_t, int> newest_record_ticks;
		newest_record_ticks.reserve( groups.size( ) );
		std::unordered_map<std::uintptr_t, bool> moving_extrapolated_targets;
		moving_extrapolated_targets.reserve( groups.size( ) );
		std::unordered_map<std::uintptr_t, bool> jitter_targets;
		jitter_targets.reserve( groups.size( ) );
		std::unordered_map<std::uintptr_t, int> jitter_reference_ticks;
		jitter_reference_ticks.reserve( groups.size( ) );
		std::unordered_map<std::uintptr_t, int> jitter_reference_parity;
		jitter_reference_parity.reserve( groups.size( ) );
		for ( const auto& h : hits )
		{
			if ( !h.record )
			{
				continue;
			}

			if ( h.record->extrapolated )
			{
				if ( h.record->extrapolated_ticks >= 1 && h.record->velocity.length_2d( ) >= 32.0f )
				{
					moving_extrapolated_targets[ h.pawn ] = true;
				}
				continue;
			}

			auto [it, inserted] = newest_record_ticks.emplace( h.pawn, h.record->tick );
			if ( !inserted )
			{
				it->second = std::max( it->second, h.record->tick );
			}

			for ( const auto& other : hits )
			{
				if ( other.pawn != h.pawn || !other.record || other.record == h.record ||
					other.record->extrapolated || h.record->extrapolated )
				{
					continue;
				}
				const auto tick_gap = std::abs( other.record->tick - h.record->tick );
				if ( tick_gap > 0 && tick_gap <= 8 )
				{
					auto yaw_delta = std::fabsf( other.record->rotation.y - h.record->rotation.y );
					while ( yaw_delta > 360.0f ) yaw_delta -= 360.0f;
					if ( yaw_delta > 180.0f ) yaw_delta = 360.0f - yaw_delta;
					if ( yaw_delta >= 35.0f )
					{
						jitter_targets[ h.pawn ] = true;
						const auto newest_tick = std::max( h.record->tick, other.record->tick );
						jitter_reference_ticks[ h.pawn ] = newest_tick;
						jitter_reference_parity[ h.pawn ] = newest_tick & 1;
					}
				}
			}
		}

		const auto& config = settings::g_combat.m_ragebot.get_group( g_shared.ctx( ).weapon_type );
		const auto command_tick = g_shared.ctx( ).current_tick;
		const auto backtrack = settings::g_combat.m_lagcomp.backtrack.value;
		const auto needed_hc = config.no_spread.value
			? 0.0f
			: ( config.hitchance_override.value
				? static_cast< float >( config.hitchance_override_value ) / 100.0f
				: static_cast< float >( config.hitchance ) / 100.0f );

		for ( auto& group : groups )
		{
			if ( !group.record || !group.record->valid )
			{
				continue;
			}

			for ( const auto idx : group.hit_indices )
			{
				const auto& h = hits[ idx ];

				if ( !h.record || !h.record->valid )
				{
					continue;
				}
				if ( !settings::g_combat.m_lagcomp.extrapolation.value && h.record->extrapolated )
				{
					continue;
				}

				if ( h.bone_index < 0 || h.bone_index >= 28 )
				{
					continue;
				}

				const auto& bone = group.record->bones[ h.bone_index ];
				const auto hc = config.no_spread.value
					? 1.0f
					: g_shared.calculate_hitchance( h.source_eye.position, h.aim_angle, h.hitbox, bone, eval_inaccuracy, aim_ctx.spread, 256, needed_hc );
				auto jitter_pose_consistent = true;
				if ( !config.no_spread.value && jitter_targets.contains( h.pawn ) )
				{
					auto checked_neighbor = false;
					for ( const auto& neighbor : hits )
					{
						if ( neighbor.pawn != h.pawn || neighbor.record == h.record ||
							!neighbor.record || neighbor.record->extrapolated ||
							neighbor.hitbox_index != h.hitbox_index ||
							std::abs( neighbor.record->tick - h.record->tick ) > 8 )
						{
							continue;
						}
						checked_neighbor = true;
						const auto direction = ( h.position - h.source_eye.position ).normalized( ) * g_shared.ctx( ).range;
						const auto& neighbor_bone = neighbor.record->bones[ neighbor.bone_index ];
						const auto capsule_start = neighbor_bone.rotation.rotate_vector( neighbor.hitbox.mins ) + neighbor_bone.position;
						const auto capsule_end = neighbor_bone.rotation.rotate_vector( neighbor.hitbox.maxs ) + neighbor_bone.position;
						auto fraction{ 1.0f };
						if ( neighbor.hitbox.radius > 0.001f &&
							g_shared.ray_vs_capsule( h.source_eye.position, direction, capsule_start, capsule_end, neighbor.hitbox.radius, fraction ) )
						{
							jitter_pose_consistent = true;
							break;
						}
						jitter_pose_consistent = false;
					}
					jitter_pose_consistent = !checked_neighbor || jitter_pose_consistent;
				}
				const auto hp = static_cast< float >( h.health );
				const auto can_kill = h.damage >= hp;
				const auto passes_hitchance = config.no_spread.value || ( hc >= needed_hc && jitter_pose_consistent );
				auto score = passes_hitchance ? 1000000.0f : 0.0f;
				const auto newest_it = newest_record_ticks.find( h.pawn );
				const auto newest_tick = newest_it != newest_record_ticks.end( ) ? newest_it->second : h.record->tick;
				const auto record_age = std::max( newest_tick - h.record->tick, 0 );

				const auto network_gap = newest_it != newest_record_ticks.end( ) && command_tick > 0
					? std::max( command_tick - newest_it->second, 0 )
					: 0;
				const auto target_speed = std::max(
					h.record->velocity.length_2d( ),
					memory::read<math::vector3>( h.pawn + SCHEMA( "C_BaseEntity", "m_vecVelocity"_hash ) ).length_2d( ) );
				const auto moving_extrapolated = moving_extrapolated_targets.contains( h.pawn )
					&& target_speed >= 32.0f;
				if ( !backtrack && !h.record->extrapolated && record_age > 0 )
				{
#if defined( DEV )
					if ( !logged_historical_skip )
					{
						logged_historical_skip = true;
						diag::writef(
							diag::level::info,
								"[rage_candidate_skip] reason=backtrack_disabled pawn=%llx newest=%d record=%d age=%d speed=%.2f network_gap=%d",
							static_cast< unsigned long long >( h.pawn ),
							newest_tick,
							h.record->tick,
							record_age,
							target_speed,
							network_gap );
					}
#endif
					continue;
				}
				const auto jitter_target = jitter_targets.contains( h.pawn );
				const auto jitter_tick_it = jitter_reference_ticks.find( h.pawn );
				const auto jitter_parity_it = jitter_reference_parity.find( h.pawn );
				const auto jitter_is_newest = jitter_target && jitter_tick_it != jitter_reference_ticks.end( )
					&& h.record->tick == jitter_tick_it->second;
				const auto jitter_parity_match = jitter_target && jitter_parity_it != jitter_reference_parity.end( )
					&& ( h.record->tick & 1 ) == jitter_parity_it->second;
				const auto speed = std::clamp( h.record->velocity.length_2d( ), 0.0f, 320.0f );
				const auto configured_extrapolation_confidence = std::clamp(
					static_cast< float >( settings::g_combat.m_lagcomp.extrapolation_confidence.value ) / 100.0f,
					0.0f,
					1.0f );
				const auto configured_extrapolation_stability = std::clamp(
					static_cast< float >( settings::g_combat.m_lagcomp.extrapolation_stability.value ) / 100.0f,
					0.0f,
					1.0f );
				const auto measured_extrapolation_stability = h.record->extrapolated
					? std::clamp( h.record->extrapolation_stability, 0.0f, 1.0f )
					: 1.0f;
				const auto attack_probability = config.no_spread.value
					? 1.0f
					: std::clamp( hc, 0.0f, 1.0f );
				const auto attack_extrapolation_stability = measured_extrapolation_stability * attack_probability;
				const auto extrapolation_stability = h.record->extrapolated
					? ( 1.0f - configured_extrapolation_stability +
						attack_extrapolation_stability * configured_extrapolation_stability )
					: 1.0f;
				const auto required_attack_stability = 0.30f + configured_extrapolation_stability * 0.50f;
				if ( h.record->extrapolated && configured_extrapolation_stability > 0.0f && passes_hitchance &&
					attack_extrapolation_stability < required_attack_stability )
				{
		#if defined( DEV )
					diag::writef(
						diag::level::info,
						"[rage_candidate_skip] reason=stability_hit pawn=%llx motion=%.3f hitchance=%.3f effective=%.3f required=%.3f",
						static_cast< unsigned long long >( h.pawn ),
						measured_extrapolation_stability,
						attack_probability,
						attack_extrapolation_stability,
						required_attack_stability );
		#endif
					continue;
				}
				const auto extrapolation_confidence = h.record->extrapolated
					? std::clamp( 1.0f - static_cast< float >( record_age ) / 16.0f, 0.0f, 1.0f )
						* ( 0.55f + 0.45f * std::clamp( speed / 220.0f, 0.0f, 1.0f ) )
						* configured_extrapolation_confidence
						* extrapolation_stability
					: 0.0f;

				if ( passes_hitchance && !h.record->extrapolated && record_age == 0 )
				{
					score += moving_extrapolated ? 12000.0f : 25000.0f;
				}
				if ( h.record->extrapolated )
				{
					score += ( moving_extrapolated ? 30000.0f : 5000.0f ) * extrapolation_confidence;
				}
				if ( jitter_target )
				{
					score += jitter_is_newest ? 18000.0f : ( jitter_parity_match ? 4000.0f : -18000.0f );
				}
				score -= static_cast< float >( record_age ) * 25.0f;

				if ( can_kill )
				{
					score += 100000.0f + h.damage * 1000.0f + hc * 10000.0f;
				}
				else
				{
					score += h.damage * 1000.0f + h.damage * hc * 100.0f;
				}

				score += h.penetrated ? 0.0f : 250.0f;
				score += h.is_center ? 10.0f : 0.0f;
				score += static_cast< float >( hitgroup_priority( h.hitbox_index ) ) * 1000.0f;
				score -= h.fov * 0.1f;

				evaluated.push_back( evaluated_hit{ idx, hc, score } );
			}
		}

		std::unordered_map<std::uintptr_t, float> newest_passes_damage;
		newest_passes_damage.reserve( newest_record_ticks.size( ) );
		std::unordered_set<std::uintptr_t> usable_extrapolated_targets;
		usable_extrapolated_targets.reserve( moving_extrapolated_targets.size( ) );
		for ( const auto& e : evaluated )
		{
			const auto& h = hits[ e.hit_index ];
			if ( h.record && h.record->extrapolated && e.hitchance >= needed_hc &&
				moving_extrapolated_targets.contains( h.pawn ) )
			{
				usable_extrapolated_targets.insert( h.pawn );
			}
			const auto newest_it = newest_record_ticks.find( h.pawn );
			if ( newest_it != newest_record_ticks.end( ) && h.record && !h.record->extrapolated &&
				h.record->tick == newest_it->second && e.hitchance >= needed_hc )
			{
				auto& damage = newest_passes_damage[ h.pawn ];
				damage = std::max( damage, h.damage );
			}
		}

		std::unordered_map<std::uintptr_t, float> direct_passes_damage;
		direct_passes_damage.reserve( newest_record_ticks.size( ) );
		for ( const auto& e : evaluated )
		{
			const auto& h = hits[ e.hit_index ];
			if ( !h.penetrated && e.hitchance >= needed_hc )
			{
				auto& damage = direct_passes_damage[ h.pawn ];
				damage = std::max( damage, h.damage );
			}
		}

		target best{};

		for ( const auto& e : evaluated )
		{
			if ( e.hit_index < 0 || e.hit_index >= static_cast< int >( hits.size( ) ) )
			{
				continue;
			}

			const auto& h = hits[ e.hit_index ];

			if ( !h.record || !h.record->valid )
			{
				continue;
			}

			if ( !backtrack && !h.record->extrapolated && usable_extrapolated_targets.contains( h.pawn ) )
			{
#if defined( DEV )
				static int last_superseded_tick = -1;
				if ( command_tick != last_superseded_tick )
				{
					diag::writef(
						diag::level::info,
						"[rage_candidate_skip] reason=network_fallback_superseded pawn=%llx record=%d command=%d",
						static_cast<unsigned long long>( h.pawn ),
						h.record->tick,
						command_tick );
					last_superseded_tick = command_tick;
				}
#endif
				continue;
			}

			const auto newest_it = newest_record_ticks.find( h.pawn );
			const auto newest_passes = newest_passes_damage.find( h.pawn );
			if ( !h.record->extrapolated && newest_passes != newest_passes_damage.end( ) && newest_it != newest_record_ticks.end( ) &&
				h.record->tick != newest_it->second && newest_passes->second >= h.damage )
			{
				continue;
			}

			const auto direct_passes = direct_passes_damage.find( h.pawn );
			if ( h.penetrated && direct_passes != direct_passes_damage.end( ) && direct_passes->second >= h.damage &&
				!( !backtrack && h.record->extrapolated && usable_extrapolated_targets.contains( h.pawn ) ) )
			{
				continue;
			}

			auto is_better = !best.valid || e.score > best.score;
			if ( best.valid && std::fabsf( e.score - best.score ) < 0.01f )
			{
				if ( h.record->tick != best.hit.record->tick )
				{
					is_better = h.record->tick > best.hit.record->tick;
				}
				else if ( h.is_center != best.hit.is_center )
				{
					is_better = h.is_center;
				}
				else
				{
					is_better = h.fov < best.hit.fov;
				}
			}

			if ( is_better )
			{
				best.hit = h;
				best.hitchance = e.hitchance;
				best.score = e.score;
				best.valid = true;
			}
		}

		return best;
	}

	rage::target rage::apply_target_switch_delay( const target& selected, const aim_context& aim_ctx, const std::vector<scan_hit>& hits, float eval_inaccuracy )
	{
		const auto delay = std::clamp( settings::g_combat.m_ragebot.target_switch_delay.value, 0, 20 );
		const auto current_tick = g_shared.ctx( ).current_tick;

		if ( delay <= 0 )
		{
			this->m_pending_target_switch_pawn = 0;
			this->m_pending_target_switch_tick = -1;
			if ( selected.valid )
			{
				this->m_target_switch_pawn = selected.hit.pawn;
			}
			return selected;
		}

		if ( !selected.valid )
		{
			this->m_target_switch_pawn = 0;
			this->m_pending_target_switch_pawn = 0;
			this->m_pending_target_switch_tick = -1;
			return selected;
		}

		if ( !this->m_target_switch_pawn || selected.hit.pawn == this->m_target_switch_pawn )
		{
			this->m_target_switch_pawn = selected.hit.pawn;
			this->m_pending_target_switch_pawn = 0;
			this->m_pending_target_switch_tick = -1;
			return selected;
		}

		if ( this->m_pending_target_switch_pawn != selected.hit.pawn )
		{
			this->m_pending_target_switch_pawn = selected.hit.pawn;
			this->m_pending_target_switch_tick = current_tick;
		}

		const auto elapsed = current_tick >= this->m_pending_target_switch_tick
			? current_tick - this->m_pending_target_switch_tick
			: 0;
		if ( elapsed < delay )
		{
			std::vector<scan_hit> held_hits;
			held_hits.reserve( hits.size( ) );
			for ( const auto& hit : hits )
			{
				if ( hit.pawn == this->m_target_switch_pawn )
				{
					held_hits.push_back( hit );
				}
			}

			if ( !held_hits.empty( ) )
			{
				const auto held = this->select_best( aim_ctx, held_hits, eval_inaccuracy );
				if ( held.valid )
				{
					return held;
				}
			}
		}

		this->m_target_switch_pawn = selected.hit.pawn;
		this->m_pending_target_switch_pawn = 0;
		this->m_pending_target_switch_tick = -1;
		return selected;
	}

	float rage::evaluate_hitchance( const scan_hit& hit, const aim_context& ctx, float inaccuracy ) const
	{
		if ( !hit.record || !hit.record->valid || hit.bone_index < 0 || hit.bone_index >= 28 )
		{
			return 0.0f;
		}

		const auto& config = settings::g_combat.m_ragebot.get_group( g_shared.ctx( ).weapon_type );
		if ( config.no_spread.value )
		{
			return 1.0f;
		}
		const auto minimum = config.hitchance_override.value
			? static_cast<float>( config.hitchance_override_value ) / 100.0f
			: static_cast<float>( config.hitchance ) / 100.0f;
		return g_shared.calculate_hitchance( hit.source_eye.position, hit.aim_angle, hit.hitbox, hit.record->bones[ hit.bone_index ], inaccuracy, ctx.spread, 256, minimum );
	}

	float rage::get_standing_inaccuracy( const systems::local::snapshot& local, const aim_context& ctx ) const
	{
		const auto& prestate = systems::g_prediction.pre( );
		auto velocity = prestate.networked_velocity;
		velocity.z = 0.0f;

		const auto speed = velocity.length_2d( );
		if ( speed > ctx.accurate_threshold )
		{
			return g_shared.get_inaccuracy_at_velocity( local.pawn, velocity );
		}

		const auto& shared_ctx = g_shared.ctx( );
		if ( !shared_ctx.weapon_vdata )
		{
			return ctx.predicted_inaccuracy;
		}

		const auto inaccuracy_stand = memory::read<float>( shared_ctx.weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_flInaccuracyStand"_hash ) );
		return std::max( inaccuracy_stand, g_shared.get_inaccuracy_at_velocity( local.pawn, velocity ) );
	}

	std::vector<rage::scan_hit> rage::scan_taser( const math::vector3& eye, const aim_context& ctx, std::vector<candidate>& candidates, const systems::local::snapshot& local ) const
	{
		const auto& shared_ctx = g_shared.ctx( );
		std::vector<scan_hit> results;

		for ( auto& cand : candidates )
		{
			for ( auto ri = 0; ri < cand.record_count; ++ri )
			{
				auto* record = cand.records[ ri ];
				if ( !record || !record->valid )
				{
					continue;
				}

				const auto game_scene_node = memory::read<std::uintptr_t>( cand.pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
				if ( !game_scene_node )
				{
					continue;
				}

				const auto hitbox_set = systems::g_hitboxes.query( game_scene_node );
				if ( hitbox_set.count <= 0 )
				{
					continue;
				}

				record->apply( );
				const auto skeleton = g_shared.lc( ).get_skeleton( *record );

				for ( auto i = 0; i < hitbox_set.count; ++i )
				{
					const auto& hb = hitbox_set.entries[ i ];

					if ( hb.bone < 0 || hb.bone >= 28 )
					{
						continue;
					}

					const auto& bone = skeleton[ hb.bone ];
					if ( bone.position.length_sqr( ) < 1.0f )
					{
						continue;
					}

					const auto center = bone.rotation.rotate_vector( ( hb.mins + hb.maxs ) * 0.5f ) + bone.position;
					const auto aim = math::helpers::calculate_angle( eye, center );
					const auto fov = math::helpers::angle_distance( ctx.view_angles, aim );

					if ( fov > settings::g_combat.m_zeusbot.max_fov )
					{
						continue;
					}

					math::vector3 forward{};
					math::helpers::angle_vectors_left( aim, &forward );

					const auto trace = this->trace_taser_hit( eye, forward, shared_ctx.range * 0.85f, cand.pawn, local.pawn );
					if ( trace.hit_entity != cand.pawn )
					{
						continue;
					}

					const auto dist = ( center - eye ).length( );
					const auto range_fraction = dist / shared_ctx.range;

					scan_hit h{};
					h.position = center;
					h.aim_angle = aim;
					h.damage = 500.0f;
					h.score = ( 10000.0f - dist ) * ( range_fraction > 0.92f ? 0.8f : 1.0f );
					h.fov = fov;
					h.hitbox_index = hb.index;
					h.hitgroup = systems::g_hitboxes.hitgroup_from_hitbox( hb.index );
					h.bone_index = hb.bone;
					h.hitbox = hb;
					h.is_center = true;
					h.pawn = cand.pawn;
					h.entity_index = cand.entity_index;
					h.health = cand.health;
					h.record = record;

					results.push_back( h );
				}

				record->restore( );
			}
		}

		return results;
	}

	rage::knife_info rage::get_knife_info( const systems::local::snapshot& local ) const
	{
		const auto& shared_ctx = g_shared.ctx( );
		const auto tick_base = memory::read<int>( local.controller + SCHEMA( "CBasePlayerController", "m_nTickBase"_hash ) );
		const auto next_primary = memory::read<int>( shared_ctx.weapon + SCHEMA( "C_BasePlayerWeapon", "m_nNextPrimaryAttackTick"_hash ) );
		const auto next_secondary = memory::read<int>( shared_ctx.weapon + SCHEMA( "C_BasePlayerWeapon", "m_nNextSecondaryAttackTick"_hash ) );
		const auto last_shot_time = memory::read<float>( shared_ctx.weapon + SCHEMA( "C_CSWeaponBase", "m_fLastShotTime"_hash ) );
		const auto cur_time = static_cast< float >( tick_base ) * cstypes::tick_interval;

		return knife_info
		{
			.can_slash = tick_base >= next_primary,
			.can_stab = tick_base >= next_secondary,
			.charged = ( cur_time - last_shot_time ) > 0.4f,
			.armor_ratio = memory::read<float>( shared_ctx.weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_flArmorRatio"_hash ) )
		};
	}

	std::vector<rage::scan_hit> rage::scan_knife( const math::vector3& eye, const aim_context& ctx, const knife_info& info, std::vector<candidate>& candidates, const systems::local::snapshot& local ) const
	{
		constexpr auto stab_range{ 50.0f };
		constexpr auto slash_range{ 66.0f };

		std::vector<scan_hit> results;

		for ( auto& cand : candidates )
		{
			const auto eye_angles = memory::read<math::vector3>( cand.pawn + SCHEMA( "C_CSPlayerPawn", "m_angEyeAngles"_hash ) );
			const auto hp = static_cast< float >( cand.health );

			const auto frontal_slash_dmg = this->get_knife_damage( info.charged ? 40.0f : 25.0f, cand.armor, info.armor_ratio );
			const auto frontal_stab_dmg = this->get_knife_damage( 65.0f, cand.armor, info.armor_ratio );
			const auto frontal_can_kill = ( info.can_slash && frontal_slash_dmg >= hp ) || ( info.can_stab && frontal_stab_dmg >= hp );

			for ( auto ri = 0; ri < cand.record_count; ++ri )
			{
				auto* record = cand.records[ ri ];
				if ( !record || !record->valid )
				{
					continue;
				}

				const auto game_scene_node = memory::read<std::uintptr_t>( cand.pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
				if ( !game_scene_node )
				{
					continue;
				}

				const auto hitbox_set = systems::g_hitboxes.query( game_scene_node );
				if ( hitbox_set.count <= 0 )
				{
					continue;
				}

				record->apply( );
				const auto skeleton = g_shared.lc( ).get_skeleton( *record );

				auto backstab{ false };
				{
					const auto delta = record->origin - systems::g_prediction.pre( ).origin;
					const auto dist_2d = std::sqrtf( delta.x * delta.x + delta.y * delta.y );

					if ( dist_2d > 0.001f )
					{
						const auto dir_x = delta.x / dist_2d;
						const auto dir_y = delta.y / dist_2d;

						math::vector3 body_forward{};
						math::helpers::angle_vectors_left( record->rotation, &body_forward );

						math::vector3 eye_forward{};
						math::helpers::angle_vectors_left( eye_angles, &eye_forward );

						backstab = ( dir_x * body_forward.x + dir_y * body_forward.y ) > 0.475f ||
							( dir_x * eye_forward.x + dir_y * eye_forward.y ) > 0.475f;
					}
				}

				const auto wait_for_backstab = backstab && !frontal_can_kill;

				for ( auto i = 0; i < hitbox_set.count; ++i )
				{
					const auto& hb = hitbox_set.entries[ i ];

					if ( hb.bone < 0 || hb.bone >= 28 )
					{
						continue;
					}

					const auto& bone = skeleton[ hb.bone ];
					if ( bone.position.length_sqr( ) < 1.0f )
					{
						continue;
					}

					const auto center = bone.rotation.rotate_vector( ( hb.mins + hb.maxs ) * 0.5f ) + bone.position;
					const auto dist = ( center - eye ).length( );
					const auto max_reach = info.can_slash ? slash_range : stab_range;

					if ( dist > max_reach )
					{
						continue;
					}

					const auto aim = math::helpers::calculate_angle( eye, center );
					const auto fov = math::helpers::angle_distance( ctx.view_angles, aim );

					if ( fov > settings::g_combat.m_knifebot.max_fov )
					{
						continue;
					}

					math::vector3 forward{};
					math::helpers::angle_vectors_left( aim, &forward );

					for ( const auto try_stab : { true, false } )
					{
						if ( try_stab && !info.can_stab )
						{
							continue;
						}

						if ( !try_stab && !info.can_slash )
						{
							continue;
						}

						const auto reach = try_stab ? stab_range : slash_range;
						if ( dist > reach )
						{
							continue;
						}

						const auto raw_dmg = try_stab ? ( backstab ? 180.0f : 65.0f ) : ( backstab ? 90.0f : ( info.charged ? 40.0f : 25.0f ) );
						const auto damage = this->get_knife_damage( raw_dmg, cand.armor, info.armor_ratio );
						const auto can_kill = damage >= hp;

						if ( wait_for_backstab && !can_kill )
						{
							continue;
						}

						const auto trace = this->trace_knife_hit( eye, forward, reach, cand.pawn, local.pawn );
						if ( trace.hit_entity != cand.pawn )
						{
							continue;
						}

						const auto reach_margin = 1.0f - ( dist / reach );

						scan_hit h{};
						h.position = center;
						h.aim_angle = aim;
						h.damage = damage;
						h.score = can_kill ? ( 10000.0f + damage * reach_margin ) : ( damage * 100.0f * reach_margin );
						h.fov = fov;
						h.hitbox_index = hb.index;
						h.hitgroup = systems::g_hitboxes.hitgroup_from_hitbox( hb.index );
						h.bone_index = hb.bone;
						h.hitbox = hb;
						h.is_center = true;
						h.is_backstab = backstab;
						h.attack_type = try_stab ? 1 : 0;
						h.pawn = cand.pawn;
						h.entity_index = cand.entity_index;
						h.health = cand.health;
						h.record = record;

						results.push_back( h );
						break;
					}
				}

				record->restore( );
			}
		}

		return results;
	}

	void rage::fire_gun( systems::input::usercmd* cmd, const target& tgt, bool was_forced, const math::vector3& requested_shoot_eye, const math::vector3& command_aim_punch, const systems::local::snapshot& local, bool subtick_attack )
	{
		// 开火阶段固定本地眼位、目标记录、穿透结果和输入历史，确保提交给服务器的是同一条射线。
		const auto cached_pair_record = this->m_doubletap_followup_fire
			&& tgt.hit.record && tgt.hit.record->valid;
		if ( !tgt.hit.record || ( !tgt.hit.record->is_valid( ) && !cached_pair_record ) )
		{
			return;
		}

		// 开火必须沿用扫描阶段的眼位；只有调用方未提供有效眼位时才回退实时读取。
		auto canonical_shoot_eye = requested_shoot_eye;
		const auto finite_eye = [ ]( const math::vector3& value )
		{
			return std::isfinite( value.x ) && std::isfinite( value.y ) &&
				std::isfinite( value.z ) && value.length_sqr( ) >= 1.0f;
		};
		// requested_shoot_eye 来自本次扫描使用的命令原点。
		// 开火阶段不要再次读取实体眼位，避免预测场景节点残留造成
		// 扫描起点、穿透起点与最终提交起点不一致。
		if ( !finite_eye( canonical_shoot_eye ) )
		{
			canonical_shoot_eye = g_shared.get_shoot_position( );
			if ( !finite_eye( canonical_shoot_eye ) )
				return;
		}
		#if defined( DEV )
		{
			const auto live_command_eye = g_shared.get_shoot_position( );
			if ( finite_eye( live_command_eye ) )
			{
				const auto eye_delta = ( live_command_eye - canonical_shoot_eye ).length( );
				if ( eye_delta > 0.5f )
				{
					diag::writef( diag::level::warning,
						"[rage_eye_sync] requested=(%.2f,%.2f,%.2f) live=(%.2f,%.2f,%.2f) delta=%.3f",
						canonical_shoot_eye.x, canonical_shoot_eye.y, canonical_shoot_eye.z,
						live_command_eye.x, live_command_eye.y, live_command_eye.z, eye_delta );
				}
			}
		}
		#endif
		this->m_double_tap.set_fire_position( canonical_shoot_eye );

		auto shoot_eye = canonical_shoot_eye;

		this->m_firing_this_tick = true;

		const auto base = cmd->csgo_user_cmd.mutable_base( );
		if ( !base )
		{
			this->m_firing_this_tick = false;
			return;
		}
		const auto tick_base = memory::read<int>( local.controller + SCHEMA( "CBasePlayerController", "m_nTickBase"_hash ) );
		const auto command_tick = base ? base->client_tick( ) : tick_base;
		auto& shared_ctx = g_shared.ctx( );
		const auto& config = settings::g_combat.m_ragebot.get_group( shared_ctx.weapon_type );
		const auto fire_tick_interval = std::isfinite( shared_ctx.tick_interval ) && shared_ctx.tick_interval > 0.0001f
			? shared_ctx.tick_interval
			: cstypes::tick_interval;
		// NS 也沿用扫描阶段的同一眼位，保证种子、穿透和最终命令一致。
		// NS 不使用本地预测时，攻击命令中的移动会在服务端先推进
		// 一 tick，再计算射击原点。清除该命令的移动分量，确保射击
		// 使用当前掩体边缘位置，而不是下一 tick 的位置。
		// 移动输入由 autostop 单独处理，开火阶段不再覆盖玩家的移动命令。
		const auto backtrack_enabled = settings::g_combat.m_lagcomp.backtrack.value;
		if ( config.no_spread.value )
		{
			const auto current_eye = canonical_shoot_eye;
			if ( finite_eye( current_eye ) )
			{
				shoot_eye = current_eye;
			}
			else if ( backtrack_enabled )
			{
				const auto fallback_eye = g_shared.get_shoot_position( );
				if ( finite_eye( fallback_eye ) )
				{
					shoot_eye = fallback_eye;
				}
			}
		}
		const auto extrapolation_enabled = settings::g_combat.m_lagcomp.extrapolation.value
			&& !config.no_spread.value;
		if ( shared_ctx.weapon && shared_ctx.weapon_vdata )
		{
			g_shared.pen( ).prepare( shared_ctx.weapon_vdata, shared_ctx.weapon );
		}
		auto* fire_record = tgt.hit.record;
		const auto selected_record = tgt.hit.record;
		const auto command_reference_tick = command_tick > 0 ? command_tick : g_shared.ctx( ).current_tick;
		const auto selected_record_is_backtrack = backtrack_enabled && selected_record &&
			selected_record->valid && !selected_record->extrapolated && selected_record->tick > 0 &&
			command_reference_tick > selected_record->tick;
		const auto selected_record_time = selected_record_is_backtrack
			? cstypes::tick_fraction::from_value( selected_record->simulation_time / fire_tick_interval )
			: cstypes::tick_fraction{};
		std::optional<shared::lagcomp::record> fire_prediction{};

		const auto verify_scene_node = fire_record->game_scene_node
			? fire_record->game_scene_node
			: memory::read<std::uintptr_t>( tgt.hit.pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
		auto verify_hitboxes = verify_scene_node
			? systems::g_hitboxes.query( verify_scene_node )
			: systems::hitboxes::set{};
		const systems::hitboxes::set* verify_hitboxes_ptr = verify_hitboxes.count > 0 ? &verify_hitboxes : nullptr;
		const auto refresh_fire_hitboxes = [ & ]( )
		{
			const auto scene_node = fire_record && fire_record->game_scene_node
				? fire_record->game_scene_node
				: memory::read<std::uintptr_t>( tgt.hit.pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
			verify_hitboxes = scene_node ? systems::g_hitboxes.query( scene_node ) : systems::hitboxes::set{};
			verify_hitboxes_ptr = verify_hitboxes.count > 0 ? &verify_hitboxes : nullptr;
		};
		const auto aim_punch = config.no_spread.value
			? command_aim_punch
			: g_shared.get_aim_punch( local.pawn );
		auto aim_position = tgt.hit.position;

		if ( extrapolation_enabled && !selected_record_is_backtrack && !tgt.hit.record->extrapolated &&
			tgt.hit.bone_index >= 0 && tgt.hit.bone_index < 28 )
		{
			std::lock_guard lock( this->m_extrapolated_mtx );
			for ( const auto& predicted : this->m_extrapolated_records )
			{
				if ( predicted.pawn != tgt.hit.pawn || !predicted.valid || !predicted.extrapolated ||
					predicted.bone_count <= tgt.hit.bone_index || tgt.hit.record->bone_count <= tgt.hit.bone_index )
				{
					continue;
				}

				const auto& source_bone = tgt.hit.record->bones[ tgt.hit.bone_index ];
				const auto& predicted_bone = predicted.bones[ tgt.hit.bone_index ];
				const auto bone_delta = predicted_bone.position - source_bone.position;
				if ( !std::isfinite( bone_delta.x ) || !std::isfinite( bone_delta.y ) ||
					!std::isfinite( bone_delta.z ) || bone_delta.length_sqr( ) > 192.0f * 192.0f )
				{
					break;
				}

				auto source_inverse = source_bone.rotation;
				source_inverse.x = -source_inverse.x;
				source_inverse.y = -source_inverse.y;
				source_inverse.z = -source_inverse.z;
				const auto local_point = source_inverse.rotate_vector( tgt.hit.position - source_bone.position );
				const auto predicted_position = predicted_bone.rotation.rotate_vector( local_point ) + predicted_bone.position;
				if ( !std::isfinite( predicted_position.x ) || !std::isfinite( predicted_position.y ) ||
					!std::isfinite( predicted_position.z ) )
				{
					break;
				}
				auto predicted_ctx = g_shared.pen( ).prepare_target( tgt.hit.pawn, const_cast< shared::lagcomp::record* >( &predicted ), verify_hitboxes_ptr );
				if ( g_shared.pen( ).first_hitbox( shoot_eye, predicted_position, predicted_ctx ) != tgt.hit.hitbox_index )
				{
					break;
				}

				fire_prediction = predicted;
				fire_record = &*fire_prediction;
				aim_position = predicted_position;
				break;
			}
		}

		auto fire_pose_correction_ticks = 0;
		auto fire_pose_correction = math::vector3{};
		if ( extrapolation_enabled && !selected_record_is_backtrack && fire_record && fire_record->extrapolated &&
			tgt.hit.bone_index >= 0 && tgt.hit.bone_index < 28 )
		{
			const auto live_velocity = memory::read<math::vector3>( tgt.hit.pawn + SCHEMA( "C_BaseEntity", "m_vecVelocity"_hash ) );
			const auto finite_velocity = [ ]( const math::vector3& value )
			{
				return std::isfinite( value.x ) && std::isfinite( value.y ) && std::isfinite( value.z ) &&
					value.length_2d( ) <= 4096.0f;
			};

			if ( finite_velocity( live_velocity ) && live_velocity.length_2d( ) >= 16.0f )
			{
				const auto command_gap = command_tick > fire_record->tick
					? std::clamp( command_tick - fire_record->tick, 0, 1 )
					: 0;
				auto correction = live_velocity * ( static_cast< float >( command_gap ) * cstypes::tick_interval );
				const auto scene_node = fire_record->game_scene_node
					? fire_record->game_scene_node
					: memory::read<std::uintptr_t>( tgt.hit.pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
				if ( scene_node )
				{
					const auto live_origin = memory::read<math::vector3>( scene_node + SCHEMA( "CGameSceneNode", "m_vecAbsOrigin"_hash ) );
					const auto residual = live_origin - fire_record->origin;
					const auto residual_limit = std::max( 24.0f, live_velocity.length_2d( ) * cstypes::tick_interval * 4.0f );
					if ( std::isfinite( residual.x ) && std::isfinite( residual.y ) && std::isfinite( residual.z ) &&
						residual.length_2d( ) <= residual_limit )
					{
						const auto heading_projection = residual.x * live_velocity.x + residual.y * live_velocity.y;
						if ( heading_projection > 0.0f )
						{
							correction = residual;
						}
					}
				}

				const auto max_correction = std::clamp( live_velocity.length_2d( ) * cstypes::tick_interval * 4.0f, 12.0f, 64.0f );
				const auto correction_speed = correction.length_2d( );
				if ( std::isfinite( correction_speed ) && correction_speed > 0.5f )
				{
					if ( correction_speed > max_correction )
					{
						correction *= max_correction / correction_speed;
					}

					auto adjusted = *fire_record;
					adjusted.origin += correction;
					adjusted.simulation_time += cstypes::tick_interval;
					adjusted.tick += 1;
					adjusted.extrapolated_ticks = std::min( adjusted.extrapolated_ticks + 1, 32 );
					adjusted.velocity = live_velocity;
					adjusted.extrapolated = true;
					for ( auto i = 0; i < adjusted.bone_count && i < 128; ++i )
					{
						adjusted.bones[ i ].position += correction;
					}

					auto adjusted_ctx = g_shared.pen( ).prepare_target( tgt.hit.pawn, &adjusted, verify_hitboxes_ptr );
					const auto adjusted_position = aim_position + correction;
					if ( g_shared.pen( ).first_hitbox( shoot_eye, adjusted_position, adjusted_ctx ) == tgt.hit.hitbox_index )
					{
						fire_prediction = std::move( adjusted );
						fire_record = &*fire_prediction;
						aim_position = adjusted_position;
						fire_pose_correction = correction;
						fire_pose_correction_ticks = 1;
					}
				}
			}
		}

#if defined( DEV )
		{
			const auto server_tick = g_shared.sh( ).server_tick( );
			const auto processing_tick = command_tick > 0 ? command_tick : server_tick;
			diag::writef(
				diag::level::info,
				"[rage_pose_align] pawn=%llx scan_tick=%d fire_tick=%d extrap_ticks=%d processing=%d correction_ticks=%d correction=(%.2f,%.2f,%.2f) velocity=(%.2f,%.2f,%.2f) aim=(%.2f,%.2f,%.2f) eye=(%.2f,%.2f,%.2f)",
				static_cast< unsigned long long >( tgt.hit.pawn ),
				tgt.hit.record ? tgt.hit.record->tick : -1,
				fire_record ? fire_record->tick : -1,
				fire_record ? fire_record->extrapolated_ticks : 0,
				processing_tick,
				fire_pose_correction_ticks,
				fire_pose_correction.x,
				fire_pose_correction.y,
				fire_pose_correction.z,
				fire_record ? fire_record->velocity.x : 0.0f,
				fire_record ? fire_record->velocity.y : 0.0f,
				fire_record ? fire_record->velocity.z : 0.0f,
				aim_position.x,
				aim_position.y,
				aim_position.z,
				shoot_eye.x,
				shoot_eye.y,
				shoot_eye.z );
		}
#endif

		const auto fire_record_age = fire_record && command_reference_tick > fire_record->tick
			? command_reference_tick - fire_record->tick
			: 0;
		const auto needs_fire_record_refresh = fire_record_age > 0;

		if ( !selected_record_is_backtrack && !fire_prediction.has_value( ) && needs_fire_record_refresh && fire_record &&
			!fire_record->extrapolated && tgt.hit.bone_index >= 0 && tgt.hit.bone_index < 28 )
		{
			shared::lagcomp::record refreshed_record{};
			if ( refreshed_record.setup( tgt.hit.pawn ) && refreshed_record.valid &&
				refreshed_record.tick >= fire_record->tick && refreshed_record.bone_count > tgt.hit.bone_index )
			{
				const auto& source_bone = fire_record->bones[ tgt.hit.bone_index ];
				const auto& refreshed_bone = refreshed_record.bones[ tgt.hit.bone_index ];
				auto source_inverse = source_bone.rotation;
				source_inverse.x = -source_inverse.x;
				source_inverse.y = -source_inverse.y;
				source_inverse.z = -source_inverse.z;
				const auto local_point = source_inverse.rotate_vector( tgt.hit.position - source_bone.position );
				const auto refreshed_position = refreshed_bone.rotation.rotate_vector( local_point ) + refreshed_bone.position;

				if ( std::isfinite( refreshed_position.x ) && std::isfinite( refreshed_position.y ) &&
					std::isfinite( refreshed_position.z ) )
				{
					auto refreshed_context = g_shared.pen( ).prepare_target(
						tgt.hit.pawn, &refreshed_record, verify_hitboxes_ptr );
					if ( g_shared.pen( ).first_hitbox( shoot_eye, refreshed_position, refreshed_context ) == tgt.hit.hitbox_index )
					{
						fire_prediction = std::move( refreshed_record );
						fire_record = &*fire_prediction;
						aim_position = refreshed_position;
#if defined( DEV )
						diag::writef(
							diag::level::info,
							"[rage_live_record] pawn=%llx source=%d refreshed=%d",
							static_cast< unsigned long long >( tgt.hit.pawn ),
							tgt.hit.record->tick,
							fire_record->tick );
#endif
					}
				}
			}
		}

		if ( !config.no_spread.value && !extrapolation_enabled && !selected_record_is_backtrack && !fire_prediction.has_value( ) && needs_fire_record_refresh && tgt.hit.record &&
			!tgt.hit.record->extrapolated && !tgt.hit.penetrated &&
			tgt.hit.bone_index >= 0 && tgt.hit.bone_index < 28 )
		{
			const auto scene_node = fire_record->game_scene_node
				? fire_record->game_scene_node
				: memory::read<std::uintptr_t>( tgt.hit.pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
			if ( scene_node )
			{
				const auto live_origin = memory::read<math::vector3>( scene_node + SCHEMA( "CGameSceneNode", "m_vecAbsOrigin"_hash ) );
				const auto residual = live_origin - fire_record->origin;
				const auto residual_speed = residual.length_2d( );
				const auto max_residual = 32.0f;
				if ( std::isfinite( residual.x ) && std::isfinite( residual.y ) && std::isfinite( residual.z ) &&
					residual_speed > 0.5f && residual_speed <= max_residual )
				{
					auto adjusted = *fire_record;
					adjusted.origin += residual;
					adjusted.extrapolated = false;
					adjusted.extrapolated_ticks = 0;
					for ( auto bone_index = 0; bone_index < adjusted.bone_count && bone_index < 128; ++bone_index )
						adjusted.bones[ bone_index ].position += residual;

					auto adjusted_ctx = g_shared.pen( ).prepare_target( tgt.hit.pawn, &adjusted, verify_hitboxes_ptr );
					const auto adjusted_position = aim_position + residual;
					if ( g_shared.pen( ).first_hitbox( shoot_eye, adjusted_position, adjusted_ctx ) == tgt.hit.hitbox_index )
					{
						fire_prediction = std::move( adjusted );
						fire_record = &*fire_prediction;
						aim_position = adjusted_position;
#if defined( DEV )
						diag::writef(
							diag::level::info,
							"[rage_live_sync] pawn=%llx record=%d residual=(%.2f,%.2f,%.2f)",
							static_cast< unsigned long long >( tgt.hit.pawn ),
							fire_record->tick,
							residual.x, residual.y, residual.z );
#endif
					}
				}
			}
		}


		auto aim_angle = math::helpers::calculate_angle( shoot_eye, aim_position );
		auto punched_aim = math::vector3{ aim_angle.x - aim_punch.x, aim_angle.y - aim_punch.y, 0.0f };
		auto seed_base_angle = punched_aim;
		auto submitted_angle = punched_aim;
		auto fire_angle = aim_angle;
		auto trajectory_angle = submitted_angle;
		// Keep the exact ray used by the verification trace.  This is also the
		// direction consumed by impact matching; it must include the current shot's
		// spread instead of being reconstructed later from view angles.
		math::vector3 trajectory_direction{};
		auto committed_damage = tgt.hit.damage;
		auto committed_raw_damage = 0.0f;
		auto committed_hitgroup = tgt.hit.hitgroup;
		auto committed_hitbox = tgt.hit.hitbox;
		auto committed_wall_count = tgt.hit.penetrated ? 1 : 0;
		auto committed_penetrated = tgt.hit.penetrated;

			const auto commit_verified_result = [ & ]( const shared::penetration::result& verified,
				const shared::penetration::run_context& penetration_context )
			{
				const auto current_health = memory::read<int>( tgt.hit.pawn + SCHEMA( "C_BaseEntity", "m_iHealth"_hash ) );
				if ( current_health <= 0 || !std::isfinite( verified.damage ) || verified.damage <= 0.0f ||
					verified.hitbox < 0 || verified.hitgroup <= 0 )
				{
					return false;
				}
				// 使用扫描命中携带的阈值，避免提交阶段重新读取到下一 tick
				// 的生命值后改变 min damage 判定。
				const auto required_damage = this->get_min_damage(
					config,
					current_health,
					config.min_damage_override.value );
				if ( verified.damage + 0.001f < required_damage )
				{
#if defined( DEV )
					diag::writef(
						diag::level::info,
						"[rage_pen_min_damage_reject] pawn=%llx record=%d damage=%.2f required=%.2f health=%d penetrated=%u",
						static_cast< unsigned long long >( tgt.hit.pawn ),
						fire_record ? fire_record->tick : -1,
						verified.damage,
						required_damage,
						current_health,
						verified.penetrated ? 1u : 0u );
#endif
					return false;
				}

				committed_damage = verified.damage;
				committed_raw_damage = verified.raw_damage;
				committed_hitgroup = verified.hitgroup;
				committed_wall_count = verified.wall_count;
				committed_penetrated = verified.penetrated;

				for ( const auto& hitbox : penetration_context.hitboxes )
				{
					if ( hitbox.index == verified.hitbox )
					{
						committed_hitbox = hitbox;
						break;
					}
				}

				return true;
			};

		const auto log_trace_failure = [ & ]( const char* phase, const math::vector3& end )
			{
#if defined( DEV )
				diag::writef(
					diag::level::info,
					"[rage_trace_fail] phase=%s pawn=%llx record=%d extrapolated=%u requested_hitbox=%d requested_group=%d penetrated=%u eye=(%.2f,%.2f,%.2f) end=(%.2f,%.2f,%.2f) aim=(%.2f,%.2f,%.2f) target_origin=(%.2f,%.2f,%.2f) target_speed=%.2f",
					phase,
					static_cast< unsigned long long >( tgt.hit.pawn ),
					fire_record ? fire_record->tick : -1,
					fire_record && fire_record->extrapolated ? 1u : 0u,
					tgt.hit.hitbox_index,
					tgt.hit.hitgroup,
					tgt.hit.penetrated ? 1u : 0u,
					shoot_eye.x, shoot_eye.y, shoot_eye.z,
					end.x, end.y, end.z,
					aim_position.x, aim_position.y, aim_position.z,
					fire_record ? fire_record->origin.x : 0.0f,
					fire_record ? fire_record->origin.y : 0.0f,
					fire_record ? fire_record->origin.z : 0.0f,
					fire_record ? fire_record->velocity.length_2d( ) : 0.0f );
#else
				(void)phase;
				(void)end;
#endif
			};

		const auto has_hitbox_margin = [ & ]( const math::vector3& ray_delta )
			{
				if ( tgt.hit.bone_index < 0 || tgt.hit.bone_index >= 28 || tgt.hit.hitbox.radius <= 0.35f )
				{
					return true;
				}

				const auto& bone = fire_record->bones[ tgt.hit.bone_index ];
				const auto capsule_start = bone.rotation.rotate_vector( tgt.hit.hitbox.mins ) + bone.position;
				const auto capsule_end = bone.rotation.rotate_vector( tgt.hit.hitbox.maxs ) + bone.position;
				auto fraction{ 1.0f };
				return g_shared.ray_vs_capsule(
					shoot_eye,
					ray_delta,
					capsule_start,
					capsule_end,
					tgt.hit.hitbox.radius - 0.30f,
					fraction );
			};
		const auto resolve_nospread_time = [ & ]( )
		{
			auto resolved_tick = 0;
			auto resolved_fraction = 0.0f;
			// Match the engine contract used by the reference implementation:
			// prefer the command history tick that will carry the attack, then the
			// base command tick, and finally the local/global fallbacks.
			for ( auto i = 0; i < cmd->csgo_user_cmd.input_history_size( ); ++i )
			{
				const auto entry = cmd->csgo_user_cmd.mutable_input_history( i );
				if ( !entry )
					continue;
				if ( entry->has_player_tick_count( ) && entry->player_tick_count( ) > 0 )
				{
					resolved_tick = entry->player_tick_count( );
					resolved_fraction = entry->has_player_tick_fraction( )
						? std::clamp( entry->player_tick_fraction( ), 0.0f, 0.999999f )
						: 0.0f;
					break;
				}
				if ( entry->has_render_tick_count( ) && entry->render_tick_count( ) > 0 )
				{
					resolved_tick = entry->render_tick_count( );
					resolved_fraction = entry->has_render_tick_fraction( )
						? std::clamp( entry->render_tick_fraction( ), 0.0f, 0.999999f )
						: 0.0f;
					break;
				}
			}
			if ( resolved_tick <= 0 )
				resolved_tick = command_tick;
			if ( resolved_tick <= 0 )
				resolved_tick = tick_base;
			if ( resolved_tick <= 0 )
				resolved_tick = shared_ctx.current_tick;
			return std::pair{ resolved_tick, resolved_fraction };
		};
		const auto nospread_time = resolve_nospread_time( );
		auto spread_seed_tick = config.no_spread.value
			? nospread_time.first
			: ( command_tick > 0 ? command_tick : tick_base );
		auto spread_seed_fraction = config.no_spread.value ? nospread_time.second : 0.0f;
		if ( config.doubletap.value && !config.no_spread.value )
		{
			const auto dt_claim = this->m_double_tap.claimed_tick( );
			if ( dt_claim > 0 && dt_claim <= tick_base + 256 )
			{
				spread_seed_tick = dt_claim;
				spread_seed_fraction = std::clamp( this->m_double_tap.next_attack_fraction( ), 0.0f, 0.999999f );
			}
		}
		auto history_seed_tick = spread_seed_tick;
		auto history_seed_fraction = spread_seed_fraction;


		if ( !config.no_spread.value )
		{
			refresh_fire_hitboxes( );
		}
		if ( shared_ctx.weapon && shared_ctx.weapon_vdata )
		{
			g_shared.pen( ).prepare( shared_ctx.weapon_vdata, shared_ctx.weapon );
		}

		if ( config.no_spread.value )
		{
			const auto scanned_inaccuracy = shared_ctx.inaccuracy;
			const auto scanned_spread = shared_ctx.spread;

			if ( !std::isfinite( shared_ctx.inaccuracy ) || !std::isfinite( shared_ctx.spread ) ||
				!std::isfinite( shared_ctx.recoil_index ) )
			{
				this->m_firing_this_tick = false;
				return;
			}

#if defined( DEV )
			diag::writef(
				diag::level::info,
				"[nospread_accuracy] tick=%d scanned=(%.6f,%.6f) fire=(%.6f,%.6f) recoil=%.3f delta=(%.6f,%.6f)",
				tick_base,
				scanned_inaccuracy,
				scanned_spread,
				scanned_inaccuracy,
				scanned_spread,
				shared_ctx.recoil_index,
				0.0f,
				0.0f );
#endif

			const auto claim_tick = spread_seed_tick;

			history_seed_tick = claim_tick;
			history_seed_fraction = spread_seed_fraction;

			// Recheck readiness against the exact timestamp that will be serialized
			// into the attack history. Earlier gates run before this timestamp is
			// finalized and therefore are insufficient for a deterministic shot.
			const auto revolver_secondary = shared_ctx.item_def_idx == cstypes::item_definition_index::weapon_r8_revolver;
			const auto next_attack_tick = revolver_secondary
				? memory::read<int>( shared_ctx.weapon + SCHEMA( "C_BasePlayerWeapon", "m_nNextSecondaryAttackTick"_hash ) )
				: memory::read<int>( shared_ctx.weapon + SCHEMA( "C_BasePlayerWeapon", "m_nNextPrimaryAttackTick"_hash ) );
			auto next_attack_fraction = revolver_secondary
				? memory::read<float>( shared_ctx.weapon + SCHEMA( "C_BasePlayerWeapon", "m_flNextSecondaryAttackTickRatio"_hash ) )
				: memory::read<float>( shared_ctx.weapon + SCHEMA( "C_BasePlayerWeapon", "m_flNextPrimaryAttackTickRatio"_hash ) );
			if ( !std::isfinite( next_attack_fraction ) )
			{
				this->m_firing_this_tick = false;
				return;
			}
			next_attack_fraction = std::clamp( next_attack_fraction, 0.0f, 0.999999f );
			if ( claim_tick < next_attack_tick ||
				( claim_tick == next_attack_tick && history_seed_fraction + 0.000001f < next_attack_fraction ) )
			{
				this->m_firing_this_tick = false;
				return;
			}

			// The server applies aim punch before hashing the firing angle for the
			// spread seed. Solve the fixed point in that final firing-angle domain,
			// then subtract punch only when serializing the command angle.
			auto selected_seed = g_shared.get_spread_seed( fire_angle, claim_tick );
			if ( selected_seed == 0 )
			{
				this->m_firing_this_tick = false;
				return;
			}
			const auto selected_spread = g_shared.calculate_spread(
				static_cast< int >( selected_seed ),
				shared_ctx.inaccuracy,
				shared_ctx.spread,
				shared_ctx.recoil_index,
				shared_ctx.item_def_idx,
				shared_ctx.num_bullets );
			auto corrected = fire_angle;
			corrected.x += math::helpers::rad_to_deg(
				std::atan( std::sqrt( selected_spread.x * selected_spread.x + selected_spread.y * selected_spread.y ) ) );
			corrected.z = -math::helpers::rad_to_deg( std::atan2( selected_spread.x, selected_spread.y ) );
			if ( g_shared.get_spread_seed( corrected, claim_tick ) != selected_seed )
			{
				corrected = g_shared.find_spread_correction( fire_angle, claim_tick, config.nospread_quality.value );
			}

			if ( !std::isfinite( corrected.x ) || !std::isfinite( corrected.y ) ||
				!std::isfinite( corrected.z ) ||
				( std::fabsf( corrected.x ) < 0.0001f && std::fabsf( corrected.y ) < 0.0001f &&
					std::fabsf( corrected.z ) < 0.0001f ) )
			{
				this->m_firing_this_tick = false;
				return;
			}
			trajectory_angle = corrected;
			fire_angle = corrected;
			punched_aim = math::vector3{
				corrected.x - aim_punch.x,
				corrected.y - aim_punch.y,
				0.0f };
			submitted_angle = math::vector3{
				punched_aim.x,
				punched_aim.y,
				corrected.z };
			const auto verified_seed = g_shared.get_spread_seed( trajectory_angle, claim_tick );
			if ( verified_seed == 0 )
			{
				this->m_firing_this_tick = false;
				return;
			}
			const auto nospread_spread = g_shared.calculate_spread(
				static_cast< int >( verified_seed ),
				shared_ctx.inaccuracy,
				shared_ctx.spread,
				shared_ctx.recoil_index,
				shared_ctx.item_def_idx,
				shared_ctx.num_bullets );
			math::vector3 nospread_forward{}, nospread_left{}, nospread_up{};
			math::helpers::angle_vectors_left( trajectory_angle, &nospread_forward, &nospread_left, &nospread_up );
			const auto bullet_delta = ( nospread_forward + nospread_left * nospread_spread.x + nospread_up * nospread_spread.y ).normalized( ) * shared_ctx.range;
			trajectory_direction = bullet_delta.normalized( );

#if defined( DEV )
			diag::writef(
				diag::level::info,
				"[nospread_contract] claim=%d+%.3f seed_base=(%.3f,%.3f,%.3f) punch=(%.3f,%.3f,%.3f) submitted=(%.3f,%.3f,%.3f) trajectory=(%.3f,%.3f,%.3f) seed=%u spread=(%.6f,%.6f) bullet=(%.5f,%.5f,%.5f)",
				claim_tick,
				history_seed_fraction,
				seed_base_angle.x, seed_base_angle.y, seed_base_angle.z,
				aim_punch.x, aim_punch.y, aim_punch.z,
				submitted_angle.x, submitted_angle.y, submitted_angle.z,
				trajectory_angle.x, trajectory_angle.y, trajectory_angle.z,
				verified_seed,
				nospread_spread.x, nospread_spread.y,
				trajectory_direction.x, trajectory_direction.y, trajectory_direction.z );
#endif

#if 0
			auto selected_seed = g_shared.get_spread_seed( seed_base_angle, claim_tick );
			if ( selected_seed == 0 )
			{
				this->m_firing_this_tick = false;
				return;
			}
			const auto spread = g_shared.calculate_spread(
				static_cast< int >( selected_seed ),
				shared_ctx.inaccuracy,
				shared_ctx.spread,
				shared_ctx.recoil_index,
				shared_ctx.item_def_idx,
				shared_ctx.num_bullets );
			auto corrected = seed_base_angle;
			math::vector3 spread_forward{}, spread_left{}, spread_up{};
			math::helpers::angle_vectors_left( seed_base_angle, &spread_forward, &spread_left, &spread_up );
			const auto compensated_direction =
				( spread_forward - spread_left * spread.x - spread_up * spread.y ).normalized( );
			corrected = math::helpers::vector_to_angle( compensated_direction );
			corrected.z = 0.0f;
			if ( g_shared.get_spread_seed( corrected, claim_tick ) != selected_seed && !config.forcenospread.value )
			{
					const auto searched = g_shared.find_spread_correction( seed_base_angle, claim_tick, config.nospread_quality.value );
				if ( std::isfinite( searched.x ) && std::isfinite( searched.y ) && std::isfinite( searched.z ) &&
					( std::fabsf( searched.x ) > 0.0001f || std::fabsf( searched.y ) > 0.0001f || std::fabsf( searched.z ) > 0.0001f ) )
				{
					corrected = searched;
				}
				else
				{
#if defined( DEV )
					diag::writef(
						diag::level::warning,
						"[nospread_correction_failed] claim=%d seed=%u direct=(%.3f,%.3f,%.3f)",
						claim_tick,
						selected_seed,
						corrected.x,
						corrected.y,
						corrected.z );
#endif
					this->m_firing_this_tick = false;
					return;
				}
			}
			const auto correction_found = std::isfinite( corrected.x )
				&& std::isfinite( corrected.y )
				&& std::isfinite( corrected.z )
				&& ( std::fabsf( corrected.x ) > 0.0001f || std::fabsf( corrected.y ) > 0.0001f || std::fabsf( corrected.z ) > 0.0001f );
			if ( !correction_found )
			{
				this->m_firing_this_tick = false;
				return;
			}
			const auto verified_seed = g_shared.get_spread_seed( corrected, claim_tick );


#if defined( DEV )
			diag::writef(
				diag::level::info,
				"[nospread_mode] forced=%u seed_test=%u mode=%d claim=%d seed=%u corrected_seed=%u direct_score=%.8f aim=(%.3f,%.3f,%.3f) corrected=(%.3f,%.3f,%.3f)",
				config.forcenospread.value ? 1u : 0u,
				0u,
				shared_ctx.weapon_mode,
				claim_tick,
				selected_seed,
				verified_seed,
				-1.0f,
				aim_angle.x, aim_angle.y, aim_angle.z,
				corrected.x, corrected.y, corrected.z );
#endif

			punched_aim = math::vector3{ corrected.x - aim_punch.x, corrected.y - aim_punch.y, 0.0f };
			submitted_angle = math::vector3{ punched_aim.x, punched_aim.y, corrected.z };
			trajectory_angle = fire_angle = corrected;
			 aim_angle = corrected;

			if ( tgt.hit.bone_index < 0 || tgt.hit.bone_index >= 28 )
			{
				this->m_firing_this_tick = false;
				return;
			}

#if defined( DEV )
			if ( verified_seed != selected_seed )
			{
				diag::writef(
					diag::level::info,
					"[nospread_seed_redirect] claim=%d selected=%u corrected=%u forced=%u fire=(%.3f,%.3f,%.3f)",
					claim_tick,
					selected_seed,
					verified_seed,
					config.forcenospread.value ? 1u : 0u,
					fire_angle.x, fire_angle.y, fire_angle.z );
			}
#endif
			const auto verified_spread = g_shared.calculate_spread(
				static_cast< int >( verified_seed ),
				shared_ctx.inaccuracy,
				shared_ctx.spread,
				shared_ctx.recoil_index,
				shared_ctx.item_def_idx,
				shared_ctx.num_bullets );
			math::vector3 forward{}, left{}, up{};
			math::helpers::angle_vectors_left( trajectory_angle, &forward, &left, &up );
			const auto bullet_delta = ( forward + left * verified_spread.x + up * verified_spread.y ).normalized( ) * shared_ctx.range;
		#if defined( DEV )
			diag::writef(
				diag::level::info,
				"[nospread_contract] claim=%d seed_base=(%.3f,%.3f,%.3f) corrected=(%.3f,%.3f,%.3f) submitted=(%.3f,%.3f,%.3f) trajectory=(%.3f,%.3f,%.3f) seed=%u spread=(%.6f,%.6f) bullet=(%.5f,%.5f,%.5f)",
				claim_tick,
				seed_base_angle.x, seed_base_angle.y, seed_base_angle.z,
				corrected.x, corrected.y, corrected.z,
				submitted_angle.x, submitted_angle.y, submitted_angle.z,
				trajectory_angle.x, trajectory_angle.y, trajectory_angle.z,
				verified_seed,
				verified_spread.x, verified_spread.y,
				bullet_delta.x, bullet_delta.y, bullet_delta.z );
		#endif
		#endif
			const auto local_velocity = systems::g_prediction.pre( ).velocity;
			const auto target_velocity = memory::read<math::vector3>( tgt.hit.pawn + SCHEMA( "C_BaseEntity", "m_vecVelocity"_hash ) );
			const auto finite_speed = [ ]( const math::vector3& value )
			{
				return std::isfinite( value.x ) && std::isfinite( value.y ) && std::isfinite( value.z ) &&
					value.length_2d( ) <= 4096.0f;
			};
			const auto current_tick = g_shared.ctx( ).current_tick;
			const auto record_age = fire_record && current_tick > 0
				? std::max( current_tick - fire_record->tick, 0 )
				: 0;
			const auto direct_revalidation = !tgt.hit.penetrated &&
				( ( finite_speed( target_velocity ) && target_velocity.length_2d( ) > 20.0f ) ||
					record_age > 1 || fire_prediction.has_value( ) );
			if ( tgt.hit.penetrated || direct_revalidation )
			{
				const auto nospread_penetration_context = g_shared.pen( ).prepare_target( tgt.hit.pawn, fire_record, verify_hitboxes_ptr );
				shared::penetration::result nospread_penetration{};
				if ( !g_shared.pen( ).run(
					shoot_eye,
					shoot_eye + bullet_delta,
					nospread_penetration_context,
					local.pawn,
					local.team,
					nospread_penetration,
					tgt.hit.hitbox_index ) )
				{
					log_trace_failure( direct_revalidation ? "nospread_direct_revalidate" : "nospread", shoot_eye + bullet_delta );
					this->m_firing_this_tick = false;
					return;
				}
				if ( !commit_verified_result( nospread_penetration, nospread_penetration_context ) )
				{
					this->m_firing_this_tick = false;
					return;
				}
			}
		}
		else if ( !this->m_doubletap_followup_fire )
		{
			const auto live_accuracy = g_shared.get_accuracy_sample( true );
			if ( !std::isfinite( live_accuracy.inaccuracy ) || !std::isfinite( live_accuracy.spread ) )
			{
				this->m_firing_this_tick = false;
				return;
			}

			shared_ctx.inaccuracy = live_accuracy.inaccuracy;
			shared_ctx.spread = live_accuracy.spread;
			shared_ctx.recoil_index = memory::read<float>( shared_ctx.weapon + SCHEMA( "C_CSWeaponBase", "m_flRecoilIndex"_hash ) );

			if ( !was_forced )
			{
				const auto needed_hc = config.hitchance_override.value
					? static_cast< float >( config.hitchance_override_value ) / 100.0f
					: static_cast< float >( config.hitchance ) / 100.0f;
				if ( tgt.hit.bone_index < 0 || tgt.hit.bone_index >= 28 ||
					g_shared.calculate_hitchance(
						shoot_eye,
						submitted_angle,
						tgt.hit.hitbox,
						fire_record->bones[ tgt.hit.bone_index ],
						shared_ctx.inaccuracy,
						shared_ctx.spread,
						256,
						needed_hc ) + 0.0001f < needed_hc )
				{
					this->m_firing_this_tick = false;
					return;
				}
			}

			const auto seed = g_shared.get_spread_seed( submitted_angle, spread_seed_tick );
			const auto shot_spread = g_shared.calculate_spread(
				static_cast< int >( seed ),
				shared_ctx.inaccuracy,
				shared_ctx.spread,
				shared_ctx.recoil_index,
				shared_ctx.item_def_idx,
				shared_ctx.num_bullets );

			math::vector3 forward{}, left{}, up{};
			math::helpers::angle_vectors_left( submitted_angle, &forward, &left, &up );
			const auto bullet_delta = ( forward + left * shot_spread.x + up * shot_spread.y ).normalized( ) * shared_ctx.range;
			trajectory_direction = bullet_delta.normalized( );
			if ( !has_hitbox_margin( bullet_delta ) )
			{
				this->m_firing_this_tick = false;
				return;
			}

			const auto spread_precheck_context = g_shared.pen( ).prepare_target( tgt.hit.pawn, fire_record, verify_hitboxes_ptr );
			const auto spread_first_hitbox = g_shared.pen( ).first_hitbox( shoot_eye, shoot_eye + bullet_delta, spread_precheck_context );
			if ( spread_first_hitbox < 0 )
			{
#if defined( DEV )
				diag::writef(
					diag::level::info,
					"[rage_occlusion_reject] pawn=%llx requested_hitbox=%d first_hitbox=%d record=%d",
					static_cast< unsigned long long >( tgt.hit.pawn ),
					tgt.hit.hitbox_index,
					spread_first_hitbox,
					fire_record ? fire_record->tick : -1 );
#endif
				this->m_firing_this_tick = false;
				return;
			}

#if defined( DEV )
			diag::writef(
				diag::level::info,
				"[spread_contract] tick=%d seed=%u fire=(%.3f,%.3f,%.3f) submitted=(%.3f,%.3f,%.3f) spread=(%.6f,%.6f)",
				spread_seed_tick,
				seed,
				fire_angle.x, fire_angle.y, fire_angle.z,
				submitted_angle.x, submitted_angle.y, submitted_angle.z,
				shot_spread.x, shot_spread.y );
#endif
			const auto penetration_context = g_shared.pen( ).prepare_target( tgt.hit.pawn, fire_record, verify_hitboxes_ptr );
			shared::penetration::result verified_penetration{};
			if ( !g_shared.pen( ).run(
				shoot_eye,
				shoot_eye + bullet_delta,
				penetration_context,
				local.pawn,
				local.team,
				verified_penetration,
				tgt.hit.hitbox_index ) )
			{
				log_trace_failure( "spread", shoot_eye + bullet_delta );
				this->m_firing_this_tick = false;
				return;
			}
			if ( !commit_verified_result( verified_penetration, penetration_context ) )
			{
				this->m_firing_this_tick = false;
				return;
			}
		}
		else
		{
			const auto followup_accuracy = g_shared.get_accuracy_sample( true );
			if ( !std::isfinite( followup_accuracy.inaccuracy ) || !std::isfinite( followup_accuracy.spread ) )
			{
				this->m_firing_this_tick = false;
				return;
			}
			shared_ctx.inaccuracy = followup_accuracy.inaccuracy;
			shared_ctx.spread = followup_accuracy.spread;
			shared_ctx.recoil_index = memory::read<float>( shared_ctx.weapon + SCHEMA( "C_CSWeaponBase", "m_flRecoilIndex"_hash ) );

			const auto followup_seed = g_shared.get_spread_seed( submitted_angle, spread_seed_tick );
			const auto followup_spread = g_shared.calculate_spread(
				static_cast< int >( followup_seed ),
				shared_ctx.inaccuracy,
				shared_ctx.spread,
				shared_ctx.recoil_index,
				shared_ctx.item_def_idx,
				shared_ctx.num_bullets );
			math::vector3 followup_forward{}, followup_left{}, followup_up{};
			math::helpers::angle_vectors_left( submitted_angle, &followup_forward, &followup_left, &followup_up );
			const auto followup_delta = ( followup_forward + followup_left * followup_spread.x + followup_up * followup_spread.y ).normalized( ) * shared_ctx.range;
			trajectory_direction = followup_delta.normalized( );
		if ( !has_hitbox_margin( followup_delta ) )
		{
			this->m_firing_this_tick = false;
			return;
		}

		const auto followup_context = g_shared.pen( ).prepare_target( tgt.hit.pawn, fire_record, verify_hitboxes_ptr );
		if ( g_shared.pen( ).first_hitbox( shoot_eye, shoot_eye + followup_delta, followup_context ) < 0 )
		{
			log_trace_failure( "doubletap_followup_occluded", shoot_eye + followup_delta );
			this->m_firing_this_tick = false;
			return;
		}

		shared::penetration::result followup_penetration{};
		if ( !g_shared.pen( ).run(
			shoot_eye,
			shoot_eye + followup_delta,
			followup_context,
			local.pawn,
			local.team,
			followup_penetration,
			tgt.hit.hitbox_index ) || !commit_verified_result( followup_penetration, followup_context ) )
		{
			log_trace_failure( "doubletap_followup", shoot_eye + followup_delta );
			this->m_firing_this_tick = false;
			return;
		}
		}

		if ( !committed_penetrated )
		{
			const auto live_health = memory::read<int>( tgt.hit.pawn + SCHEMA( "C_BaseEntity", "m_iHealth"_hash ) );
			const auto required_damage = this->get_min_damage(
				config,
				live_health,
				config.min_damage_override.value );
			if ( live_health <= 0 || !std::isfinite( committed_damage ) ||
				committed_damage + 0.001f < required_damage )
			{
#if defined( DEV )
				diag::writef(
					diag::level::info,
					"[rage_direct_min_damage_reject] pawn=%llx damage=%.2f required=%.2f health=%d record=%d",
					static_cast< unsigned long long >( tgt.hit.pawn ),
					committed_damage,
					required_damage,
					live_health,
					fire_record ? fire_record->tick : -1 );
#endif
				this->m_firing_this_tick = false;
				return;
			}
		}

#if defined( DEV )
		diag::writef(
			diag::level::info,
			"[rage_commit] pawn=%llx record=%d extrapolated=%u requested_hitbox=%d committed_hitbox=%d requested_group=%d committed_group=%d penetrated=%u walls=%d raw_damage=%.1f damage=%.1f eye=(%.2f,%.2f,%.2f) aim=(%.2f,%.2f,%.2f) target_origin=(%.2f,%.2f,%.2f)",
			static_cast< unsigned long long >( tgt.hit.pawn ),
			fire_record ? fire_record->tick : -1,
			fire_record && fire_record->extrapolated ? 1u : 0u,
			tgt.hit.hitbox_index,
			committed_hitbox.index,
			tgt.hit.hitgroup,
			committed_hitgroup,
			committed_penetrated ? 1u : 0u,
			committed_wall_count,
			committed_raw_damage,
			committed_damage,
			shoot_eye.x, shoot_eye.y, shoot_eye.z,
			aim_position.x, aim_position.y, aim_position.z,
			fire_record ? fire_record->origin.x : 0.0f,
			fire_record ? fire_record->origin.y : 0.0f,
			fire_record ? fire_record->origin.z : 0.0f );
#endif

		const auto doubletap_active = this->m_double_tap.pair_armed( )
			&& config.doubletap.value && !config.no_spread.value
			&& shared_ctx.item_def_idx != cstypes::item_definition_index::weapon_r8_revolver;
		if ( doubletap_active && !this->m_double_tap.is_second_attack( tick_base ) )
		{
			this->m_doubletap_record = *fire_record;
			this->m_doubletap_target = tgt;
			this->m_doubletap_target.hit.record = &this->m_doubletap_record;
			this->m_doubletap_target.hit.position = aim_position;
			this->m_doubletap_target.hit.aim_angle = math::helpers::calculate_angle( shoot_eye, aim_position );
			this->m_doubletap_shoot_eye = shoot_eye;
			this->m_doubletap_was_forced = was_forced;
			this->m_doubletap_pair_target_tick = tick_base;
			this->m_doubletap_pair_target_valid = true;
		}

		const auto previous_shot_tick = g_shared.last_shoot_tick( );
		g_shared.mark_shot( cmd, tick_base );

		if ( settings::g_misc.m_impacts.console_log.value )
		{
			const auto hitgroup_name = systems::g_hitboxes.hitgroup_to_name( committed_hitgroup );
			const auto bt_delta = g_shared.ctx( ).current_tick - fire_record->tick;
			logging::console::print(
				xs( "[rage] shot target hp {} for {:.0f} in {} (hc {:.0f}%, bt {}t{})" ),
				tgt.hit.health,
				committed_damage,
				hitgroup_name,
				tgt.hitchance * 100.0f,
				bt_delta,
				was_forced ? xs( ", forced" ) : ""
			);
		}

		auto shot_target_velocity = fire_record->velocity;
		const auto live_target_velocity = memory::read<math::vector3>( tgt.hit.pawn + SCHEMA( "C_BaseEntity", "m_vecVelocity"_hash ) );
		if ( std::isfinite( live_target_velocity.x ) && std::isfinite( live_target_velocity.y ) &&
			std::isfinite( live_target_velocity.z ) && live_target_velocity.length_2d( ) > shot_target_velocity.length_2d( ) )
		{
			shot_target_velocity = live_target_velocity;
		}
		if ( trajectory_direction.length_sqr( ) < 0.0001f )
		{
			math::helpers::angle_vectors_left( submitted_angle, &trajectory_direction );
			trajectory_direction = trajectory_direction.normalized( );
		}
		features::misc::g_impacts.on_boom( tgt.hit.pawn, committed_hitgroup, committed_damage, tgt.hitchance, shared_ctx.inaccuracy, shared_ctx.spread, submitted_angle, trajectory_direction, shoot_eye, fire_record->tick, g_shared.lc( ).get_skeleton( *fire_record ), committed_hitbox, aim_position, fire_record->origin, shot_target_velocity, fire_record->extrapolated, committed_penetrated, was_forced, config.no_spread.value, shared_ctx.weapon );
		features::esp::player::g_chams.os ().push (tgt.hit.pawn);
		const auto record_time = cstypes::tick_fraction::from_value( fire_record->simulation_time / fire_tick_interval );
		const auto backtrack_timestamped = selected_record_is_backtrack;
		auto history_render_time = backtrack_timestamped ? selected_record_time : record_time;
		if ( !backtrack_timestamped )
		{
			// Non-backtrack shots belong to the current command. The target record
			// often ends one tick earlier; using its simulation time here makes the
			// server rebuild the local shoot position from a stale history slot.
			history_render_time.tick = std::max( ( command_tick > 0 ? command_tick : tick_base ) - 1, 0 );
			history_render_time.frac = 0.0f;
		}
		// Player time identifies the current local firing command. Render time is
		// independently rewound to the selected enemy record below.
		const auto history_player_tick = std::max( history_seed_tick, 1 );
		const auto history_player_fraction = history_seed_fraction;

#if defined( DEV )
		math::vector3 view_punch{};
		auto view_punch_tick{ -1 };
		auto view_punch_ratio{ 0.0f };
		if ( const auto camera_services = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BasePlayerPawn", "m_pCameraServices"_hash ) ) )
		{
			view_punch = memory::read<math::vector3>( camera_services + SCHEMA( "CPlayer_CameraServices", "m_vecCsViewPunchAngle"_hash ) );
			view_punch_tick = memory::read<int>( camera_services + SCHEMA( "CPlayer_CameraServices", "m_nCsViewPunchAngleTick"_hash ) );
			view_punch_ratio = memory::read<float>( camera_services + SCHEMA( "CPlayer_CameraServices", "m_flCsViewPunchAngleTickRatio"_hash ) );
		}
		const auto next_primary_tick = memory::read<int>( shared_ctx.weapon + SCHEMA( "C_BasePlayerWeapon", "m_nNextPrimaryAttackTick"_hash ) );
		auto diagnostic_stamp_tick = history_player_tick;
		const auto diagnostic_claim_tick = config.no_spread.value
			? history_seed_tick
			: diagnostic_stamp_tick;
		const auto diagnostic_seed = config.no_spread.value
			? g_shared.get_spread_seed( trajectory_angle, diagnostic_claim_tick )
			: 0u;
		diag::writef(
			diag::level::info,
			"[rage_shot] cmd=%lld tickbase=%d last_shot=%d next_primary=%d stamp=%d claim=%d record=%d render=%d+%.3f live=%u extrapolated=%u eye_uninterp=%u eye_tick=%d+%d eye_frac=%.3f+%.3f target_speed=%.2f inaccuracy=%.6f spread=%.6f seed=%u aim=(%.3f,%.3f,%.3f) submitted=(%.3f,%.3f,%.3f) view_punch=(%.3f,%.3f,%.3f) view_punch_tick=%d ratio=%.3f",
			static_cast< long long >( cmd->command_number ),
			tick_base,
			previous_shot_tick,
			next_primary_tick,
			diagnostic_stamp_tick,
			diagnostic_claim_tick,
				fire_record->tick,
			record_time.tick + 1,
			record_time.frac,
			fire_record->live_snapshot ? 1u : 0u,
			fire_record->extrapolated ? 1u : 0u,
			tgt.hit.source_eye.is_uninterpolated ? 1u : 0u,
			tgt.hit.source_eye.player_tick,
			tgt.hit.source_eye.lerp_ticks_int,
			tgt.hit.source_eye.player_frac,
			tgt.hit.source_eye.lerp_ticks_frac,
				fire_record->velocity.length_2d( ),
			shared_ctx.inaccuracy,
			shared_ctx.spread,
			diagnostic_seed,
			aim_angle.x,
			aim_angle.y,
			aim_angle.z,
			submitted_angle.x,
			submitted_angle.y,
			submitted_angle.z,
			view_punch.x,
			view_punch.y,
			view_punch.z,
			view_punch_tick,
			view_punch_ratio );
#endif

		const auto history_size = cmd->csgo_user_cmd.input_history_size( );
	#if defined( DEV )
		if ( config.no_spread.value )
		{
			const auto engine_shoot_eye = g_shared.get_shoot_position( );
			const auto local_velocity = systems::g_prediction.pre( ).velocity;
			diag::writef(
				diag::level::info,
				"[rage_ns_origin] requested=(%.2f,%.2f,%.2f) final=(%.2f,%.2f,%.2f) engine=(%.2f,%.2f,%.2f) delta=%.3f velocity=(%.2f,%.2f,%.2f) seed_tick=%d seed_frac=%.3f source_uninterp=%u source_tick=%d+%.3f history=%d",
				requested_shoot_eye.x, requested_shoot_eye.y, requested_shoot_eye.z,
				shoot_eye.x, shoot_eye.y, shoot_eye.z,
				engine_shoot_eye.x, engine_shoot_eye.y, engine_shoot_eye.z,
				( engine_shoot_eye - shoot_eye ).length( ),
				local_velocity.x, local_velocity.y, local_velocity.z,
				history_seed_tick, history_seed_fraction,
				tgt.hit.source_eye.is_uninterpolated ? 1u : 0u,
				tgt.hit.source_eye.player_tick,
				tgt.hit.source_eye.player_frac,
				history_size );
		}
	#endif
		const auto shot_history_index = history_size > 0
			? history_size - 1
			: -1;
		for ( auto i = 0; i < history_size; ++i )
		{
			const auto entry = cmd->csgo_user_cmd.mutable_input_history( i );
			if ( !entry )
			{
				continue;
			}

			if ( !config.no_spread.value && i != shot_history_index )
			{
				continue;
			}

			if ( const auto angles = entry->mutable_view_angles( ) )
			{
				angles->set_x( submitted_angle.x );
				angles->set_y( submitted_angle.y );
				angles->set_z( submitted_angle.z );
			}

			// Player time remains on the current command. No-spread keeps all angle
			// history entries in that seed domain; target rewind belongs only to the
			// actual attack entry's render timestamp.
			if ( config.no_spread.value || backtrack_timestamped )
			{
				entry->set_player_tick_count( history_player_tick );
				entry->set_player_tick_fraction( history_player_fraction );
			}
			if ( i == shot_history_index )
			{
				entry->set_render_tick_count( history_render_time.tick + 1 );
				entry->set_render_tick_fraction( history_render_time.frac );
			}
			if ( i == shot_history_index )
			{
				const auto shoot_position_written = systems::g_input.set_shoot_position( cmd, entry, shoot_eye );
#if defined( DEV )
				diag::writef(
					diag::level::info,
					"[rage_history_commit] index=%d player=%d+%.3f render=%d+%.3f backtrack=%u shoot_position=%u",
					i,
					entry->player_tick_count( ), entry->player_tick_fraction( ),
					entry->render_tick_count( ), entry->render_tick_fraction( ),
					backtrack_timestamped ? 1u : 0u,
					shoot_position_written ? 1u : 0u );
#else
				static_cast< void >( shoot_position_written );
#endif
			}

			if ( entry->has_sv_interp0( ) )
			{
				const auto interp = entry->mutable_sv_interp0( );
				interp->set_src_tick( -1 );
				interp->set_dst_tick( -1 );
				interp->set_frac( 0.0f );
			}

			if ( entry->has_sv_interp1( ) )
			{
				const auto interp = entry->mutable_sv_interp1( );
				interp->set_src_tick( -1 );
				interp->set_dst_tick( -1 );
				interp->set_frac( 0.0f );
			}

			if ( entry->has_cl_interp( ) )
			{
				const auto interp = entry->mutable_cl_interp( );
				interp->set_frac( 0.0f );
			}

			if ( i == shot_history_index )
			{
				const auto& head = fire_record->bones[ cstypes::bone_ids::head ].position;
				if ( entry->m_target_head_pos_check )
				{
					const auto position = entry->mutable_target_head_pos_check( );
					position->set_x( head.x );
					position->set_y( head.y );
					position->set_z( head.z );
				}

				if ( entry->m_target_abs_pos_check )
				{
					const auto position = entry->mutable_target_abs_pos_check( );
					position->set_x( fire_record->origin.x );
					position->set_y( fire_record->origin.y );
					position->set_z( fire_record->origin.z );
				}

				if ( entry->m_target_abs_ang_check )
				{
					const auto angles = entry->mutable_target_abs_ang_check( );
					angles->set_x( fire_record->rotation.x );
					angles->set_y( fire_record->rotation.y );
					angles->set_z( fire_record->rotation.z );
				}

				entry->set_target_ent_index( tgt.hit.entity_index );
			}
		}

#if defined( DEV )
		if ( backtrack_timestamped )
		{
			const auto stamped_entry = shot_history_index >= 0
				? cmd->csgo_user_cmd.mutable_input_history( shot_history_index )
				: nullptr;
			const auto stamped_eye = stamped_entry ? stamped_entry->shoot_position( ) : nullptr;
			diag::writef(
				diag::level::info,
				"[rage_backtrack_stamp] selected=%d+%.3f command=%d age=%d attack_history=%d nospread=%u player=%d+%.3f render=%d+%.3f shoot=(%.2f,%.2f,%.2f)",
				selected_record_time.tick,
				selected_record_time.frac,
				command_reference_tick,
				std::max( command_reference_tick - selected_record_time.tick, 0 ),
				shot_history_index,
				config.no_spread.value ? 1u : 0u,
				stamped_entry ? stamped_entry->player_tick_count( ) : -1,
				stamped_entry ? stamped_entry->player_tick_fraction( ) : 0.0f,
				stamped_entry ? stamped_entry->render_tick_count( ) : -1,
				stamped_entry ? stamped_entry->render_tick_fraction( ) : 0.0f,
				stamped_eye ? stamped_eye->x( ) : 0.0f,
				stamped_eye ? stamped_eye->y( ) : 0.0f,
				stamped_eye ? stamped_eye->z( ) : 0.0f );
		}
#endif

		const auto revolver_nospread_secondary = config.no_spread.value
			&& shared_ctx.item_def_idx == cstypes::item_definition_index::weapon_r8_revolver;
		const auto use_secondary_attack = revolver_nospread_secondary || tgt.hit.attack_type == 1;
		const auto attack_button = use_secondary_attack
			? cstypes::command_buttons::in_second_attack
			: cstypes::command_buttons::in_attack;

		if ( !subtick_attack && ( !doubletap_active || config.no_spread.value ) )
		{
			if ( revolver_nospread_secondary )
			{
				cmd->buttons.value &= ~cstypes::command_buttons::in_attack;
				cmd->buttons.value_changed |= cstypes::command_buttons::in_attack;
				cmd->buttons.value_scroll &= ~cstypes::command_buttons::in_attack;
				cmd->csgo_user_cmd.set_attack1_start_history_index( -1 );
			}

			cmd->buttons.value |= attack_button;
			cmd->buttons.value_changed |= attack_button;
			cmd->buttons.value_scroll |= attack_button;

			if ( history_size > 0 )
			{
				if ( use_secondary_attack )
				{
					cmd->csgo_user_cmd.set_attack2_start_history_index( history_size - 1 );
				}
				else
				{
					cmd->csgo_user_cmd.set_attack1_start_history_index( history_size - 1 );
				}
			}
		}

		math::vector3 forward{};
		{
			if ( const auto angles = base->viewangles( ) )
			{
				math::helpers::angle_vectors_left( { angles->x( ), angles->y( ), angles->z( ) }, &forward );
			}
		}

		const auto facing_away = forward.dot( ( fire_record->origin - systems::g_prediction.pre( ).networked_origin ).normalized( ) ) < 0.707107f;
		const auto vac_bypass = settings::g_combat.m_ragebot.vac_bypass.value;

		auto command_aim = punched_aim;
		if ( !vac_bypass )
		{
			command_aim.z = submitted_angle.z;
		}
		if ( !subtick_attack && ( vac_bypass || ( !config.no_spread.value && facing_away && settings::g_combat.m_antiaim.hide_shots.value ) ) )
		{
			if ( vac_bypass )
			{
				command_aim.x = -punched_aim.x;
				command_aim.y = std::remainderf( punched_aim.y + 180.0f + random::floating( -0.35f, 0.35f ), 360.0f );
			}
			else
			{
				command_aim.x = 179.9f;
				command_aim.y = std::remainderf( punched_aim.y + 180.0f, 360.0f );
			}
		}

		if ( const auto angles = base->mutable_viewangles( ) )
		{
			angles->set_x( command_aim.x );
			angles->set_y( command_aim.y );
			angles->set_z( command_aim.z );
		}
		this->m_double_tap.set_fire_angle( submitted_angle );

		if ( !config.silent.value && !vac_bypass && !( config.no_spread.value && this->m_doubletap_followup_fire ) )
		{
			systems::g_input.set_view_angles( config.no_spread.value ? submitted_angle : punched_aim );
		}

		if ( shot_history_index >= 0 )
		{
			this->m_protected_shot_command = cmd->command_number;
			this->m_protected_shot_history_index = shot_history_index;
			this->m_protected_consumed_server_angle_changes = base->consumed_server_angle_changes( );
			this->m_protected_shot_angle = submitted_angle;
			this->m_protected_command_angle = command_aim;
		}
	}

	void rage::fire_melee( systems::input::usercmd* cmd, const target& tgt, const systems::local::snapshot& local )
	{
		if ( !tgt.hit.record || !tgt.hit.record->valid )
		{
			return;
		}

		this->m_firing_this_tick = true;

		const auto base = cmd->csgo_user_cmd.mutable_base( );
		const auto tick_base = memory::read<int>( local.controller + SCHEMA( "CBasePlayerController", "m_nTickBase"_hash ) );
		const auto backtrack_enabled = settings::g_combat.m_lagcomp.backtrack.value;

		g_shared.mark_shot( cmd, tick_base );

		const auto record_time = cstypes::tick_fraction::from_value( tgt.hit.record->simulation_time / cstypes::tick_interval );
		const auto history_index = cmd->csgo_user_cmd.input_history_size( ) - 1;
		const auto entry = history_index >= 0 ? cmd->csgo_user_cmd.mutable_input_history( history_index ) : nullptr;

		if ( entry )
		{
			if ( const auto angles = entry->mutable_view_angles( ) )
			{
				angles->set_x( tgt.hit.aim_angle.x );
				angles->set_y( tgt.hit.aim_angle.y );
			}

			entry->set_render_tick_count( record_time.tick + 1 );
			entry->set_render_tick_fraction( 0.0f );

			if ( backtrack_enabled && !tgt.hit.source_eye.is_uninterpolated )
			{
				auto tick_add = [ ]( int t, float f, int dt, float df )
					{
						f += df;
						auto carry = static_cast< int >( std::floor( f ) );
						f -= static_cast< float >( carry );
						return std::pair{ t + dt + carry, f };
					};

				const auto [stamp_tick, stamp_frac] = tick_add( tgt.hit.source_eye.player_tick, tgt.hit.source_eye.player_frac, tgt.hit.source_eye.lerp_ticks_int, tgt.hit.source_eye.lerp_ticks_frac );

				entry->set_player_tick_count( stamp_tick );
				entry->set_player_tick_fraction( stamp_frac );
			}

			if ( entry->has_sv_interp0( ) )
			{
				const auto interp = entry->mutable_sv_interp0( );
				interp->set_src_tick( -1 );
				interp->set_dst_tick( -1 );
				interp->set_frac( 0.0f );
			}

			if ( entry->has_sv_interp1( ) )
			{
				const auto interp = entry->mutable_sv_interp1( );
				interp->set_src_tick( -1 );
				interp->set_dst_tick( -1 );
				interp->set_frac( 0.0f );
			}

			if ( entry->has_cl_interp( ) )
			{
				const auto interp = entry->mutable_cl_interp( );
				interp->set_frac( 0.0f );
			}

		}

		const auto is_secondary = tgt.hit.attack_type == 1;
		const auto attack_button = is_secondary
			? cstypes::command_buttons::in_second_attack
			: cstypes::command_buttons::in_attack;

		cmd->buttons.value |= attack_button;
		cmd->buttons.value_changed |= attack_button;
		cmd->buttons.value_scroll |= attack_button;

		if ( history_index >= 0 )
		{
			if ( is_secondary )
			{
				cmd->csgo_user_cmd.set_attack2_start_history_index( history_index );
			}
			else
			{
				cmd->csgo_user_cmd.set_attack1_start_history_index( history_index );
			}
		}

		if ( const auto angles = base->mutable_viewangles( ) )
		{
			angles->set_x( tgt.hit.aim_angle.x );
			angles->set_y( tgt.hit.aim_angle.y );
		}
	}

	std::vector<math::vector3> rage::generate_multipoints( const systems::hitboxes::entry& hitbox, const math::vector3& center, const math::quaternion& bone_rot, float pointscale, const math::vector3& shoot_pos, float inaccuracy, const math::vector3& predicted_velocity, int extrapolated_ticks ) const
	{
		std::vector<math::vector3> out;
		const auto finite_vector = [ ]( const math::vector3& value )
		{
			return std::isfinite( value.x ) && std::isfinite( value.y ) && std::isfinite( value.z );
		};

		if ( !finite_vector( center ) || !finite_vector( shoot_pos ) || !finite_vector( predicted_velocity ) || !finite_vector( hitbox.mins ) || !finite_vector( hitbox.maxs ) ||
			!std::isfinite( bone_rot.x ) || !std::isfinite( bone_rot.y ) || !std::isfinite( bone_rot.z ) || !std::isfinite( bone_rot.w ) ||
			!std::isfinite( hitbox.radius ) || hitbox.radius < 0.0f || !std::isfinite( pointscale ) || !std::isfinite( inaccuracy ) )
		{
			return out;
		}

		auto scale = std::clamp( pointscale / 100.0f, 0.0f, 1.0f );
		if ( scale <= 0.01f )
		{
			return out;
		}

		const auto hb_mid   = ( hitbox.mins + hitbox.maxs ) * 0.5f;
		const auto capsule_a = center + bone_rot.rotate_vector( hitbox.mins - hb_mid );
		const auto capsule_b = center + bone_rot.rotate_vector( hitbox.maxs - hb_mid );

		const auto& config = settings::g_combat.m_ragebot.get_group( g_shared.ctx( ).weapon_type );
		if ( config.dynamic_pointscale.value && hitbox.radius > 0.001f )
		{
			const auto cone = config.no_spread.value
				? 0.0f
				: std::clamp( inaccuracy + g_shared.ctx( ).spread, 0.0f, 1.4f );
			const auto cone_radius = std::tanf( cone ) * ( center - shoot_pos ).length( );
			const auto automatic_scale = std::isfinite( cone_radius )
				? std::clamp( 0.9f - cone_radius / hitbox.radius, 0.0f, 1.0f )
				: 0.0f;
			const auto minimum_head_scale = hitbox.index == 0 ? 0.15f : 0.0f;
			scale = std::min( scale, std::max( automatic_scale, minimum_head_scale ) );

			if ( scale <= 0.01f )
			{
				return out;
			}
		}

		const auto shoot_delta = center - shoot_pos;
		if ( !finite_vector( shoot_delta ) || shoot_delta.length_sqr( ) < 1.0e-4f )
		{
			return out;
		}

		const auto shoot_dir = shoot_delta.normalized( );
		const auto ang       = math::helpers::vector_to_angle( shoot_dir );
		if ( !finite_vector( ang ) )
		{
			return out;
		}

		math::vector3 left{}, up{};
		math::helpers::angle_vectors_left( ang, nullptr, &left, &up );

		const auto right = math::vector3{ -left.x, -left.y, -left.z };
		const auto motion_speed = predicted_velocity.length( );
		const auto has_motion_prediction = extrapolated_ticks > 0 && std::isfinite( motion_speed ) && motion_speed > 8.0f;
		const auto motion_dir = has_motion_prediction ? predicted_velocity / motion_speed : math::vector3{};
		const auto motion_scale = has_motion_prediction
			? std::clamp( 0.68f + static_cast< float >( extrapolated_ticks ) * 0.035f, 0.68f, 0.92f )
			: 0.0f;

		const auto surface_point = [ & ]( const math::vector3& direction ) -> math::vector3
		{
			const auto dir = direction.normalized( );

			if ( hitbox.radius > 0.001f )
			{
				const auto reach = ( capsule_b - capsule_a ).length( ) + hitbox.radius * 2.0f + 1.0f;
				const auto origin = center + dir * reach;
				const auto delta = dir * ( reach * -2.0f );
				auto fraction{ 1.0f };

				if ( g_shared.ray_vs_capsule( origin, delta, capsule_a, capsule_b, hitbox.radius, fraction ) )
				{
					return origin + delta * fraction;
				}
			}
			else
			{
				auto inverse = bone_rot;
				inverse.x = -inverse.x;
				inverse.y = -inverse.y;
				inverse.z = -inverse.z;
				const auto local_dir = inverse.rotate_vector( dir );
				const auto extents = ( hitbox.maxs - hitbox.mins ) * 0.5f;
				auto distance = 8192.0f;

				if ( std::fabs( local_dir.x ) > 1.0e-6f ) distance = std::min( distance, std::fabs( extents.x / local_dir.x ) );
				if ( std::fabs( local_dir.y ) > 1.0e-6f ) distance = std::min( distance, std::fabs( extents.y / local_dir.y ) );
				if ( std::fabs( local_dir.z ) > 1.0e-6f ) distance = std::min( distance, std::fabs( extents.z / local_dir.z ) );

				if ( distance < 8192.0f )
				{
					return center + dir * distance;
				}
			}

			return center;
		};

		const auto scaled_surface = [ & ]( const math::vector3& direction )
		{
			const auto surface = surface_point( direction );
			return center + ( surface - center ) * scale;
		};
		const auto scaled_surface_at = [ & ]( const math::vector3& direction, float sample_scale )
		{
			const auto surface = surface_point( direction );
			return center + ( surface - center ) * std::clamp( sample_scale, 0.0f, 1.0f );
		};

		switch ( hitbox.index )
		{
		case 0: // head
		{
			out.reserve( 44 );
			constexpr auto tau = 6.2831853071795864769f;
			const auto add_ring = [ & ]( int samples, float ring_scale, float phase )
			{
				for ( auto i = 0; i < samples; ++i )
				{
					const auto angle = phase + tau * static_cast< float >( i ) / static_cast< float >( samples );
					const auto direction = right * std::cosf( angle ) + up * std::sinf( angle );
					out.push_back( scaled_surface_at( direction, scale * ring_scale ) );
				}
			};
			add_ring( 16, 0.96f, 0.0f );
			add_ring( 12, 0.78f, tau / 24.0f );
			add_ring( 8, 0.55f, tau / 16.0f );

			out.push_back( scaled_surface_at( shoot_dir, scale * 0.90f ) );
			out.push_back( scaled_surface_at( -shoot_dir, scale * 0.90f ) );
			if ( has_motion_prediction )
			{
				out.push_back( scaled_surface_at( motion_dir, scale * motion_scale ) );
				out.push_back( scaled_surface_at( -motion_dir, scale * motion_scale ) );
			}
			for ( const auto axis_scale : { 0.45f, 0.78f, 0.96f } )
			{
				out.push_back( center + ( capsule_a - center ) * ( scale * axis_scale ) );
				out.push_back( center + ( capsule_b - center ) * ( scale * axis_scale ) );
			}
			break;
		}

		case 2: case 3: // stomach / pelvis
		{
			out.reserve( 4 );
			out.push_back( scaled_surface( right ) );
			out.push_back( scaled_surface( -right ) );
			out.push_back( scaled_surface( up ) );
			out.push_back( scaled_surface( -up ) );
			break;
		}

		case 4: case 5: case 6: // chest
		{
			out.reserve( 6 );
			out.push_back( scaled_surface( right ) );
			out.push_back( scaled_surface( -right ) );
			out.push_back( scaled_surface( up ) );
			out.push_back( scaled_surface( -up ) );
			out.push_back( scaled_surface( ( right + up ).normalized( ) ) );
			out.push_back( scaled_surface( shoot_dir ) );
			if ( has_motion_prediction )
			{
				out.push_back( scaled_surface_at( motion_dir, scale * motion_scale ) );
				out.push_back( scaled_surface_at( -motion_dir, scale * motion_scale ) );
			}
			break;
		}

		case 7: case 8: case 9: case 10: case 11: case 12: // legs / feet
		{
			out.reserve( 2 );
			out.push_back( center + ( capsule_a - center ) * scale );
			out.push_back( center + ( capsule_b - center ) * scale );
			break;
		}

		case 13: case 14: case 15: case 16: case 17: case 18: // arms
		{
			out.reserve( 1 );
			out.push_back( center + ( capsule_b - center ) * scale );
			break;
		}

		default:
		{
			out.reserve( 2 );
			out.push_back( scaled_surface( right ) );
			out.push_back( scaled_surface( -right ) );
			break;
		}
		}

		std::vector<math::vector3> unique;
		unique.reserve( out.size( ) );
		for ( const auto& point : out )
		{
			if ( !finite_vector( point ) )
			{
				continue;
			}

			const auto duplicate = std::ranges::any_of( unique, [ & ]( const math::vector3& existing )
				{
					return ( existing - point ).length_sqr( ) < 0.04f;
				} );
			if ( !duplicate )
			{
				unique.push_back( point );
			}
		}

		return unique;
	}

	bool rage::should_stop_movement( const aim_context& ctx ) const
	{
		const auto& shared_ctx = g_shared.ctx( );
		const auto& prestate = systems::g_prediction.pre( );
		const auto& group = settings::g_combat.m_ragebot.get_group( shared_ctx.weapon_type );
		// 急停门控使用网络同步速度，避免预测速度把玩家错误地锁在急停状态。
		const auto velocity = prestate.networked_velocity;
		if ( shared_ctx.weapon_type == cstypes::weapon_type::sniper && !ctx.is_scoped )
		{
			return false;
		}

		if ( ctx.on_ground )
		{
			const auto speed_2d = velocity.length_2d( );
			if ( !std::isfinite( speed_2d ) || speed_2d <= 0.1f )
			{
				return false;
			}

			// Keep braking active until the complete accurate-speed window is
			// reached. Releasing at an early threshold leaves scoped AWP movement
			// around 60 u/s, then the next command accelerates it again.
			return speed_2d > ctx.accurate_threshold;

			#if 0
			const auto inaccuracy_move = memory::read<float>( shared_ctx.weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_flInaccuracyMove"_hash ) );
			const auto inaccuracy_stand = memory::read<float>( shared_ctx.weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_flInaccuracyStand"_hash ) );

			// 提前一个小窗口开始反向制动，给速度留出一个完整 tick 的下降时间。
			// 仅在 run_gun() 已确认有效目标且当前不能开火时调用，避免见人锁停。
			const auto early_threshold = group.autostop.values[ 0 ]
				? std::max( 1.0f, accurate_threshold * 0.88f )
				: accurate_threshold;
			return speed_2d > early_threshold || speed_2d * inaccuracy_move > inaccuracy_stand;
			#endif
		}

		if ( shared_ctx.weapon_type != cstypes::weapon_type::sniper )
		{
			return false;
		}
		if ( !group.autostop.values[ 1 ] )
		{
			return false;
		}

		if ( velocity.z > 140.0f )
		{
			return false;
		}

		const auto sv_gravity = CONVAR ("sv_gravity")->get<float>( );
		const auto sv_friction = CONVAR ("sv_friction")->get<float>( );
		const auto sv_stopspeed = CONVAR ("sv_stopspeed")->get<float>( );

		const auto inac_jump_initial = memory::read<float>( shared_ctx.weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_flInaccuracyJumpInitial"_hash ) );
		const auto inac_jump_apex = memory::read<float>( shared_ctx.weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_flInaccuracyJumpApex"_hash ) );
		const auto shootable_threshold = inac_jump_apex + 0.001f;
		const auto early_threshold = inac_jump_initial * 0.55f + inac_jump_apex * 0.45f;
		const auto air_inaccuracy = g_shared.get_air_inaccuracy( velocity.z, inac_jump_initial, inac_jump_apex );

		if ( air_inaccuracy <= shootable_threshold || air_inaccuracy <= early_threshold )
		{
			return true;
		}

		auto sim_vz = velocity.z;
		auto ticks_to_shootable{ 0 };

		for ( auto i = 1; i <= 32; ++i )
		{
			sim_vz -= sv_gravity * cstypes::tick_interval;

			if ( g_shared.get_air_inaccuracy( sim_vz, inac_jump_initial, inac_jump_apex ) <= shootable_threshold )
			{
				ticks_to_shootable = i;
				break;
			}
		}

		if ( ticks_to_shootable == 0 )
		{
			return false;
		}

		const auto speed_2d = velocity.length_2d( );
		const auto max_speed = memory::read<float>( shared_ctx.weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_flMaxSpeed"_hash ) );
		const auto accurate_threshold = max_speed * 0.34f;

		if ( speed_2d <= accurate_threshold )
		{
			return true;
		}

		auto sim_speed = speed_2d;
		auto ticks_to_stop{ 32 };

		for ( auto i = 1; i <= 32; ++i )
		{
			const auto drop = std::fmaxf( sim_speed, sv_stopspeed ) * sv_friction * cstypes::tick_interval;
			sim_speed -= drop;

			if ( sim_speed <= accurate_threshold )
			{
				ticks_to_stop = i;
				break;
			}
		}

		return ticks_to_shootable <= ticks_to_stop + 2;
	}

	bool rage::prepare_air_stop_for_shot( const aim_context& ctx ) noexcept
	{
		if ( !this->air_stop_enabled( ) || ctx.on_ground )
		{
			this->m_air_stop_settle_ticks = 0;
			return true;
		}

		this->m_air_stop_requested = true;
		const auto& prestate = systems::g_prediction.pre( );
		const auto speed = std::max(
			ctx.velocity.length_2d( ),
			prestate.networked_velocity.length_2d( ) );
		constexpr float settled_speed = 32.0f;
		constexpr std::uint8_t required_settle_ticks = 2;

		if ( speed <= settled_speed )
		{
			this->m_air_stop_settle_ticks = std::min<std::uint8_t>(
				static_cast<std::uint8_t>( this->m_air_stop_settle_ticks + 1 ),
				required_settle_ticks );
			if ( this->m_air_stop_settle_ticks >= required_settle_ticks )
			{
				// 已达到空中可开火速度，不再继续覆盖玩家的水平输入。
				this->m_air_stop_requested = false;
				return true;
			}
		}
		else
		{
			this->m_air_stop_settle_ticks = 0;
		}

		return false;
	}

	float rage::get_min_damage( const settings::combat::ragebot::weapon_group& config, int target_health, bool override_active ) const
	{
		if ( override_active )
		{
			return static_cast< float >( config.min_damage_override_value );
		}

		const auto base = static_cast< float >( config.min_damage );

		const auto hp = static_cast< float >( target_health );
		if ( hp < base )
		{
			return hp + 1.0f;
		}

		return base;
	}

	float rage::get_knife_damage( float raw, int armor, float armor_ratio ) const
	{
		if ( armor <= 0 )
		{
			return raw;
		}

		const auto ratio = armor_ratio * 0.5f;
		auto damage_to_health = raw * ratio;
		const auto damage_to_armor = ( raw - damage_to_health ) * 0.5f;

		if ( damage_to_armor > static_cast< float >( armor ) )
		{
			damage_to_health = raw - static_cast< float >( armor ) * 2.0f;
		}

		return std::max( 0.0f, std::floorf( damage_to_health ) );
	}

	systems::tracing::result rage::trace_taser_hit( const math::vector3& origin, const math::vector3& forward, float range, std::uintptr_t target_pawn, std::uintptr_t local_pawn ) const
	{
		const auto end = origin + forward * range;
		const int filter_extras[ ]{ 0, 15 };

		for ( const auto extra : filter_extras )
		{
			const auto filter = extra == 0 ? systems::g_tracing.make_filter( local_pawn, 0x001c1003, 4 ) : systems::g_tracing.make_filter( local_pawn, 0x001c1003, 4, 15 );
			auto result = systems::g_tracing.trace( origin, end, filter );

			if ( ( result.fraction < 1.0f || result.all_solid ) && result.hit_entity == target_pawn )
			{
				return result;
			}

			for ( auto radius = 2.0f; radius <= 4.0f; radius += 2.0f )
			{
				const auto sweep_end = end - forward * radius;
				result = systems::g_tracing.trace_sphere( origin, sweep_end, radius, filter );

				if ( ( result.fraction < 1.0f || result.all_solid ) && result.hit_entity == target_pawn )
				{
					return result;
				}
			}
		}

		systems::tracing::result miss{};
		miss.fraction = 1.0f;
		miss.hit_entity = 0;
		return miss;
	}

	systems::tracing::result rage::trace_knife_hit( const math::vector3& origin, const math::vector3& forward, float reach, std::uintptr_t target_pawn, std::uintptr_t local_pawn ) const
	{
		const auto end = origin + forward * reach;
		const auto knife_filter = systems::g_tracing.make_filter( local_pawn, 0x0c3001, 4 );
		auto result = systems::g_tracing.trace( origin, end, knife_filter );

		if ( ( result.fraction < 1.0f || result.all_solid ) && result.hit_entity == target_pawn )
		{
			return result;
		}

		const auto weapon_filter = systems::g_tracing.make_filter( local_pawn, 0x0c3001, 4, 15 );
		result = systems::g_tracing.trace( origin, end, weapon_filter );

		if ( ( result.fraction < 1.0f || result.all_solid ) && result.hit_entity == target_pawn )
		{
			return result;
		}

		for ( auto radius = 14.0f; radius > 0.0f; radius -= 3.0f )
		{
			const auto sweep_end = end - forward * radius;
			result = systems::g_tracing.trace_sphere( origin, sweep_end, radius, weapon_filter );

			if ( ( result.fraction < 1.0f || result.all_solid ) && result.hit_entity == target_pawn )
			{
				return result;
			}
		}

		result.fraction = 1.0f;
		result.hit_entity = 0;
		return result;
	}

	void rage::update_penetration_crosshair( const systems::local::snapshot& local )
	{
		const auto& cfg = settings::g_combat.m_penetration_crosshair;
		const auto& ctx = g_shared.ctx( );

		if ( !cfg.enabled.value || !ctx.valid || !local.is_alive || local.team < 2
			|| !local.pawn || !ctx.weapon
			|| ctx.weapon_type < cstypes::weapon_type::pistol || ctx.weapon_type > cstypes::weapon_type::lmg )
		{
			this->m_penetration_crosshair_state.store( penetration_crosshair_state::unavailable, std::memory_order_relaxed );
			return;
		}

		const auto eye_pos = g_shared.get_eye_position( local.pawn );
		auto view_angles = systems::g_input.get_view_angles( );
		const auto aim_punch = g_shared.get_aim_punch( local.pawn );
		view_angles.x += aim_punch.x;
		view_angles.y += aim_punch.y;

		math::vector3 forward{};
		math::helpers::angle_vectors_left( view_angles, &forward );

		auto pen_damage{ 0.0f };
		const auto can_pen = g_shared.pen( ).can( eye_pos, forward, pen_damage, local );
		this->m_penetration_crosshair_state.store(
			can_pen ? penetration_crosshair_state::penetrable : penetration_crosshair_state::blocked,
			std::memory_order_relaxed );
	}

	void rage::update_hittable_regions( const std::vector<scan_hit>& hits )
	{
		const auto& cfg = settings::g_combat.m_ragebot.get_group( g_shared.ctx( ).weapon_type );
		const auto now = g_shared.ctx( ).current_time;

		if ( !cfg.hittable_regions.value || !std::isfinite( now ) )
		{
			std::lock_guard lock( m_hittable_regions_mtx );
			m_hittable_regions.clear( );
			return;
		}

		std::vector<hittable_region> fresh;
		fresh.reserve( hits.size( ) );

		for ( const auto& hit : hits )
		{
			if ( !hit.record || !hit.record->valid || hit.hitbox_index < 0
				|| hit.bone_index < 0 || hit.bone_index >= 27 )
			{
				continue;
			}

			const auto skeleton = g_shared.lc( ).get_skeleton( *hit.record );
			const auto& bone = skeleton[ hit.bone_index ];
			if ( bone.position.length_sqr( ) < 1.0f )
			{
				continue;
			}

			auto duplicate = std::find_if( fresh.begin( ), fresh.end( ), [ & ]( const hittable_region& region )
				{
					return region.pawn == hit.pawn && region.hitbox_index == hit.hitbox_index;
				} );

			if ( duplicate != fresh.end( ) )
			{
				if ( duplicate->penetrated && !hit.penetrated )
				{
					duplicate->hitbox = hit.hitbox;
					duplicate->bone = bone;
					duplicate->penetrated = false;
				}
				continue;
			}

			fresh.push_back( {
				.pawn = hit.pawn,
				.hitbox_index = hit.hitbox_index,
				.hitbox = hit.hitbox,
				.bone = bone,
				.penetrated = hit.penetrated,
				.expires_at = now + std::max( 0.05f, cfg.hittable_region_duration.value )
			} );
		}

		std::lock_guard lock( m_hittable_regions_mtx );
		std::erase_if( m_hittable_regions, [ now ]( const hittable_region& region )
			{
				return region.expires_at <= now;
			} );

		for ( const auto& region : fresh )
		{
			auto existing = std::find_if( m_hittable_regions.begin( ), m_hittable_regions.end( ), [ & ]( const hittable_region& cached )
				{
					return cached.pawn == region.pawn && cached.hitbox_index == region.hitbox_index;
				} );

			if ( existing != m_hittable_regions.end( ) )
			{
				*existing = region;
			}
			else
			{
				m_hittable_regions.push_back( region );
			}
		}
	}

	void rage::draw_hittable_regions( xdraw::draw_list& draw_list ) const
	{
		const auto& cfg = settings::g_combat.m_ragebot.get_group( g_shared.ctx( ).weapon_type );
		const auto now = g_shared.ctx( ).current_time;
		if ( !cfg.hittable_regions.value || !std::isfinite( now ) || !systems::g_view.has_camera( ) )
		{
			return;
		}

		std::vector<hittable_region> regions;
		{
			std::lock_guard lock( m_hittable_regions_mtx );
			regions = m_hittable_regions;
		}

		const auto hold_time = std::max( 0.05f, cfg.hittable_region_duration.value );

		std::array<std::vector<std::vector<poly2d::point>>, 2> polygons{};
		std::array<std::uint8_t, 2> polygon_alpha{};

		const auto convex_hull = []( std::vector<poly2d::point> points )
		{
			if ( points.size( ) < 3 )
			{
				return points;
			}

			std::sort( points.begin( ), points.end( ), []( const auto& lhs, const auto& rhs )
				{
					return lhs.x < rhs.x || ( lhs.x == rhs.x && lhs.y < rhs.y );
				} );
			points.erase( std::unique( points.begin( ), points.end( ) ), points.end( ) );
			if ( points.size( ) < 3 )
			{
				return points;
			}

			const auto cross = []( const auto& o, const auto& a, const auto& b )
				{
					return ( a.x - o.x ) * ( b.y - o.y ) - ( a.y - o.y ) * ( b.x - o.x );
				};

			std::vector<poly2d::point> hull;
			hull.reserve( points.size( ) * 2 );
			for ( const auto& point : points )
			{
				while ( hull.size( ) >= 2 && cross( hull[ hull.size( ) - 2 ], hull.back( ), point ) <= 0.0f )
				{
					hull.pop_back( );
				}
				hull.push_back( point );
			}

			const auto lower_size = hull.size( );
			for ( auto i = points.size( ) - 1; i > 0; --i )
			{
				const auto& point = points[ i - 1 ];
				while ( hull.size( ) > lower_size && cross( hull[ hull.size( ) - 2 ], hull.back( ), point ) <= 0.0f )
				{
					hull.pop_back( );
				}
				hull.push_back( point );
			}

			if ( hull.size( ) > 1 )
			{
				hull.pop_back( );
			}
			return hull;
		};

		for ( const auto& region : regions )
		{
			const auto life = std::clamp( ( region.expires_at - now ) / hold_time, 0.0f, 1.0f );
			if ( life <= 0.0f )
			{
				continue;
			}

			const auto base = region.penetrated
				? cfg.hittable_region_penetrated_color.value
				: cfg.hittable_region_direct_color.value;
			const auto alpha = static_cast<std::uint8_t>( static_cast<float>( base.a ) * life );
			polygon_alpha[ region.penetrated ? 1 : 0 ] = std::max(
				polygon_alpha[ region.penetrated ? 1 : 0 ], alpha );

			const auto project = [ ]( const math::vector3& point, math::vector2& out )
				{
					out = systems::g_view.project( point );
					return systems::g_view.projection_valid( out );
				};
			const auto transform = [ & ]( const math::vector3& point )
				{
					return region.bone.rotation.rotate_vector( point ) + region.bone.position;
				};

			if ( region.hitbox.radius > 0.001f )
			{
				math::vector2 a{}, b{}, radius_point{};
				const auto capsule_a = transform( region.hitbox.mins );
				const auto capsule_b = transform( region.hitbox.maxs );
				const auto radius_world = transform( region.hitbox.mins + math::vector3{ region.hitbox.radius, 0.0f, 0.0f } );
				if ( !project( capsule_a, a ) || !project( capsule_b, b ) || !project( radius_world, radius_point ) )
				{
					continue;
				}

				const auto radius = std::clamp( ( radius_point - a ).length( ), 1.0f, 128.0f );
				std::vector<poly2d::point> samples;
				constexpr auto segments = 16;
				for ( auto i = 0; i < segments; ++i )
				{
					const auto angle = ( 2.0f * std::numbers::pi_v<float> * static_cast<float>( i ) ) / static_cast<float>( segments );
					const auto offset = math::vector2{ std::cosf( angle ) * radius, std::sinf( angle ) * radius };
					samples.push_back( { a.x + offset.x, a.y + offset.y } );
					samples.push_back( { b.x + offset.x, b.y + offset.y } );
				}

				auto pill = convex_hull( std::move( samples ) );
				if ( pill.size( ) >= 3 )
				{
					polygons[ region.penetrated ? 1 : 0 ].push_back( std::move( pill ) );
				}
				continue;
			}

			std::array<math::vector2, 8> projected{};
			bool visible = true;
			for ( auto i = 0; i < 8; ++i )
			{
				const math::vector3 local{
					( i & 1 ) ? region.hitbox.maxs.x : region.hitbox.mins.x,
					( i & 2 ) ? region.hitbox.maxs.y : region.hitbox.mins.y,
					( i & 4 ) ? region.hitbox.maxs.z : region.hitbox.mins.z
				};
				visible &= project( transform( local ), projected[ i ] );
			}

			if ( !visible )
			{
				continue;
			}

			std::vector<poly2d::point> box_points;
			box_points.reserve( projected.size( ) );
			for ( const auto& point : projected )
			{
				box_points.push_back( { point.x, point.y } );
			}

			auto box = convex_hull( std::move( box_points ) );
			if ( box.size( ) >= 3 )
			{
				polygons[ region.penetrated ? 1 : 0 ].push_back( std::move( box ) );
			}
		}

		for ( auto group = 0u; group < polygons.size( ); ++group )
		{
			if ( polygons[ group ].empty( ) )
			{
				continue;
			}

			const auto base = group == 1
				? cfg.hittable_region_penetrated_color.value
				: cfg.hittable_region_direct_color.value;
			const auto union_result = poly2d::union_pills( polygons[ group ] );
			for ( const auto& outline : union_result.outlines )
			{
				if ( outline.size( ) < 3 )
				{
					continue;
				}

				const auto triangles = poly2d::triangulate( outline );
				const auto color = base.alpha( polygon_alpha[ group ] );
				for ( std::size_t i = 0; i + 5 < triangles.size( ); i += 6 )
				{
					draw_list.triangle_filled(
						triangles[ i ], triangles[ i + 1 ],
						triangles[ i + 2 ], triangles[ i + 3 ],
						triangles[ i + 4 ], triangles[ i + 5 ], color, false );
				}
			}
		}
	}

	void rage::draw_penetration_crosshair( xdraw::draw_list& draw_list ) const
	{
		const auto& cfg = settings::g_combat.m_penetration_crosshair;
		if ( !cfg.enabled.value )
		{
			return;
		}

		const auto state = this->m_penetration_crosshair_state.load( std::memory_order_relaxed );
		const auto local = systems::g_local.get( );
		if ( state == penetration_crosshair_state::unavailable || !local.is_alive || systems::g_local.is_in_cinematic( ) )
		{
			return;
		}

		const auto can_pen = state == penetration_crosshair_state::penetrable;

		const auto& fill = can_pen ? cfg.can_penetrate_fill : cfg.blocked_fill;
		const auto& outline = can_pen ? cfg.can_penetrate_outline : cfg.blocked_outline;
		const auto [ screen_w, screen_h ] = xdraw::viewport_size( );
		const auto cx = std::floorf( static_cast< float >( screen_w ) * 0.5f );
		const auto cy = std::floorf( static_cast< float >( screen_h ) * 0.5f );
		constexpr auto half_size{ 3.0f };
		constexpr auto outline_size{ 1.0f };

		if ( cfg.glow )
		{
			auto& glow = xdraw::get_glow( );
			const auto glow_a = static_cast< std::uint8_t >( static_cast< float >( outline.value.a ) * cfg.glow_strength );
			const auto glow_col = xdraw::color{ outline.value.r, outline.value.g, outline.value.b, glow_a };

			glow.rect_filled( cx - half_size - outline_size, cy - half_size - outline_size,
				( half_size + outline_size ) * 2.0f, ( half_size + outline_size ) * 2.0f, glow_col );
		}

		draw_list.rect_filled( cx - half_size - outline_size, cy - half_size - outline_size,
			( half_size + outline_size ) * 2.0f, ( half_size + outline_size ) * 2.0f, outline );
		draw_list.rect_filled( cx - half_size, cy - half_size, half_size * 2.0f, half_size * 2.0f, fill );
	}

} // namespace features::combat
