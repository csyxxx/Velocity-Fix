#pragma once

#include <filesystem>

namespace features::changer::model_files {

	inline constexpr std::wstring_view directory{ L"C:\\velocity-pro\\models" };
	inline constexpr std::string_view resource_prefix{ "models/velocity/" };

	struct entry
	{
		std::string name{};
		std::string resource_path{};
	};

	namespace detail {

		inline std::shared_mutex registry_mutex{};
		inline std::unordered_map<std::string, std::filesystem::path> resource_registry{};

		[[nodiscard]] inline std::string lower_copy( std::string value )
		{
			std::transform( value.begin( ), value.end( ), value.begin( ), [ ]( unsigned char c )
				{
					return static_cast< char >( std::tolower( c ) );
				} );
			return value;
		}

		[[nodiscard]] inline bool is_resource_root( std::string_view path )
		{
			constexpr std::array roots
			{
				"characters/", "materials/", "models/", "animations/", "particles/",
				"sounds/", "soundevents/", "scripts/", "panorama/"
			};

			return std::ranges::any_of( roots, [ path ]( std::string_view root )
				{
					return path.starts_with( root );
				} );
		}

		[[nodiscard]] inline std::string infer_model_path( const std::filesystem::path& file )
		{
			auto filename = lower_copy( file.filename( ).string( ) );
			constexpr std::string_view suffix{ ".vmdl_c" };
			if ( !filename.ends_with( suffix ) )
			{
				return {};
			}

			const auto stem = filename.substr( 0, filename.size( ) - suffix.size( ) );
			const auto resource_stem = stem.ends_with( "_ag2" )
				? stem.substr( 0, stem.size( ) - 4 )
				: stem;
			std::ifstream stream{ file, std::ios::binary | std::ios::ate };
			if ( !stream )
			{
				return {};
			}

			const auto size = stream.tellg( );
			if ( size <= 0 || size > static_cast< std::streamoff >( 512ull * 1024ull * 1024ull ) )
			{
				return {};
			}

			std::vector<char> bytes( static_cast< std::size_t >( size ) );
			stream.seekg( 0, std::ios::beg );
			if ( !stream.read( bytes.data( ), size ) )
			{
				return {};
			}

			std::unordered_map<std::string, int> candidates{};
			std::string printable{};
			printable.reserve( 256 );

			const auto inspect = [ & ]( )
				{
					if ( printable.size( ) < 8 )
					{
						printable.clear( );
						return;
					}

					auto text = lower_copy( printable );
					std::replace( text.begin( ), text.end( ), '\\', '/' );

					for ( const auto root : { std::string_view{ "characters/" }, std::string_view{ "models/" } } )
					{
						const auto root_pos = text.find( root );
						if ( root_pos == std::string::npos )
						{
							continue;
						}

						const auto token = std::string{ "/" } + stem + "/";
						const auto resource_token = std::string{ "/" } + resource_stem + "/";
						auto stem_pos = text.find( token, root_pos );
						if ( stem_pos == std::string::npos && resource_stem != stem )
						{
							stem_pos = text.find( resource_token, root_pos );
						}
						if ( stem_pos == std::string::npos )
						{
							continue;
						}

						const auto directory_end = stem_pos + 1 + ( text.compare( stem_pos + 1, resource_stem.size( ), resource_stem ) == 0 ? resource_stem.size( ) : stem.size( ) );
						auto candidate = text.substr( root_pos, directory_end - root_pos );
						candidate += "/" + stem + ".vmdl";
						++candidates[ std::move( candidate ) ];
						break;
					}

					printable.clear( );
				};

			for ( const auto byte : bytes )
			{
				const auto value = static_cast< unsigned char >( byte );
				if ( value >= 0x20 && value <= 0x7e )
				{
					printable.push_back( static_cast< char >( value ) );
				}
				else
				{
					inspect( );
				}
			}
			inspect( );

			std::string best{};
			int best_score{};
			for ( const auto& [ candidate, score ] : candidates )
			{
				if ( score > best_score || ( score == best_score && ( best.empty( ) || candidate.size( ) < best.size( ) ) ) )
				{
					best = candidate;
					best_score = score;
				}
			}

			return best;
		}

	} // namespace detail

	[[nodiscard]] inline std::vector<entry> list( )
	{
		struct compiled_file
		{
			std::filesystem::path path{};
			std::filesystem::path relative{};
		};

		struct package_mount
		{
			std::filesystem::path physical_root{};
			std::filesystem::path resource_root{};
		};

		const std::filesystem::path root{ directory };
		std::error_code error{};
		std::filesystem::create_directories( root, error );

		std::vector<entry> result{};
		std::vector<compiled_file> compiled_files{};
		std::vector<package_mount> package_mounts{};
		std::unordered_map<std::string, std::filesystem::path> registry{};
		std::filesystem::recursive_directory_iterator it{ root, std::filesystem::directory_options::skip_permission_denied, error };
		const std::filesystem::recursive_directory_iterator end{};

		for ( ; it != end; it.increment( error ) )
		{
			if ( error )
			{
				error.clear( );
				continue;
			}

			if ( !it->is_regular_file( error ) )
			{
				continue;
			}

			auto relative = std::filesystem::relative( it->path( ), root, error );
			if ( error )
			{
				error.clear( );
				continue;
			}

			auto generic = relative.generic_string( );
			auto logical = detail::lower_copy( generic );
			if ( !logical.ends_with( "_c" ) )
			{
				continue;
			}

			logical.erase( logical.size( ) - 2 );
			compiled_files.push_back( { it->path( ), relative } );
			registry[ std::string{ resource_prefix } + logical ] = it->path( );
			if ( detail::is_resource_root( logical ) )
			{
				registry[ logical ] = it->path( );
			}

			if ( !logical.ends_with( ".vmdl" ) )
			{
				continue;
			}

			auto resource_path = detail::infer_model_path( it->path( ) );
			if ( resource_path.empty( ) )
			{
				resource_path = std::string{ resource_prefix } + logical;
			}
			else
			{
				package_mounts.push_back( {
					it->path( ).parent_path( ),
					std::filesystem::path{ resource_path }.parent_path( )
					} );
			}

			registry[ resource_path ] = it->path( );
			result.push_back( { generic.substr( 0, generic.size( ) - 2 ), std::move( resource_path ) } );
		}

		for ( const auto& mount : package_mounts )
		{
			for ( const auto& file : compiled_files )
			{
				auto package_relative = std::filesystem::relative( file.path, mount.physical_root, error );
				if ( error )
				{
					error.clear( );
					continue;
				}

				const auto first = package_relative.begin( );
				if ( package_relative.empty( ) || ( first != package_relative.end( ) && *first == L".." ) )
				{
					continue;
				}

				auto package_logical = detail::lower_copy( package_relative.generic_string( ) );
				if ( !package_logical.ends_with( "_c" ) )
				{
					continue;
				}

				package_logical.erase( package_logical.size( ) - 2 );
				auto mounted_path = ( mount.resource_root / std::filesystem::path{ package_logical } ).generic_string( );
				registry[ detail::lower_copy( std::move( mounted_path ) ) ] = file.path;
			}
		}

		{
			std::unique_lock lock{ detail::registry_mutex };
			detail::resource_registry = std::move( registry );
		}

		std::ranges::sort( result, [ ]( const entry& lhs, const entry& rhs )
			{
				return detail::lower_copy( lhs.name ) < detail::lower_copy( rhs.name );
			} );
		return result;
	}

	[[nodiscard]] inline std::optional<std::filesystem::path> resolve_resource( std::string_view requested )
	{
		auto normalized = detail::lower_copy( std::string{ requested } );
		std::replace( normalized.begin( ), normalized.end( ), '\\', '/' );

		std::shared_lock lock{ detail::registry_mutex };
		if ( const auto found = detail::resource_registry.find( normalized ); found != detail::resource_registry.end( ) )
		{
			return found->second;
		}

		if ( normalized.ends_with( "_c" ) )
		{
			normalized.erase( normalized.size( ) - 2 );
			if ( const auto found = detail::resource_registry.find( normalized ); found != detail::resource_registry.end( ) )
			{
				return found->second;
			}
		}

		return std::nullopt;
	}

} // namespace features::changer::model_files
