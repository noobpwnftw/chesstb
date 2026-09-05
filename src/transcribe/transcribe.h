#pragma once

#include "transcribe/source_tables.h"

#include "egtb/egtb_compress.h"

#include "util/thread_pool.h"

#include <filesystem>

// Re-encode a finished table into another shipping form. Everything the save
// path consumes -- classified cells, histogram, layout -- is recoverable from
// the source, so the output is what a generation run with the same settings
// would have written, without the solve.

struct Transcribe_Options
{
	std::filesystem::path out_dir;
	std::filesystem::path tmp_dir = "./tmp/";
	size_t block_size = 0;
	size_t perm_samples = 0;
	bool loss_only = false;
	bool relaxed = false;
	bool extract_dtz = false;
	bool extract_dtm = false;
};

// The distance metrics also read the WDL companion, which carries the class
// their cells decode against.
void transcribe_wdl(In_Out_Param<Thread_Pool> thread_pool, const Piece_Config& ps,
                    const std::filesystem::path& wdl_dir,
                    const Transcribe_Options& opt);

void transcribe_dtz(In_Out_Param<Thread_Pool> thread_pool, const Piece_Config& ps,
                    const std::filesystem::path& dtz_dir,
                    const std::filesystem::path& wdl_dir,
                    const Transcribe_Options& opt);

void transcribe_dtc(In_Out_Param<Thread_Pool> thread_pool, const Piece_Config& ps,
                    const std::filesystem::path& dtc_dir,
                    const std::filesystem::path& wdl_dir,
                    const Transcribe_Options& opt);

void transcribe_dtm(In_Out_Param<Thread_Pool> thread_pool, const Piece_Config& ps,
                    const std::filesystem::path& dtm_dir,
                    const std::filesystem::path& wdl_dir,
                    const Transcribe_Options& opt);

void transcribe_dtm50(In_Out_Param<Thread_Pool> thread_pool, const Piece_Config& ps,
                      const std::filesystem::path& dtm50_dir,
                      const std::filesystem::path& wdl_dir,
                      const Transcribe_Options& opt);
