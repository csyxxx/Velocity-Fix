#include <pch/pch.hpp>

#include <core/scripting/scripting.hpp>

#include "../../rendering.hpp"

namespace rendering {

	void menu::draw_lua( float group_w )
	{
		static_cast<void>( group_w );

		auto& dl = xui::draw::current( );
		const auto& style = xui::ctx( ).style;
		const auto& input = xui::ctx( ).input;
		auto& scripts = scripting::g_engine;

		xui::layout::set_cursor( this->m_body_x - this->m_x, this->m_body_y - this->m_y );
		if ( !xui::begin_child( "##lua_scripts", this->m_body_w, this->m_body_h, false ) )
		{
			return;
		}

		const auto [ width, height ] = xui::layout::avail( );
		const auto button_width = ( width - style.item_spacing_x ) * 0.5f;
		dl.text( xui::layout::item( width, 20.0f ).x + 4.0f, xui::layout::current_window( )->last_item.y + 2.0f, scripts.directory( ), tokens::col_text_dim );
		if ( xui::button( "lua console", width, 26.0f ) )
		{
			this->m_lua_console_open = true;
		}
		xui::layout::new_line( );
		if ( xui::button( "clear lua console", button_width, 26.0f ) )
		{
			scripts.clear_console( );
		}
		xui::layout::same_line( );
		if ( xui::button( "export console log", button_width, 26.0f ) )
		{
			scripts.export_console( );
		}
		xui::layout::new_line( );

		constexpr auto button_height{ 28.0f };
		if ( xui::button( "refresh", button_width, button_height ) )
		{
			scripts.refresh( );
		}
		xui::layout::same_line( );
		if ( xui::button( "reload all", button_width, button_height ) )
		{
			scripts.reload_all( );
		}
		xui::layout::new_line( );

		const auto entries = scripts.scripts( );
		const auto has_loaded_scripts = std::ranges::any_of( entries, []( const auto& entry ) { return entry.loaded; } );
		const auto content_width = xui::layout::avail( ).first;
		const auto remaining_height = xui::layout::avail( ).second;
		const auto desired_list_height = std::max( 80.0f,
			static_cast<float>( entries.size( ) ) * ( 36.0f + style.item_spacing_y ) + style.window_pad_y * 2.0f );
		const auto controls_reserve = has_loaded_scripts ? 120.0f + style.item_spacing_y : 0.0f;
		const auto list_height = std::clamp(
			desired_list_height,
			std::min( 80.0f, remaining_height ),
			std::max( 80.0f, remaining_height - controls_reserve ) );
		if ( xui::begin_child( "##lua_script_list", content_width, list_height, true ) )
		{
			if ( entries.empty( ) )
			{
				const auto row = xui::layout::item( xui::layout::avail( ).first, 32.0f );
				dl.text( row.x + 8.0f, row.y + 8.0f, "no .lua files found", tokens::col_text_dim );
			}

			for ( const auto& entry : entries )
			{
				const auto row = xui::layout::item( xui::layout::avail( ).first, entry.error.empty( ) ? 36.0f : 58.0f );
				const auto hovered = input.in_rect( row );
				const auto background = entry.loaded ? tokens::col_accent.alpha( 36 ) : tokens::col_elevated.alpha( hovered ? 115 : 70 );
				dl.rect_filled( row.x, row.y, row.w, row.h, background, xdraw::corner_radius{ 6.0f } );

				const auto state = entry.loaded ? "loaded" : "unloaded";
				const auto state_color = entry.loaded ? xdraw::color{ 95, 220, 150, 255 } : tokens::col_text_dim;
				dl.text( row.x + 8.0f, row.y + 7.0f, entry.name, tokens::col_text );
				const auto [ state_w, state_h ] = xdraw::measure_text( state );
				dl.text( row.right( ) - state_w - 140.0f, row.y + ( 28.0f - state_h ) * 0.5f, state, state_color );

				xui::push_id( entry.name );
				const auto list_window = xui::layout::current_window_const( );
				if ( list_window )
				{
					xui::layout::set_cursor(
						row.right( ) - 128.0f - list_window->bounds.x,
						row.y + 4.0f - list_window->bounds.y );
				}
				if ( xui::button( "load", 60.0f, 24.0f ) && !entry.loaded )
				{
					scripts.load( entry.name );
				}
				xui::layout::same_line( );
				if ( xui::button( "unload", 64.0f, 24.0f ) && entry.loaded )
				{
					scripts.unload( entry.name );
				}
				xui::pop_id( );
				if ( list_window )
				{
					// The button is positioned inside the manually drawn row. Restore the
					// row as the current layout item so the next script advances by the
					// full row height instead of continuing from the button coordinates.
					xui::layout::set_cursor( style.window_pad_x, row.y - list_window->bounds.y );
					static_cast<void>( xui::layout::item( 0.0f, row.h ) );
				}

				if ( !entry.error.empty( ) )
				{
					dl.text( row.x + 8.0f, row.y + 33.0f, entry.error, xdraw::color{ 240, 115, 115, 255 } );
				}
			}

			xui::end_child( );
		}

		if ( has_loaded_scripts )
		{
			const auto controls_height = xui::layout::avail( ).second;
			if ( controls_height > 24.0f && xui::begin_child( "##lua_script_controls", content_width, controls_height, true ) )
			{
				scripts.on_menu( );
				xui::end_child( );
			}
		}

		xui::end_child( );
	}

	void menu::draw_lua_console( )
	{
		if ( !this->m_lua_console_open ) return;
		if ( !xui::begin_window( "##lua_console_window", this->m_lua_console_x, this->m_lua_console_y,
			this->m_lua_console_w, this->m_lua_console_h, true, 420.0f, 240.0f ) ) return;

		auto& dl = xui::draw::current( );
		const auto& style = xui::ctx( ).style;
		const auto title_row = xui::layout::item( xui::layout::avail( ).first, 28.0f );
		dl.text( title_row.x + 4.0f, title_row.y + 6.0f, "lua console", tokens::col_text );

		const auto window = xui::layout::current_window_const( );
		if ( window )
		{
			xui::layout::set_cursor( title_row.right( ) - 124.0f - window->bounds.x, title_row.y + 2.0f - window->bounds.y );
		}
		if ( xui::button( "clear", 72.0f, 24.0f ) ) scripting::g_engine.clear_console( );
		xui::layout::same_line( );
		if ( xui::button( "x", 44.0f, 24.0f ) ) this->m_lua_console_open = false;
		if ( window )
		{
			xui::layout::set_cursor( style.window_pad_x, title_row.y - window->bounds.y );
			static_cast<void>( xui::layout::item( 0.0f, title_row.h ) );
		}

		const auto [ content_width, content_height ] = xui::layout::avail( );
		if ( xui::begin_child( "##lua_console_output", content_width, content_height, true ) )
		{
			const auto entries = scripting::g_engine.console_entries( );
			if ( entries.empty( ) )
			{
				xui::text( "no output", tokens::col_text_dim );
			}
			for ( const auto& entry : entries )
			{
				const auto color = entry.level == scripting::console_level::error
					? xdraw::color{ 240, 105, 105, 255 }
					: entry.level == scripting::console_level::warning
						? xdraw::color{ 240, 190, 90, 255 }
						: tokens::col_text;
				const auto line = "[" + entry.script + "] " + entry.message;
				const auto row = xui::layout::item( xui::layout::avail( ).first, 20.0f );
				dl.text( row.x + 4.0f, row.y + 2.0f, line, color );
			}
			xui::end_child( );
		}

		const auto resize_color = tokens::col_text_dim.alpha( 150 );
		dl.line( this->m_lua_console_x + this->m_lua_console_w - 14.0f, this->m_lua_console_y + this->m_lua_console_h - 5.0f,
			this->m_lua_console_x + this->m_lua_console_w - 5.0f, this->m_lua_console_y + this->m_lua_console_h - 14.0f, resize_color, 1.0f );
		xui::end_window( );
	}

} // namespace rendering
