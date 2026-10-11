// Extras, see extras.h.
#include "extras.h"
#include "board.h"
#include "logic.h"
#include "sprites.h"
#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>

namespace {

struct Setting { const char *name; bool ExtrasSettings::*member; };
const Setting SETTINGS[] = {
	{"dev_pause", &ExtrasSettings::dev_pause}, {"point_flags", &ExtrasSettings::point_flags},
	{"cut_cast", &ExtrasSettings::cut_cast}, {"hex_labels", &ExtrasSettings::hex_labels},
	{"fix_subpixel", &ExtrasSettings::fix_subpixel}, {"fix_roller", &ExtrasSettings::fix_roller},
	{"fix_hazard", &ExtrasSettings::fix_hazard}, {"fix_dir3", &ExtrasSettings::fix_dir3},
};

constexpr size_t MESSAGE_OFFSET = 0x010000 + 2 * 0x8FD6;   // chips 6FH / 6MN interleaved at 0x010000

// collects word patches; a patch whose current word fails the check makes the group unusable
struct Builder {
	const RomSet &roms;
	std::vector<uint8_t> const &main = roms.main;
	std::vector<std::pair<size_t, uint16_t>> words;
	std::vector<std::pair<size_t, uint8_t>> bytes;
	std::string error;

	int64_t at(uint16_t bank_a, uint16_t bank_b, uint16_t addr) const
	{
		return sprites::RomView{&main, bank_a, bank_b}.offset(addr);
	}
	uint16_t word(int64_t o) const
	{
		return o >= 0 && (size_t)o + 1 < main.size() ? (uint16_t)(main[(size_t)o] | (main[(size_t)o + 1] << 8)) : 0xFFFF;
	}
	// replace the word at addr (bank A 0 / bank B 0x0400 view unless given) if it is 'expect'
	void put(uint16_t addr, uint16_t expect, uint16_t value, uint16_t bank_a = 0x0000, uint16_t bank_b = 0x0400)
	{
		const int64_t o = at(bank_a, bank_b, addr);
		if (word(o) != expect)
		{
			if (error.empty())
			{
				char t[96];
				snprintf(t, sizeof(t), "%04X holds %04X instead of %04X", addr, word(o), expect);
				error = t;
			}
			return;
		}
		words.push_back({(size_t)o, value});
	}
	// replace a frame word (any frame id) at addr
	void frame(uint16_t addr, uint16_t value)
	{
		const int64_t o = at(0x0000, 0x0400, addr);
		const uint16_t w = word(o);
		if (w == 0 || w >= 0x4000)
		{
			if (error.empty())
			{
				char t[96];
				snprintf(t, sizeof(t), "%04X holds %04X, not a frame", addr, w);
				error = t;
			}
			return;
		}
		words.push_back({(size_t)o, value});
	}
	void put_byte(uint16_t addr, uint8_t expect, uint8_t value)
	{
		const int64_t o = at(0x0000, 0x0400, addr);
		if (o < 0 || (size_t)o >= main.size() || main[(size_t)o] != expect)
		{
			if (error.empty())
			{
				char t[96];
				snprintf(t, sizeof(t), "%04X holds %02X instead of %02X", addr, o >= 0 ? main[(size_t)o] : 0, expect);
				error = t;
			}
			return;
		}
		bytes.push_back({(size_t)o, value});
	}
};

}

std::string ExtrasSettings::save() const
{
	std::string out = "# 720 Degrees extras (debug menu, Extras tab): 1 = on\n";
	for (const Setting &st : SETTINGS) out += std::string(st.name) + " " + (this->*st.member ? "1" : "0") + "\n";
	return out;
}

void ExtrasSettings::load(const std::string &text)
{
	std::istringstream in(text);
	std::string name;
	int value;
	while (in >> name)
	{
		if (name[0] == '#') { std::getline(in, name); continue; }
		if (!(in >> value)) break;
		for (const Setting &st : SETTINGS)
			if (name == st.name) this->*st.member = value != 0;
	}
}

void Extras::build(int group, const RomSet &roms)
{
	Builder b{roms};
	switch (group)
	{
	case POINT_FLAGS:
		// the hidden points, types 72-82 (50, 100 ... 1000 points): props 0xC0 (bit 6: no sprite
		// pieces, never drawn); list 0x57C6 + 0x18 * k = two markers, then the value's sign frames
		// (0x3474 + 0x10 * k, + 8), ending in the shared tail 0x57BE (markers, then blank).  Made
		// visible: every frame of the list becomes the flag for the value (the 50 keeps its
		// sign), the tail's markers become blank so a collected point simply disappears
		for (int k = 0; k <= 10; k++)
		{
			const uint16_t type = (uint16_t)(72 + k), list = (uint16_t)(0x57C6 + 0x18 * k);
			const uint16_t shown = k ? (uint16_t)(0x265B + 0x0B * (k - 1)) : (uint16_t)0x3474;
			b.put((uint16_t)(0x5F7C + 2 * type), list, list);                 // the type's list (check only)
			for (int i = 0; i <= 10; i++) b.frame((uint16_t)(list + 2 * i), shown);
			b.put_byte((uint16_t)(0x6438 + type), 0xC0, 0x80);                  // with sprite pieces
			if (k) b.put_byte((uint16_t)(0x5C96 + type), 2, 3);                 // 3 pieces for a flag
		}
		b.frame(0x57BE, 0x0000);
		b.frame(0x57C0, 0x0000);
		break;
	case CUT_CAST:
	{
		b.put((uint16_t)(0x5F7C + 2 * 97), 0x5EEC, 0x5EEC);                     // unicyclist
		b.put((uint16_t)(0x5F7C + 2 * 50), 0x5F1C, 0x5F1C);                     // kid on a tricycle
		// the man in suspenders: jogging cycle facing down-left / down-right, standing frames
		const uint16_t jog_l[4] = {0x31BA, 0x31CE, 0x31E2, 0x31F8}, jog_r[4] = {0x31C4, 0x31D8, 0x31ED, 0x3202};
		const uint16_t stand_l = 0x314A, stand_r = 0x3154;
		struct List { uint16_t addr; bool right; int n; };
		const List uni[] = {
			{0x547C, true, 4}, {0x5486, false, 4}, {0x5490, true, 4}, {0x549A, false, 4},   // dirs 2-3, 5-6, 1, 10
			{0x54A4, true, 4}, {0x54AE, false, 4}, {0x54B8, false, 1}, {0x54BC, true, 1},    // 14, 7-9, 11, 13
			{0x54C0, true, 1}};                                                              // 15
		for (const List &l : uni)
			for (int i = 0; i < l.n; i++) b.frame((uint16_t)(l.addr + 2 * i), (l.right ? jog_r : jog_l)[i]);
		b.frame(0x54C8, stand_r);                                              // turning ring: up,
		b.frame(0x54CC, jog_l[0]);                                             // left,
		b.frame(0x54D0, stand_l);                                              // down,
		b.frame(0x54D4, jog_r[0]);                                             // right
		b.frame(0x54E8, 0x3209);                                               // knocked over
		// the businessman: walking cycle and standing frames
		const uint16_t walk_l[4] = {0x323A, 0x3250, 0x3266, 0x327C}, walk_r[4] = {0x3245, 0x325B, 0x3271, 0x3283};
		const uint16_t suit_l = 0x3224, suit_r = 0x322F;
		const List tri[] = {
			{0x55C2, true, 4}, {0x55CC, false, 4}, {0x55D6, false, 4}, {0x55E0, true, 4},    // dirs 2, 6, 10, 14
			{0x55EA, true, 1}, {0x55EE, true, 1}, {0x55F2, false, 1}, {0x55F6, false, 1},    // 1, 3, 5, 7
			{0x55FA, false, 1}, {0x55FE, false, 1}, {0x5602, true, 1}, {0x5606, true, 1}};   // 9, 11, 13, 15
		for (const List &l : tri)
			for (int i = 0; i < l.n; i++) b.frame((uint16_t)(l.addr + 2 * i), (l.right ? walk_r : walk_l)[i]);
		const struct { uint16_t addr; uint16_t frame; } ring[] = {
			{0x560E, suit_r}, {0x5612, suit_l}, {0x5616, suit_l}, {0x561A, suit_l}, {0x561E, suit_r}, {0x5622, suit_r}};
		for (const auto &r : ring) b.frame(r.addr, r.frame);
		const uint16_t stunned[4] = {suit_l, suit_l, suit_r, suit_r};          // knocked over: looks round
		for (int i = 0; i < 4; i++) b.frame((uint16_t)(0x5636 + 2 * i), stunned[i]);
		b.put_byte((uint16_t)(0x6438 + 50), 0x20, 0x10);                      // palette 2 -> 1
		break;
	}
	case ROLLER:
	{
		// stage lists: pointer per stage at 0x6B9E + 2 * s (bank B 0x0400), entries (type,
		// descriptor) up to 0xFFFF; stages 6 and 7 share one
		const sprites::RomView v{&roms.main, 0x0000, 0x0400};
		std::set<uint16_t> seen;
		for (int s = 1; s <= 7; s++)
		{
			uint16_t a = v.w((uint16_t)(0x6B9E + 2 * s));
			for (int n = 0; n < 64 && a >= 0x4000 && v.w(a) != 0xFFFF; n++, a = (uint16_t)(a + 4))
				if (v.w(a) == 51 && v.w((uint16_t)(a + 2)) == 0x02E1 && seen.insert(a).second)
					b.put((uint16_t)(a + 2), 0x02E1, 0x02E5);
		}
		if (seen.empty() && b.error.empty()) b.error = "no stage list holds the missing roller skater";
		break;
	}
	case DIR3:
		b.put(0x6048 + 6, 7088, 7292, 0x0000, 0x2C00);
		b.put(0x6588 + 6, 9737, 9700, 0x0000, 0x2C00);
		break;
	}
	Group &g = m_groups[group];
	g.built = true;
	g.ok = b.error.empty();
	g.patches.clear();
	if (!g.ok)
	{
		static const char *names[GROUPS] = {"point flags", "cut pedestrians", "roller skater fix", "direction 3 fix"};
		problems += std::string(names[group]) + ": the ROM differs from the original (" + b.error + "), not applied\n";
		return;
	}
	for (const auto &[o, w] : b.words)
		g.patches.push_back({o, {roms.main[o], roms.main[o + 1]}, {(uint8_t)w, (uint8_t)(w >> 8)}});
	for (const auto &[o, v] : b.bytes)
		g.patches.push_back({o, {roms.main[o]}, {v}});
}

void Extras::set(int group, bool on, RomSet &roms)
{
	Group &g = m_groups[group];
	if (on == g.applied) return;
	if (on && !g.built) build(group, roms);
	if (on && !g.ok) return;
	for (const Patch &p : g.patches)
		memcpy(&roms.main[p.offset], (on ? p.data : p.original).data(), p.data.size());
	g.applied = on;
}

void Extras::apply(RomSet &roms)
{
	set(POINT_FLAGS, s.point_flags, roms);
	set(CUT_CAST, s.cut_cast, roms);
	set(ROLLER, s.fix_roller, roms);
	set(DIR3, s.fix_dir3, roms);
	logic::g_fix_subpixel_carry = s.fix_subpixel;
	logic::g_fix_hazard_bonus = s.fix_hazard;
}

std::string Extras::rom_message(const RomSet &roms, int chip)
{
	std::string t;
	for (size_t i = MESSAGE_OFFSET + (chip & 1); i < roms.main.size() && t.size() < 40; i += 2)
	{
		const uint8_t c = roms.main[i];
		if (c < 0x20 || c >= 0x7F) break;
		t += (char)c;
	}
	return t;
}

std::string Extras::cpu_view(const RomSet &roms)
{
	std::string t;
	for (size_t i = MESSAGE_OFFSET; i < roms.main.size() && t.size() < 46; i++)
	{
		const uint8_t c = roms.main[i];
		if (c < 0x20 || c >= 0x7F) break;
		t += (char)c;
	}
	return t;
}
