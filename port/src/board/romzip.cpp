// ROM zip extraction, see romzip.h.  The inflate decoder follows the structure of
// Mark Adler's puff.c (zlib licence): canonical Huffman tables decoded one bit at a time.
#include "romzip.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace {

namespace fs = std::filesystem;

constexpr int MAXBITS = 15;

struct Huffman {
	uint16_t count[MAXBITS + 1];
	uint16_t symbol[288];
};

class Inflater {
public:
	Inflater(const uint8_t *in, size_t n, std::vector<uint8_t> &out) : m_in(in), m_n(n), m_out(out) {}

	bool run()
	{
		int last;
		do
		{
			last = bits(1);
			const int type = bits(2);
			if (m_err) return false;
			if (type == 0) { if (!stored()) return false; }
			else if (type == 1) { if (!fixed()) return false; }
			else if (type == 2) { if (!dynamic()) return false; }
			else return false;
		} while (!last);
		return !m_err;
	}

private:
	const uint8_t *m_in;
	size_t m_n, m_pos = 0;
	uint32_t m_bitbuf = 0;
	int m_bitcnt = 0;
	bool m_err = false;
	std::vector<uint8_t> &m_out;

	int bits(int need)
	{
		uint32_t val = m_bitbuf;
		while (m_bitcnt < need)
		{
			if (m_pos >= m_n) { m_err = true; return 0; }
			val |= (uint32_t)m_in[m_pos++] << m_bitcnt;
			m_bitcnt += 8;
		}
		m_bitbuf = val >> need;
		m_bitcnt -= need;
		return (int)(val & ((1u << need) - 1));
	}

	bool stored()
	{
		m_bitbuf = 0;
		m_bitcnt = 0;
		if (m_pos + 4 > m_n) return false;
		const unsigned len = m_in[m_pos] | (m_in[m_pos + 1] << 8);
		const unsigned nlen = m_in[m_pos + 2] | (m_in[m_pos + 3] << 8);
		m_pos += 4;
		if (len != (~nlen & 0xffff) || m_pos + len > m_n) return false;
		m_out.insert(m_out.end(), m_in + m_pos, m_in + m_pos + len);
		m_pos += len;
		return true;
	}

	int decode(const Huffman &h)
	{
		int code = 0, first = 0, index = 0;
		for (int len = 1; len <= MAXBITS; len++)
		{
			code |= bits(1);
			const int count = h.count[len];
			if (code - count < first) return h.symbol[index + (code - first)];
			index += count;
			first += count;
			first <<= 1;
			code <<= 1;
		}
		m_err = true;
		return -1;
	}

	static int build(Huffman &h, const uint8_t *length, int n)
	{
		memset(h.count, 0, sizeof(h.count));
		for (int s = 0; s < n; s++) h.count[length[s]]++;
		if (h.count[0] == n) return 0;
		int left = 1;
		for (int len = 1; len <= MAXBITS; len++)
		{
			left <<= 1;
			left -= h.count[len];
			if (left < 0) return left;                        // over-subscribed
		}
		uint16_t offs[MAXBITS + 1];
		offs[1] = 0;
		for (int len = 1; len < MAXBITS; len++) offs[len + 1] = (uint16_t)(offs[len] + h.count[len]);
		for (int s = 0; s < n; s++)
			if (length[s]) h.symbol[offs[length[s]]++] = (uint16_t)s;
		return left;                                          // > 0: incomplete
	}

	bool codes(const Huffman &lencode, const Huffman &distcode)
	{
		static const uint16_t lens[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
			35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
		static const uint8_t lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
		static const uint16_t dists[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
			257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
		static const uint8_t dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
		int symbol;
		do
		{
			symbol = decode(lencode);
			if (m_err || symbol < 0) return false;
			if (symbol < 256) m_out.push_back((uint8_t)symbol);
			else if (symbol > 256)
			{
				symbol -= 257;
				if (symbol >= 29) return false;
				const int len = lens[symbol] + bits(lext[symbol]);
				const int ds = decode(distcode);
				if (m_err || ds < 0 || ds >= 30) return false;
				const size_t dist = dists[ds] + (size_t)bits(dext[ds]);
				if (m_err || dist > m_out.size()) return false;
				const size_t from = m_out.size() - dist;
				for (int i = 0; i < len; i++) m_out.push_back(m_out[from + i]);
			}
		} while (symbol != 256);
		return true;
	}

	bool fixed()
	{
		static Huffman lencode, distcode;
		static bool ready = false;
		if (!ready)
		{
			uint8_t lengths[288];
			int s = 0;
			for (; s < 144; s++) lengths[s] = 8;
			for (; s < 256; s++) lengths[s] = 9;
			for (; s < 280; s++) lengths[s] = 7;
			for (; s < 288; s++) lengths[s] = 8;
			build(lencode, lengths, 288);
			for (s = 0; s < 30; s++) lengths[s] = 5;
			build(distcode, lengths, 30);
			ready = true;
		}
		return codes(lencode, distcode);
	}

	bool dynamic()
	{
		static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
		const int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
		if (m_err || nlen > 286 || ndist > 30) return false;
		uint8_t lengths[320] = {};
		int index = 0;
		for (; index < ncode; index++) lengths[order[index]] = (uint8_t)bits(3);
		Huffman lencode, distcode;
		if (build(lencode, lengths, 19) != 0) return false;      // must be complete
		index = 0;
		while (index < nlen + ndist)
		{
			int symbol = decode(lencode);
			if (m_err || symbol < 0) return false;
			if (symbol < 16) lengths[index++] = (uint8_t)symbol;
			else
			{
				uint8_t len = 0;
				if (symbol == 16)
				{
					if (index == 0) return false;
					len = lengths[index - 1];
					symbol = 3 + bits(2);
				}
				else if (symbol == 17) symbol = 3 + bits(3);
				else symbol = 11 + bits(7);
				if (index + symbol > nlen + ndist) return false;
				while (symbol--) lengths[index++] = len;
			}
		}
		if (lengths[256] == 0) return false;
		const int el = build(lencode, lengths, nlen);
		if (el < 0 || (el > 0 && nlen - lencode.count[0] != 1)) return false;
		const int ed = build(distcode, lengths + nlen, ndist);
		if (ed < 0 || (ed > 0 && ndist - distcode.count[0] != 1)) return false;
		return codes(lencode, distcode);
	}
};

uint32_t crc32(const uint8_t *p, size_t n)
{
	static uint32_t table[256];
	static bool ready = false;
	if (!ready)
	{
		for (uint32_t i = 0; i < 256; i++)
		{
			uint32_t c = i;
			for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
		ready = true;
	}
	uint32_t c = 0xffffffffu;
	for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xff] ^ (c >> 8);
	return c ^ 0xffffffffu;
}

uint16_t r16(const std::vector<uint8_t> &d, size_t o) { return (uint16_t)(d[o] | (d[o + 1] << 8)); }
uint32_t r32(const std::vector<uint8_t> &d, size_t o) { return d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24); }

bool is_rom_name(const std::string &base)
{
	return base.rfind("136047-", 0) == 0 || base == "720-eeprom.bin";
}

struct ZipEntry { std::string name; uint16_t method; uint32_t crc, csize, usize, local; };

bool read_zip_directory(const std::vector<uint8_t> &d, std::vector<ZipEntry> &entries)
{
	if (d.size() < 22) return false;
	size_t eocd = std::string::npos;
	for (size_t i = d.size() - 22 + 1; i-- > 0 && d.size() - i <= 22 + 65535;)
		if (r32(d, i) == 0x06054b50) { eocd = i; break; }
	if (eocd == std::string::npos) return false;
	const unsigned count = r16(d, eocd + 10);
	size_t p = r32(d, eocd + 16);
	for (unsigned k = 0; k < count; k++)
	{
		if (p + 46 > d.size() || r32(d, p) != 0x02014b50) return false;
		ZipEntry e;
		e.method = r16(d, p + 10);
		e.crc = r32(d, p + 16);
		e.csize = r32(d, p + 20);
		e.usize = r32(d, p + 24);
		const unsigned nlen = r16(d, p + 28), xlen = r16(d, p + 30), clen = r16(d, p + 32);
		e.local = r32(d, p + 42);
		if (p + 46 + nlen > d.size()) return false;
		e.name.assign((const char *)&d[p + 46], nlen);
		entries.push_back(e);
		p += 46 + nlen + xlen + clen;
	}
	return true;
}

bool read_all(const fs::path &path, std::vector<uint8_t> &d)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) return false;
	d.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	return true;
}

std::vector<fs::path> zips_in(const std::string &dir)
{
	std::vector<fs::path> out;
	std::error_code ec;
	if (!fs::is_directory(dir, ec)) return out;
	for (const auto &e : fs::directory_iterator(dir, ec))
	{
		std::string ext = e.path().extension().string();
		for (char &c : ext) c = (char)tolower((unsigned char)c);
		if (ext == ".zip") out.push_back(e.path());
	}
	std::sort(out.begin(), out.end());
	return out;
}

}

bool inflate_raw(const uint8_t *in, size_t n, std::vector<uint8_t> &out, size_t expected_size)
{
	out.clear();
	out.reserve(expected_size);
	Inflater inf(in, n, out);
	return inf.run();
}

int extract_rom_zips(const std::string &dir, std::string &log)
{
	int written = 0;
	for (const auto &zp : zips_in(dir))
	{
		std::vector<uint8_t> d;
		std::vector<ZipEntry> entries;
		if (!read_all(zp, d) || !read_zip_directory(d, entries))
		{
			log += zp.filename().string() + ": not a readable zip file\n";
			continue;
		}
		for (const auto &e : entries)
		{
			const std::string base = fs::path(e.name).filename().string();
			if (base.empty() || !is_rom_name(base)) continue;
			const fs::path target = fs::path(dir) / base;
			std::error_code ec;
			if (fs::exists(target, ec)) continue;
			if (e.local + 30 > d.size() || r32(d, e.local) != 0x04034b50) { log += base + ": bad zip entry\n"; continue; }
			const size_t data = e.local + 30 + r16(d, e.local + 26) + r16(d, e.local + 28);
			if (data + e.csize > d.size()) { log += base + ": truncated\n"; continue; }
			std::vector<uint8_t> out;
			if (e.method == 0) out.assign(d.begin() + data, d.begin() + data + e.csize);
			else if (e.method == 8)
			{
				if (!inflate_raw(&d[data], e.csize, out, e.usize)) { log += base + ": cannot decompress\n"; continue; }
			}
			else { log += base + ": unsupported compression method " + std::to_string(e.method) + "\n"; continue; }
			if (out.size() != e.usize || crc32(out.data(), out.size()) != e.crc) { log += base + ": checksum mismatch\n"; continue; }
			std::ofstream f(target, std::ios::binary);
			if (!f.write((const char *)out.data(), (std::streamsize)out.size())) { log += base + ": cannot write\n"; continue; }
			written++;
		}
	}
	return written;
}

bool rom_folder_usable(const std::string &dir)
{
	std::error_code ec;
	if (fs::exists(fs::path(dir) / "136047-3126.7lm", ec)) return true;
	for (const auto &zp : zips_in(dir))
	{
		std::vector<uint8_t> d;
		std::vector<ZipEntry> entries;
		if (!read_all(zp, d) || !read_zip_directory(d, entries)) continue;
		for (const auto &e : entries)
			if (fs::path(e.name).filename().string() == "136047-3126.7lm") return true;
	}
	return false;
}
