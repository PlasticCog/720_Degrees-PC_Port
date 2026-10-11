// The main CPU's code entered at T.r[7] (a call, jump, return or interrupt vector), with the
// translation linked in: the translated function for that address in the fixed ROM or the
// selected bank.  The translated functions of rewritten routines run the rewrite instead
// (logic::enter).  The build without the translation uses dispatch_native.cpp.
#include "t11rt.h"
#include "board.h"

namespace t11rt {

void dispatch()
{
	const uint16_t pc = T.r[7];
	const T11CodeSeg *seg = nullptr;
	if (pc >= 0x8000) seg = &t11_seg_fixed;
	else if (pc >= 0x4000)
	{
		const int w = (pc >> 13) & 1;
		const int e = B->bank_entry(w);
		seg = (e >= 0 && e < 64) ? t11_seg_bank[w][e] : nullptr;
	}
	if (!seg || (pc & 1)) fatal_unknown_pc(pc);
	const unsigned idx = (unsigned)(pc - seg->base) >> 1;
	if (idx >= seg->size / 2u || !seg->fn[idx]) fatal_unknown_pc(pc);
	seg->fn[idx]();
}

} // namespace t11rt
