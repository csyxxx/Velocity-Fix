#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <utilities/hooking/hooking.hpp>
#include <utilities/logging/logging.hpp>
#include <core/settings.hpp>
#include <core/features/changer/model_files.hpp>
#include <core/resources/particles/effects.hpp>
#include <core/resources/particles/weather.hpp>
#include <protection/game_addresses.hpp>
#include "../hooks.hpp"

namespace {

	bool read_external_model( const std::string& filename, std::vector<unsigned char>& bytes )
	{
		const auto path = features::changer::model_files::resolve_resource( filename );
		if ( !path )
		{
			return false;
		}

		std::ifstream file{ *path, std::ios::binary | std::ios::ate };
		if ( !file )
		{
			return false;
		}

		const auto size = file.tellg( );
		if ( size <= 0 || size > static_cast< std::streamoff >( 512ull * 1024ull * 1024ull ) )
		{
			return false;
		}

		bytes.resize( static_cast< std::size_t >( size ) );
		file.seekg( 0, std::ios::beg );
		return static_cast< bool >( file.read( reinterpret_cast< char* >( bytes.data( ) ), size ) );
	}

	std::span<const unsigned char> find_embedded_particle( const std::string& filename )
	{
		if ( filename.find( xs( "snow" ) ) != std::string::npos )
		{
			return std::span<const unsigned char>{ resources::particles::weather::snow };
		}
		if ( filename.find( xs( "rain" ) ) != std::string::npos )
		{
			return std::span<const unsigned char>{ resources::particles::weather::rain };
		}
		if ( filename.find( xs( "kill" ) ) != std::string::npos )
		{
			return std::span<const unsigned char>{ resources::particles::effects::killstars };
		}
		if ( filename.find( xs( "stars" ) ) != std::string::npos )
		{
			return std::span<const unsigned char>{ resources::particles::weather::stars };
		}
		if ( filename.find( xs( "tracer" ) ) != std::string::npos )
		{
			return std::span<const unsigned char>{ resources::particles::effects::tracer };
		}
		if ( filename.find( xs( "sparks" ) ) != std::string::npos )
		{
			return std::span<const unsigned char>{ resources::particles::effects::sparks };
		}
		if ( filename.find( xs( "fade" ) ) != std::string::npos )
		{
			return std::span<const unsigned char>{ resources::particles::effects::fade };
		}
		if ( filename.find( xs( "halo" ) ) != std::string::npos )
		{
			return std::span<const unsigned char>{ resources::particles::effects::halo };
		}

		return {};
	}

} // namespace

namespace hooks {

	bool utility::initialize( )
	{
		if ( !hooking::manager::create( {
			{ &m_service_read, &service_read, xs( "service_read" ), PATTERN (patterns::service_read) },
			{ &m_log_internal, &log_internal, xs( "log_internal" ), PATTERN (patterns::log_internal) }
			} ) )
		{
			return false;
		}

		return true;
	}

	void utility::shutdown( )
	{
		m_service_read.reset( );
		m_log_internal.reset( );
	}

	std::uintptr_t __fastcall utility::service_read( std::uintptr_t a1 )
	{
		const auto flags_len = memory::read<std::uint32_t>( a1 - 212 );
		const auto len = flags_len & 0x3fffffff;

		std::string filename;
		if ( len > 0 && len < 512 )
		{
			char buffer[ 512 ]{};

			if ( flags_len & 0x40000000 )
			{
				std::memcpy( buffer, reinterpret_cast< void* >( a1 - 208 ), std::min( len, 511u ) );
			}
			else
			{
				const auto string = memory::read<std::uintptr_t>( a1 - 208 );
				if ( string )
				{
					std::memcpy( buffer, reinterpret_cast< void* >( string ), std::min( len, 511u ) );
				}
			}

			filename = buffer;
		}

		std::vector<unsigned char> external_model{};
		const auto external_model_found = read_external_model( filename, external_model );
		if ( external_model_found )
		{
			logging::console::print( "[model changer] serving {}", filename );
		}

		if ( filename.find( xs( "particles/embedded/" ) ) != std::string::npos || external_model_found )
		{
			const auto particle = external_model_found
				? std::span<const unsigned char>{ external_model }
				: find_embedded_particle( filename );
			const auto async_filesystem = memory::read<std::uintptr_t>( a1 + 24 );

			if ( !particle.empty( ) && async_filesystem )
			{
				const auto buffer = memory::call_vfunc<std::uintptr_t>( async_filesystem, 22, particle.size( ), filename.c_str( ) );
				if ( buffer )
				{
					std::memcpy( reinterpret_cast< void* >( buffer ), particle.data( ), particle.size( ) );

					memory::write<std::uintptr_t>( a1 + 56, buffer );
					memory::write<std::uintptr_t>( a1 + 64, particle.size( ) );
					memory::write<std::uintptr_t>( a1 + 72, particle.size( ) );
					memory::call<void>( PATTERN (patterns::filesystem_close), a1 - 224, 0 );

					return 0;
				}
			}
		}

		return m_service_read.call<std::uintptr_t>( a1 );
	}

	std::intptr_t __fastcall utility::log_internal( std::uintptr_t a1, std::uint32_t channel, std::int32_t severity, std::uintptr_t metadata, const char* message, std::intptr_t* args )
	{
		if ( settings::g_misc.disable_game_logs && !logging::console::emitting )
		{
			return 0;
		}

		return m_log_internal.call<std::intptr_t>( a1, channel, severity, metadata, message, args );
	}

} // namespace hooks
