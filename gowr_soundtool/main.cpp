#include <iostream>
#include <cstddef>
#include <fstream>
#include <string>
#include <mutex>
#include <filesystem>
#include <span>
#include <algorithm>
#include <mutex>
#include <format>

namespace fs = std::filesystem;

struct TOC_Entry_t
{
	std::uint32_t m_file_id;
	std::uint32_t m_file_size;
	std::uint32_t m_offset;
};

struct TOC_Header_t
{
	std::uint32_t m_magic; // 0x4b415041
	std::uint32_t m_version;
	std::uint32_t m_entries_count;
	std::uint32_t m_parts_count; // always '1'
};


// Mutexes
static std::mutex files_sorted_mtx;
static std::mutex files_entries_mtx;


// Read the binary file into string
[[nodiscard]] inline std::string BinToString( 
	const fs::path & file_path
)
{
	std::ifstream file_in( file_path, std::ios::binary );

	if (!file_in)
		throw std::runtime_error( std::format( "Failed to open file '{}'", file_path.string() ) );

	file_in.seekg( 0, std::ios::end );
	std::streamsize size = file_in.tellg();
	file_in.seekg( 0, std::ios::beg );

	if (size == 0)
		throw std::runtime_error( std::format( "File '{}' is empty!", file_path.string() ) );

	std::string buffer( static_cast<std::size_t>(size), '\0' );

	if (!file_in.read( buffer.data(), size ))
		throw std::runtime_error( std::format( "Failed to read file '{}'", file_path.string() ) );

	return buffer;
}


// Write the specified file with content
inline void WriteFile( 
	const std::string file_name,
	std::span<const char> data
)
{
	std::ofstream file_out( file_name, std::ios_base::binary | std::ios::out );

	if (!file_out)
		throw std::runtime_error( std::format( "Failed to write the output file '{}'", file_name ) );

	file_out.write( data.data(), static_cast<std::streamsize>(data.size()) );
	file_out.close();

	if (!file_out)
		throw std::runtime_error( std::format( "Failed to write complete data to file '{}'", file_name ) );
}


// Unpack everything
inline void UnpackAudio( 
	const fs::path & toc_path
)
{
	const fs::path out_dir = toc_path.parent_path() / toc_path.filename().stem();
	fs::create_directories( out_dir );

	// Read TOC and audio pack data
	const std::string toc_file = BinToString( toc_path );

	const fs::path audiopack_path = toc_path.stem().stem() += fs::path {".0.audiopack"};
	const std::string audio_pack_file = BinToString( audiopack_path );

	if (toc_file.size() < sizeof( TOC_Header_t ))
		throw std::runtime_error( "TOC file is too small to contain a valid header." );

	// Parse TOC header
	const TOC_Header_t & toc_header = *reinterpret_cast<const TOC_Header_t *>(toc_file.data());
	std::size_t entry_offset = sizeof( TOC_Header_t );

	// Check if the magic is valid
	if(toc_header.m_magic != 0x4b415041)
		throw std::runtime_error( "Invalid TOC file magic." );

	// Check if the number of entries is a valid one
	if(toc_header.m_entries_count <= 0)
		throw std::runtime_error( "Bad number of entries." );

	for (std::uint32_t i { 0 }; i < toc_header.m_entries_count; ++i)
	{
		if (entry_offset + sizeof( TOC_Entry_t ) > toc_file.size())
			throw std::runtime_error( "TOC file is corrupted or incomplete." );

		// Parse each entry inside the TOC
		const TOC_Entry_t & entry = *reinterpret_cast<const TOC_Entry_t *>(&toc_file[entry_offset]);
		entry_offset += sizeof( TOC_Entry_t );

		// Check that the file offset and size are valid
		if (entry.m_offset + entry.m_file_size > audio_pack_file.size())
			throw std::runtime_error( "Audio pack file is corrupted or incomplete." );

		// Read the data and write it to a file
		std::span<const char> file_data( &audio_pack_file[entry.m_offset], entry.m_file_size );
		std::string file_name = std::format( "{}\\{}.wem", out_dir.string(), entry.m_file_id );

		std::cout << std::format( "Writing '{}'\n", file_name );

		WriteFile( file_name, file_data );
	}

	std::cout << "\nSuccessfully unpacked all the files!" << std::endl;
}


// Pack all the .wem files back into .toc and .audiopack
inline void PackAudio(
	const fs::path & wem_directory
)
{
	static std::vector<TOC_Entry_t> entries;
	std::uint32_t current_offset { 0 };

	const fs::path pack_file = wem_directory.stem() += fs::path { ".0.audiopack" };
	std::ofstream pack_out( pack_file, std::ios::binary );

	if (!pack_out)
		throw std::runtime_error( std::format( "Failed to open output pack file '{}'", pack_file.string() ) );

	static std::vector<fs::path> wem_files;

	// Collect .wem files into a vector
	for (const auto & entry : fs::directory_iterator( wem_directory ))
	{
		if ((entry.path().extension() == ".wem" && entry.is_regular_file()))
		{
			std::lock_guard guard( files_sorted_mtx );
			wem_files.push_back( entry.path() );
		}
	}

	// Comparator to sort files by the numeric part of the filename
	auto sorted_files = []( 
		const fs::path & lhs,
		const fs::path & rhs ) -> bool
	{
		try
		{
			const std::uint32_t lhs_num = std::stoi( lhs.stem().string() );
			const std::uint32_t rhs_num = std::stoi( rhs.stem().string() );

			return lhs_num < rhs_num;
		}
		catch (const std::exception & e)
		{
			std::cerr << "Error parsing filename:" << ' ' << e.what() << '\n';
			return false;
		}
	};

	// Sort files by the numeric part of the filename
	std::sort( wem_files.begin(), wem_files.end(), sorted_files );

	std::cout << "Packing Audiopack file" << ' ' << pack_file << std::endl;

	for (const auto & wem_file : wem_files)
	{
		const std::string file = BinToString( wem_file );

		std::uint32_t file_size = static_cast<std::uint32_t>(file.size());
		std::uint32_t file_id = static_cast<std::uint32_t>(std::stoi( wem_file.stem().string() ));

		pack_out.write( file.data(), file.size() );

		// Fill in an entry struct
		TOC_Entry_t stream_entry { file_id, file_size, current_offset };
		current_offset += file_size;

		// Add to the list of entries
		std::lock_guard guard( files_entries_mtx );
		entries.push_back( stream_entry );
	}

	pack_out.close();

	if (entries.size() == 0)
		throw std::runtime_error( "The directory does not contain any valid .wem file." );

	// Now create the TOC file
	TOC_Header_t toc_header { 0x4b415041, 1, static_cast<std::uint32_t>(entries.size()), 1 };

	const fs::path toc_file = wem_directory.stem() += fs::path { ".audiopack.toc" };
	std::ofstream toc_out( toc_file, std::ios::binary );

	if (!toc_out)
		throw std::runtime_error( std::format( "Failed to open output TOC file '{}'", toc_file.string() ) );

	std::cout << "Packing TOC file" << ' ' << toc_file << std::endl;

	// Write the TOC header
	toc_out.write( reinterpret_cast<const char *>(&toc_header), sizeof( TOC_Header_t ) );

	// Write the TOC entries
	for (const auto & entry : entries)
		toc_out.write( reinterpret_cast<const char *>(&entry), sizeof( TOC_Entry_t ) );

	toc_out.close();

	std::cout << "\nPacking completed successfully!" << std::endl;
}


int main( 
	int argc,
	char * argv[]
)
{
	try
	{
		if (argc < 3)
			throw std::invalid_argument( "Usage:\n\n gowr_soundtool --pack <dir_with_wem_files>\n\n or \n\ngowr_soundtool --unpack <toc_file_path>" );

		if (std::strcmp( argv[1], "--unpack" ) == 0)
		{
			const fs::path toc_path = argv[2];

			if (!fs::exists( toc_path ))
				throw std::runtime_error( std::format( "TOC file {} does not exist !", toc_path.string() ) );

			UnpackAudio( toc_path );
		}

		else if (std::strcmp( argv[1], "--pack" ) == 0)
		{
			const fs::path wem_directory = argv[2];

			if (!fs::exists( wem_directory ) || !fs::is_directory( wem_directory ))
				throw std::invalid_argument( "The specified directory does not exist or is not a directory." );

			PackAudio( wem_directory );
		}
		else
			throw std::invalid_argument( "You need to have either '--pack' or '--unpack' as the first argument." );
	}
	catch (const std::exception & e)
	{
		std::cerr << "[Error]" << ' ' << e.what() << std::endl;
		static_cast<void>(std::getchar());

		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}