// The main CPU's code entered at T.r[7] (a call, jump, return or interrupt vector), without
// the translation: every address the program enters is the start of a rewritten routine
// (port/src/logic), which runs natively.  The translated build uses t11dispatch.cpp.
#include "t11rt.h"
#include "logic.h"

namespace t11rt {

void dispatch()
{
	const uint16_t pc = T.r[7];
	if ((pc & 1) || !logic::run_entry(pc)) fatal_unknown_pc(pc);
}

} // namespace t11rt
