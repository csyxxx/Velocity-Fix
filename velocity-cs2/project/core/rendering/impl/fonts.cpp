#include <pch/pch.hpp>
#include <core/resources/fonts/inter.hpp>
#include <core/resources/fonts/pixel7.hpp>
#include "../rendering.hpp"

namespace rendering {

	void fonts::initialize( )
	{
		this->load_family( this->inter_medium, std::as_bytes( std::span{ resources::fonts::inter::regular } ), { 12.0f, 15.0f, 18.0f } );
		this->load_family( this->inter_bold, std::as_bytes( std::span{ resources::fonts::inter::bold } ), { 12.0f, 15.0f, 18.0f } );
		this->load_family( this->smallest_pixel7, std::as_bytes( std::span{ resources::fonts::pixel7::smallest } ), { 9.0f, 10.5f, 14.0f } );

		// Inter and Pixel7 do not contain CJK glyphs. Keep the fallback bytes alive
		// for the lifetime of the FreeType faces created from them.
		constexpr std::array cjk_paths{
			"C:\\Windows\\Fonts\\msyh.ttc",
			"C:\\Windows\\Fonts\\Noto Sans SC (TrueType).otf",
			"C:\\Windows\\Fonts\\simhei.ttf"
		};

		for ( const auto* path : cjk_paths )
		{
			std::ifstream file{ path, std::ios::binary | std::ios::ate };
			if ( !file )
			{
				continue;
			}

			const auto size = file.tellg( );
			if ( size <= 0 || size > static_cast< std::streamoff >( 64ull * 1024ull * 1024ull ) )
			{
				continue;
			}

			this->cjk_font_data.resize( static_cast< std::size_t >( size ) );
			file.seekg( 0, std::ios::beg );
			if ( file.read( reinterpret_cast< char* >( this->cjk_font_data.data( ) ), size ) )
			{
				break;
			}

			this->cjk_font_data.clear( );
		}

		if ( !this->cjk_font_data.empty( ) )
		{
			this->load_family( this->cjk_fallback, std::as_bytes( std::span{ this->cjk_font_data } ), { 12.0f, 15.0f, 18.0f } );
			for ( auto i = 0ull; i < static_cast< std::size_t >( size::count ); ++i )
			{
				if ( this->inter_medium.sizes[ i ] )
				{
					this->inter_medium.sizes[ i ]->fallback = this->cjk_fallback.sizes[ i ];
				}
				if ( this->inter_bold.sizes[ i ] )
				{
					this->inter_bold.sizes[ i ]->fallback = this->cjk_fallback.sizes[ i ];
				}
			}

			for ( auto i = 0ull; i < static_cast< std::size_t >( size::count ); ++i )
			{
				if ( this->smallest_pixel7.sizes[ i ] )
				{
					this->smallest_pixel7.sizes[ i ]->fallback = this->cjk_fallback.sizes[ i ];
				}
			}

			// xui uses xdraw's primary font, which is created before project fonts.
			// Preserve its existing math fallback behind the CJK font.
			if ( auto* primary = xdraw::primary_font( ); primary && this->cjk_fallback[ size::normal ] )
			{
				this->cjk_fallback[ size::normal ]->fallback = primary->fallback;
				primary->fallback = this->cjk_fallback[ size::normal ];
			}
		}
	}

	void fonts::load_family( family_t& family, std::span<const std::byte> data, const std::array<float, static_cast< std::size_t >( size::count )>& sizes )
	{
		for ( auto i = 0ull; i < sizes.size( ); ++i )
		{
			family.sizes[ i ] = xdraw::load_font( data, sizes[ i ] );
		}
	}

} // namespace rendering
