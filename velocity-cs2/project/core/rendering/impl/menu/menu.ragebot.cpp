#include <pch/pch.hpp>
#include <core/settings.hpp>

#include "../../rendering.hpp"

namespace rendering {

	namespace detail {

		constexpr const char* hitbox_names[ ]{ "head", "chest", "stomach", "arms", "legs", "paws" };
		constexpr const char* pitch_items[ ]{ "none", "down", "up" };
		constexpr const char* yaw_items[ ]{ "at target", "local view" };
		constexpr const char* yaw_pattern_items[ ]{ "180", "spin", "jitter" };
		constexpr const char* autostop_names[ ]{ "early", "in air", "between shot" };
		constexpr const char* nospread_quality_items[ ]{ "low", "medium", "high" };

	} // namespace detail

	void menu::draw_ragebot( float group_w ) const
	{
		auto& s = settings::g_combat;
		auto& rb = s.m_ragebot;
		auto& aa = s.m_antiaim;
		auto& qp = s.m_quickpeek;
		auto& dp = s.m_duckpeek;
		auto& zb = s.m_zeusbot;
		auto& kb = s.m_knifebot;
		auto& autos = s.m_autos;
		auto& lg = s.m_lagcomp;

		auto& wg = rb.groups[ this->m_subtab ];

		const auto wx = this->m_x;
		const auto wy = this->m_y;
		const auto content_x = this->m_body_x;
		const auto body_y = this->m_body_y;
		const auto content_w = this->m_body_w;
		const auto col_w = ( content_w - tokens::gap ) * 0.5f;
		const auto right_x = content_x + col_w + tokens::gap;

		xui::layout::set_cursor( content_x - wx, body_y - wy );

		if ( xui::begin_child( "##ragebot_aimbot", col_w ) )
		{
			xui::checkbox( "enabled", rb.enabled );
			xui::checkbox( "vac bypass", rb.vac_bypass );
			xui::checkbox( "silent", wg.silent );
			xui::checkbox( "no spread", wg.no_spread );
			if ( xui::begin_popup( "##nospread_popup", 220.0f ) )
			{
				wg.nospread_quality.value = std::clamp( wg.nospread_quality.value, 0, 2 );
				xui::combo( "quality", wg.nospread_quality.value, detail::nospread_quality_items, 3 );
				 xui::checkbox( "forcenospread", wg.forcenospread );
				xui::end_popup( );
			}
			xui::checkbox( "double tap", wg.doubletap );
			xui::checkbox( "force shot in air", wg.force_shot_air );
			xui::checkbox( "force shot on ground", wg.force_shot );
			xui::multicombo( "autostop", wg.autostop, detail::autostop_names, 3 );
			xui::checkbox( "extrapolation", lg.extrapolation );
			xui::checkbox( "extrapolation skeleton", lg.extrapolation_skeleton );
			if ( xui::begin_popup( "##extrapolation_popup", 240.0f ) )
			{
				xui::slider_int( "confidence", lg.extrapolation_confidence, 0, 100, "%d%%" );
				xui::slider_int( "stability", lg.extrapolation_stability, 0, 100, "%d%%" );
				xui::end_popup( );
			}
			xui::checkbox( "auto extrapolation", lg.extrapolation_auto );
			xui::checkbox( "extrapolation correction", lg.extrapolation_correct );
			xui::checkbox( "air extrapolation", lg.extrapolation_air );
			xui::slider_int( "extrapolation ticks", lg.max_extrapolate_ticks, 1, 16, "%d tick(s)" );
			xui::slider_int( "minimum extrapolation", lg.min_extrapolate_ticks, 0, 16, "%d tick(s)" );
			xui::slider_int( "local prediction (16=auto)", lg.local_prediction_ticks, 0, 16, "%d tick(s)" );
			xui::slider_int( "target switch delay", rb.target_switch_delay, 0, 20, "%d tick(s)" );
			xui::checkbox( "backtrack", lg.backtrack );
			xui::slider_int( "max backtrack", lg.max_backtrack_ticks, 1, 960, "%d tick(s)" );
			xui::slider_float( "max fov", wg.max_fov, 1.0f, 180.0f, "%.0f°" );

			xui::slider_int( "hit chance", wg.hitchance, 25, 100, "%d%%" );
			xui::slider_int( "min damage", wg.min_damage, 5, 125, "%d" );
			/*xui::slider_int( "max backtrack", s.m_lagcomp.max_backtrack_ticks, 1, 16, "%d tick(s)" );*/

			xui::checkbox( "hit chance override", wg.hitchance_override );
			if ( xui::begin_popup( "##hitchance_popup", 220.0f ) )
			{
				xui::slider_int( "value##hc", wg.hitchance_override_value, 0, 100, "%d%%" );
				xui::end_popup( );
			}

			xui::checkbox( "min damage override", wg.min_damage_override );
			if ( xui::begin_popup( "##mindamage_popup", 220.0f ) )
			{
				xui::slider_int( "value##md", wg.min_damage_override_value, 0, 130, "%d" );
				xui::end_popup( );
			}

			xui::end_child( );
		}

		if ( xui::begin_child( "##ragebot_extras", col_w, 218.0f, true ) )
		{
			xui::checkbox( "force b-aim", wg.body_aim );
			xui::checkbox( "dynamic point scale", wg.dynamic_pointscale );
			xui::checkbox( "debug multipoints", wg.debug_multipoints );
			xui::checkbox( "hittable hitboxes", wg.hittable_regions );
			if ( xui::begin_popup( "##hittable_hitboxes", 220.0f ) )
			{
				xui::slider_float( "hold time", wg.hittable_region_duration, 0.05f, 2.0f, "%.2fs" );
				xui::color_picker( "direct color", wg.hittable_region_direct_color );
				xui::color_picker( "penetrated color", wg.hittable_region_penetrated_color );
				xui::end_popup( );
			}
			xui::slider_float( "point scale", wg.pointscale, 0.0f, 100.0f, "%.0f%%" );
			xui::slider_float( "head point scale", wg.head_pointscale, 0.0f, 100.0f, "%.0f%%" );
			xui::multicombo( "hitboxes", wg.hitboxes, detail::hitbox_names, 6 );

			xui::end_child( );
		}

		xui::layout::set_cursor( right_x - wx, body_y - wy );

		if ( xui::begin_child( "##ragebot_antiaim", col_w ) )
		{
			xui::checkbox( "anti aim", aa.enabled );

			xui::combo( "pitch", aa.pitch.value, detail::pitch_items, 3 );
			xui::combo( "yaw base", aa.yaw.value, detail::yaw_items, 2 );
			xui::combo( "yaw mode", aa.yaw_pattern_mode.value, detail::yaw_pattern_items, 3 );

			if ( aa.yaw_pattern_mode == settings::combat::antiaim::yaw_pattern::spin )
			{
				xui::slider_float( "spin speed", aa.spin_speed, 10.0f, 720.0f, "%.0f deg/s" );
			}
			else if ( aa.yaw_pattern_mode == settings::combat::antiaim::yaw_pattern::jitter )
			{
				xui::slider_float( "jitter range", aa.jitter_range, 0.0f, 180.0f, "%.0f deg" );
				xui::slider_float( "jitter speed", aa.jitter_speed, 0.5f, 20.0f, "%.1f Hz" );
			}

			xui::checkbox( "compensate roll", aa.auto_yaw_adjust );
			xui::checkbox( "force left", aa.manual_left );
			xui::checkbox( "force right", aa.manual_right );
			xui::checkbox( "hide onshot", aa.hide_shots );
			xui::checkbox( "avoid backstab", aa.avoid_backstab );
			xui::checkbox( "direction indicator", aa.direction_indicator );

			if ( xui::begin_popup( "##aa_indicator", 220.0f ) )
			{
				xui::color_picker( "color##aa_ind", aa.direction_indicator_color );
				xui::checkbox( "glow##aa_ind", aa.direction_indicator_glow );
				xui::slider_float( "glow strength##aa_ind", aa.direction_indicator_glow_strength, 0.1f, 1.0f, "%.2f" );
				xui::end_popup( );
			}

			xui::end_child( );
		}

		if ( xui::begin_child( "##ragebot_otherbots", col_w ) )
		{
			xui::checkbox( "auto revolver", autos.revolver );

			xui::checkbox( "zeusbot", zb.enabled );
			if ( xui::begin_popup( "##zb_settings", 220.0f ) )
			{
				xui::slider_float( "max fov##zb", zb.max_fov, 1, 180, "%.0f°" );
				xui::checkbox( "drop after##zb", zb.drop_after );
				xui::end_popup( );
			}

			xui::checkbox( "knifebot", kb.enabled );
			if ( xui::begin_popup( "##kb_settings", 220.0f ) )
			{
				xui::slider_float( "max fov##kb", kb.max_fov, 1, 180, "%.0f°" );
				xui::end_popup( );
			}

			xui::end_child( );
		}

		if ( xui::begin_child( "##ragebot_peek", col_w ) )
		{
			xui::checkbox( "quick peek", qp.enabled );
			if ( xui::begin_popup( "##qp_colors", 220.0f ) )
			{
				xui::color_picker( "base color##qp", qp.color );
				xui::color_picker( "retracting color##qp", qp.retrack_color );
				xui::end_popup( );
			}

			xui::checkbox( "duck peek", dp.enabled );

			xui::end_child( );
		}
	}

} // namespace rendering
