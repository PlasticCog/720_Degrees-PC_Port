// The sound CPU's code entered at S.pc (a call, jump, return or interrupt vector), without the
// translation: every address the program enters is the start of a rewritten routine or an
// INNER entry (port/src/sound), which runs natively.  The translated build uses s65dispatch.cpp.
#include "s65rt.h"
#include "sound.h"
#include <cstdio>
#include <cstdlib>

namespace s65rt {

void sdispatch()
{
	const uint16_t pc = S.pc;
	if (!sound::run_entry(pc))
	{
		fprintf(stderr, "FATAL: the sound program reached %04X, which is not the start of a rewritten routine\n", pc);
		fflush(stderr);
		abort();
	}
}

} // namespace s65rt
