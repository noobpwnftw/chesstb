// Standalone reference generator: solves materials and prints what it found.
//
//   ./refgen [-t N] [--verify] [--no-dtm] [--no-dtm50] [--no-dtc] MATERIAL...
//   ./refgen -t 16 --verify KQK KRK KPK KRKR
//
// For each material: legal positions per side, the five WDL classes, the
// longest DTZ and DTM wins with a position for each, and for DTM50 and DTC the
// number of positions whose value changes across halfmove clocks or budgets.

#include "refgen.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace ref;

namespace {

struct Stats_Sink : Sink
{
	std::string target;
	uint64_t dtm50_changes = 0;
	std::vector<uint16_t> dtm50_prev;
	std::vector<uint64_t> dtm50_prev_slices;
	uint64_t dtc_changes = 0;
	std::vector<uint8_t> dtc_prev;

	void on_log(const std::string& line) override { std::printf("%s\n", line.c_str()); }

	void on_table(const Table& t) override
	{
		uint64_t classes[5] = {};
		uint16_t longest_dtz = 0, longest_dtm = 0;
		uint64_t dtz_at = 0, dtm_at = 0;
		for (uint64_t e = 0; e < t.layout.size(); ++e)
		{
			const Wdl w = t.wdl[e];
			if (w == Wdl::INVALID) continue;
			++classes[static_cast<int>(w)];
			if ((w == Wdl::WIN || w == Wdl::CURSED_WIN) && t.dtz[e] > longest_dtz) { longest_dtz = t.dtz[e]; dtz_at = e; }
			if (t.dtm.empty()) continue;
			const Outcome m = unpack(t.dtm[e]);
			if (m.kind == Outcome::WIN && m.plies > longest_dtm) { longest_dtm = m.plies; dtm_at = e; }
		}
		std::printf("%s: win %llu, cursed win %llu, draw %llu, blessed loss %llu, loss %llu\n",
			t.layout.material.name().c_str(),
			static_cast<unsigned long long>(classes[4]), static_cast<unsigned long long>(classes[3]),
			static_cast<unsigned long long>(classes[2]), static_cast<unsigned long long>(classes[1]),
			static_cast<unsigned long long>(classes[0]));
		Board b;
		if (longest_dtz && t.layout.decode(dtz_at, b))
			std::printf("  longest DTZ win %u plies: %s\n", longest_dtz, b.fen().c_str());
		if (longest_dtm && t.layout.decode(dtm_at, b))
			std::printf("  longest DTM win %u plies: %s\n", longest_dtm, b.fen().c_str());
	}

	void on_dtm50_layer(const Table& t, int hmc, const std::vector<uint64_t>& slices,
	                    const std::vector<uint16_t>& layer) override
	{
		if (hmc == HMC_LAYERS - 1 || dtm50_prev_slices != slices)
		{
			dtm50_prev = layer;
			dtm50_prev_slices = slices;
		}
		else
		{
			const uint64_t per = t.layout.slice_entries();
			for (size_t i = 0; i < layer.size(); ++i)
				if (t.wdl[slices[i / per] * per + i % per] != Wdl::INVALID && layer[i] != dtm50_prev[i]) ++dtm50_changes;
			dtm50_prev = layer;
		}
		if (hmc == 0 && slices == std::vector<uint64_t>{})
			std::printf("  DTM50: no positions\n");
	}

	void on_dtc_budget(const Table& t, int budget, const std::vector<uint8_t>& layer) override
	{
		if (budget > 0)
			for (uint64_t e = 0; e < layer.size(); ++e)
				if (t.wdl[e] != Wdl::INVALID && layer[e] != dtc_prev[e]) ++dtc_changes;
		dtc_prev = layer;
	}
};

}  // namespace

int main(int argc, char** argv)
{
	Generator_Options opt;
	std::vector<std::string> names;
	for (int i = 1; i < argc; ++i)
	{
		const std::string a = argv[i];
		if (a == "-t" && i + 1 < argc) opt.threads = std::atoi(argv[++i]);
		else if (a == "--verify") opt.verify = true;
		else if (a == "--no-dtm") opt.dtm = false;
		else if (a == "--no-dtm50") opt.dtm50 = false;
		else if (a == "--no-dtc") opt.dtc = false;
		else if (!a.empty() && a[0] != '-') names.push_back(a);
		else
		{
			std::fprintf(stderr, "usage: %s [-t N] [--verify] [--no-dtm] [--no-dtm50] [--no-dtc] MATERIAL...\n", argv[0]);
			return 2;
		}
	}
	if (names.empty())
	{
		std::fprintf(stderr, "no materials given\n");
		return 2;
	}

	Generator gen(opt);
	for (const std::string& name : names)
	{
		Material m;
		std::string err;
		if (!Material::parse(name, m, &err))
		{
			std::fprintf(stderr, "%s\n", err.c_str());
			return 2;
		}
		Stats_Sink sink;
		if (!gen.build(m, sink, false, &err))
		{
			std::fprintf(stderr, "%s\n", err.c_str());
			return 1;
		}
		if (opt.dtm50) std::printf("  DTM50: %llu value changes from one halfmove clock to the next\n",
			static_cast<unsigned long long>(sink.dtm50_changes));
		if (opt.dtc && m.has_pawns()) std::printf("  DTC: %llu value changes from one budget to the next\n",
			static_cast<unsigned long long>(sink.dtc_changes));
	}
	if (opt.verify)
	{
		std::printf("verify: %llu disagreement%s\n", static_cast<unsigned long long>(gen.verify_failures()),
			gen.verify_failures() == 1 ? "" : "s");
		return gen.verify_failures() ? 1 : 0;
	}
	return 0;
}
