#pragma once

#include <core/systems/systems.hpp>

namespace features::combat {

	class shared
	{
	public:
		class lagcomp
		{
		public:
			// 15 seconds at the fixed 64 tick simulation rate. Keep storage bounded
			// independently from rage's small per-candidate scan array.
			static constexpr auto k_max_history_ticks{ 960 };
			static constexpr auto k_max_records_per_player{ 1024u };

			struct record
			{
				std::uintptr_t pawn{};
				std::uintptr_t game_scene_node{};
				std::uintptr_t bone_cache{};
				int bone_count{};

				systems::bones::data bones[ 128 ]{};
				systems::bones::data bones_backup[ 128 ]{};

				math::vector3 origin{};
				math::vector3 rotation{};
				math::vector3 velocity{};
				std::uint32_t flags{};

				float simulation_time{};
				int tick{};
				// Number of ticks simulated forward from the newest received pose.
				// Network records remain zero so minimum extrapolation can reject them.
				int extrapolated_ticks{};
				// [0, 1] estimate of how consistent the source motion samples were.
				float extrapolation_stability{};
				bool valid{};
				bool was_valid{};
				bool is_applied{};
				bool extrapolated{};
				bool live_snapshot{};

				bool setup( std::uintptr_t pawn );
				[[nodiscard]] bool is_valid( ) const;

				void apply( );
				void restore( );
			};

			struct visual_record
			{
				math::vector3 origin{};
				std::array<systems::bones::data, 128> bones{};
				int bone_count{};
			};

			struct extrapolation_data
			{
				math::vector3 origin{};
				math::vector3 velocity{};
				math::vector3 acceleration{};
				math::vector3 obb_mins{};
				math::vector3 obb_maxs{};
				std::uint32_t flags{};
				float sim_time{};
				float direction{};
				// Bounded horizontal turn rate inferred from the last two movement
				// intervals. Used only for airborne continuation.
				float turn_rate{};
				float gravity_scale{ 1.0f };
			};

			void run( );

			[[nodiscard]] record* get_oldest_valid( std::uintptr_t pawn );
			[[nodiscard]] record* get_oldest_was_valid( std::uintptr_t pawn );
			[[nodiscard]] std::optional<visual_record> get_oldest_valid_visual( std::uintptr_t pawn ) const;
			[[nodiscard]] std::vector<record*> get_valid_records( std::uintptr_t pawn );
			[[nodiscard]] std::array<systems::bones::data, 27> get_skeleton( const record& record ) const;

			[[nodiscard]] std::optional<record> extrapolate( std::uintptr_t pawn );

		private:
			void predict_movement( extrapolation_data& data, std::uintptr_t skip_entity ) const;

			std::unordered_map<std::uintptr_t, std::deque<record>> m_records{};
			mutable std::shared_mutex m_records_mtx{};
		};

		class penetration
		{
		public:
			struct weapon_data
			{
				float damage;
				float penetration;
				float range_modifier;
				float range;
				float armor_ratio;
				float headshot_multiplier;
			};

			struct damage_scales
			{
				float ct_head;
				float t_head;
				float ct_body;
				float t_body;
			};

			struct run_context
			{
				std::uintptr_t target_pawn{};
				int target_armor{};
				int target_team{};
				bool has_helmet{};
				damage_scales scales{};
				float armor_ratio{};
				float headshot_multiplier{};
				systems::hitboxes::set hitboxes{};
				lagcomp::record* record{};
			};

			struct result
			{
				float damage{};
				int hitbox{ -1 };
				int hitgroup{ -1 };
				bool penetrated{};
			};

			void prepare( std::uintptr_t weapon_vdata, std::uintptr_t weapon );

			[[nodiscard]] run_context prepare_target( std::uintptr_t target_pawn, lagcomp::record* record, const systems::hitboxes::set* external_hitboxes = nullptr ) const;
			[[nodiscard]] int first_hitbox( const math::vector3& start, const math::vector3& end, const run_context& ctx ) const;
			[[nodiscard]] bool run( const math::vector3& start, const math::vector3& end, const run_context& ctx, std::uintptr_t local_pawn, int local_team, result& out, int fallback_hitbox = -1 ) const;
			[[nodiscard]] bool can( const math::vector3& start, const math::vector3& direction, float& out_damage, const systems::local::snapshot& local ) const;
			[[nodiscard]] float get_max_damage( int hitgroup, int target_armor, bool has_helmet, int target_team ) const;
			[[nodiscard]] const weapon_data& get_weapon_data( ) const { return this->m_weapon_data; }

		private:
			void scale_damage( int hitgroup, int armor, bool has_helmet, int team, float armor_ratio, float headshot_multiplier, const damage_scales& scales, float& damage ) const;
			weapon_data m_weapon_data{};
		};

		class shoot_history
		{
		public:
			struct ring_entry
			{
				int tick{};
				float fraction{};
				math::vector3 position{};
			};

			struct eye_candidate
			{
				math::vector3 position{};
				int player_tick{};
				float player_frac{};
				int lerp_ticks_int{};
				float lerp_ticks_frac{};
				bool is_uninterpolated{};
			};

			struct eye_candidates
			{
				eye_candidate entries[ 2 ]{};
				int count{};
			};

			void snapshot( std::uintptr_t local_pawn, std::uintptr_t weapon_services );
			[[nodiscard]] eye_candidates get_candidates( ) const;
			[[nodiscard]] std::optional<math::vector3> get_position_at( int tick, float fraction = 0.0f ) const;
			[[nodiscard]] bool has_data( ) const { return this->m_count > 0; }
			[[nodiscard]] int client_tick( ) const { return this->m_client_tick; }
			[[nodiscard]] float client_tick_frac( ) const { return this->m_client_tick_frac; }
			[[nodiscard]] int server_tick( ) const { return this->m_server_tick; }
			[[nodiscard]] int lerp_ticks_int( ) const { return this->m_lerp_ticks_int; }
			[[nodiscard]] float lerp_ticks_frac( ) const { return this->m_lerp_ticks_frac; }
			[[nodiscard]] int count( ) const { return this->m_count; }
			[[nodiscard]] int oldest_tick( ) const { return this->m_count > 0 ? this->m_entries[ 0 ].tick : -1; }
			[[nodiscard]] int newest_tick( ) const { return this->m_count > 0 ? this->m_entries[ this->m_count - 1 ].tick : -1; }

		private:
			ring_entry m_entries[ 32 ]{};
			// Pawn used to validate the short tail projection.  Keeping this with the
			// ring avoids borrowing a stale entity from a worker thread.
			std::uintptr_t m_local_pawn{};
			int m_count{};
			int m_client_tick{};
			float m_client_tick_frac{};
			int m_server_tick{};
			int m_lerp_ticks_int{};
			float m_lerp_ticks_frac{};
		};

		struct context
		{
			std::uintptr_t weapon{};
			std::uintptr_t weapon_services{};
			std::uintptr_t weapon_vdata{};
			std::uint32_t weapon_type{};
			std::uint16_t item_def_idx{};
			int weapon_mode{};
			int num_bullets{};
			float recoil_index{};
			int current_tick{};
			int ticks_since_land{};
			float current_time{};
			float tick_interval{};
			int tickrate{};
			float weapon_max_speed{};
			float range{};
			bool is_jump_scouting{};
			bool is_scoped{};
			bool tickrate_normal{};
			bool valid{};

			float inaccuracy{};
			float spread{};
		};

		struct accuracy_sample
		{
			float inaccuracy{};
			float spread{};
		};

		void update( );
		void invalidate_if_needed( );

		[[nodiscard]] context& ctx( ) { return this->m_ctx; }
		[[nodiscard]] bool tickrate_normal( ) const noexcept { return this->m_ctx.tickrate_normal; }
		[[nodiscard]] penetration& pen( ) { return this->m_pen; }
		[[nodiscard]] lagcomp& lc( ) { return this->m_lc; }
		[[nodiscard]] shoot_history& sh( ) { return this->m_sh; }
		[[nodiscard]] float latency_budget( ) const { return this->m_latency_budget; }

		[[nodiscard]] bool autowalling( ) const { return this->m_autowalling; }
		[[nodiscard]] lagcomp::record* current_autowall_record( ) const { return this->m_current_autowall_record; }

		[[nodiscard]] int& last_shoot_tick( ) { return this->m_last_shoot_tick; }
		void mark_shot( const systems::input::usercmd* cmd, int tick )
		{
			this->m_last_shoot_tick = tick;
			this->m_last_shoot_command = cmd ? cmd->command_number : -1;
			this->m_last_shoot_weapon = this->m_ctx.weapon;
		}

		[[nodiscard]] std::uint32_t get_spread_seed( const math::vector3& angles, int tick ) const;
		[[nodiscard]] math::vector2 calculate_spread( int seed, float accuracy, float spread, float recoil_index, int item_def_idx, int num_bullets ) const;
		[[nodiscard]] math::vector3 get_aim_punch( std::uintptr_t local_pawn ) const;
		[[nodiscard]] float calculate_hitchance( const math::vector3& shoot_position, const math::vector3& aim_angle, const systems::hitboxes::entry& hitbox, const systems::bones::data& bone, float inaccuracy, float spread, int samples = 256, float minimum = 0.0f ) const;
		[[nodiscard]] math::vector3 find_spread_correction( const math::vector3& aim_angle, int tick, int quality = 1 ) const;
		[[nodiscard]] math::vector3 get_eye_position( std::uintptr_t local_pawn ) const;
		[[nodiscard]] math::vector3 get_shoot_position( ) const;
		[[nodiscard]] math::vector3 get_command_shoot_position( std::uintptr_t local_pawn ) const;
		[[nodiscard]] math::vector3 get_interpolated_shoot_position( std::uintptr_t local_pawn, bool newest = false ) const;
		[[nodiscard]] int calculate_stop_ticks( const math::vector3& velocity, float max_speed, std::uintptr_t local_pawn ) const;
		[[nodiscard]] accuracy_sample get_accuracy_sample( bool update_accuracy_penalty ) const;
		[[nodiscard]] float get_spread( ) const;
		[[nodiscard]] float get_inaccuracy( bool update_accuracy_penalty ) const;
		[[nodiscard]] float get_inaccuracy_at_velocity( std::uintptr_t local_pawn, const math::vector3& velocity ) const;
		[[nodiscard]] float get_air_inaccuracy( float vertical_speed, float jump_initial, float jump_apex ) const;
		[[nodiscard]] bool can_shoot( systems::input::usercmd* cmd, std::uintptr_t local_controller, bool check_next_attack = true ) const;
		[[nodiscard]] bool is_max_accuracy( float inaccuracy ) const;
		[[nodiscard]] math::vector3 simulate_aim_punch( int recoil_index ) const;

		bool ray_vs_capsule( const math::vector3& ray_origin, const math::vector3& ray_dir, const math::vector3& capsule_a, const math::vector3& capsule_b, float radius, float& out_fraction ) const;

	private:
		context m_ctx{};
		penetration m_pen{};
		lagcomp m_lc{};
		shoot_history m_sh{};

		// Parallel rage workers must not overwrite each other's trace record.
		inline static thread_local bool m_autowalling{};
		inline static thread_local lagcomp::record* m_current_autowall_record{ nullptr };

		int m_last_shoot_tick{};
		std::intptr_t m_last_shoot_command{ -1 };
		std::uintptr_t m_last_shoot_weapon{};
		float m_latency_budget{ -1.0f };
	};

	class misc
	{
	private:
		class antiaim
		{
		public:
			void on_create_move( systems::input::usercmd* cmd );
			void finalize_movement( systems::input::usercmd* cmd, bool movement_is_command_relative );
			void apply_vac_bypass( systems::input::usercmd* cmd );
			void on_render( xdraw::draw_list& draw_list ) const;
			void set_desync_choke( bool choke ) { this->m_desync_choke = choke; }

			[[nodiscard]] bool has_modified_angles( ) const { return this->m_should_correct || this->m_modified_angles.y != this->m_old_angles.y; }
			[[nodiscard]] const math::vector3& get_modified_angles( ) const { return this->m_modified_angles; }
			[[nodiscard]] const math::vector3& get_visual_angles( ) const { return this->m_visual_angles; }
			[[nodiscard]] bool has_visual_angles( ) const { return this->m_visual_angles_valid; }
			[[nodiscard]] bool get_render_yaws( float& command_yaw, float& visual_yaw ) const
			{
				if ( !this->m_render_yaws_valid.load( std::memory_order_acquire ) )
				{
					return false;
				}

				command_yaw = this->m_render_command_yaw.load( std::memory_order_relaxed );
				visual_yaw = this->m_render_visual_yaw.load( std::memory_order_relaxed );
				return true;
			}
			[[nodiscard]] bool has_final_ground_movement( ) const { return this->m_final_ground_movement; }
			[[nodiscard]] bool is_active( ) const { return this->m_antiaim_active; }
			[[nodiscard]] bool is_desync_choking( ) const { return this->m_desync_choke; }

		private:
			[[nodiscard]] float get_pitch( float view_pitch );
			[[nodiscard]] float get_yaw( const math::vector3& view_angles, const systems::local::snapshot& local );
			void correct_movement( systems::input::usercmd* cmd );
			[[nodiscard]] bool is_near_ladder( std::uintptr_t local_pawn ) const;

			math::vector3 m_old_angles{};
			math::vector3 m_modified_angles{};
			math::vector3 m_visual_angles{};
			math::vector3 m_vac_reference_angles{};
			float m_raw_forward_move{};
			float m_raw_side_move{};

			int m_yaw_side{};
			bool m_should_correct{};
			bool m_antiaim_active{};
			bool m_final_ground_movement{};
			bool m_desync_choke{};
			bool m_visual_angles_valid{};
			bool m_vac_was_enabled{};
			std::atomic<float> m_render_command_yaw{};
			std::atomic<float> m_render_visual_yaw{};
			std::atomic_bool m_render_yaws_valid{};
			float m_indicator_yaw{};
		};

		class duckpeek
		{
		public:
			void on_create_move( systems::input::usercmd* cmd );
			void on_override_view( std::uintptr_t view_setup );

		private:
			bool m_was_active{};
			bool m_fake_stand_active{};
		};

		class quickpeek
		{
		public:
			void on_create_move( systems::input::usercmd* cmd );
			void reset_if_needed( );
			[[nodiscard]] bool is_retracking( ) const { return this->m_should_retrack; }
			[[nodiscard]] bool is_active( ) const { return this->m_active; }

		private:
			static constexpr std::uint32_t invalid_effect_index{ static_cast<std::uint32_t>( -1 ) };

			void create_particle( );
			void update_particle( );
			void release_particle( );
			void reset( );

			math::vector3 m_saved_origin{};
			bool m_should_retrack{};
			bool m_fired{};
			bool m_active{};
			std::uint32_t m_particle_effect{ invalid_effect_index };
			bool m_particle_loaded{};
			std::uintptr_t m_prev_movement_bits{};
		};

		class autostop
		{
		public:
			void on_create_move( systems::input::usercmd* cmd, bool force = false );
			// Shared by rage's early-stop predictor so its tick simulation matches
			// the movement command that will actually be submitted.
			[[nodiscard]] float get_effective_accel_base( std::uintptr_t local_pawn, std::uintptr_t movement_services, std::uint32_t flags, float max_weapon_speed ) const;

		private:
		};

		antiaim m_antiaim{};
		duckpeek m_duckpeek{};
		quickpeek m_quickpeek{};
		autostop m_autostop{};

	public:
		[[nodiscard]] antiaim& antiaim( ) { return this->m_antiaim; }
		[[nodiscard]] duckpeek& duckpeek( ) { return this->m_duckpeek; }
		[[nodiscard]] quickpeek& quickpeek( ) { return this->m_quickpeek; }
		[[nodiscard]] autostop& autostop( ) { return this->m_autostop; }
	};

	class rage
	{
	public:
		enum class doubletap_state : std::uint8_t { uncharged, charging, ready };
		struct target_state_snapshot
		{
			math::vector3 position{};
			int entity_index{ -1 };
			int hitbox_index{ -1 };
			int hitgroup{ -1 };
			float damage{};
			float hitchance{};
			float fov{};
			bool valid{};
		};

		void on_create_move( systems::input::usercmd* cmd );
		void on_render( xdraw::draw_list& draw_list );

		[[nodiscard]] bool should_stop( ) const noexcept { return this->m_should_stop; }
		[[nodiscard]] bool air_stop_requested( ) const noexcept { return this->m_air_stop_requested; }
		[[nodiscard]] bool air_stop_enabled( ) const noexcept;
		[[nodiscard]] bool has_target( ) const noexcept { return this->m_state_target.valid; }
		[[nodiscard]] int target_entity_index( ) const noexcept { return this->m_state_target.entity_index; }
		[[nodiscard]] int target_hitbox( ) const noexcept { return this->m_state_target.hitbox_index; }
		[[nodiscard]] int target_hitgroup( ) const noexcept { return this->m_state_target.hitgroup; }
		[[nodiscard]] float target_damage( ) const noexcept { return this->m_state_target.damage; }
		[[nodiscard]] float target_hitchance( ) const noexcept { return this->m_state_target.hitchance; }
		[[nodiscard]] float target_fov( ) const noexcept { return this->m_state_target.fov; }
		[[nodiscard]] const math::vector3& target_position( ) const noexcept { return this->m_state_target.position; }
		[[nodiscard]] bool movement_stop_requested( ) const noexcept
		{
			return this->m_should_stop || this->m_air_stop_requested;
		}
		[[nodiscard]] bool is_firing_this_tick( ) const noexcept { return this->m_firing_this_tick; }
		[[nodiscard]] bool previous_stop_requested( ) const noexcept { return this->m_should_stop; }
		void finalize_shot_command( systems::input::usercmd* cmd );
		[[nodiscard]] bool is_cocking_revolver( ) const noexcept { return this->m_revolver_cock_ticks > 0; }

		// DT timing follows dt.txt: calculate the weapon-ready tick, then serialize
		// the two attack history slots on the accepted command.
		class double_tap
		{
		public:
			enum class charge_state : std::uint8_t { charging, ready, failed };

			void on_create_move( systems::input::usercmd* cmd, bool enabled );
			[[nodiscard]] bool on_fired( systems::input::usercmd* cmd, bool fired, bool enabled, bool no_spread );
			void on_render( xdraw::draw_list& draw_list, bool enabled ) const;
			void update_aim( const math::vector3& angles, int tick ) noexcept { this->m_aim = angles; this->m_aim_tick = tick; this->m_has_aim = true; }
			void set_fire_angle( const math::vector3& angles ) noexcept { this->m_fire_angle = angles; this->m_has_fire_angle = true; }
			// The DT history pair must use the same predicted origin that was accepted
			// by fire_gun(). Prediction restores the pawn before on_fired() runs, so
			// querying the weapon service there can return the pre-prediction eye.
			void set_fire_position( const math::vector3& position ) noexcept { this->m_fire_position = position; this->m_has_fire_position = true; }
			[[nodiscard]] bool has_fire_position( ) const noexcept { return this->m_has_fire_position; }
			[[nodiscard]] const math::vector3& fire_position( ) const noexcept { return this->m_fire_position; }
			[[nodiscard]] charge_state state( ) const noexcept { return this->m_state; }
			[[nodiscard]] float progress( ) const noexcept { return this->m_progress; }
			[[nodiscard]] int claimed_tick( ) const noexcept { return this->m_claimed_tick; }
			[[nodiscard]] int next_attack_tick( ) const noexcept { return this->m_next_attack; }
			[[nodiscard]] int shoot_tick( ) const noexcept { return this->m_shoot_tick; }
			[[nodiscard]] float next_attack_fraction( ) const noexcept { return this->m_ratio; }
			[[nodiscard]] int last_fire_tick( ) const noexcept { return this->m_last_fire_tick; }
			[[nodiscard]] int cycle_ticks( ) const noexcept { return this->m_cycle_ticks; }
			[[nodiscard]] bool is_second_attack( int tick_base ) const noexcept
			{
				return this->m_fired && !this->m_second_fired && tick_base - this->m_last_fire_tick == 1;
			}
			[[nodiscard]] int no_spread_claim_tick( ) const noexcept { return this->m_no_spread_claim_tick; }
			void set_no_spread_claim_tick( int tick ) noexcept { this->m_no_spread_claim_tick = tick; }
			[[nodiscard]] bool pair_armed( ) const noexcept { return this->m_pair_armed; }
			[[nodiscard]] bool history_pair( ) const noexcept { return this->m_history_pair; }
			void clear_history_pair( ) noexcept { this->m_history_pair = false; }
			void set_pair_armed( bool value ) noexcept { this->m_pair_armed = value; }
			void refresh_followup_timing( int next_attack, float ratio, float wat_offset, int tick_base ) noexcept;
			void reset( ) noexcept;

		private:
			charge_state m_state{ charge_state::charging };
			std::uintptr_t m_weapon{};
			int m_next_attack{};
			int m_shoot_tick{};
			int m_charge_weapon_ready_tick{ -1 };
			int m_charge_start_tick{ -1 };
			int m_claimed_tick{};
			int m_last_claimed_tick{ -1 };
			int m_last_fire_tick{ -1 };
			int m_cycle_ticks{ 4 };
			float m_ratio{};
			float m_wat_offset{};
			int m_prev_clip{ -1 };
			int m_shot_count{};
			int m_no_spread_claim_tick{};
			int m_aim_tick{ -1 };
			math::vector3 m_aim{};
			math::vector3 m_fire_position{};
			math::vector3 m_fire_angle{};
			bool m_has_aim{};
			bool m_has_fire_position{};
			bool m_has_fire_angle{};
			bool m_fired{};
			bool m_second_fired{};
			bool m_pair_armed{};
			bool m_history_pair{};
			float m_progress{};
		};

		[[nodiscard]] double_tap& dt( ) noexcept { return this->m_double_tap; }
		[[nodiscard]] const double_tap& dt( ) const noexcept { return this->m_double_tap; }
		[[nodiscard]] bool should_release_duck_for_shot( ) const noexcept { return this->m_release_duck_for_shot; }
		[[nodiscard]] bool duckpeek_wants_reduck( ) const noexcept { return this->m_duckpeek_reduck; }
		void clear_duckpeek_reduck( ) noexcept { this->m_duckpeek_reduck = false; }
		[[nodiscard]] bool has_doubletap_command_pending( ) const noexcept { return this->m_doubletap_command_pending; }
		[[nodiscard]] std::intptr_t doubletap_primary_command( ) const noexcept { return this->m_doubletap_primary_command; }
		[[nodiscard]] doubletap_state get_doubletap_state( ) const noexcept { return this->m_doubletap_state.load( std::memory_order_relaxed ); }
		[[nodiscard]] int doubletap_release_client_tick( ) const noexcept { return this->m_doubletap_release_client_tick; }
		[[nodiscard]] int doubletap_required_ticks( ) const noexcept { return this->m_doubletap_required_ticks; }
		[[nodiscard]] std::intptr_t doubletap_charge_command( ) const noexcept { return this->m_doubletap_charge_command; }
		void cancel_doubletap_charge( ) noexcept;
		bool write_doubletap_command( systems::input::usercmd* cmd, const systems::local::snapshot& local );
		void cancel_doubletap_command( ) noexcept { this->m_doubletap_command_pending = false; this->m_doubletap_primary_command = -1; }

		static constexpr auto k_max_lagcomp_records{ 16 };
		// Keep record scans adjacent to the latest networked pose.  The oldest
		// valid pose is still inside the server unlag window, but is unreliable
		// against moving players by the time an attack command is consumed.
		// Keep one predicted pose, the newest network pose, and a short historical
		// fallback.  The rage scanner still bounds the active pass, while selection
		// can recover when a remote server invalidates one sample mid-command.
		static constexpr auto k_max_scan_records{ 3 };

	private:
		struct aim_context
		{
			math::vector3 view_angles{};
			math::vector3 velocity{};

			float predicted_inaccuracy{};
			float spread{};

			float weapon_max_speed{};
			float accurate_threshold{};
			bool on_ground{};
			bool is_scoped{};
			bool local_prediction_applied{};
			math::vector3 predicted_eye{};
		};

		struct stop_prediction
		{
			math::vector3 eye{};
			float inaccuracy{};
			int ticks_to_stop{};
			float time_to_stop{};
		};

		struct candidate
		{
			std::uintptr_t pawn{};
			int entity_index{ -1 };
			int health{};
			int armor{};
			float min_damage{};
			std::array<shared::lagcomp::record*, k_max_lagcomp_records> records{};
			int record_count{};
		};

		struct scan_hit
		{
			math::vector3 position{};
			math::vector3 aim_angle{};
			float damage{};
			float score{};
			float fov{};
			int hitbox_index{};
			int hitgroup{};
			int bone_index{};
			systems::hitboxes::entry hitbox{};
			bool is_center{};
			bool penetrated{};
			bool is_backstab{};
			int attack_type{};
			shared::shoot_history::eye_candidate source_eye{};

			std::uintptr_t pawn{};
			int entity_index{ -1 };
			int health{};
			shared::lagcomp::record* record{};
		};

		struct target
		{
			scan_hit hit{};
			float hitchance{};
			float score{};
			bool valid{};

			[[nodiscard]] bool is_lethal( ) const noexcept
			{
				return this->hit.damage >= static_cast< float >( this->hit.health );
			}
		};

		void update_state_target( const target& value ) noexcept
		{
			if ( !value.valid )
			{
				this->m_state_target = {};
				return;
			}
			this->m_state_target = {
				.position = value.hit.position,
				.entity_index = value.hit.entity_index,
				.hitbox_index = value.hit.hitbox_index,
				.hitgroup = value.hit.hitgroup,
				.damage = value.hit.damage,
				.hitchance = value.hitchance,
				.fov = value.hit.fov,
				.valid = value.valid
			};
		}

		struct knife_info
		{
			bool can_slash{};
			bool can_stab{};
			bool charged{};
			float armor_ratio{};
		};

		[[nodiscard]] aim_context build_context( systems::input::usercmd* cmd, const systems::local::snapshot& local ) const;
		[[nodiscard]] std::optional<stop_prediction> predict_stop( const aim_context& ctx, const math::vector3& current_eye, const systems::local::snapshot& local ) const;
		[[nodiscard]] std::vector<candidate> gather_candidates( const systems::local::snapshot& local, float max_distance_sq = 0.0f ) const;

		void run_gun( systems::input::usercmd* cmd, const aim_context& ctx, const systems::local::snapshot& local, bool allow_fire = true );
		void run_taser( systems::input::usercmd* cmd, const aim_context& ctx, const systems::local::snapshot& local );
		void run_knife( systems::input::usercmd* cmd, const aim_context& ctx, const systems::local::snapshot& local );
		void auto_revolver( systems::input::usercmd* cmd, const aim_context& ctx, const systems::local::snapshot& local );

		[[nodiscard]] std::vector<scan_hit> scan_players( const math::vector3& eye, float inaccuracy, const aim_context& ctx, std::vector<candidate>& candidates, const systems::local::snapshot& local, int first_record, int record_limit ) const;
		[[nodiscard]] std::vector<scan_hit> scan_player( const math::vector3& eye, float inaccuracy, const aim_context& ctx, candidate& cand, shared::lagcomp::record* record, const systems::local::snapshot& local ) const;
		[[nodiscard]] target select_best( const aim_context& aim_ctx, const std::vector<scan_hit>& hits, float eval_inaccuracy ) const;
		[[nodiscard]] target apply_target_switch_delay( const target& selected, const aim_context& aim_ctx, const std::vector<scan_hit>& hits, float eval_inaccuracy );
		[[nodiscard]] float evaluate_hitchance( const scan_hit& hit, const aim_context& ctx, float inaccuracy ) const;
		[[nodiscard]] float get_standing_inaccuracy( const systems::local::snapshot& local, const aim_context& ctx ) const;
		[[nodiscard]] bool prepare_air_stop_for_shot( const aim_context& ctx ) noexcept;

		[[nodiscard]] std::vector<scan_hit> scan_taser( const math::vector3& eye, const aim_context& ctx, std::vector<candidate>& candidates, const systems::local::snapshot& local ) const;

		[[nodiscard]] knife_info get_knife_info( const systems::local::snapshot& local ) const;
		[[nodiscard]] std::vector<scan_hit> scan_knife( const math::vector3& eye, const aim_context& ctx, const knife_info& info, std::vector<candidate>& candidates, const systems::local::snapshot& local ) const;

		void fire_gun( systems::input::usercmd* cmd, const target& tgt, bool was_forced, const math::vector3& shoot_eye, const math::vector3& command_aim_punch, const systems::local::snapshot& local, bool subtick_attack );
		void fire_melee( systems::input::usercmd* cmd, const target& tgt, const systems::local::snapshot& local );

		[[nodiscard]] std::vector<math::vector3> generate_multipoints( const systems::hitboxes::entry& hitbox, const math::vector3& center, const math::quaternion& bone_rot, float pointscale, const math::vector3& shoot_pos, float inaccuracy, const math::vector3& predicted_velocity = {}, int extrapolated_ticks = 0 ) const;
		[[nodiscard]] bool should_stop_movement( const aim_context& ctx ) const;
		[[nodiscard]] float get_min_damage( const settings::combat::ragebot::weapon_group& config, int target_health, bool override_active ) const;
		[[nodiscard]] float get_knife_damage( float raw, int armor, float armor_ratio ) const;
		[[nodiscard]] systems::tracing::result trace_taser_hit( const math::vector3& origin, const math::vector3& forward, float range, std::uintptr_t target_pawn, std::uintptr_t local_pawn ) const;
		[[nodiscard]] systems::tracing::result trace_knife_hit( const math::vector3& origin, const math::vector3& forward, float reach, std::uintptr_t target_pawn, std::uintptr_t local_pawn ) const;

		enum class penetration_crosshair_state : std::uint8_t
		{
			unavailable,
			blocked,
			penetrable
		};

		void update_penetration_crosshair( const systems::local::snapshot& local );
		void draw_penetration_crosshair( xdraw::draw_list& draw_list ) const;
		void update_hittable_regions( const std::vector<scan_hit>& hits );
		void draw_hittable_regions( xdraw::draw_list& draw_list ) const;
		void draw_extrapolated_skeletons( xdraw::draw_list& draw_list ) const;

		enum class doubletap_action : std::uint8_t
		{
			none,
			charging,
			command_pair
		};

		[[nodiscard]] doubletap_action process_doubletap( systems::input::usercmd* cmd, const systems::local::snapshot& local, bool charge_dt );

		bool m_should_stop{};
		bool m_air_stop_requested{};
		bool m_firing_this_tick{};
		int m_deferred_fire_command{ -1 };
		math::vector3 m_deferred_fire_angle{};
		bool m_has_deferred_fire_angle{};
		target_state_snapshot m_state_target{};
		std::uintptr_t m_target_switch_pawn{};
		std::uintptr_t m_pending_target_switch_pawn{};
		std::uintptr_t m_target_switch_weapon{};
		int m_pending_target_switch_tick{ -1 };
		std::uintptr_t m_nospread_seed_target{};
		int m_nospread_seed_target_tick{ -1 };
		std::intptr_t m_protected_shot_command{ -1 };
		int m_protected_shot_history_index{ -1 };
		std::uint32_t m_protected_consumed_server_angle_changes{};
		math::vector3 m_protected_shot_angle{};
		math::vector3 m_protected_command_angle{};
		std::uint8_t m_air_stop_settle_ticks{};
		bool m_doubletap_release_pending{};
		double_tap m_double_tap{};
		std::uintptr_t m_doubletap_weapon{};
		bool m_doubletap_command_pending{};
		bool m_doubletap_followup_fire{};
		std::intptr_t m_doubletap_primary_command{ -1 };
		int m_doubletap_stored_client_tick{ -1 };
		int m_doubletap_release_client_tick{ -1 };
		int m_doubletap_required_ticks{};
		std::intptr_t m_doubletap_charge_command{ -1 };
		std::atomic<doubletap_state> m_doubletap_state{ doubletap_state::uncharged };
		target m_doubletap_target{};
		shared::lagcomp::record m_doubletap_record{};
		math::vector3 m_doubletap_shoot_eye{};
		bool m_doubletap_was_forced{};
		bool m_doubletap_pair_target_valid{};
			int m_doubletap_pair_target_tick{ -1 };
			int m_doubletap_sent_tick{ -1 };
		bool m_release_duck_for_shot{};
		bool m_duckpeek_reduck{};

		std::uint8_t m_knife_attack{};
		bool m_zeus_fired{};

		int m_revolver_cock_ticks{};
		std::atomic<penetration_crosshair_state> m_penetration_crosshair_state{ penetration_crosshair_state::unavailable };

		std::vector<shared::lagcomp::record> m_extrapolated_records{};
		mutable std::mutex m_extrapolated_mtx{};
		// Fire/scan-time native snapshots. These keep debug points and the primary
		// candidate on the current evaluated pose even when the lagcomp deque has
		// not received a new simulation-time sample yet.
		std::vector<shared::lagcomp::record> m_live_records{};
		mutable std::mutex m_live_records_mtx{};

		struct debug_point
		{
			math::vector3 position{};
			int hitbox_index{};
			bool is_center{};
		};

		mutable std::vector<debug_point> m_debug_points{};
		mutable std::mutex m_debug_mtx{};

		struct hittable_region
		{
			std::uintptr_t pawn{};
			int hitbox_index{};
			systems::hitboxes::entry hitbox{};
			systems::bones::data bone{};
			bool penetrated{};
			float expires_at{};
		};

		mutable std::vector<hittable_region> m_hittable_regions{};
		mutable std::mutex m_hittable_regions_mtx{};

	};

	class legit
	{
	public:
		void on_create_move( systems::input::usercmd* cmd );
		void on_render( xdraw::draw_list& draw_list );
		void invalidate_if_needed( );

		[[nodiscard]] bool has_target( ) const noexcept { return this->m_target.has_target( ); }
		[[nodiscard]] std::uintptr_t target_pawn( ) const noexcept { return this->m_target.pawn; }
		[[nodiscard]] int target_hitbox( ) const noexcept { return this->has_target( ) ? this->m_target.best_point.hitbox.index : -1; }
		[[nodiscard]] int target_hitgroup( ) const noexcept { return this->has_target( ) ? this->m_target.best_point.hitgroup : -1; }
		[[nodiscard]] float target_damage( ) const noexcept { return this->m_target.best_point.damage; }
		[[nodiscard]] float target_hitchance( ) const noexcept { return this->m_target.hitchance; }
		[[nodiscard]] float target_fov( ) const noexcept { return this->m_target.best_point.fov; }
		[[nodiscard]] const math::vector3& target_position( ) const noexcept { return this->m_target.best_point.position; }
		[[nodiscard]] bool movement_stop_requested( ) const noexcept { return this->m_trigger_stop_pending; }
		[[nodiscard]] bool is_firing_this_tick( ) const noexcept { return this->m_trigger_fired_this_tick; }

	private:
		struct scan_point
		{
			math::vector3 position{};
			float damage{};
			float fov{};
			int hitgroup{};
			std::size_t cfg_index{};
			int bone_index{};
			systems::hitboxes::entry hitbox{};
			bool visible{};
			bool is_center{};
			bool valid{};
		};

		struct target_result
		{
			std::uintptr_t pawn{};
			scan_point best_point{};
			math::vector3 aim_angle{};
			float hitchance{};
			float score{};
			float fov{};
			int health{};
			shared::lagcomp::record* record{};

			[[nodiscard]] bool has_target( ) const noexcept { return this->best_point.valid; }
		};

		[[nodiscard]] target_result find_target( const math::vector3& shoot_position, const math::vector3& view_angles, const settings::combat::legitbot::weapon_group& config, const systems::local::snapshot& local ) const;
		[[nodiscard]] scan_point scan_player( std::uintptr_t pawn, shared::lagcomp::record* record, const math::vector3& shoot_position, const math::vector3& view_angles, const settings::combat::legitbot::weapon_group& config, const systems::local::snapshot& local ) const;

		void apply_aimbot( systems::input::usercmd* cmd, const target_result& tgt, const math::vector3& view_angles, const math::vector3& aim_punch, const settings::combat::legitbot::weapon_group& config, const systems::local::snapshot& local );
		void apply_triggerbot( systems::input::usercmd* cmd, const math::vector3& shoot_position, const math::vector3& view_angles, const math::vector3& aim_punch, const settings::combat::legitbot::weapon_group& config, const systems::local::snapshot& local );
		void apply_rcs( math::vector3& aim_angle, const math::vector3& aim_punch, int rand_min, int rand_max ) const;

		void update_standalone_rcs( const math::vector3& view_angles, const math::vector3& aim_punch, int amount, int rand_min, int rand_max, bool apply, const systems::local::snapshot& local );
		[[nodiscard]] float compute_rcs_factor( int rand_min, int rand_max ) const;

		void draw_fov( xdraw::draw_list& draw_list, const math::vector3& view_angles, const math::vector3& aim_punch, float fov_degrees, const config::col& color, bool rcs_active ) const;

		[[nodiscard]] static int hitgroup_to_cfg( int hitgroup );

		target_result m_target{};
		math::vector3 m_old_punch{};
		mutable float m_last_significant_punch_time{};

		float m_remainder_x{};
		float m_remainder_y{};

		float m_trigger_delay_start{};
		float m_trigger_release_time{};
		std::uintptr_t m_trigger_pending_pawn{};
		bool m_trigger_stop_pending{};
		bool m_trigger_fired_this_tick{};

		math::vector3 m_cached_view_angles{};
		math::vector3 m_cached_aim_punch{};
	};

	class vac_bypass
	{
	public:
		void on_create_move( systems::input::usercmd* cmd );

	private:
		void suppress_cheat_cvars( ) const;
	};

} // namespace features::combat
