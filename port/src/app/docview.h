// A small Markdown viewer for ImGui, for the documents in docs/ that the game shows (debug menu,
// "Tricks" tab).  It understands the subset those documents use:
//   # title, ## sections (collapsible), ### subheadings, paragraphs, - bullets (nested by two
//   spaces), 1. numbered items, ``` code blocks ```, | tables |, > notes;
//   **bold**, `code` and [links](url) are shown as plain text (links as their text).
// A filter box shows only the sections containing the typed text.
#pragma once
#include <string>
#include <vector>

class DocView {
public:
	void load(const std::string &markdown);
	bool empty() const { return m_sections.empty() && m_intro.empty(); }
	const std::string &title() const { return m_title; }
	void draw(const char *id);            // into the current window (id: unique per window)

private:
	struct Block {
		enum Kind { Para, Bullet, Number, Code, Table, Heading, Note } kind = Para;
		int level = 0;                    // list nesting
		int number = 0;                   // numbered item
		std::string text;
		std::vector<std::string> lines;   // code
		std::vector<std::vector<std::string>> rows;   // table (row 0 = header)
	};
	struct Section {
		std::string title;
		std::vector<Block> blocks;
		std::string lower;                // all text in lower case, for the filter
	};
	std::string m_title;
	std::vector<Block> m_intro;           // before the first section
	std::vector<Section> m_sections;
	char m_filter[64] = "";
	int m_expand = 0;                     // 1 expand all, -1 collapse all (next frame)

	static void draw_blocks(const std::vector<Block> &blocks, int id);
};
