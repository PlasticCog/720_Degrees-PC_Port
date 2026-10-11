// The sound CPU's code entered at S.pc (a call, jump, return or interrupt vector), with the
// translation linked in: the translated function for that address.  The translated functions
// of rewritten routines run the rewrite instead (sound::enter).
#include "s65rt.h"
#include <cstdio>
#include <cstdlib>

namespace s65rt {

void sdispatch()
{
	const uint16_t pc = S.pc;
	if (pc < 0x4000 || !s65_table[pc - 0x4000])
	{
		fprintf(stderr, "FATAL: the sound program reached code that is neither rewritten nor translated at PC=%04X\n", pc);
		fflush(stderr);
		abort();
	}
	s65_table[pc - 0x4000]();
}

} // namespace s65rt
