#include "chess/attack.h"
#include "chess/piece_config.h"

#include "egtb/egtb_gen_dtc.h"
#include "egtb/egtb_gen_dtz.h"
#include "egtb/egtb_gen_dtm.h"
#include "egtb/egtb_gen_dtm50.h"
#include "egtb/egtb_probe.h"

#include "util/cache.h"
#include "util/endian.h"
#include "util/thread_pool.h"
#include "util/utility.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

struct Options
{
	size_t num_threads = std::max(1u, std::thread::hardware_concurrency());
	std::vector<std::string> materials;
	std::filesystem::path list_file;
	// Extra search directories; the write target is always the cwd folder the
	// path lists are seeded with, so empty means "default only".
	std::vector<std::filesystem::path> wdl_dirs;
	std::vector<std::filesystem::path> dtz_dirs;
	std::vector<std::filesystem::path> dtc_dirs;
	std::vector<std::filesystem::path> dtm_dirs;
	std::vector<std::filesystem::path> dtm50_dirs;
	std::filesystem::path tmp_dir = "./tmp/";
	std::vector<std::filesystem::path> info_paths;
	size_t mem_mib = 0;
	size_t cache_mib = DEFAULT_BLOCK_CACHE_BYTES / (1024 * 1024);
	size_t enumerate_up_to = 0;
	bool estimate_only = false;
	bool build_dtc = false;
	bool build_dtm = false;
	bool build_dtm50 = false;
	bool probe = false;
	bool fleet = false;
};

static void print_usage()
{
	std::cerr <<
		"Usage: chesstb [options]\n"
		"  -r LIST       comma-separated materials (e.g. -r KQK,KRK)\n"
		"  --list FILE   read newline-separated materials from FILE\n"
		"  -t N          worker threads (default: hardware concurrency)\n"
		"  --wdl DIR     extra WDL search dir, repeatable (output: ./wdl/)\n"
		"  --dtz DIR     extra DTZ search dir, repeatable (output: ./dtz/)\n"
		"  --dtc DIR     extra DTC search dir, repeatable (output: ./dtc/)\n"
		"  --tmp DIR     scratch directory (default: ./tmp/)\n"
		"  --dtm DIR     extra DTM search dir, repeatable (output: ./dtm/)\n"
		"  --dtm50 DIR   extra DTM50 search dir, repeatable (output: ./dtm50/)\n"
		"  --mem MiB     resident table cap per material, every pass (0 = unbounded,\n"
		"                values below 64 are raised to 64)\n"
		"  --cache MiB   decoded-block cache budget shared by the sub-table\n"
		"                probe readers (default 64)\n"
		"  --builddtc    after DTZ, also build DTC for each material\n"
		"  --builddtm    after DTZ, also build DTM for each material\n"
		"  --builddtm50  after DTM, also build DTM50 for each material\n"
		"  --probe       read DTM/DTM50 sub-tables via the direct probe reader\n"
		"                instead of flat-decompressed temp files\n"
		"  --fleet       cooperative multi-process mode: take a per-material lock\n"
		"                before generating (skipping materials owned by another\n"
		"                worker) and skip materials whose sub-tables aren't ready\n"
		"                yet instead of aborting\n"
		"  --enumerate N print canonical material names with <= N total pieces\n"
		"  --estimate    print the working-set estimate and exit\n"
		"  --info PATHS  dump one or more EGTB_Info (.info) files and exit\n"
		"                accepts shell globs: chesstb --info dtz/*.info\n";
}

NODISCARD static bool parse_args(int argc, char** argv, Options& out)
{
	for (int i = 1; i < argc; ++i)
	{
		std::string a = argv[i];
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
		else if (const char* v = take("--list")) out.list_file = v;
		else if (const char* v = take("-t"))
			out.num_threads = std::max<size_t>(1, std::strtoull(v, nullptr, 10));
		else if (const char* v = take("--wdl"))  out.wdl_dirs.emplace_back(v);
		else if (const char* v = take("--dtz"))  out.dtz_dirs.emplace_back(v);
		else if (const char* v = take("--dtc"))  out.dtc_dirs.emplace_back(v);
		else if (const char* v = take("--dtm"))  out.dtm_dirs.emplace_back(v);
		else if (const char* v = take("--dtm50")) out.dtm50_dirs.emplace_back(v);
		else if (const char* v = take("--tmp"))  out.tmp_dir = v;
		else if (a == "--builddtc")              out.build_dtc = true;
		else if (a == "--builddtm")              out.build_dtm = true;
		else if (a == "--builddtm50")            out.build_dtm50 = true;
		else if (a == "--probe")                 out.probe = true;
		else if (a == "--fleet")                 out.fleet = true;
		else if (const char* v = take("--mem"))  {
			out.mem_mib = std::strtoull(v, nullptr, 10);
			if (out.mem_mib > 0 && out.mem_mib < 64) out.mem_mib = 64;
		}
		else if (const char* v = take("--cache"))
			out.cache_mib = std::strtoull(v, nullptr, 10);
		else if (const char* v = take("--enumerate")) out.enumerate_up_to = std::strtoull(v, nullptr, 10);
		else if (const char* v = take("--info")) {
			out.info_paths.emplace_back(v);
			// Gobble any subsequent positional paths so shell globs like
			// `--info dtz/*.info` expand cleanly without bumping into the
			// unknown-arg error.
			while (i + 1 < argc && argv[i + 1][0] != '-')
				out.info_paths.emplace_back(argv[++i]);
		}
		else if (a == "--estimate") out.estimate_only = true;
		else if (a == "-h" || a == "--help") { print_usage(); std::exit(0); }
		else { std::cerr << "unknown arg: " << a << "\n"; print_usage(); return false; }
	}
	return true;
}

NODISCARD static std::vector<std::string> enumerate_materials(size_t max_pieces)
{
	const Piece_Type types[] = { QUEEN, ROOK, BISHOP, KNIGHT, PAWN };
	Unique_Piece_Configs seen;

	std::function<void(size_t, size_t, std::vector<Piece_Type>&,
	                   const std::function<void(const std::vector<Piece_Type>&)>&)> enum_ms;
	enum_ms = [&](size_t start, size_t left, std::vector<Piece_Type>& cur,
	              const std::function<void(const std::vector<Piece_Type>&)>& cb) {
		if (left == 0) { cb(cur); return; }
		for (size_t i = start; i < 5; ++i) {
			cur.push_back(types[i]);
			enum_ms(i, left - 1, cur, cb);
			cur.pop_back();
		}
	};

	for (size_t total = 3; total <= max_pieces; ++total)
	{
		const size_t nk = total - 2;
		for (size_t w = 0; w <= nk; ++w)
		{
			const size_t b = nk - w;
			std::vector<Piece_Type> wp;
			enum_ms(0, w, wp, [&](const std::vector<Piece_Type>& w_pieces) {
				std::vector<Piece_Type> bp;
				enum_ms(0, b, bp, [&](const std::vector<Piece_Type>& b_pieces) {
					std::vector<Piece> pcs;
					pcs.reserve(total);
					pcs.push_back(piece_make(WHITE, KING));
					for (auto t : w_pieces) pcs.push_back(piece_make(WHITE, t));
					pcs.push_back(piece_make(BLACK, KING));
					for (auto t : b_pieces) pcs.push_back(piece_make(BLACK, t));
					if (!Piece_Config::is_constructible_from(
					        Const_Span<Piece>(pcs.data(), pcs.size())))
						return;
					seen.add_unique(Piece_Config(Const_Span<Piece>(pcs.data(), pcs.size())));
				});
			});
		}
	}

	Unique_Piece_Configs ordered;
	for (const Piece_Config& ps : seen)
		ps.add_closure_in_dependency_order_to(ordered, true);
	ordered.remove_if([](const Piece_Config& ps) { return ps.is_bare_kings(); });

	std::vector<std::string> out;
	out.reserve(ordered.size());
	for (const Piece_Config& ps : ordered) out.push_back(ps.name());
	return out;
}

NODISCARD static std::vector<std::string> read_list_file(const std::filesystem::path& path)
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

NODISCARD static int dump_info_file(const std::filesystem::path& path)
{
	std::ifstream fp(path, std::ios::binary);
	if (!fp) { std::cerr << "Cannot open " << path << "\n"; return 1; }
	EGTB_Info info;
	fp.read(reinterpret_cast<char*>(&info), sizeof(EGTB_Info));
	if (fp.gcount() != static_cast<std::streamsize>(sizeof(EGTB_Info)))
	{
		std::cerr << "Short read on " << path
			<< " (got " << fp.gcount() << " bytes, expected " << sizeof(EGTB_Info) << ")\n";
		return 1;
	}

	std::cout << path.string() << " (" << sizeof(EGTB_Info) << " bytes)\n";
	for (Color c : { WHITE, BLACK })
	{
		const char* tag = (c == WHITE) ? "WHITE" : "BLACK";
		const uint64_t legal = info.win_cnt[c] + info.draw_cnt[c] + info.lose_cnt[c];
		std::cout << "  " << tag
			<< "  win=" << info.win_cnt[c]
			<< "  draw=" << info.draw_cnt[c]
			<< "  lose=" << info.lose_cnt[c]
			<< "  illegal=" << info.illegal_cnt[c]
			<< "  legal=" << legal << "\n";
		std::cout << "         longest_win=" << info.longest_win[c]
			<< " idx=" << info.longest_idx[c]
			<< " fen=\"" << info.longest_fen[c] << "\"\n";
	}
	return 0;
}

class Fleet_Lock
{
public:
	Fleet_Lock() = default;
	explicit Fleet_Lock(std::filesystem::path path)
		: m_path(std::move(path))
	{
		m_fd = ::open(m_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
		if (m_fd < 0) return;
		if (::flock(m_fd, LOCK_EX | LOCK_NB) != 0) { ::close(m_fd); m_fd = -1; }
	}
	~Fleet_Lock() { release(); }
	Fleet_Lock(Fleet_Lock&& o) noexcept : m_path(std::move(o.m_path)), m_fd(o.m_fd) { o.m_fd = -1; }
	Fleet_Lock& operator=(Fleet_Lock&& o) noexcept
	{
		if (this != &o) { release(); m_path = std::move(o.m_path); m_fd = o.m_fd; o.m_fd = -1; }
		return *this;
	}
	Fleet_Lock(const Fleet_Lock&) = delete;
	Fleet_Lock& operator=(const Fleet_Lock&) = delete;

	NODISCARD bool held() const { return m_fd >= 0; }

	void remove_file()
	{
		if (m_path.empty()) return;
		std::error_code ec;
		std::filesystem::remove(m_path, ec);
	}

private:
	void release()
	{
		if (m_fd < 0) return;
		::flock(m_fd, LOCK_UN);
		::close(m_fd);
		m_fd = -1;
	}
	std::filesystem::path m_path;
	int m_fd = -1;
};

int main(int argc, char** argv)
{
	try {
		if (!is_little_endian()) { std::cerr << "Only little-endian hosts supported.\n"; return 1; }

		Options opt;
		if (!parse_args(argc, argv, opt)) return 1;

		default_block_pool()->set_max_bytes(opt.cache_mib * 1024 * 1024);

		if (!opt.info_paths.empty())
		{
			int rc = 0;
			for (const auto& p : opt.info_paths)
				rc |= dump_info_file(p);
			return rc;
		}

		if (opt.enumerate_up_to > 0)
		{
			for (const auto& name : enumerate_materials(opt.enumerate_up_to))
				std::cout << name << "\n";
			return 0;
		}

		if (!opt.list_file.empty())
		{
			const auto more = read_list_file(opt.list_file);
			opt.materials.insert(opt.materials.end(), more.begin(), more.end());
		}
		if (opt.materials.empty())
		{
			std::cerr << "No materials given. Use -r or --list.\n";
			print_usage();
			return 1;
		}

		// DTC is pawnful-only: with no push to budget it would be a DTZ table with
		// the cursed band thrown away, so there is nothing to write.
		auto wants_dtc = [&](const Piece_Config& ps) {
			return opt.build_dtc && ps.has_pawns();
		};

		if (opt.estimate_only)
		{
			auto fmt_bytes = [](size_t bytes) {
				std::ostringstream os;
				os << std::fixed << std::setprecision(2);
				if (bytes >= 1ull << 40)      os << bytes / static_cast<double>(1ull << 40) << " TiB";
				else if (bytes >= 1ull << 30) os << bytes / static_cast<double>(1ull << 30) << " GiB";
				else if (bytes >= 1ull << 20) os << bytes / static_cast<double>(1ull << 20) << " MiB";
				else if (bytes >= 1ull << 10) os << bytes / static_cast<double>(1ull << 10) << " KiB";
				else                          os << bytes << " B";
				return os.str();
			};
			for (const auto& name : opt.materials)
			{
				if (!Piece_Config::is_constructible_from(name))
				{
					std::cout << name << ": invalid material name\n";
					continue;
				}
				const Piece_Config ps(name);
				if (ps.is_bare_kings())
				{
					std::cout << ps.name() << ": <=2 pieces, trivial draw, no table generated\n";
					continue;
				}
				const Working_Set_Estimate w = compute_working_set(ps);
				std::cout << ps.name() << ":\n";
				std::cout << "  positions             : " << w.num_positions << "\n";
				std::cout << "  total table (resident): " << fmt_bytes(w.total_table_bytes) << "  (both colors)\n";
				std::cout << "  bytes per slice       : " << fmt_bytes(w.bytes_per_slice)
					<< "  (within=" << w.bytes_per_slice / sizeof(DTZ_Final_Entry) << " cells x 2 B)\n";
				std::cout << "  slices per group      : " << w.slices_per_group << "\n";
				std::cout << "  bytes per group       : " << fmt_bytes(w.bytes_per_group) << "\n";
				std::cout << "  num slices / groups   : " << w.num_slices << " / " << w.num_groups << "\n";
				std::cout << "  iter per-dispatch     : " << w.peak_dispatch_iter_groups << " groups = "
					<< fmt_bytes(w.peak_dispatch_iter_groups * w.bytes_per_group) << "  (minimum)\n";
				std::cout << "  iter per-mirror-pair  : " << w.peak_pair_iter_groups << " groups = "
					<< fmt_bytes(w.peak_pair_iter_groups * w.bytes_per_group)
					<< "  (cache fits one mirror pair's trajectory)\n";
				std::cout << "  iter per-batch        : " << w.peak_batch_iter_groups << " groups = "
					<< fmt_bytes(w.peak_batch_iter_groups * w.bytes_per_group)
					<< "  (unbounded fusion, --mem 0 ceiling)\n";
				std::cout << "  init per-dispatch     : " << w.peak_dispatch_init_groups << " groups = "
					<< fmt_bytes(w.peak_dispatch_init_groups * w.bytes_per_group) << "\n";
				std::cout << "  init per-mirror-pair  : " << w.peak_pair_init_groups << " groups = "
					<< fmt_bytes(w.peak_pair_init_groups * w.bytes_per_group) << "\n";
				std::cout << "  init per-batch        : " << w.peak_batch_init_groups << " groups = "
					<< fmt_bytes(w.peak_batch_init_groups * w.bytes_per_group) << "\n";
				if (wants_dtc(ps))
				{
					// Tightest page_in tier in egtb_gen_dtc.cpp, over the peaks above.
					// Init consults push targets at the budget below as well as its own,
					// so its whole need-set is resident twice; iterate stays inside one
					// budget and pages like DTZ.
					const size_t init_g = 2 * w.peak_dispatch_init_groups;
					const size_t iter_g = w.peak_dispatch_iter_groups;
					const size_t floor_g = std::max(init_g, iter_g);
					std::cout << "  dtc per-dispatch      : " << floor_g << " groups = "
						<< fmt_bytes(floor_g * w.bytes_per_group)
						<< "  (" << (init_g >= iter_g ? "init" : "iterate")
						<< " binds, 2 live layers)\n";
				}
				if (opt.build_dtm50)
				{
					// Tightest page_in tier in egtb_gen_dtm50.cpp, over the peaks above:
					// init adds both colors' separate write layer at g; iterate adds me's
					// layer 0 alongside its layer hmc.
					const size_t init_g = 2 + w.peak_dispatch_init_groups;
					const size_t iter_g = 1 + w.peak_dispatch_iter_groups;
					const size_t floor_g = std::max(init_g, iter_g);
					std::cout << "  dtm50 per-dispatch    : " << floor_g << " groups = "
						<< fmt_bytes(floor_g * w.bytes_per_group)
						<< "  (" << (init_g >= iter_g ? "init" : "iterate") << " binds)\n";
				}
				std::cout << "\n";
			}
			return 0;
		}

		EGTB_Paths paths;
		for (const auto& d : opt.wdl_dirs)   paths.add_wdl_path(d);
		for (const auto& d : opt.dtz_dirs)   paths.add_dtz_path(d);
		for (const auto& d : opt.dtc_dirs)   paths.add_dtc_path(d);
		for (const auto& d : opt.dtm_dirs)   paths.add_dtm_path(d);
		for (const auto& d : opt.dtm50_dirs) paths.add_dtm50_path(d);
		paths.set_tmp_path(opt.tmp_dir);
		paths.init_directories();

		Unique_Piece_Configs requested;
		for (const auto& name : opt.materials)
		{
			if (!Piece_Config::is_constructible_from(name))
			{
				std::cerr << "Skipping " << name << ": not a valid piece configuration.\n";
				continue;
			}
			requested.add_unique(Piece_Config(name));
		}

		Unique_Piece_Configs closured;
		for (const Piece_Config& ps : requested)
			ps.add_closure_in_dependency_order_to(closured, true);
		closured.remove_if([](const Piece_Config& ps) {
			return ps.is_bare_kings();
		});

		const size_t budget_bytes = opt.mem_mib * 1024ull * 1024ull;
		std::cout << closured.size() << " piece configurations in plan"
			<< (opt.mem_mib > 0 ? " (--mem " + std::to_string(opt.mem_mib) + " MiB)" : "")
			<< (opt.build_dtc ? " (+dtc)" : "")
			<< ((opt.build_dtm || opt.build_dtm50) ? " (+dtm)" : "")
			<< (opt.build_dtm50 ? " (+dtm50)" : "")
			<< "\n";

		Thread_Pool pool(opt.num_threads);

		{
			struct sigaction sa{};
			sa.sa_handler = [](int) { egtb_request_interrupt(); };
			sigemptyset(&sa.sa_mask);
			sigaction(SIGHUP,  &sa, nullptr);
			sigaction(SIGINT,  &sa, nullptr);
			sigaction(SIGTERM, &sa, nullptr);
		}

		auto is_complete = [&](const Piece_Config& ps) {
			if (!paths.find_wdl_file(ps)) return false;
			if (wants_dtc(ps) && paths.find_dtz_file(ps) && !paths.find_dtc_file(ps)) return false;
			if ((opt.build_dtm || opt.build_dtm50) && !paths.find_dtm_file(ps)) return false;
			if (opt.build_dtm50 && paths.find_dtm_file(ps) && !paths.find_dtm50_file(ps)) return false;
			return true;
		};

		auto sub_tables_ready = [&](const Piece_Config& ps, auto&& has_file) {
			for (const auto& [key, sub] : EGTB_Generator::enumerate_sub_materials(ps))
				if (!sub.is_bare_kings() && !has_file(sub)) return false;
			return true;
		};

		const auto t_total_start = std::chrono::steady_clock::now();
		while (true)
		{
			size_t idx = 0;
			for (const Piece_Config& ps : closured)
			{
				++idx;
				if (egtb_is_interrupt_requested())
				{
					std::cout << "interrupted before " << ps.name() << ".\n";
					return 130;
				}

				try
				{
					Fleet_Lock lock;
					if (opt.fleet)
					{
						lock = Fleet_Lock(opt.tmp_dir / (ps.name() + ".lock"));
						if (!lock.held())
						{
							std::cout << "[" << idx << "/" << closured.size() << "] " << ps.name()
								<< ": locked by another worker, skipping.\n";
							continue;
						}
					}

					// DTZ pass (always; gated by file existence).
					if (!paths.find_wdl_file(ps))
					{
						std::cout << "[" << idx << "/" << closured.size() << "] " << ps.name()
							<< ": generating DTZ...\n";
						if (opt.fleet && !sub_tables_ready(ps, [&](const Piece_Config& s) { return paths.find_wdl_file(s); }))
							throw std::runtime_error("sub tables not ready");
						const auto t_start = std::chrono::steady_clock::now();

						auto subs = EGTB_Generator::open_sub_probes<WDL_File_For_Probe>(ps, paths, inout_param(pool));
						DTZ_Generator g(ps, opt.tmp_dir, budget_bytes);
						try { g.gen(std::move(subs), inout_param(pool), paths); }
						catch (const DTZ_Interrupted&) { return 130; }
						g.save_to_disk(inout_param(pool), paths);

						const auto t_end = std::chrono::steady_clock::now();
						std::cout << "  " << ps.name() << " DTZ done in " << format_elapsed_time(t_start, t_end)
							<< "  (WDL " << std::filesystem::file_size(paths.wdl_save_path(ps)) << " B, "
							<< "DTZ " << std::filesystem::file_size(paths.dtz_save_path(ps)) << " B)\n";
					}
					else
					{
						std::cout << "[" << idx << "/" << closured.size() << "] " << ps.name() << ": DTZ already on disk.\n";
					}

					// Optional DTC pass for this material. Its classes are the ones
					// the DTZ pass already wrote, so it leans on that .lzw and emits
					// only the .lzdtc frame. The pack embeds that pass's DTZ table as
					// its unbounded row, so it waits on one being there.
					if (wants_dtc(ps) && paths.find_dtz_file(ps) && !paths.find_dtc_file(ps))
					{
						std::cout << "  " << ps.name() << ": generating DTC...\n";
						if (opt.fleet && !sub_tables_ready(ps, [&](const Piece_Config& s) { return paths.find_wdl_file(s); }))
							throw std::runtime_error("sub tables not ready");
						const auto t_start = std::chrono::steady_clock::now();

						auto subs = EGTB_Generator::open_sub_probes<WDL_File_For_Probe>(ps, paths, inout_param(pool));
						DTC_Generator g(ps, opt.tmp_dir, budget_bytes);
						try { g.gen(std::move(subs), inout_param(pool), paths); }
						catch (const DTC_Interrupted&) { return 130; }
						g.save_to_disk(inout_param(pool), paths);

						const auto t_end = std::chrono::steady_clock::now();
						std::cout << "  " << ps.name() << " DTC done in " << format_elapsed_time(t_start, t_end)
							<< "  (DTC " << std::filesystem::file_size(paths.dtc_save_path(ps)) << " B)\n";
					}

					// Optional DTM pass for this material.
					if ((opt.build_dtm || opt.build_dtm50) && !paths.find_dtm_file(ps))
					{
						std::cout << "  " << ps.name() << ": generating DTM...\n";
						if (opt.fleet && !sub_tables_ready(ps, [&](const Piece_Config& s) { return paths.find_dtm_file(s); }))
							throw std::runtime_error("sub tables not ready");
						const auto t_start = std::chrono::steady_clock::now();

						auto subs = opt.probe
							? EGTB_Generator::open_sub_probes<DTM_File_For_Probe>(ps, paths, inout_param(pool))
							: EGTB_Generator::open_sub_probes<DTM_Sub_File_Flat>(ps, paths, inout_param(pool));
						if (!opt.probe) pool.respawn_all_threads();
						DTM_Generator g(ps, opt.tmp_dir, budget_bytes);
						try { g.gen(std::move(subs), inout_param(pool), paths); }
						catch (const DTM_Interrupted&) { return 130; }
						g.save_to_disk(inout_param(pool), paths);

						const auto t_end = std::chrono::steady_clock::now();
						std::cout << "  " << ps.name() << " DTM done in " << format_elapsed_time(t_start, t_end)
							<< "  (DTM " << std::filesystem::file_size(paths.dtm_save_path(ps)) << " B)\n";
					}

					// Optional DTM50 pass for this material, embedding the DTM
					// table above as its unbounded row.
					if (opt.build_dtm50 && paths.find_dtm_file(ps) && !paths.find_dtm50_file(ps))
					{
						std::cout << "  " << ps.name() << ": generating DTM50...\n";
						if (opt.fleet && !sub_tables_ready(ps, [&](const Piece_Config& s) { return paths.find_dtm50_file(s); }))
							throw std::runtime_error("sub tables not ready");
						const auto t_start = std::chrono::steady_clock::now();

						auto subs = opt.probe
							? EGTB_Generator::open_sub_probes<DTM50_File_For_Probe>(ps, paths, inout_param(pool))
							: EGTB_Generator::open_sub_probes<DTM50_Sub_File_Flat>(ps, paths, inout_param(pool));
						if (!opt.probe) pool.respawn_all_threads();
						DTM50_Generator g(ps, opt.tmp_dir, budget_bytes);
						try { g.gen(std::move(subs), inout_param(pool), paths); }
						catch (const DTM50_Interrupted&) { return 130; }
						g.save_to_disk(inout_param(pool), paths);

						const auto t_end = std::chrono::steady_clock::now();
						std::cout << "  " << ps.name() << " DTM50 done in " << format_elapsed_time(t_start, t_end)
							<< "  (DTM50 " << std::filesystem::file_size(paths.dtm50_save_path(ps)) << " B)\n";
					}
					lock.remove_file();
				}
				catch (const std::runtime_error& e)
				{
					if (!opt.fleet) throw;
					std::cout << "  " << ps.name() << ": skipping (" << e.what() << ")\n";
					continue;
				}
			}
			if (!opt.fleet || std::all_of(closured.begin(), closured.end(), is_complete)) break;
			std::this_thread::sleep_for(std::chrono::seconds(60));
		}
		const auto t_total_end = std::chrono::steady_clock::now();
		std::cout << "All done in " << format_elapsed_time(t_total_start, t_total_end) << ".\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
