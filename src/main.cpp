#include "chess/attack.h"
#include "chess/piece_config.h"

#include "egtb/egtb_gen_dtc.h"
#include "egtb/egtb_gen_dtm.h"
#include "egtb/egtb_gen_dtm50.h"
#include "egtb/egtb_probe.h"

#include "util/endian.h"
#include "util/thread_pool.h"
#include "util/utility.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
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
	std::filesystem::path wdl_dir = "./wdl/";
	std::filesystem::path dtc_dir = "./dtc/";
	std::filesystem::path dtm_dir = "./dtm/";
	std::filesystem::path dtm50_dir = "./dtm50/";
	std::filesystem::path tmp_dir = "./tmp/";
	std::vector<std::filesystem::path> info_paths;
	size_t mem_mib = 0;
	size_t enumerate_up_to = 0;
	bool estimate_only = false;
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
		"  --wdl DIR     WDL output directory (default: ./wdl/)\n"
		"  --dtc DIR     DTC output directory (default: ./dtc/)\n"
		"  --tmp DIR     scratch directory (default: ./tmp/)\n"
		"  --dtm DIR     DTM output directory (default: ./dtm/)\n"
		"  --dtm50 DIR   DTM50 output directory (default: ./dtm50/)\n"
		"  --mem MiB     resident DTC memory cap per material (0 = unbounded)\n"
		"  --builddtm    after DTC, also build DTM for each material\n"
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
		"                accepts shell globs: chesstb --info dtc/*.info\n";
}

static bool parse_args(int argc, char** argv, Options& out)
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
		else if (const char* v = take("-t")) {
			char* end = nullptr;
			const long long n = std::strtoll(v, &end, 10);
			if (end == v || *end != '\0' || n <= 0) {
				std::cerr << "-t needs a positive integer (got \"" << v << "\")\n";
				return false;
			}
			out.num_threads = static_cast<size_t>(n);
		}
		else if (const char* v = take("--wdl"))  out.wdl_dir = v;
		else if (const char* v = take("--dtc"))  out.dtc_dir = v;
		else if (const char* v = take("--dtm"))  out.dtm_dir = v;
		else if (const char* v = take("--dtm50")) out.dtm50_dir = v;
		else if (const char* v = take("--tmp"))  out.tmp_dir = v;
		else if (a == "--builddtm")              out.build_dtm = true;
		else if (a == "--builddtm50")            out.build_dtm50 = true;
		else if (a == "--probe")                 out.probe = true;
		else if (a == "--fleet")                 out.fleet = true;
		else if (const char* v = take("--mem"))  {
			out.mem_mib = std::strtoull(v, nullptr, 10);
			if (out.mem_mib > 0 && out.mem_mib < 64) out.mem_mib = 64;
		}
		else if (const char* v = take("--enumerate")) out.enumerate_up_to = std::strtoull(v, nullptr, 10);
		else if (const char* v = take("--info")) {
			out.info_paths.emplace_back(v);
			// Gobble any subsequent positional paths so shell globs like
			// `--info dtc/*.info` expand cleanly without bumping into the
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

static size_t estimate_dtc_ram_bytes(const Piece_Config& ps)
{
	Piece_Config_For_Gen epsi(ps);
	const size_t N = epsi.num_positions();
	return static_cast<size_t>(2) * N * sizeof(DTC_Final_Entry);
}

static std::vector<std::string> enumerate_materials(size_t max_pieces)
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
	ordered.remove_if([](const Piece_Config& ps) { return ps.num_pieces() <= 2; });

	std::vector<std::string> out;
	out.reserve(ordered.size());
	for (const Piece_Config& ps : ordered) out.push_back(ps.name());
	return out;
}

static std::vector<std::string> read_list_file(const std::filesystem::path& path)
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

static int dump_info_file(const std::filesystem::path& path)
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
		attack_init();

		Options opt;
		if (!parse_args(argc, argv, opt)) return 1;

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

		if (opt.estimate_only)
		{
			auto fmt_bytes = [](size_t bytes) {
				char buf[64];
				if (bytes >= 1ull << 40)      std::snprintf(buf, sizeof(buf), "%.2f TiB", bytes / static_cast<double>(1ull << 40));
				else if (bytes >= 1ull << 30) std::snprintf(buf, sizeof(buf), "%.2f GiB", bytes / static_cast<double>(1ull << 30));
				else if (bytes >= 1ull << 20) std::snprintf(buf, sizeof(buf), "%.2f MiB", bytes / static_cast<double>(1ull << 20));
				else if (bytes >= 1ull << 10) std::snprintf(buf, sizeof(buf), "%.2f KiB", bytes / static_cast<double>(1ull << 10));
				else                          std::snprintf(buf, sizeof(buf), "%zu B", bytes);
				return std::string(buf);
			};
			for (const auto& name : opt.materials)
			{
				if (!Piece_Config::is_constructible_from(name))
				{
					std::cout << name << ": invalid material name\n";
					continue;
				}
				const Piece_Config ps(name);
				if (ps.num_pieces() <= 2 && !ps.has_frozen_pair())
				{
					std::cout << ps.name() << ": <=2 pieces, trivial draw, no table generated\n";
					continue;
				}
				const Working_Set_Estimate w = compute_working_set(ps, opt.build_dtm || opt.build_dtm50);
				std::cout << ps.name() << ":\n";
				std::printf("  positions             : %zu\n", w.num_positions);
				std::printf("  total table (resident): %s  (both colors)\n", fmt_bytes(w.total_table_bytes).c_str());
				std::printf("  bytes per slice       : %s  (within=%zu cells x 2 B)\n",
					fmt_bytes(w.bytes_per_slice).c_str(),
					w.bytes_per_slice / sizeof(DTC_Final_Entry));
				std::printf("  slices per group      : %zu\n", w.slices_per_group);
				std::printf("  bytes per group       : %s\n", fmt_bytes(w.bytes_per_group).c_str());
				std::printf("  num slices / groups   : %zu / %zu\n", w.num_slices, w.num_groups);
				std::printf("  iter per-dispatch     : %zu groups = %s  (minimum)\n",
					w.peak_per_group_iter_groups,
					fmt_bytes(w.peak_per_group_iter_groups * w.bytes_per_group).c_str());
				std::printf("  iter per-pair_sid     : %zu groups = %s  (cache fits one pair_sid trajectory)\n",
					w.peak_pair_iter_groups,
					fmt_bytes(w.peak_pair_iter_groups * w.bytes_per_group).c_str());
				std::printf("  iter per-batch        : %zu groups = %s  (unbounded fusion, --mem 0 ceiling)\n",
					w.peak_batch_iter_groups,
					fmt_bytes(w.peak_batch_iter_groups * w.bytes_per_group).c_str());
				std::printf("  init per-dispatch     : %zu groups = %s\n",
					w.peak_per_group_init_groups,
					fmt_bytes(w.peak_per_group_init_groups * w.bytes_per_group).c_str());
				std::printf("  init per-pair_sid     : %zu groups = %s\n",
					w.peak_pair_init_groups,
					fmt_bytes(w.peak_pair_init_groups * w.bytes_per_group).c_str());
				std::printf("  init per-batch        : %zu groups = %s\n",
					w.peak_batch_init_groups,
					fmt_bytes(w.peak_batch_init_groups * w.bytes_per_group).c_str());
				if (opt.build_dtm50)
				{
					const size_t floor_groups =
						std::max<size_t>(DTM50_HMC_COUNT, 2 * w.peak_per_group_iter_groups);
					std::printf("  dtm50 minimum         : %zu groups = %s\n",
						floor_groups, fmt_bytes(floor_groups * w.bytes_per_group).c_str());
				}
				std::cout << "\n";
			}
			return 0;
		}

		EGTB_Paths paths;
		paths.add_wdl_path(opt.wdl_dir);
		paths.add_dtc_path(opt.dtc_dir);
		paths.add_dtm_path(opt.dtm_dir);
		paths.add_dtm50_path(opt.dtm50_dir);
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
			return ps.num_pieces() <= 2 && !ps.has_frozen_pair();
		});

		const size_t budget_bytes = opt.mem_mib * 1024ull * 1024ull;
		std::cout << closured.size() << " piece configurations in plan"
				<< (opt.mem_mib > 0 ? " (--mem " + std::to_string(opt.mem_mib) + " MiB)" : "")
				<< ((opt.build_dtm || opt.build_dtm50) ? " (+dtm)" : "")
				<< (opt.build_dtm50 ? " (+dtm50)" : "")
				<< ":\n";
		for (const Piece_Config& ps : closured)
		{
			const bool has_wdl = paths.find_wdl_file(ps);
			const bool has_dtc = paths.find_dtc_file(ps);
			const bool has_dtm = (opt.build_dtm || opt.build_dtm50) ? paths.find_dtm_file(ps) : true;
			const bool has_dtm50 = opt.build_dtm50 ? paths.find_dtm50_file(ps) : true;
			const size_t est = estimate_dtc_ram_bytes(ps);
			std::string line = "  " + ps.name();
			line.resize(10, ' ');
			line += "  WDL "; line += has_wdl ? '+' : '-';
			line += "  DTC "; line += has_dtc ? '+' : '-';
			if (opt.build_dtm || opt.build_dtm50)   { line += "  DTM ";   line += has_dtm   ? '+' : '-'; }
			if (opt.build_dtm50) { line += "  DTM50 "; line += has_dtm50 ? '+' : '-'; }
			std::printf("%s  ~%6.1f MiB\n", line.c_str(), est / (1024.0 * 1024.0));
		}

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
			if (!paths.find_wdl_file(ps) || !paths.find_dtc_file(ps)) return false;
			if ((opt.build_dtm || opt.build_dtm50) && !paths.find_dtm_file(ps)) return false;
			if (opt.build_dtm50 && !paths.find_dtm50_file(ps)) return false;
			return true;
		};

		auto sub_tables_ready = [&](const Piece_Config& ps, auto&& has_file) {
			for (const auto& [key, sub] : EGTB_Generator::enumerate_sub_materials(ps))
				if (sub.num_pieces() > 2 && !has_file(sub)) return false;
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

					// DTC pass (always; gated by file existence).
					const bool need_wdl = !paths.find_wdl_file(ps);
					const bool need_dtc = !paths.find_dtc_file(ps);
					if (need_wdl || need_dtc)
					{
						std::cout << "[" << idx << "/" << closured.size() << "] " << ps.name()
								<< ": generating DTC...\n";
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
								<< "  (WDL " << std::filesystem::file_size(paths.wdl_save_path(ps)) << " B, "
								<< "DTC " << std::filesystem::file_size(paths.dtc_save_path(ps)) << " B)\n";
					}
					else
					{
						std::cout << "[" << idx << "/" << closured.size() << "] " << ps.name() << ": DTC already on disk.\n";
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
						DTM_Generator g(ps, opt.tmp_dir, budget_bytes);
						try { g.gen(std::move(subs), inout_param(pool), paths); }
						catch (const DTM_Interrupted&) { return 130; }
						g.save_to_disk(inout_param(pool), paths);

						const auto t_end = std::chrono::steady_clock::now();
						std::cout << "  " << ps.name() << " DTM done in " << format_elapsed_time(t_start, t_end)
								<< "  (DTM " << std::filesystem::file_size(paths.dtm_save_path(ps)) << " B)\n";
					}

					// Optional DTM50 pass for this material.
					if (opt.build_dtm50 && !paths.find_dtm50_file(ps))
					{
						std::cout << "  " << ps.name() << ": generating DTM50...\n";
						if (opt.fleet && !sub_tables_ready(ps, [&](const Piece_Config& s) { return paths.find_dtm50_file(s); }))
							throw std::runtime_error("sub tables not ready");
						const auto t_start = std::chrono::steady_clock::now();

						auto subs = opt.probe
							? EGTB_Generator::open_sub_probes<DTM50_File_For_Probe>(ps, paths, inout_param(pool))
							: EGTB_Generator::open_sub_probes<DTM50_Sub_File_Flat>(ps, paths, inout_param(pool));
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
		std::fprintf(stderr, "error: %s\n", e.what());
		return 1;
	}
}
