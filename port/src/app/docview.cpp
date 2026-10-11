// Markdown viewer, see docview.h.
#include "docview.h"
#include "uiscale.h"
#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace {

std::string trim(const std::string &s)
{
	size_t a = 0, b = s.size();
	while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) a++;
	while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
	return s.substr(a, b - a);
}

// inline markup to plain text: **bold**, `code`, [text](url) -> text, \| -> |
std::string plain(const std::string &s)
{
	std::string o;
	for (size_t i = 0; i < s.size(); i++)
	{
		if (s[i] == '\\' && i + 1 < s.size()) { o += s[++i]; continue; }
		if (s[i] == '`') continue;
		if (s[i] == '*' && i + 1 < s.size() && s[i + 1] == '*') { i++; continue; }
		if (s[i] == '[')
		{
			const size_t close = s.find("](", i);
			const size_t end = close == std::string::npos ? close : s.find(')', close);
			if (close != std::string::npos && end != std::string::npos && s.find('\n', i) > end)
			{
				o += plain(s.substr(i + 1, close - i - 1));
				i = end;
				continue;
			}
		}
		o += s[i];
	}
	return o;
}

std::vector<std::string> table_cells(const std::string &line)
{
	std::vector<std::string> cells;
	std::string t = trim(line), cur;
	if (!t.empty() && t[0] == '|') t.erase(0, 1);
	if (!t.empty() && t.back() == '|' && (t.size() < 2 || t[t.size() - 2] != '\\')) t.pop_back();
	for (size_t i = 0; i < t.size(); i++)
	{
		if (t[i] == '\\' && i + 1 < t.size() && t[i + 1] == '|') { cur += "\\|"; i++; continue; }
		if (t[i] == '|') { cells.push_back(plain(trim(cur))); cur.clear(); continue; }
		cur += t[i];
	}
	cells.push_back(plain(trim(cur)));
	return cells;
}

bool is_table_rule(const std::string &line)
{
	const std::string t = trim(line);
	if (t.empty() || t[0] != '|') return false;
	for (char c : t)
		if (c != '|' && c != '-' && c != ':' && c != ' ') return false;
	return true;
}

std::string lower(std::string s)
{
	for (char &c : s) c = (char)std::tolower((unsigned char)c);
	return s;
}

const ImVec4 TITLE_COLOUR = {1.0f, 0.85f, 0.35f, 1.0f}, HEAD_COLOUR = {0.55f, 0.85f, 1.0f, 1.0f},
	CODE_COLOUR = {0.75f, 0.95f, 0.75f, 1.0f}, NOTE_COLOUR = {0.85f, 0.85f, 0.65f, 1.0f};

}

void DocView::load(const std::string &md)
{
	m_title.clear();
	m_intro.clear();
	m_sections.clear();
	std::vector<std::string> lines;
	size_t p = 0;
	while (p <= md.size())
	{
		size_t e = md.find('\n', p);
		if (e == std::string::npos) e = md.size();
		lines.push_back(md.substr(p, e - p));
		if (!lines.back().empty() && lines.back().back() == '\r') lines.back().pop_back();
		p = e + 1;
	}
	std::vector<Block> *out = &m_intro;
	Block para;
	auto flush = [&]() {
		if (!para.text.empty()) { para.text = plain(para.text); out->push_back(para); }
		para = Block();
	};
	for (size_t i = 0; i < lines.size(); i++)
	{
		const std::string &raw = lines[i];
		const std::string t = trim(raw);
		size_t indent = 0;
		while (indent < raw.size() && raw[indent] == ' ') indent++;
		if (t.rfind("```", 0) == 0)
		{
			flush();
			Block b;
			b.kind = Block::Code;
			for (i++; i < lines.size() && trim(lines[i]).rfind("```", 0) != 0; i++) b.lines.push_back(lines[i]);
			out->push_back(b);
			continue;
		}
		if (t.empty()) { flush(); continue; }
		if (t.rfind("# ", 0) == 0 && m_title.empty()) { flush(); m_title = plain(t.substr(2)); continue; }
		if (t.rfind("## ", 0) == 0)
		{
			flush();
			m_sections.push_back({plain(t.substr(3)), {}, {}});
			out = &m_sections.back().blocks;
			continue;
		}
		if (t.rfind("#", 0) == 0)
		{
			flush();
			Block b;
			b.kind = Block::Heading;
			b.text = plain(trim(t.substr(t.find_first_not_of('#'))));
			out->push_back(b);
			continue;
		}
		if (t[0] == '|')
		{
			flush();
			Block b;
			b.kind = Block::Table;
			for (; i < lines.size() && !trim(lines[i]).empty() && trim(lines[i])[0] == '|'; i++)
				if (!is_table_rule(lines[i])) b.rows.push_back(table_cells(lines[i]));
			i--;
			out->push_back(b);
			continue;
		}
		if (t.rfind("> ", 0) == 0 || t == ">")
		{
			if (para.kind != Block::Note) flush();
			para.kind = Block::Note;
			para.text += (para.text.empty() ? "" : " ") + trim(t.substr(1));
			continue;
		}
		if (t.rfind("- ", 0) == 0 || t.rfind("* ", 0) == 0)
		{
			flush();
			para.kind = Block::Bullet;
			para.level = (int)indent / 2;
			para.text = trim(t.substr(2));
			continue;
		}
		size_t d = 0;
		while (d < t.size() && std::isdigit((unsigned char)t[d])) d++;
		if (d > 0 && d + 1 < t.size() && t[d] == '.' && t[d + 1] == ' ')
		{
			flush();
			para.kind = Block::Number;
			para.level = (int)indent / 3;
			para.number = atoi(t.c_str());
			para.text = trim(t.substr(d + 2));
			continue;
		}
		// continuation of a paragraph / list item, or a new paragraph
		para.text += (para.text.empty() ? "" : " ") + t;
	}
	flush();
	for (Section &s : m_sections)
	{
		std::string all = s.title;
		for (const Block &b : s.blocks)
		{
			all += '\n' + b.text;
			for (const auto &l : b.lines) all += '\n' + l;
			for (const auto &r : b.rows)
				for (const auto &c : r) all += '\n' + c;
		}
		s.lower = lower(all);
	}
}

void DocView::draw_blocks(const std::vector<Block> &blocks, int id)
{
	int n = 0;
	for (const Block &b : blocks)
	{
		ImGui::PushID(n++);
		switch (b.kind)
		{
		case Block::Para:
			ImGui::TextWrapped("%s", b.text.c_str());
			ImGui::Spacing();
			break;
		case Block::Heading:
			ImGui::Spacing();
			ImGui::PushStyleColor(ImGuiCol_Text, HEAD_COLOUR);
			ImGui::TextWrapped("%s", b.text.c_str());
			ImGui::PopStyleColor();
			break;
		case Block::Note:
			ImGui::PushStyleColor(ImGuiCol_Text, NOTE_COLOUR);
			ImGui::Indent();
			ImGui::TextWrapped("%s", b.text.c_str());
			ImGui::Unindent();
			ImGui::PopStyleColor();
			ImGui::Spacing();
			break;
		case Block::Bullet:
		case Block::Number:
		{
			const float ind = ImGui::GetStyle().IndentSpacing * b.level;
			if (ind > 0) ImGui::Indent(ind);
			if (b.kind == Block::Bullet) ImGui::Bullet();
			else ImGui::Text("%d.", b.number);
			ImGui::SameLine();
			ImGui::TextWrapped("%s", b.text.c_str());
			if (ind > 0) ImGui::Unindent(ind);
			break;
		}
		case Block::Code:
		{
			ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.07f, 0.09f, 1.0f));
			char cid[32];
			snprintf(cid, sizeof(cid), "code%d_%d", id, n);
			if (ImGui::BeginChild(cid, {0, 0}, ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_HorizontalScrollbar))
			{
				ImGui::PushStyleColor(ImGuiCol_Text, CODE_COLOUR);
				for (const auto &l : b.lines) ImGui::TextUnformatted(l.empty() ? " " : l.c_str());
				ImGui::PopStyleColor();
			}
			ImGui::EndChild();
			ImGui::PopStyleColor();
			ImGui::Spacing();
			break;
		}
		case Block::Table:
		{
			size_t cols = 0;
			for (const auto &r : b.rows) cols = std::max(cols, r.size());
			if (cols == 0 || cols > 16) break;
			char tid[32];
			snprintf(tid, sizeof(tid), "table%d_%d", id, n);
			if (ImGui::BeginTable(tid, (int)cols, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
			{
				for (size_t c = 0; c < cols; c++)
					ImGui::TableSetupColumn(c < b.rows[0].size() ? b.rows[0][c].c_str() : "");
				ImGui::TableHeadersRow();
				for (size_t r = 1; r < b.rows.size(); r++)
				{
					ImGui::TableNextRow();
					for (size_t c = 0; c < cols; c++)
					{
						ImGui::TableSetColumnIndex((int)c);
						if (c < b.rows[r].size()) ImGui::TextWrapped("%s", b.rows[r][c].c_str());
					}
				}
				ImGui::EndTable();
			}
			ImGui::Spacing();
			break;
		}
		}
		ImGui::PopID();
	}
}

void DocView::draw(const char *id)
{
	ImGui::PushID(id);
	if (!m_title.empty())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, TITLE_COLOUR);
		ImGui::TextWrapped("%s", m_title.c_str());
		ImGui::PopStyleColor();
	}
	ImGui::SetNextItemWidth(std::max(ui_px(120), ImGui::GetContentRegionAvail().x - ui_px(190)));
	ImGui::InputTextWithHint("##filter", "search...", m_filter, sizeof(m_filter));
	ImGui::SameLine();
	if (ImGui::SmallButton("Expand all")) m_expand = 1;
	ImGui::SameLine();
	if (ImGui::SmallButton("Collapse")) m_expand = -1;
	ImGui::Separator();
	if (ImGui::BeginChild("doc", {0, 0}, ImGuiChildFlags_None))
	{
		const std::string f = lower(trim(m_filter));
		if (f.empty()) draw_blocks(m_intro, -1);
		int shown = 0;
		for (size_t i = 0; i < m_sections.size(); i++)
		{
			const Section &s = m_sections[i];
			if (!f.empty() && s.lower.find(f) == std::string::npos) continue;
			shown++;
			if (m_expand) ImGui::SetNextItemOpen(m_expand > 0);
			else if (!f.empty()) ImGui::SetNextItemOpen(true);
			else if (i == 0) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
			ImGui::PushStyleColor(ImGuiCol_Text, HEAD_COLOUR);
			const bool open = ImGui::CollapsingHeader((s.title + "##s" + std::to_string(i)).c_str());
			ImGui::PopStyleColor();
			if (open)
			{
				ImGui::Indent(6);
				draw_blocks(s.blocks, (int)i);
				ImGui::Unindent(6);
			}
		}
		if (!f.empty() && !shown) ImGui::TextDisabled("Nothing matches \"%s\".", m_filter);
		m_expand = 0;
	}
	ImGui::EndChild();
	ImGui::PopID();
}
