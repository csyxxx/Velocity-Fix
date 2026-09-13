#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <utilities/diag.hpp>
#include <utilities/logging/logging.hpp>
#include <protection/game_addresses.hpp>
#include "../systems.hpp"

namespace {

	[[nodiscard]] void* call_history_message_alloc(
		std::uintptr_t fn,
		void* prototype,
		std::uintptr_t arena )
	{
		void* raw{};
		__try
		{
			raw = memory::call<void*>( fn, prototype, reinterpret_cast<void*>( arena ) );
		}
		__except ( EXCEPTION_EXECUTE_HANDLER )
		{
			return nullptr;
		}

		return raw;
	}

	template <typename Message>
	Message* allocate_history_message( Message* prototype, std::uintptr_t arena )
	{
		if ( !prototype )
		{
			return nullptr;
		}

		const auto vtable = memory::safe_read<std::uintptr_t>( reinterpret_cast<std::uintptr_t>( prototype ) );
		if ( !vtable || !*vtable )
		{
			return nullptr;
		}

		const auto allocate = memory::safe_read<std::uintptr_t>( *vtable + 2 * sizeof( std::uintptr_t ) );
		if ( !allocate || !*allocate )
		{
			return nullptr;
		}

		return reinterpret_cast<Message*>( call_history_message_alloc( *allocate, prototype, arena ) );
	}

	[[nodiscard]] std::optional<std::uintptr_t> resolve_history_arena( void* raw_arena )
	{
		const auto arena_raw = reinterpret_cast<std::uintptr_t>( raw_arena );
		auto arena = arena_raw & ~std::uintptr_t{ 3 };
		if ( arena_raw & 1 )
		{
			const auto indirection = memory::safe_read<std::uintptr_t>( arena );
			if ( !indirection )
			{
				return std::nullopt;
			}

			arena = *indirection;
		}

		return arena;
	}

	[[nodiscard]] bool history_alloc_fingerprint( std::uintptr_t candidate )
	{
		if ( !candidate )
		{
			return false;
		}

		constexpr std::uint8_t prologue[ 8 ] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83 };
		constexpr std::uint8_t size_imm[ 5 ] = { 0xB9, 0x78, 0x00, 0x00, 0x00 };

		__try
		{
			const auto* bytes = reinterpret_cast<const std::uint8_t*>( candidate );
			for ( auto i = 0; i < 8; ++i )
			{
				if ( bytes[ i ] != prologue[ i ] )
				{
					return false;
				}
			}

			for ( auto i = 0; i < 5; ++i )
			{
				if ( bytes[ 0x14 + i ] != size_imm[ i ] )
				{
					return false;
				}
			}

			return true;
		}
		__except ( EXCEPTION_EXECUTE_HANDLER )
		{
			return false;
		}
	}

	[[nodiscard]] std::uintptr_t verified_history_alloc( )
	{
		static std::atomic<std::uintptr_t> cached{ 1 };

		auto fn = cached.load( std::memory_order_acquire );
		if ( fn != 1 )
		{
			return fn;
		}

		const auto candidate = PATTERN( patterns::history_field_alloc );
		const auto verified = history_alloc_fingerprint( candidate ) ? candidate : std::uintptr_t{ 0 };
		cached.store( verified, std::memory_order_release );
		return verified;
	}

	// Keep SEH in POD-only helpers; MSVC rejects __try in functions with
	// unwindable C++ locals.
	[[nodiscard]] void* call_history_alloc( std::uintptr_t fn, std::uintptr_t arena )
	{
		void* raw{};
		__try
		{
			raw = memory::call<void*>( fn, arena );
		}
		__except ( EXCEPTION_EXECUTE_HANDLER )
		{
			return nullptr;
		}

		return raw;
	}

	[[nodiscard]] bool call_history_vector_push( std::uintptr_t fn, std::uintptr_t field, std::uintptr_t raw )
	{
		__try
		{
			memory::call<std::uintptr_t>( fn, field, raw );
			return true;
		}
		__except ( EXCEPTION_EXECUTE_HANDLER )
		{
			return false;
		}
	}

} // namespace

namespace systems {

	void input::update( )
	{
		const auto local_controller = memory::read<std::uintptr_t>( addresses::globals::local_player_controller );
		if ( !local_controller )
		{
			this->m_current_cmd = nullptr;
			return;
		}

		this->m_current_cmd = this->get_current_cmd( local_controller );
	}

	void input::apply( )
	{
		const auto local = systems::g_local.get ();

		if ( !this->m_current_cmd ) {
			return;
		}

		const auto base = this->m_current_cmd->csgo_user_cmd.mutable_base( );
		if ( !base ) {
			return;
		}

		auto has_move_subticks = [] ( proto::base_usercmd_pb* base_cmd ) {
			for ( auto i = 0; i < base_cmd->subtick_moves_size( ); ++i )
			{
				const auto step = base_cmd->mutable_subtick_moves( i );
				if ( step && ( step->m_has_bits.test( 0x8u ) || step->m_has_bits.test( 0x10u ) ) )
				{
					return true;
				}
			}

			return false;
		};

		// fix movement for ag2
		diag::set_exception_phase( "input apply: subtick movement" );
		const auto subtick_moves = base->mutable_subtick_moves( );
		if ( !has_move_subticks( base ) )
		{
			const auto movement_services = local.pawn ? memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BasePlayerPawn", "m_pMovementServices"_hash ) ) : 0;
			if ( movement_services && subtick_moves )
			{
				if ( const auto step = systems::g_input.acquire_subtick_step( subtick_moves ) )
				{
					step->set_when( 0.0f );
					step->set_analog_forward_delta( base->forwardmove( ) - memory::read<float>( movement_services + SCHEMA( "CPlayer_MovementServices", "m_flCmdForwardMove"_hash ) ) );
					step->set_analog_left_delta( base->leftmove( ) - memory::read<float>( movement_services + SCHEMA( "CPlayer_MovementServices", "m_flCmdLeftMove"_hash ) ) );
				}
			}
		}

		// Features append independently; publish every step in chronological order.
		if ( subtick_moves )
		{
			for ( auto i = 1; i < subtick_moves->size( ); ++i )
			{
				for ( auto j = i; j > 0; --j )
				{
					const auto current = subtick_moves->mutable_at( j );
					const auto previous = subtick_moves->mutable_at( j - 1 );
					if ( !current || !previous || previous->when( ) <= current->when( ) )
					{
						break;
					}

					std::swap( *previous, *current );
				}
			}
		}

		diag::set_exception_phase( "input apply: buttons" );
		auto buttons = const_cast<proto::in_button_state_pb*>( base->buttons_pb( ) );
		if ( !buttons )
		{
			const auto raw_base =
				reinterpret_cast<std::uintptr_t>( base ) - proto::message_impl_offset;
			const auto arena_bits = memory::read<std::uintptr_t>( raw_base + 0x08 );
			auto arena = arena_bits & ~0x3ull;
			if ( arena_bits & 1 )
			{
				arena = memory::read<std::uintptr_t>( arena );
			}

			const auto raw_buttons =
				memory::call<void*>( PATTERN( patterns::button_state_alloc ), arena );
			if ( raw_buttons )
			{
				base->m_buttons_pb =
					reinterpret_cast<proto::in_button_state_pb*>( raw_buttons );
				buttons = proto::impl_ptr<proto::in_button_state_pb>( raw_buttons );
			}
		}

		if ( buttons )
		{
			base->m_has_bits.set( 0x2u );
			buttons->set_buttonstate1( this->m_current_cmd->buttons.value );
			buttons->set_buttonstate2( this->m_current_cmd->buttons.value_changed );
			buttons->set_buttonstate3( this->m_current_cmd->buttons.value_scroll );
		}

		diag::set_exception_phase( "input apply: crc" );
		this->calculate_crc( base );
	}

	void input::apply( usercmd* cmd )
	{
		const auto previous = this->m_current_cmd;
		this->m_current_cmd = cmd;
		this->apply( );
		this->m_current_cmd = previous;
	}

	input::usercmd* input::get_current_cmd( std::uintptr_t local_controller ) const
	{
		const auto get_usercmd = PATTERN (patterns::get_usercmd);
		if ( !get_usercmd )
		{
			return nullptr;
		}

		const auto sequence = this->get_command_sequence( local_controller );
		if ( !sequence )
		{
			return nullptr;
		}

		return memory::call<usercmd*>( get_usercmd, local_controller, *sequence );
	}

	std::optional<int> input::get_command_sequence( std::uintptr_t local_controller ) const
	{
		const auto get_usercmd_base = PATTERN (patterns::get_usercmd_base);
		if ( !get_usercmd_base || !local_controller )
		{
			return std::nullopt;
		}

		const auto usercmd_base = memory::call<std::uintptr_t>( get_usercmd_base, local_controller );
		if ( !usercmd_base )
		{
			return std::nullopt;
		}

		return memory::safe_read<int>( usercmd_base + 0x5910 );
	}

	proto::subtick_move_step* input::acquire_subtick_step( proto::repeated_ptr_field<proto::subtick_move_step>* subtick_moves ) const
	{
		if ( !subtick_moves )
		{
			return nullptr;
		}

		if ( subtick_moves->m_rep )
		{
			if ( subtick_moves->m_current_size < subtick_moves->m_rep->allocated_size )
			{
				const auto element = subtick_moves->m_rep->elements[ subtick_moves->m_current_size ];
				if ( element )
				{
					subtick_moves->m_current_size++;
					const auto step = proto::impl_ptr<proto::subtick_move_step>( element );
					*step = {};
					return step;
				}
			}
		}

		const auto move_step = memory::call<void*>(PATTERN (patterns::subtick_move_alloc), subtick_moves->m_arena );
		if ( move_step )
		{
			memory::call<std::uintptr_t>(PATTERN (patterns::utl_vector_push), reinterpret_cast< std::uintptr_t >( subtick_moves ), reinterpret_cast< std::uintptr_t >( move_step ) );
			return proto::impl_ptr<proto::subtick_move_step>( move_step );
		}

		return nullptr;
	}

	math::vector3 input::get_view_angles( ) const
	{
		return *memory::call<math::vector3*>(PATTERN (patterns::get_view_angles), addresses::globals::csgo_input, 0 );
	}

	bool input::set_shoot_position( usercmd* cmd, proto::input_history_entry* entry, const math::vector3& position ) const
	{
		if ( !cmd || !entry )
		{
			return false;
		}

		if ( !std::isfinite( position.x ) || !std::isfinite( position.y ) ||
			!std::isfinite( position.z ) || position.length_sqr( ) < 1.0f )
		{
			return false;
		}

		auto history_field = cmd->csgo_user_cmd.mutable_input_history( );
		if ( !history_field )
		{
			return false;
		}

		const auto arena = resolve_history_arena( history_field->m_arena );
		if ( !arena )
		{
			return false;
		}

		if ( !entry->m_shoot_position )
		{
			proto::msg_vector* prototype{ nullptr };
			const auto inspect_candidate = [ & ]( proto::input_history_entry* candidate )
			{
				if ( !candidate )
				{
					return;
				}

				if ( candidate->m_shoot_position )
				{
					prototype = candidate->m_shoot_position;
					return;
				}

				if ( candidate->m_target_head_pos_check )
				{
					prototype = candidate->m_target_head_pos_check;
				}
				else if ( candidate->m_target_abs_pos_check )
				{
					prototype = candidate->m_target_abs_pos_check;
				}
			};

			for ( auto i = 0; i < history_field->m_current_size && !prototype; ++i )
			{
				inspect_candidate( cmd->csgo_user_cmd.mutable_input_history( i ) );
			}

			if ( !prototype && history_field->m_rep )
			{
				const auto pooled_size = std::clamp( history_field->m_rep->allocated_size, 0, 64 );
				for ( auto i = 0; i < pooled_size && !prototype; ++i )
				{
					inspect_candidate( proto::impl_ptr<proto::input_history_entry>( history_field->m_rep->elements[ i ] ) );
				}
			}

			if ( !prototype )
			{
				prototype = this->m_shoot_position_prototype;
			}

			if ( !prototype )
			{
				return false;
			}

			entry->m_shoot_position = allocate_history_message( prototype, *arena );
			if ( !entry->m_shoot_position )
			{
				return false;
			}

			this->m_shoot_position_prototype = entry->m_shoot_position;
		}

		if ( auto shoot_position = entry->mutable_shoot_position( ) )
		{
			this->m_shoot_position_prototype = entry->m_shoot_position;
			shoot_position->set_x( position.x );
			shoot_position->set_y( position.y );
			shoot_position->set_z( position.z );
			return true;
		}

		return false;
	}

	proto::input_history_entry* input::push_input_history( usercmd* cmd, const input_history_params& params ) const
	{
		if ( !cmd )
		{
			return nullptr;
		}

		auto history_field = cmd->csgo_user_cmd.mutable_input_history( );
		if ( !history_field )
		{
			return nullptr;
		}

		constexpr auto history_capacity{ 16 };
		if ( history_field->m_current_size < 0
			|| history_field->m_current_size >= history_capacity )
		{
			return nullptr;
		}

		proto::input_history_entry* entry{ nullptr };

		if ( history_field->m_rep )
		{
			if ( history_field->m_current_size < history_field->m_rep->allocated_size )
			{
				auto element = history_field->m_rep->elements[ history_field->m_current_size ];
				if ( element )
				{
					history_field->m_current_size++;
					entry = proto::impl_ptr<proto::input_history_entry>( element );
				}
			}
		}

		if ( !entry )
		{
			const auto alloc = verified_history_alloc( );
			if ( !alloc )
			{
				logging::console::print( xs( "[push] history allocator fingerprint mismatch - entry skipped" ) );
				return nullptr;
			}

			const auto arena = resolve_history_arena( history_field->m_arena );
			if ( !arena )
			{
				return nullptr;
			}

			auto raw = call_history_alloc( alloc, *arena );
			if ( !raw )
			{
				return nullptr;
			}

			const auto push_fn = PATTERN( patterns::utl_vector_push );
			if ( !push_fn
				|| !call_history_vector_push(
					push_fn,
					reinterpret_cast<std::uintptr_t>( history_field ),
					reinterpret_cast<std::uintptr_t>( raw ) ) )
			{
				return nullptr;
			}

			entry = proto::impl_ptr<proto::input_history_entry>( raw );
		}

		if ( !entry )
		{
			return nullptr;
		}

		entry->m_has_bits.bits[ 0 ] &= ~( 0x1FFu | 0x18000u );
		entry->m_attack_flags = 0;
		entry->m_attack_unk = 0;

		if ( !entry->m_view_angles )
		{
			const auto base = cmd->csgo_user_cmd.mutable_base( );
			const auto arena = resolve_history_arena( history_field->m_arena );
			entry->m_view_angles = arena
				? allocate_history_message( base ? base->m_viewangles : nullptr, *arena )
				: nullptr;
			if ( !entry->m_view_angles )
			{
				--history_field->m_current_size;
				return nullptr;
			}
		}

		entry->set_render_tick_count( params.render_tick );
		entry->set_render_tick_fraction( params.render_frac );
		entry->set_player_tick_count( params.player_tick );
		entry->set_player_tick_fraction( params.player_frac );
		entry->set_frame_number( params.frame_number );
		entry->set_target_ent_index( params.target_ent_index );

		if ( auto va = entry->mutable_view_angles( ) )
		{
			va->set_x( params.view_angles.x );
			va->set_y( params.view_angles.y );
			va->set_z( params.view_angles.z );
		}

		const auto has_shoot_position = params.fill_cheat_check_data
			|| std::fabsf( params.shoot_position.x ) > 0.001f
			|| std::fabsf( params.shoot_position.y ) > 0.001f
			|| std::fabsf( params.shoot_position.z ) > 0.001f
			|| entry->m_shoot_position != nullptr;
		if ( has_shoot_position )
		{
			static_cast< void >( this->set_shoot_position( cmd, entry, params.shoot_position ) );
		}

		if ( entry->m_cl_interp )
		{
			if ( auto ci = entry->mutable_cl_interp( ) )
			{
				ci->set_frac( params.cl_interp_frac );
			}
		}

		if ( entry->m_sv_interp0 )
		{
			if ( auto si0 = entry->mutable_sv_interp0( ) )
			{
				si0->set_src_tick( params.sv_interp0_src );
				si0->set_dst_tick( params.sv_interp0_dst );
				si0->set_frac( params.sv_interp0_frac );
			}
		}

		if ( entry->m_sv_interp1 )
		{
			if ( auto si1 = entry->mutable_sv_interp1( ) )
			{
				si1->set_src_tick( params.sv_interp1_src );
				si1->set_dst_tick( params.sv_interp1_dst );
				si1->set_frac( params.sv_interp1_frac );
			}
		}

		if ( entry->m_player_interp )
		{
			if ( auto pi = entry->mutable_player_interp( ) )
			{
				pi->set_src_tick( params.player_interp_src );
				pi->set_dst_tick( params.player_interp_dst );
				pi->set_frac( params.player_interp_frac );
			}
		}

		if ( params.fill_cheat_check_data )
		{
			if ( entry->m_target_head_pos_check )
			{
				if ( auto head = entry->mutable_target_head_pos_check( ) )
				{
					head->set_x( params.target_head_pos.x );
					head->set_y( params.target_head_pos.y );
					head->set_z( params.target_head_pos.z );
				}
			}

			if ( entry->m_target_abs_pos_check )
			{
				if ( auto abs_pos = entry->mutable_target_abs_pos_check( ) )
				{
					abs_pos->set_x( params.target_abs_pos.x );
					abs_pos->set_y( params.target_abs_pos.y );
					abs_pos->set_z( params.target_abs_pos.z );
				}
			}

			if ( entry->m_target_abs_ang_check )
			{
				if ( auto abs_ang = entry->mutable_target_abs_ang_check( ) )
				{
					abs_ang->set_x( params.target_abs_ang.x );
					abs_ang->set_y( params.target_abs_ang.y );
					abs_ang->set_z( params.target_abs_ang.z );
				}
			}
		}

		return entry;
	}

	int input::emit_view_angle_history( usercmd* cmd, const math::vector3& from, const math::vector3& to ) const
	{
		if ( !cmd )
		{
			return 0;
		}

		constexpr auto history_capacity{ 16 };
		const auto current_size = cmd->csgo_user_cmd.input_history_size( );
		const auto remaining = history_capacity - current_size;
		if ( remaining <= 0 )
		{
			return 0;
		}

		auto render_tick{ 0 };
		for ( auto i = 0; i < current_size; ++i )
		{
			const auto entry = cmd->csgo_user_cmd.mutable_input_history( i );
			if ( entry && entry->has_render_tick_count( ) )
			{
				render_tick = entry->render_tick_count( );
			}
		}

		auto delta = math::vector3
		{
			std::remainderf( to.x - from.x, 360.0f ),
			std::remainderf( to.y - from.y, 360.0f ),
			std::remainderf( to.z - from.z, 360.0f )
		};

		constexpr auto when_base{ 0.023590f };
		constexpr auto when_range{ 0.908453f };
		auto emitted{ 0 };

		for ( auto i = 0; i < remaining; ++i )
		{
			const auto fraction = static_cast< float >( i + 1 ) / static_cast< float >( remaining );
			input_history_params params{};
			params.view_angles = from + delta * fraction;
			params.render_tick = render_tick;
			params.render_frac = when_base + fraction * when_range;
			params.player_tick = 0;
			params.player_frac = 0.0f;

			const auto entry = this->push_input_history( cmd, params );
			if ( !entry )
			{
				break;
			}

			entry->m_has_bits.bits[ 0 ] &= 0x1E01u;
			++emitted;
		}

#if defined( DEV )
		if ( emitted > 0 )
		{
			static std::atomic_uint32_t log_count{};
			const auto index = log_count.fetch_add( 1, std::memory_order_relaxed );
			if ( index < 32 || ( index & 0xffu ) == 0 )
			{
				const auto first = cmd->csgo_user_cmd.mutable_input_history( current_size );
				const auto last = cmd->csgo_user_cmd.mutable_input_history( current_size + emitted - 1 );
				const auto first_angles = first ? first->view_angles( ) : nullptr;
				const auto last_angles = last ? last->view_angles( ) : nullptr;
				diag::writef(
					diag::level::info,
					"[aa_history] index=%u cmd=%lld before=%d emitted=%d after=%d from=(%.2f,%.2f,%.2f) to=(%.2f,%.2f,%.2f) first=(%.2f,%.2f,%.2f; frac=%.3f) last=(%.2f,%.2f,%.2f; frac=%.3f)",
					index,
					static_cast<long long>( cmd->command_number ),
					current_size,
					emitted,
					cmd->csgo_user_cmd.input_history_size( ),
					from.x,
					from.y,
					from.z,
					to.x,
					to.y,
					to.z,
					first_angles ? first_angles->x( ) : 0.0f,
					first_angles ? first_angles->y( ) : 0.0f,
					first_angles ? first_angles->z( ) : 0.0f,
					first ? first->render_tick_fraction( ) : 0.0f,
					last_angles ? last_angles->x( ) : 0.0f,
					last_angles ? last_angles->y( ) : 0.0f,
					last_angles ? last_angles->z( ) : 0.0f,
					last ? last->render_tick_fraction( ) : 0.0f );
			}
		}
#endif

		return emitted;
	}

	void input::set_view_angles( const math::vector3& angles ) const
	{
		memory::call<void>(PATTERN (patterns::set_view_angles), addresses::globals::csgo_input, 0, &angles );
	}

	void input::desubtick( usercmd* cmd ) const
	{
		cmd->csgo_user_cmd.mutable_base( )->mutable_subtick_moves( )->clear( );
	}

	void input::set_weapon_select( usercmd* cmd, std::uintptr_t csgo_input ) const
	{
		const auto frame_data = csgo_input + 552;
		const auto weapon_handle = memory::read<uint32_t>( frame_data + 1132 );

		if ( !weapon_handle )
		{
			return;
		}

		const auto weapon = systems::g_entities.lookup( weapon_handle );
		if ( !weapon )
		{
			return;
		}

		auto entity_index{ -1 };
		memory::call<int*>(PATTERN (patterns::weapon_get_entity_index), weapon, &entity_index );

		if ( entity_index == -1 )
		{
			return;
		}

		const auto base = cmd->csgo_user_cmd.mutable_base( );
		if ( base )
		{
			base->set_weaponselect( entity_index );
		}
	}

	bool input::is_subtick_overwrite( usercmd* cmd ) const
	{
		return memory::read<int>( reinterpret_cast< std::uintptr_t >( cmd ) + 148 ) == 2;
	}

	bool input::calculate_crc( proto::base_usercmd_pb* base ) const
	{
		if ( !base )
		{
			return false;
		}

		const auto string_copy = PATTERN( patterns::string_copy );
		const auto serialize_move_crc = PATTERN( patterns::serialize_move_crc );
		if ( !string_copy || !serialize_move_crc )
		{
			diag::write( diag::level::error, "input CRC helpers unavailable; capturing a pre-termination diagnostic dump" );
			diag::capture_snapshot( "input CRC helpers unavailable" );
			return false;
		}

		const auto btns = base->buttons_pb( );
		const auto va = base->viewangles( );

		std::uint8_t buf[ 64 ]{};
		std::uint8_t btn_size{ 0 };

		auto p = buf;

		auto bs1 = btns ? btns->buttonstate1( ) : 0;
		auto bs2 = btns ? btns->buttonstate2( ) : 0;
		auto bs3 = btns ? btns->buttonstate3( ) : 0;

		if ( bs1 ) { btn_size += 9; }
		if ( bs2 ) { btn_size += 9; }
		if ( bs3 ) { btn_size += 9; }

		if ( btn_size > 0 )
		{
			*p++ = 0x1a;
			*p++ = btn_size;

			if ( bs1 ) { *p++ = 0x09; std::memcpy( p, &bs1, 8 ); p += 8; }
			if ( bs2 ) { *p++ = 0x11; std::memcpy( p, &bs2, 8 ); p += 8; }
			if ( bs3 ) { *p++ = 0x19; std::memcpy( p, &bs3, 8 ); p += 8; }
		}

		auto pitch = va ? va->x( ) : 0.0f;
		auto yaw = va ? va->y( ) : 0.0f;
		auto roll = va ? va->z( ) : 0.0f;

		std::uint8_t va_size{ 0 };
		if ( pitch != 0.0f ) { va_size += 5; }
		if ( yaw != 0.0f ) { va_size += 5; }
		if ( roll != 0.0f ) { va_size += 5; }

		if ( va_size > 0 )
		{
			*p++ = 0x22;
			*p++ = va_size;

			if ( pitch != 0.0f ) { *p++ = 0x0d; std::memcpy( p, &pitch, 4 ); p += 4; }
			if ( yaw != 0.0f ) { *p++ = 0x15; std::memcpy( p, &yaw, 4 ); p += 4; }
			if ( roll != 0.0f ) { *p++ = 0x1d; std::memcpy( p, &roll, 4 ); p += 4; }
		}

		auto total = static_cast< int >( p - buf );

		base->m_has_bits.set( 0x1u );

		auto raw_msg = reinterpret_cast< std::uintptr_t >( base ) - proto::message_impl_offset;
		auto arena_raw = memory::read<std::uintptr_t>( raw_msg + 0x08 );
		auto arena = arena_raw & ~0x3ull;

		if ( arena_raw & 1 )
		{
			arena = memory::read<std::uintptr_t>( arena );
		}

		std::uint8_t msg[ 0x18 ]{};
		memory::call<void>( string_copy, reinterpret_cast< std::uintptr_t >( msg ), reinterpret_cast< std::uintptr_t >( buf ), total );
		memory::call<void>( serialize_move_crc, &base->m_move_crc, reinterpret_cast< std::uintptr_t >( msg ), arena );

		return true;
	}

} // namespace systems
