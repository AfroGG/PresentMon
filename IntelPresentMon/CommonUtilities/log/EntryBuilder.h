#pragma once
#include "Entry.h"
#include <format>

namespace pmon::util::log
{
	class IEntrySink;

	class EntryBuilder : private Entry
	{
	public:
		EntryBuilder(Level lvl, const wchar_t* sourceFile, const wchar_t* sourceFunctionName, int sourceLine) noexcept;
		template<typename T>
		EntryBuilder& watch(const wchar_t* symbol, const T& value) noexcept
		{
			if (note_.empty()) {
				note_ += std::format(L"     {} = > {}", symbol, value);
			}
			else {
				note_ += std::format(L"\n     {} => {}", symbol, value);
			}
			return *this;
		}
		EntryBuilder& note(std::wstring note = L"") noexcept;
		EntryBuilder& to(IEntrySink*) noexcept;
		//EntryBuilder& trace_skip(int depth);
		//EntryBuilder& no_trace();
		//EntryBuilder& trace();
		EntryBuilder& no_line() noexcept;
		EntryBuilder& line() noexcept;
		EntryBuilder& hr() noexcept;
		EntryBuilder& hr(unsigned int) noexcept;
		EntryBuilder& every(int n, bool includeFirst = true) noexcept;
		EntryBuilder& first(int n) noexcept;
		EntryBuilder& after(int n) noexcept;
		EntryBuilder& hitcount() noexcept;
		~EntryBuilder();
		EntryBuilder& operator<<(std::wstring note) noexcept;
	private:
		IEntrySink* pDest_ = nullptr;
		int traceSkipDepth_ = 6;
	};

}
