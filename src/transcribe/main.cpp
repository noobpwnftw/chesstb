// Re-encode finished EGTB tables into another shipping form.
//
// Usage:
//   ./transcribe --out output --dtz dtz -r KQK,KRK
//   ./transcribe --out output --dtz dtz --list five.txt --loss-only
//   ./transcribe --out output --do-wdl --list five.txt --block 64
//
// Writes under --out, never in place: the source is mmap'd for the duration.
// Each metric is independent, so a run may name any subset.

#include "transcribe/transcribe.h"

#include "chess/attack.h"
#include "chess/piece_config.h"

#include "egtb/egtb_gen.h"

#include "util/endian.h"
#include "util/fleet_lock.h"
#include "util/thread_pool.h"
#include "util/utility.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options
{
	size_t num_threads = std::max(1u, std::thread::hardware_concurrency());
	size_t cache_mib = DEFAULT_BLOCK_CACHE_BYTES / (1024 * 1024);
	std::vector<std::string> materials;
	std::filesystem::path list_file;
	std::filesystem::path wdl_dir = "./wdl/";
	bool do_wdl = false;
	std::filesystem::path dtz_dir;
	std::filesystem::path dtc_dir;
	std::filesystem::path dtm_dir;
	std::filesystem::path dtm50_dir;
	bool fleet = false;
	Transcribe_Options to;
};

void print_usage()
{
	std::cerr <<
		"Usage: transcribe --out DIR [options]\n"
		"  -r LIST        comma-separated materials (e.g. -r KQK,KRK)\n"
		"  --list FILE    newline-separated material names\n"
		"  --out DIR      output directory, must differ from the input one\n"
		"  --wdl DIR      WDL companions, and the source for --do-wdl (default: ./wdl/)\n"
		"  --do-wdl       transcribe only the WDL tables\n"
		"  --dtz DIR      transcribe dtz/<material>.lzdtz from DIR\n"
		"  --dtc DIR      transcribe dtc/<material>.lzdtc from DIR\n"
		"  --dtm DIR      transcribe dtm/<material>.lzdtm from DIR\n"
		"  --dtm50 DIR    transcribe dtm50/<material>.lzdtm50 from DIR\n"
		"  --extract-dtz  transcribe DTC's embedded DTZ as a standalone table;\n"
		"                 automatic with --loss-only\n"
		"  --extract-dtm  transcribe DTM50's embedded DTM as a standalone table;\n"
		"                 automatic with --loss-only\n"
		"  --loss-only    store loss-class cells only; wins derive at probe time\n"
		"                 (rejected with --do-wdl: WDL carries the class)\n"
		"  --relaxed\n"
		"                 --do-wdl: cap each cell a capture or promotion already\n"
		"                 reaches and let the probe raise it back.\n"
		"                 --dtz: omit winning values proved to be 1 by a zeroing\n"
		"                 move that preserves the WDL class.\n"
		"                 Symmetric materials use loss-only for every requested\n"
		"                 distance table. Other distance types are rejected for\n"
		"                 asymmetric materials.\n"
		"                 Relaxed WDL/DTZ needs the WDL sub-table closure under\n"
		"                 --wdl, in full form\n"
		"  --block KiB    output block size (default: the metric's own)\n"
		"  --samples N    blocks compressed per permutation candidate\n"
		"                 (default: the metric's own, 1024 for WDL and 64 otherwise)\n"
		"  -t N           worker threads (default: hardware concurrency)\n"
		"  --cache MiB    decoded-block budget shared by the sources (default 64,\n"
		"                 0 = one block)\n"
		"  --fleet        cooperate through per-material locks, skip finished output\n"
		"                 and materials whose source tables are unavailable\n"
		"  --tmp DIR      scratch directory for block spill (default ./tmp/)\n";
}

NODISCARD bool parse_args(int argc, char** argv, Options& out)
{
	for (int i = 1; i < argc; ++i)
	{
		const std::string a = argv[i];
		auto take = [&](const char* flag) -> const char* {
			if (a != flag) return nullptr;
			if (i + 1 >= argc) { std::cerr << flag << " needs an argument\n"; std::exit(1); }
			return argv[++i];
		};
		if (const char* v = take("-r")) {
			std::stringstream ss(v);
			for (std::string s; std::getline(ss, s, ',');)
				if (!s.empty()) out.materials.push_back(s);
		}
		else if (const char* v = take("--list"))    out.list_file = v;
		else if (const char* v = take("--out"))     out.to.out_dir = v;
		else if (const char* v = take("--wdl"))     out.wdl_dir = v;
		else if (const char* v = take("--dtz"))     out.dtz_dir = v;
		else if (const char* v = take("--dtc"))     out.dtc_dir = v;
		else if (const char* v = take("--dtm"))     out.dtm_dir = v;
		else if (const char* v = take("--dtm50"))   out.dtm50_dir = v;
		else if (const char* v = take("--tmp"))     out.to.tmp_dir = v;
		else if (const char* v = take("--block"))
			out.to.block_size = std::strtoull(v, nullptr, 10) * 1024;
		else if (const char* v = take("--samples"))
			out.to.perm_samples = std::strtoull(v, nullptr, 10);
		else if (const char* v = take("-t"))
			out.num_threads = std::max<size_t>(1, std::strtoull(v, nullptr, 10));
		else if (const char* v = take("--cache"))
			out.cache_mib = std::strtoull(v, nullptr, 10);
		else if (a == "--do-wdl")    out.do_wdl = true;
		else if (a == "--extract-dtz") out.to.extract_dtz = true;
		else if (a == "--extract-dtm") out.to.extract_dtm = true;
		else if (a == "--loss-only") out.to.loss_only = true;
		else if (a == "--relaxed") out.to.relaxed = true;
		else if (a == "--fleet") out.fleet = true;
		else if (a == "-h" || a == "--help") { print_usage(); std::exit(0); }
		else { std::cerr << "unknown arg: " << a << "\n"; print_usage(); return false; }
	}
	return true;
}

NODISCARD std::vector<std::string> read_list_file(const std::filesystem::path& path)
{
	std::ifstream f(path);
	if (!f) throw std::runtime_error("Could not open list file: " + path.string());
	std::vector<std::string> out;
	for (std::string line; std::getline(f, line);)
	{
		auto h = line.find_first_of(";#");
		if (h != std::string::npos) line.resize(h);
		while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
		size_t i = 0; while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
		if (i < line.size()) out.push_back(line.substr(i));
	}
	return out;
}

NODISCARD Transcribe_Options material_options(const Options& opt, const Piece_Config& ps)
{
	Transcribe_Options out = opt.to;
	if (!out.relaxed || opt.do_wdl) return out;

	const auto [material_key, mirror_key] = ps.material_keys();
	if (material_key == mirror_key)
	{
		out.relaxed = false;
		out.loss_only = true;
	}
	else if ((!opt.dtc_dir.empty() && ps.has_pawns())
	      || !opt.dtm_dir.empty() || !opt.dtm50_dir.empty())
	{
		throw std::runtime_error("--relaxed supports only DTZ for asymmetric materials");
	}
	return out;
}

NODISCARD bool table_exists(const std::filesystem::path& dir,
	                         const Piece_Config& ps, const std::string& ext)
{
	return file_exists_case_exact(path_join(dir, ps.name() + ext));
}

NODISCARD bool output_complete(const Options& opt, const Piece_Config& ps,
	                            const Transcribe_Options& material_opt)
{
	const auto has = [&](const std::string& ext) {
		return table_exists(material_opt.out_dir, ps, ext);
	};
	if (opt.do_wdl) return has(EGTB_Paths::WDL_EXT);
	if (!opt.dtz_dir.empty() && !has(EGTB_Paths::DTZ_EXT)) return false;
	if (!opt.dtc_dir.empty() && ps.has_pawns())
	{
		if (!has(EGTB_Paths::DTZ_EXT) && (material_opt.loss_only || material_opt.extract_dtz))
			return false;
		if (!material_opt.extract_dtz || material_opt.loss_only)
			if (!has(EGTB_Paths::DTC_EXT)) return false;
	}
	if (!opt.dtm_dir.empty() && !has(EGTB_Paths::DTM_EXT)) return false;
	if (!opt.dtm50_dir.empty())
	{
		if (!has(EGTB_Paths::DTM_EXT) && (material_opt.loss_only || material_opt.extract_dtm))
			return false;
		if (!material_opt.extract_dtm || material_opt.loss_only)
			if (!has(EGTB_Paths::DTM50_EXT)) return false;
	}
	return true;
}

NODISCARD bool sources_ready(const Options& opt, const Piece_Config& ps,
	                          const Transcribe_Options& material_opt)
{
	const auto has_wdl = [&](const Piece_Config& material) {
		return table_exists(opt.wdl_dir, material, EGTB_Paths::WDL_EXT);
	};
	if (!has_wdl(ps)) return false;
	if (opt.do_wdl)
	{
		if (!material_opt.relaxed) return true;
	}
	else
	{
		if (!opt.dtz_dir.empty() && !table_exists(opt.dtz_dir, ps, EGTB_Paths::DTZ_EXT))
			return false;
		if (!opt.dtc_dir.empty() && ps.has_pawns()
		 && !table_exists(opt.dtc_dir, ps, EGTB_Paths::DTC_EXT))
			return false;
		if (!opt.dtm_dir.empty() && !table_exists(opt.dtm_dir, ps, EGTB_Paths::DTM_EXT))
			return false;
		if (!opt.dtm50_dir.empty()
		 && !table_exists(opt.dtm50_dir, ps, EGTB_Paths::DTM50_EXT))
			return false;
		if (!material_opt.relaxed) return true;
	}

	for (const auto& [key, sub] : EGTB_Generator::enumerate_sub_materials(ps))
		if (!sub.is_bare_kings() && !has_wdl(sub)) return false;
	return true;
}

}  // namespace

int main(int argc, char** argv)
{
	try
	{
		if (!is_little_endian()) { std::cerr << "Only little-endian hosts supported.\n"; return 1; }

		Options opt;
		if (!parse_args(argc, argv, opt)) return 1;

		if (!opt.list_file.empty())
		{
			const auto from_file = read_list_file(opt.list_file);
			opt.materials.insert(opt.materials.end(), from_file.begin(), from_file.end());
		}
		if (opt.materials.empty() || opt.to.out_dir.empty()
			|| (!opt.do_wdl && opt.dtz_dir.empty() && opt.dtc_dir.empty()
			    && opt.dtm_dir.empty() && opt.dtm50_dir.empty()))
		{
			print_usage();
			return 1;
		}

		if (opt.do_wdl && opt.to.loss_only)
		{
			std::cerr << "--loss-only does not apply to --do-wdl: WDL carries the class\n";
			return 1;
		}

		if (opt.to.relaxed && opt.to.loss_only)
		{
			std::cerr << "--relaxed does not stack with --loss-only: it already drops wins\n";
			return 1;
		}

		// A tail is one short of a block; narrowing_static_cast only asserts.
		if (opt.do_wdl && opt.to.block_size > size_t{ UINT16_MAX } + 1)
		{
			std::cerr << "--block " << opt.to.block_size / 1024 << " exceeds WDL's 16-bit tail field\n";
			return 1;
		}
		if (opt.to.block_size > size_t{ UINT32_MAX } + 1)
		{
			std::cerr << "--block " << opt.to.block_size / 1024 << " exceeds the 32-bit tail field\n";
			return 1;
		}
		const auto same_as_output = [&](const std::filesystem::path& input) {
			return !input.empty()
			    && opt.to.out_dir.lexically_normal() == input.lexically_normal();
		};
		if ((opt.do_wdl && same_as_output(opt.wdl_dir))
		 || (!opt.do_wdl && (same_as_output(opt.dtz_dir)
		                      || same_as_output(opt.dtc_dir)
		                      || same_as_output(opt.dtm_dir)
		                      || same_as_output(opt.dtm50_dir))))
		{
			std::cerr << "Output directory must differ from every input directory\n";
			return 1;
		}

		default_block_pool()->set_max_bytes(opt.cache_mib * 1024 * 1024);

		Thread_Pool pool(opt.num_threads);
		auto transcribe_material = [&](const Piece_Config& ps,
		                              const Transcribe_Options& material_opt) {
			const std::string& name = ps.name();
			if (opt.to.relaxed && material_opt.loss_only)
				std::cout << "  " << name
				          << ": symmetric material; using loss-only distance output\n";
			if (opt.do_wdl)
			{
				std::cout << "  " << name << ": transcribing WDL...\n";
				transcribe_wdl(inout_param(pool), ps, opt.wdl_dir, material_opt);
				return;
			}
			if (!opt.dtz_dir.empty())
			{
				std::cout << "  " << name << ": transcribing DTZ...\n";
				transcribe_dtz(inout_param(pool), ps, opt.dtz_dir, opt.wdl_dir, material_opt);
			}
			// DTC is pawnful-only, so a pawnless material has no file to read.
			if (!opt.dtc_dir.empty() && ps.has_pawns())
			{
				std::cout << "  " << name << ": transcribing DTC...\n";
				transcribe_dtc(inout_param(pool), ps, opt.dtc_dir, opt.wdl_dir, material_opt);
			}
			if (!opt.dtm_dir.empty())
			{
				std::cout << "  " << name << ": transcribing DTM...\n";
				transcribe_dtm(inout_param(pool), ps, opt.dtm_dir, opt.wdl_dir, material_opt);
			}
			if (!opt.dtm50_dir.empty())
			{
				std::cout << "  " << name << ": transcribing DTM50...\n";
				transcribe_dtm50(inout_param(pool), ps, opt.dtm50_dir, opt.wdl_dir, material_opt);
			}
		};

		// The settings follow from the material, so they are derived where they
		// are needed rather than carried beside it. Deriving one here also
		// rejects an unsupported combination before any work starts.
		std::vector<Piece_Config> materials;
		materials.reserve(opt.materials.size());
		for (const std::string& name : opt.materials)
		{
			Piece_Config ps(name);
			if (sources_ready(opt, ps, material_options(opt, ps)))
				materials.push_back(std::move(ps));
		}
		std::filesystem::create_directories(opt.to.tmp_dir);
		std::filesystem::create_directories(opt.to.out_dir);

		while (true)
		{
			size_t idx = 0;
			for (const Piece_Config& ps : materials)
			{
				++idx;
				try
				{
					const Transcribe_Options material_opt = material_options(opt, ps);
					Fleet_Lock lock;
					if (opt.fleet)
					{
						lock = Fleet_Lock(opt.to.tmp_dir / (ps.name() + ".lock"));
						if (!lock.held())
						{
							std::cout << "[" << idx << "/" << materials.size() << "] "
							          << ps.name() << ": locked by another worker, skipping.\n";
							continue;
						}
					}

					if (output_complete(opt, ps, material_opt))
					{
						std::cout << "[" << idx << "/" << materials.size() << "] "
						          << ps.name() << ": output already on disk.\n";
						lock.remove_file();
						continue;
					}

					const auto t_start = std::chrono::steady_clock::now();
					std::cout << "[" << idx << "/" << materials.size() << "] "
					          << ps.name() << ": starting.\n";
					transcribe_material(ps, material_opt);
					pool.respawn_all_threads();
					std::cout << "  " << ps.name() << " done in "
					          << format_elapsed_time(t_start, std::chrono::steady_clock::now()) << "\n";
					lock.remove_file();
				}
				catch (const std::runtime_error& e)
				{
					if (!opt.fleet) throw;
					std::cout << "  " << ps.name() << ": skipping (" << e.what() << ")\n";
				}
			}
			if (!opt.fleet || std::all_of(materials.begin(), materials.end(),
			    [&](const Piece_Config& ps) {
				    return output_complete(opt, ps, material_options(opt, ps));
			    }))
				break;
			std::this_thread::sleep_for(std::chrono::seconds(60));
		}
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
