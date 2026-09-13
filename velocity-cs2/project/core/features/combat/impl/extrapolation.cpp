#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <protection/game_addresses.hpp>

namespace features::combat {

	void shared::lagcomp::predict_movement( extrapolation_data& data, std::uintptr_t skip_entity ) const
	{
		// 按服务器移动规则推进目标位置，并在每个 tick 处理摩擦、加速度和碰撞滑移。
		if ( !addresses::globals::game_trace_manager )
		{
			return;
		}

		const auto interval = cstypes::tick_interval;
		const auto sv_gravity = CONVAR ("sv_gravity")->get<float>( );
		const auto gravity = std::max( std::isfinite( sv_gravity ) ? sv_gravity : 0.0f, 0.0f ) *
			std::clamp( std::isfinite( data.gravity_scale ) ? data.gravity_scale : 1.0f, 0.05f, 4.0f );
		const auto started_on_ground = ( data.flags & cstypes::entity_flags::on_ground ) != 0 &&
			data.velocity.z <= 1.0f;
		const auto start = data.origin;
		auto move_velocity = data.velocity;

		if ( started_on_ground )
		{
			move_velocity.z = 0.0f;
		}
		else
		{
			move_velocity.z -= gravity * interval * 0.5f;
		}

		auto trace_result = systems::g_tracing.trace_hull(
			start, start + move_velocity * interval,
			data.obb_mins, data.obb_maxs,
			skip_entity, 0x1c3003, 4
		);

		data.origin = trace_result.end_pos;
		data.velocity = move_velocity;

		if ( trace_result.fraction < 1.0f )
		{
			const auto remaining = 1.0f - trace_result.fraction;
			const auto into_plane = data.velocity.dot( trace_result.normal );
			data.velocity -= trace_result.normal * into_plane;

			const auto slide = systems::g_tracing.trace_hull(
				trace_result.end_pos,
				trace_result.end_pos + data.velocity * ( interval * remaining ),
				data.obb_mins, data.obb_maxs,
				skip_entity, 0x1c3003, 4
			);
			data.origin = slide.end_pos;

			// Source movement can step over low ledges while grounded. Only accept a
			// step path if it lands on a walkable plane and advances farther.
			if ( started_on_ground && trace_result.normal.z < 0.7f )
			{
				constexpr auto step_height{ 18.0f };
				const auto step_up = systems::g_tracing.trace_hull(
					start, start + math::vector3{ 0.0f, 0.0f, step_height },
					data.obb_mins, data.obb_maxs,
					skip_entity, 0x1c3003, 4
				);

				if ( step_up.fraction == 1.0f )
				{
					const auto step_forward = systems::g_tracing.trace_hull(
						step_up.end_pos, step_up.end_pos + math::vector3{ move_velocity.x, move_velocity.y, 0.0f } * interval,
						data.obb_mins, data.obb_maxs,
						skip_entity, 0x1c3003, 4
					);
					const auto step_down = systems::g_tracing.trace_hull(
						step_forward.end_pos, step_forward.end_pos - math::vector3{ 0.0f, 0.0f, step_height },
						data.obb_mins, data.obb_maxs,
						skip_entity, 0x1c3003, 4
					);

					const auto slide_delta = data.origin - start;
					const auto step_delta = step_down.end_pos - start;
					if ( step_down.fraction < 1.0f && step_down.normal.z > 0.7f &&
						step_delta.length_2d( ) > slide_delta.length_2d( ) )
					{
						data.origin = step_down.end_pos;
						data.velocity.x = move_velocity.x;
						data.velocity.y = move_velocity.y;
						data.velocity.z = 0.0f;
					}
				}
			}
		}

		const auto ground_end = math::vector3{ data.origin.x, data.origin.y, data.origin.z - 2.0f };
		const auto ground_trace = systems::g_tracing.trace_hull(
			data.origin, ground_end,
			data.obb_mins, data.obb_maxs,
			skip_entity, 0x1c3003, 4
		);

		data.flags &= ~cstypes::entity_flags::on_ground;

		if ( data.velocity.z <= 0.0f && ground_trace.fraction != 1.0f && ground_trace.normal.z > 0.7f )
		{
			data.flags |= cstypes::entity_flags::on_ground;
			data.origin = ground_trace.end_pos;
			data.velocity.z = 0.0f;
		}
		else
		{
			data.velocity.z -= gravity * interval * 0.5f;
		}
	}

	std::optional<shared::lagcomp::record> shared::lagcomp::extrapolate( std::uintptr_t pawn )
	{
		// 根据最近网络记录生成完整的未来姿态；无法确认运动连续性时返回网络记录回退。
		if ( !settings::g_combat.m_lagcomp.extrapolation.value )
		{
			return std::nullopt;
		}

		std::shared_lock records_lock( this->m_records_mtx );

		auto it = this->m_records.find( pawn );
		if ( it == this->m_records.end( ) || it->second.empty( ) )
		{
			return std::nullopt;
		}

		const auto& latest = it->second.front( );
		if ( !latest.valid || !latest.pawn )
		{
			return std::nullopt;
		}

		const auto command_tick = g_shared.sh( ).client_tick( ) > 0
			? g_shared.sh( ).client_tick( )
			: g_shared.ctx( ).current_tick;
		const auto server_tick = g_shared.sh( ).server_tick( );
		const auto command_processing_ticks = server_tick > 0 && command_tick > server_tick
			? std::clamp( command_tick - server_tick, 0, 1 )
			: 0;

		const auto server_gap = server_tick > 0
			? std::max( server_tick - latest.tick, 0 ) + command_processing_ticks
			: 0;
		const auto server_available_ticks = static_cast< float >( server_gap );
		const auto target_duration_ticks = server_tick > 0
			? server_available_ticks
			: static_cast< float >( std::max( command_tick - latest.tick, 0 ) );
		const auto full_prediction_ticks = static_cast< int >( std::floor( target_duration_ticks ) );
		const auto prediction_fraction = target_duration_ticks - static_cast< float >( full_prediction_ticks );
		auto prediction_gap = full_prediction_ticks + ( prediction_fraction > 0.001f ? 1 : 0 );
		auto target_tick = latest.tick + prediction_gap;
		if ( target_duration_ticks <= 0.001f )
		{
			return std::nullopt;
		}

		const auto max_extrap = settings::g_combat.m_lagcomp.extrapolation_auto.value
			? 18
			: std::clamp( settings::g_combat.m_lagcomp.max_extrapolate_ticks.value, 1, 18 );
		if ( prediction_gap > max_extrap && it->second.size( ) > 1 )
		{
#if defined( DEV )
			diag::writef(
				diag::level::info,
				"[lagcomp_extrap_skip] pawn=%llx latest=%d target=%d target_gap=%d server_gap=%d max=%d server=%d command=%d duration=%.3f",
				static_cast< unsigned long long >( pawn ),
				latest.tick,
				target_tick,
				prediction_gap,
				server_gap,
				max_extrap,
				server_tick,
				command_tick,
				target_duration_ticks );
#endif
			return std::nullopt;
		}

		if ( prediction_gap <= 0 )
		{
			return std::nullopt;
		}
		constexpr auto max_record_delta{ 128.0f };
		constexpr auto max_player_speed{ 4096.0f };
		constexpr auto max_horizontal_acceleration{ 5000.0f };

		auto finite_vector = [ ]( const math::vector3& value )
			{
				return std::isfinite( value.x ) && std::isfinite( value.y ) && std::isfinite( value.z );
			};

		auto measured_velocity = [ & ]( const record& newer, const record& older ) -> std::optional<math::vector3>
			{
				const auto dt = newer.simulation_time - older.simulation_time;
				const auto delta = newer.origin - older.origin;
				if ( dt <= 0.0f || dt > 0.25f || !finite_vector( delta ) || delta.length( ) > max_record_delta )
				{
					return std::nullopt;
				}

				const auto value = delta * ( 1.0f / dt );
				if ( !finite_vector( value ) || value.length_2d( ) > max_player_speed )
				{
					return std::nullopt;
				}

				return value;
			};

		auto velocity = latest.velocity;
		const auto network_velocity_valid = finite_vector( velocity ) && velocity.length_2d( ) <= max_player_speed;
		const auto latest_measured = it->second.size( ) > 1
			? measured_velocity( latest, it->second[ 1 ] )
			: std::nullopt;
		const auto previous_measured = it->second.size( ) > 2
			? measured_velocity( it->second[ 1 ], it->second[ 2 ] )
			: std::nullopt;
		const auto pawn_gravity_scale = memory::read<float>( pawn + SCHEMA( "C_BaseEntity", "m_flGravityScale"_hash ) );
		const auto gravity_scale = std::clamp(
			std::isfinite( pawn_gravity_scale ) ? pawn_gravity_scale : 1.0f,
			0.05f,
			4.0f );
		const auto sv_gravity = CONVAR ("sv_gravity")->get<float>( );
		const auto gravity = std::max( std::isfinite( sv_gravity ) ? sv_gravity : 0.0f, 0.0f ) * gravity_scale;

		auto velocity_disagreement{ 0.0f };
		auto measured_acceleration = math::vector3{};
		if ( latest_measured.has_value( ) )
		{
			velocity_disagreement = network_velocity_valid
				? ( velocity - *latest_measured ).length_2d( )
				: 0.0f;

			if ( previous_measured.has_value( ) )
			{
				const auto current_dt = std::clamp( latest.simulation_time - it->second[ 1 ].simulation_time, cstypes::tick_interval, 0.25f );
				const auto previous_dt = std::clamp( it->second[ 1 ].simulation_time - it->second[ 2 ].simulation_time, cstypes::tick_interval, 0.25f );
				const auto midpoint_dt = std::max( ( current_dt + previous_dt ) * 0.5f, cstypes::tick_interval );
				measured_acceleration = ( *latest_measured - *previous_measured ) * ( 1.0f / midpoint_dt );
				measured_acceleration.z = 0.0f;
				const auto accel_length = measured_acceleration.length_2d( );
				if ( accel_length > max_horizontal_acceleration )
				{
					measured_acceleration *= max_horizontal_acceleration / accel_length;
				}
			}

			const auto current_dt = std::clamp( latest.simulation_time - it->second[ 1 ].simulation_time, cstypes::tick_interval, 0.25f );
			const auto measured_current = *latest_measured + measured_acceleration * ( current_dt * 0.5f );
			const auto measured_weight = network_velocity_valid ? 0.72f : 1.0f;
			velocity.x = measured_current.x * measured_weight + velocity.x * ( 1.0f - measured_weight );
			velocity.y = measured_current.y * measured_weight + velocity.y * ( 1.0f - measured_weight );
		}
		else if ( !network_velocity_valid )
		{
			return std::nullopt;
		}

		const auto measured_dt = latest_measured.has_value( )
			? std::clamp( latest.simulation_time - it->second[ 1 ].simulation_time, cstypes::tick_interval, 0.25f )
			: 0.0f;
		const auto measured_airborne = latest_measured.has_value( ) && latest_measured->z > 8.0f;
		const auto network_airborne = velocity.z > 8.0f;
		const auto airborne = ( latest.flags & cstypes::entity_flags::on_ground ) == 0 || measured_airborne || network_airborne;

		if ( !airborne )
		{
			velocity.z = 0.0f;
		}
		else if ( latest_measured.has_value( ) )
		{
			const auto measured_current_z = latest_measured->z + gravity * measured_dt * 0.5f;
			const auto vertical_disagreement = std::fabsf( velocity.z - measured_current_z );
			const auto network_weight = std::clamp( 0.70f - vertical_disagreement / 1200.0f, 0.25f, 0.70f );
			velocity.z = velocity.z * network_weight + measured_current_z * ( 1.0f - network_weight );
		}

		auto speed = velocity.length_2d( );
		if ( speed < 0.1f && ( latest.flags & cstypes::entity_flags::on_ground ) )
		{
			return std::nullopt;
		}
		if ( !latest_measured.has_value( ) )
		{
			prediction_gap = std::min( prediction_gap, 1 );
		}

		if ( airborne
			&& !settings::g_combat.m_lagcomp.extrapolation_air.value )
		{
			return std::nullopt;
		}

		auto acceleration = measured_acceleration;
		auto heading_change = 0.0f;

		if ( velocity_disagreement > 768.0f )
		{
			acceleration *= 0.65f;
			prediction_gap = std::min( prediction_gap, 3 );
		}
		else if ( velocity_disagreement > 384.0f )
		{
			prediction_gap = std::min( prediction_gap, 4 );
		}

		if ( previous_measured.has_value( ) && latest_measured.has_value( ) )
		{
			const auto current_speed = latest_measured->length_2d( );
			const auto previous_speed = previous_measured->length_2d( );
			if ( current_speed > 20.0f && previous_speed > 20.0f )
			{
				const auto latest_heading = std::atan2f( latest_measured->y, latest_measured->x );
				const auto previous_heading = std::atan2f( previous_measured->y, previous_measured->x );
				auto heading_delta = latest_heading - previous_heading;
				while ( heading_delta > 3.14159265f ) heading_delta -= 6.28318530f;
				while ( heading_delta < -3.14159265f ) heading_delta += 6.28318530f;
				heading_change = std::fabsf( heading_delta );
				const auto alignment = latest_measured->dot( *previous_measured ) /
					( current_speed * previous_speed );
				if ( alignment < -0.15f )
					prediction_gap = std::min( prediction_gap, 2 );
				else if ( alignment < 0.5f )
					prediction_gap = std::min( prediction_gap, 4 );
			}
		}

		if ( heading_change > 1.05f )
			prediction_gap = std::min( prediction_gap, 1 );
		else if ( heading_change > 0.60f )
			prediction_gap = std::min( prediction_gap, 2 );

		const auto extrapolation_speed = std::max( speed,
			latest_measured.has_value( ) ? latest_measured->length_2d( ) : speed );
		if ( extrapolation_speed > 1.0f )
		{
			constexpr auto max_extrapolation_distance = 130.0f;
			const auto distance_ticks = static_cast< int >( std::floor(
				max_extrapolation_distance / ( extrapolation_speed * cstypes::tick_interval ) ) );
			prediction_gap = std::min( prediction_gap, std::max( 1, distance_ticks ) );
		}
		target_tick = latest.tick + prediction_gap;

		auto extrapolation_stability = latest_measured.has_value( ) ? 0.58f : 0.30f;
		if ( latest_measured.has_value( ) )
		{
			const auto velocity_consistency = network_velocity_valid
				? 1.0f - std::clamp( velocity_disagreement / 768.0f, 0.0f, 1.0f )
				: 0.55f;
			extrapolation_stability += velocity_consistency * 0.22f;
		}
		if ( previous_measured.has_value( ) && latest_measured.has_value( ) )
		{
			const auto current_speed = latest_measured->length_2d( );
			const auto previous_speed = previous_measured->length_2d( );
			if ( current_speed > 20.0f && previous_speed > 20.0f )
			{
				const auto alignment = latest_measured->dot( *previous_measured ) /
					( current_speed * previous_speed );
				const auto direction_consistency = std::clamp( ( alignment + 1.0f ) * 0.5f, 0.0f, 1.0f );
				extrapolation_stability += direction_consistency * 0.14f;
			}
		}
		const auto acceleration_consistency = 1.0f - std::clamp(
			measured_acceleration.length_2d( ) / 5000.0f, 0.0f, 1.0f );
		extrapolation_stability += acceleration_consistency * 0.06f;
		const auto window_penalty = std::clamp(
			static_cast< float >( std::max( prediction_gap - 2, 0 ) ) / 12.0f,
			0.0f,
			1.0f );
		extrapolation_stability *= 1.0f - window_penalty * 0.25f;
		extrapolation_stability = std::clamp( extrapolation_stability, 0.0f, 1.0f );
		const auto configured_stability = std::clamp(
			static_cast< float >( settings::g_combat.m_lagcomp.extrapolation_stability.value ) / 100.0f,
			0.0f,
			1.0f );
		const auto required_stability = 0.30f + configured_stability * 0.50f;
		if ( configured_stability > 0.0f && prediction_gap > 1 && extrapolation_stability < required_stability )
		{
#if defined( DEV )
			diag::writef(
				diag::level::info,
				"[lagcomp_extrap_skip] pawn=%llx reason=stability measured=%.3f required=%.3f gap=%d",
				static_cast< unsigned long long >( pawn ),
				extrapolation_stability,
				required_stability,
				prediction_gap );
#endif
			return std::nullopt;
		}

		if ( !( latest.flags & cstypes::entity_flags::on_ground ) )
		{
			acceleration.x *= 0.35f;
			acceleration.y *= 0.35f;
		}

		const auto game_scene_node = memory::read<std::uintptr_t>( pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
		if ( !game_scene_node )
		{
			return std::nullopt;
		}

		const auto collision = memory::read<std::uintptr_t>( pawn + SCHEMA( "C_BaseEntity", "m_pCollision"_hash ) );
		math::vector3 obb_mins{}, obb_maxs{};

		if ( collision )
		{
			obb_mins = memory::read<math::vector3>( collision + SCHEMA( "CCollisionProperty", "m_vecMins"_hash ) );
			obb_maxs = memory::read<math::vector3>( collision + SCHEMA( "CCollisionProperty", "m_vecMaxs"_hash ) );
		}
		else
		{
			obb_mins = { -16.0f, -16.0f, 0.0f };
			obb_maxs = { 16.0f, 16.0f, 72.0f };
		}

		extrapolation_data data{};
		data.origin = latest.origin;
		data.velocity = velocity;
		data.acceleration = acceleration;
		data.obb_mins = obb_mins;
		data.obb_maxs = obb_maxs;
		data.flags = latest.flags;
		data.sim_time = latest.simulation_time;
		data.direction = 0.0f;
		data.turn_rate = 0.0f;
		data.gravity_scale = gravity_scale;
		if ( latest_measured.has_value( ) && latest_measured->length_2d( ) > 1.0f )
		{
			data.direction = std::atan2f( latest_measured->y, latest_measured->x ) * ( 180.0f / 3.14159265f );
		}
		else if ( velocity.length_2d( ) > 1.0f )
		{
			data.direction = std::atan2f( velocity.y, velocity.x ) * ( 180.0f / 3.14159265f );
		}
		if ( latest_measured.has_value( ) && previous_measured.has_value( ) )
		{
			const auto latest_speed = latest_measured->length_2d( );
			const auto previous_speed = previous_measured->length_2d( );
			if ( latest_speed > 24.0f && previous_speed > 24.0f )
			{
				const auto latest_heading = std::atan2f( latest_measured->y, latest_measured->x );
				const auto previous_heading = std::atan2f( previous_measured->y, previous_measured->x );
				auto heading_delta = latest_heading - previous_heading;
				while ( heading_delta > 3.14159265f ) heading_delta -= 6.28318530f;
				while ( heading_delta < -3.14159265f ) heading_delta += 6.28318530f;
				const auto turn_dt = std::max( ( latest.simulation_time - it->second[ 1 ].simulation_time +
					it->second[ 1 ].simulation_time - it->second[ 2 ].simulation_time ) * 0.5f, cstypes::tick_interval );
				data.turn_rate = std::clamp( heading_delta / turn_dt, -12.0f, 12.0f );
			}
		}

		const auto max_sim_speed = std::min(
			max_player_speed,
			std::max( 320.0f, speed * 1.35f + 64.0f ) );

		auto simulated_ticks = 0;
		const auto full_ticks = std::min( full_prediction_ticks, prediction_gap );
		for ( auto i = 0; i < full_ticks; ++i )
		{
			const auto previous_velocity = data.velocity;
			data.velocity.x += data.acceleration.x * cstypes::tick_interval;
			data.velocity.y += data.acceleration.y * cstypes::tick_interval;

			const auto previous_horizontal = math::vector3{ previous_velocity.x, previous_velocity.y, 0.0f };
			const auto next_horizontal = math::vector3{ data.velocity.x, data.velocity.y, 0.0f };
			const auto braking = previous_horizontal.dot( math::vector3{ data.acceleration.x, data.acceleration.y, 0.0f } ) < 0.0f;
			if ( braking && previous_horizontal.length_2d( ) > 1.0f &&
				previous_horizontal.dot( next_horizontal ) <= 0.0f )
			{
				data.velocity.x = 0.0f;
				data.velocity.y = 0.0f;
				data.acceleration.x = 0.0f;
				data.acceleration.y = 0.0f;
			}
			const auto integrated_speed = data.velocity.length_2d( );
			if ( integrated_speed > max_sim_speed )
			{
				const auto scale = max_sim_speed / integrated_speed;
				data.velocity.x *= scale;
				data.velocity.y *= scale;
			}

			if ( !( data.flags & cstypes::entity_flags::on_ground ) &&
				std::fabsf( data.turn_rate ) > 0.01f && data.velocity.length_2d( ) > 24.0f )
			{
				const auto turn = std::clamp( data.turn_rate * cstypes::tick_interval * 0.35f, -0.12f, 0.12f );
				const auto cs = std::cosf( turn );
				const auto sn = std::sinf( turn );
				const auto old_x = data.velocity.x;
				const auto old_y = data.velocity.y;
				data.velocity.x = old_x * cs - old_y * sn;
				data.velocity.y = old_x * sn + old_y * cs;
				data.turn_rate *= 0.62f;
			}

			data.sim_time += cstypes::tick_interval;
			++simulated_ticks;

			this->predict_movement( data, pawn );

			const auto acceleration_decay = ( data.flags & cstypes::entity_flags::on_ground )
				? 0.72f
				: 0.50f;
			data.acceleration.x *= acceleration_decay;
			data.acceleration.y *= acceleration_decay;

		}

		const auto fractional_tail = ( prediction_fraction > 0.001f && simulated_ticks < prediction_gap )
			? prediction_fraction * cstypes::tick_interval
			: 0.0f;
		if ( fractional_tail > 0.0f )
		{
			const auto tail_start = data.origin;
			data.velocity.x += data.acceleration.x * fractional_tail;
			data.velocity.y += data.acceleration.y * fractional_tail;
			data.velocity.z -= gravity * fractional_tail * 0.5f;
			const auto tail_end = tail_start + data.velocity * fractional_tail;
			const auto tail_trace = systems::g_tracing.trace_hull(
				tail_start, tail_end, data.obb_mins, data.obb_maxs,
				pawn, 0x1c3003, 4 );
			data.origin = tail_trace.end_pos;
			data.sim_time += fractional_tail;
			++simulated_ticks;
		}

		if ( simulated_ticks != prediction_gap )
		{
			return std::nullopt;
		}

		const auto origin_delta = data.origin - latest.origin;
		{
			const auto reference_velocity = latest_measured.has_value( )
				? math::vector3{ latest_measured->x, latest_measured->y, 0.0f }
				: math::vector3{ velocity.x, velocity.y, 0.0f };
			const auto reference_speed = reference_velocity.length_2d( );
			if ( reference_speed > 16.0f )
			{
				const auto forward_displacement = origin_delta.x * reference_velocity.x + origin_delta.y * reference_velocity.y;
				if ( forward_displacement <= 0.0f )
				{
					return std::nullopt;
				}
			}

			const auto live_scene_node = memory::read<std::uintptr_t>( pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
			if ( live_scene_node && reference_speed > 16.0f )
			{
				const auto live_origin = memory::read<math::vector3>( live_scene_node + SCHEMA( "CGameSceneNode", "m_vecAbsOrigin"_hash ) );
				const auto live_delta = live_origin - latest.origin;
				const auto direction = reference_velocity * ( 1.0f / reference_speed );
				const auto simulated_projection = origin_delta.x * direction.x + origin_delta.y * direction.y;
				const auto live_projection = live_delta.x * direction.x + live_delta.y * direction.y;
				const auto projection_tolerance = std::max(
					8.0f,
					reference_speed * cstypes::tick_interval * 3.0f );
				if ( std::isfinite( live_projection ) && live_projection > 0.0f &&
					( simulated_projection + projection_tolerance < live_projection ||
						simulated_projection > live_projection + projection_tolerance ) )
				{
					return std::nullopt;
				}
			}
		}

		if ( origin_delta.length_sqr( ) < 0.01f )
		{
			return std::nullopt;
		}

		record extrap_record = latest;
		extrap_record.origin = data.origin;
		extrap_record.simulation_time = data.sim_time;
		extrap_record.tick = target_tick;
		extrap_record.extrapolated_ticks = simulated_ticks;
		extrap_record.extrapolation_stability = extrapolation_stability;
		extrap_record.velocity = data.velocity;
		extrap_record.flags = data.flags;
		extrap_record.extrapolated = true;

		for ( auto i = 0; i < extrap_record.bone_count && i < 128; ++i )
		{
			extrap_record.bones[ i ].position += origin_delta;
		}


#if defined( DEV )
		diag::writef(
			diag::level::info,
			"[lagcomp_extrap] pawn=%llx latest=%d server=%d server_gap=%d target=%d target_gap=%d command=%d steps=%d predicted=%d stability=%.3f duration=%.3f origin_delta=(%.2f,%.2f,%.2f) velocity=(%.2f,%.2f,%.2f)",
			static_cast< unsigned long long >( pawn ),
			latest.tick,
			server_tick,
			server_gap,
			target_tick,
			prediction_gap,
			command_tick,
			simulated_ticks,
			extrap_record.tick,
			extrapolation_stability,
			target_duration_ticks,
			origin_delta.x,
			origin_delta.y,
			origin_delta.z,
			data.velocity.x,
			data.velocity.y,
			data.velocity.z );
#endif

		return extrap_record;
	}

} // namespace features::combat
