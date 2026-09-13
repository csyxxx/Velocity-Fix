#pragma once

namespace features::movement {

	class bhop
	{
	public:
		void on_create_move( systems::input::usercmd* cmd );
		void store_input( systems::input::usercmd* cmd );

	private:
		bool m_jump_held{};
	};

	class airstrafe
	{
	public:
		void on_create_move( systems::input::usercmd* cmd );
		void store_input( systems::input::usercmd* cmd );

	private:
		void check_button( std::uintptr_t current_buttons, std::uintptr_t button );
		void rotate_movement( proto::base_usercmd_pb* base, float target_yaw, float view_yaw ) const;
		void rotate_to_stop( proto::base_usercmd_pb* base, const math::vector3& velocity, bool full_strength = false ) const;

		std::uintptr_t m_last_buttons{};
		std::uintptr_t m_last_pressed{};
		std::uintptr_t m_raw_buttons{};
		bool m_side_switch{};
		math::vector3 m_angles{};
	};

	class jumpbug
	{
	public:
		void on_create_move( systems::input::usercmd* cmd );
		[[nodiscard]] bool active_this_tick( ) const { return this->m_active_this_tick; }
		[[nodiscard]] float landing_fraction( ) const { return this->m_landing_fraction; }

	private:
		[[nodiscard]] float get_impulse_mul( std::uintptr_t local_pawn ) const;

		float m_landing_fraction{ 1.0f };
		bool m_active_this_tick{ false };
		float m_when_bias{};
		bool m_jump_fired_prev{};
		float m_pre_takeoff_speed_2d{};
		float m_avg_tick_gap{ 1.0f };
		int m_last_tick{};
	};

	class fastladder
	{
	public:
		void on_create_move( systems::input::usercmd* cmd ) const;
	};

	class edgejump
	{
	public:
		void on_create_move( systems::input::usercmd* cmd ) const;
	};

	class edgestop
	{
	public:
		void on_create_move( systems::input::usercmd* cmd ) const;
	};

	class edgebug
	{
	public:
		void on_create_move( systems::input::usercmd* cmd );
		void on_render( xdraw::draw_list& draw_list );

		[[nodiscard]] bool active_this_tick( ) const { return this->m_active_this_tick; }

	private:
		bool m_active_this_tick{ false };
	};

	class slowwalk
	{
	public:
		void on_create_move( systems::input::usercmd* cmd ) const;
	};

	class test_strafer
	{
	public:
		void on_create_move( systems::input::usercmd* cmd );
		void store_input( systems::input::usercmd* cmd );
		[[nodiscard]] bool is_active( ) const;
		[[nodiscard]] bool handled_this_tick( ) const { return this->m_handled_this_tick; }

	private:
		void quantized_path( systems::input::usercmd* cmd );
		[[nodiscard]] bool apply_yaw_subtick( proto::base_usercmd_pb* base, float when, float yaw_delta, bool fixed_path, const math::vector2* movement_delta, bool publish_movement ) const;
		void check_button( std::uintptr_t current_buttons, std::uintptr_t button );
		[[nodiscard]] static math::vector2 movement_from_buttons( std::uintptr_t pressed );

		std::uintptr_t m_last_buttons{};
		std::uintptr_t m_last_pressed{};
		std::uintptr_t m_raw_buttons{};
		int m_substep_counter{};
		math::vector3 m_angles{};
		bool m_handled_this_tick{};
	};

} // namespace features::movement
