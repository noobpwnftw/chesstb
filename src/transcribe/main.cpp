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

#include "util/endian.h"
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
		"                 --do-wdl only: cap each cell a capture or promotion already\n"
		"                 reaches and let the probe raise it back. Needs the WDL\n"
		"                 sub-table closure under --wdl, in full form\n"
		"  --block KiB    output block size (default: the metric's own)\n"
		"  --samples N    blocks compressed per permutation candidate\n"
		"                 (default: the metric's own, 1024 for WDL and 64 otherwise)\n"
		"  -t N           worker threads (default: hardware concurrency)\n"
		"  --cache MiB    decoded-block budget shared by the sources (default 64,\n"
		"                 0 = one block)\n"
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

		if (opt.to.relaxed && !opt.do_wdl)
		{
			std::cerr << "--relaxed applies to --do-wdl only\n";
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

		default_block_pool()->set_max_bytes(opt.cache_mib * 1024 * 1024);

		Thread_Pool pool(opt.num_threads);

		size_t failed = 0;
		for (const std::string& name : opt.materials)
		{
			const auto t_start = std::chrono::steady_clock::now();
			try
			{
				const Piece_Config ps(name);
				if (opt.do_wdl)
				{
					std::cout << "  " << name << ": transcribing WDL...\n";
					transcribe_wdl(inout_param(pool), ps, opt.wdl_dir, opt.to);
				}
				else
				{
					if (!opt.dtz_dir.empty())
					{
						std::cout << "  " << name << ": transcribing DTZ...\n";
						transcribe_dtz(inout_param(pool), ps, opt.dtz_dir, opt.wdl_dir, opt.to);
					}
					// DTC is pawnful-only, so a pawnless material has no file to read.
					if (!opt.dtc_dir.empty() && ps.has_pawns())
					{
						std::cout << "  " << name << ": transcribing DTC...\n";
						transcribe_dtc(inout_param(pool), ps, opt.dtc_dir, opt.wdl_dir, opt.to);
					}
					if (!opt.dtm_dir.empty())
					{
						std::cout << "  " << name << ": transcribing DTM...\n";
						transcribe_dtm(inout_param(pool), ps, opt.dtm_dir, opt.wdl_dir, opt.to);
					}
					if (!opt.dtm50_dir.empty())
					{
						std::cout << "  " << name << ": transcribing DTM50...\n";
						transcribe_dtm50(inout_param(pool), ps, opt.dtm50_dir, opt.wdl_dir, opt.to);
					}
				}
				// The thread-local block caches hold strong references the pool's LRU
				// cannot reclaim, so a finished material stays resident until later
				// fetches cycle it out. Respawning drops them with the threads.
				pool.respawn_all_threads();
				std::cout << "  " << name << " done in "
						<< format_elapsed_time(t_start, std::chrono::steady_clock::now()) << "\n";
			}
			catch (const std::exception& e)
			{
				std::cerr << name << ": " << e.what() << "\n";
				++failed;
			}
		}

		if (failed != 0)
			std::cerr << failed << " of " << opt.materials.size() << " materials failed\n";
		return failed == 0 ? 0 : 1;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
