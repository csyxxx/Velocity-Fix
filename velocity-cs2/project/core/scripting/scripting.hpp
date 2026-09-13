#pragma once

#include <memory>
#include <string>
#include <vector>

namespace xdraw {
	struct draw_list;
}

namespace scripting {
	enum class console_level
	{
		info,
		warning,
		error
	};

	struct console_entry
	{
		std::string script{};
		std::string message{};
		console_level level{ console_level::info };
	};

	struct script_info
	{
		std::string name{};
		bool loaded{};
		std::string error{};
	};

	class engine
	{
	public:
		engine( );
		~engine( );

		engine( const engine& ) = delete;
		engine& operator=( const engine& ) = delete;

		void initialize( );
		void shutdown( );
		void refresh( );
		void reload_all( );
		void on_render( xdraw::draw_list& draw_list );
		void on_menu( );
		void on_create_move_pre( void* command );
		void on_create_move( void* command );
		void on_create_move_post( void* command );
		void on_game_event( const char* event_name, void* event );
		void on_level_init( const char* map_name );
		void on_level_shutdown( );

		[[nodiscard]] bool load( const std::string& name );
		void unload( const std::string& name );
		[[nodiscard]] std::vector<script_info> scripts( ) const;
		[[nodiscard]] std::vector<console_entry> console_entries( ) const;
		void clear_console( );
		[[nodiscard]] bool export_console( ) const;
		[[nodiscard]] std::string directory( ) const;

	private:
		struct impl;
		std::unique_ptr<impl> m_impl{};
	};

	inline engine g_engine{};

} // namespace scripting
