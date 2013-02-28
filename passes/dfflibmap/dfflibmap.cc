/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Clifford Wolf <clifford@clifford.at>
 *  
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *  
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "kernel/register.h"
#include "kernel/log.h"
#include "libparse.h"
#include <string.h>

using namespace PASS_DFFLIBMAP;

struct cell_mapping {
	std::string cell_name;
	std::map<std::string, char> ports;
};
static std::map<std::string, cell_mapping> cell_mappings;

static void logmap(std::string dff)
{
	if (cell_mappings.count(dff) == 0) {
		log("    unmapped dff cell: %s\n", dff.c_str());
	} else {
		log("    %s %s(", cell_mappings[dff].cell_name.c_str(), dff.substr(1).c_str());
		bool first = true;
		for (auto &port : cell_mappings[dff].ports) {
			char arg[3] = { port.second, 0, 0 };
			if ('a' <= arg[0] && arg[0] <= 'z')
				arg[1] = arg[0] - ('a' - 'A'), arg[0] = '~';
			log("%s.%s(%s)", first ? "" : ", ", port.first.c_str(), arg);
			first = false;
		}
		log(");\n");
	}
}

static void logmap_all()
{
	logmap("$_DFF_N_");
	logmap("$_DFF_P_");
	logmap("$_DFF_NN0_");
	logmap("$_DFF_NN1_");
	logmap("$_DFF_NP0_");
	logmap("$_DFF_NP1_");
	logmap("$_DFF_PN0_");
	logmap("$_DFF_PN1_");
	logmap("$_DFF_PP0_");
	logmap("$_DFF_PP1_");
}

static bool parse_pin(LibertyAst *cell, LibertyAst *attr, std::string &pin_name, bool &pin_pol)
{
	if (cell == NULL || attr == NULL || attr->value.empty())
		return false;
	
	std::string value = attr->value;

	for (size_t pos = value.find_first_of("\" \t"); pos != std::string::npos; pos = value.find_first_of("\" \t"))
		value.erase(pos, 1);

	if (value[value.size()-1] == '\'') {
		pin_name = value.substr(0, value.size()-1);
		pin_pol = false;
	} else {
		pin_name = value;
		pin_pol = true;
	}

	for (auto child : cell->children)
		if (child->id == "pin" && child->args.size() == 1 && child->args[0] == pin_name)
			return true;
	return false;
}

static void find_cell(LibertyAst *ast, std::string cell_type, bool clkpol, bool has_reset, bool rstpol, bool rstval)
{
	LibertyAst *best_cell = NULL;
	std::map<std::string, char> best_cell_ports;
	int best_cell_pins = 0;

	if (ast->id != "library")
		log_error("Format error in liberty file.\n");

	for (auto cell : ast->children)
	{
		if (cell->id != "cell" || cell->args.size() != 1)
			continue;

		LibertyAst *ff = cell->find("ff");
		if (ff == NULL)
			continue;

		std::string cell_clk_pin, cell_rst_pin, cell_next_pin;
		bool cell_clk_pol, cell_rst_pol, cell_next_pol;

		if (!parse_pin(cell, ff->find("clocked_on"), cell_clk_pin, cell_clk_pol) || cell_clk_pol != clkpol)
			continue;
		if (!parse_pin(cell, ff->find("next_state"), cell_next_pin, cell_next_pol))
			continue;
		if (has_reset && rstval == false) {
			if (!parse_pin(cell, ff->find("clear"), cell_rst_pin, cell_rst_pol) || cell_rst_pol != rstpol)
				continue;
		}
		if (has_reset && rstval == true) {
			if (!parse_pin(cell, ff->find("preset"), cell_rst_pin, cell_rst_pol) || cell_rst_pol != rstpol)
				continue;
		}

		std::map<std::string, char> this_cell_ports;
		this_cell_ports[cell_clk_pin] = 'C';
		if (has_reset)
			this_cell_ports[cell_rst_pin] = 'R';
		this_cell_ports[cell_next_pin] = 'D';

		int num_pins = 0;
		bool found_output = false;
		for (auto pin : cell->children)
		{
			if (pin->id != "pin" || pin->args.size() != 1)
				continue;

			LibertyAst *dir = pin->find("direction");
			if (dir == NULL || dir->value == "internal")
				continue;
			num_pins++;

			if (dir->value == "input" && this_cell_ports.count(pin->args[0]) == 0)
				goto continue_cell_loop;

			LibertyAst *func = pin->find("function");
			if (dir->value == "output" && func != NULL) {
				std::string value = func->value;
				for (size_t pos = value.find_first_of("\" \t"); pos != std::string::npos; pos = value.find_first_of("\" \t"))
					value.erase(pos, 1);
				if ((cell_next_pol == true && value == ff->args[0]) || (cell_next_pol == false && value == ff->args[1])) {
					this_cell_ports[pin->args[0]] = 'Q';
					found_output = true;
				}
			}

			if (this_cell_ports.count(pin->args[0]) == 0)
				this_cell_ports[pin->args[0]] = 0;
		}

		if (!found_output || (best_cell != NULL && num_pins > best_cell_pins))
			continue;

		best_cell = cell;
		best_cell_pins = num_pins;
		best_cell_ports.swap(this_cell_ports);
	continue_cell_loop:;
	}

	if (best_cell != NULL) {
		log("  cell %s is a direct match for cell type %s.\n", best_cell->args[0].c_str(), cell_type.substr(1).c_str());
		cell_mappings[cell_type].cell_name = best_cell->args[0];
		cell_mappings[cell_type].ports = best_cell_ports;
	}
}

static bool expand_cellmap_worker(std::string from, std::string to, std::string inv)
{
	if (cell_mappings.count(to) > 0)
		return false;

	log("  create mapping for %s from mapping for %s.\n", to.c_str(), from.c_str());
	cell_mappings[to].cell_name = cell_mappings[from].cell_name;
	cell_mappings[to].ports = cell_mappings[from].ports;

	for (auto &it : cell_mappings[to].ports) {
		if (inv.find(it.second) == std::string::npos)
			continue;
		if ('a' <= it.second && it.second <= 'z')
			it.second -= 'a' - 'A';
		else if ('A' <= it.second && it.second <= 'Z')
			it.second += 'a' - 'A';
	}
	return true;
}

static bool expand_cellmap(std::string pattern, std::string inv)
{
	std::vector<std::pair<std::string, std::string>> from_to_list;
	bool return_status = false;

	for (auto &it : cell_mappings) {
		std::string from = it.first, to = it.first;
		if (from.size() != pattern.size())
			continue;
		for (size_t i = 0; i < from.size(); i++) {
			if (pattern[i] == '*') {
				to[i] = from[i] == 'P' ? 'N' :
					from[i] == 'N' ? 'P' :
					from[i] == '1' ? '0' :
					from[i] == '0' ? '1' : '*';
			} else
			if (pattern[i] != '?' && pattern[i] != from[i])
				goto pattern_failed;
		}
		from_to_list.push_back(std::pair<std::string, std::string>(from, to));
	pattern_failed:;
	}

	for (auto &it : from_to_list)
		return_status = return_status || expand_cellmap_worker(it.first, it.second, inv);
	return return_status;
}

static void dfflibmap(RTLIL::Design *design, RTLIL::Module *module)
{
	log("Mapping DFF cells in module `%s':\n", module->name.c_str());

	std::vector<RTLIL::Cell*> cell_list;
	for (auto &it : module->cells) {
		if (design->selected(module, it.second) && cell_mappings.count(it.second->type) > 0)
			cell_list.push_back(it.second);
	}

	std::map<std::string, int> stats;
	for (auto cell : cell_list) {
		cell_mapping &cm = cell_mappings[cell->type];
		RTLIL::Cell *new_cell = new RTLIL::Cell;
		new_cell->name = cell->name;
		new_cell->type = "\\" + cm.cell_name;
		for (auto &port : cm.ports) {
			RTLIL::SigSpec sig;
			if ('A' <= port.second && port.second <= 'Z') {
				sig = cell->connections[std::string("\\") + port.second];
			} else
			if ('a' <= port.second && port.second <= 'z') {
				sig = cell->connections[std::string("\\") + char(port.second - ('a' - 'A'))];
				RTLIL::Cell *inv_cell = new RTLIL::Cell;
				RTLIL::Wire *inv_wire = new RTLIL::Wire;
				inv_cell->name = stringf("$dfflibmap$inv$%d", RTLIL::autoidx);
				inv_wire->name = stringf("$dfflibmap$sig$%d", RTLIL::autoidx++);
				inv_cell->type = "$_INV_";
				inv_cell->connections[port.second == 'q' ? "\\Y" : "\\A"] = sig;
				sig = RTLIL::SigSpec(inv_wire);
				inv_cell->connections[port.second == 'q' ? "\\A" : "\\Y"] = sig;
				module->cells[inv_cell->name] = inv_cell;
				module->wires[inv_wire->name] = inv_wire;
			}
			new_cell->connections["\\" + port.first] = sig;
		}
		stats[stringf("  mapped %%d %s cells to %s cells.\n", cell->type.c_str(), new_cell->type.c_str())]++;
		module->cells[cell->name] = new_cell;
		delete cell;
	}

	for (auto &stat: stats)
		log(stat.first.c_str(), stat.second);
}

struct DfflibmapPass : public Pass {
	DfflibmapPass() : Pass("dfflibmap", "technology mapping of flip-flops") { }
	virtual void help()
	{
		log("\n");
		log("    dfflibmap -liberty <file> [selection]\n");
		log("\n");
		log("Map internal flip-flop cells to the flip-flop cells in the technology\n");
		log("library specified in the given liberty file.\n");
		log("\n");
		log("This pass may add inverters as needed. Therefore it is recommended to\n");
		log("first run this pass and then map the logic paths to the target technology.\n");
		log("\n");
	}
	virtual void execute(std::vector<std::string> args, RTLIL::Design *design)
	{
		log_header("Executing DFFLIBMAP pass (mapping DFF cells to sequential cells from liberty file).\n");

		std::string liberty_file;

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++)
		{
			std::string arg = args[argidx];
			if (arg == "-liberty" && argidx+1 < args.size()) {
				liberty_file = args[++argidx];
				continue;
			}
			break;
		}
		extra_args(args, argidx, design);

		if (liberty_file.empty())
			log_cmd_error("Missing `-liberty liberty_file' option!\n");

		FILE *f = fopen(liberty_file.c_str(), "r");
		if (f == NULL)
			log_cmd_error("Can't open liberty file `%s': %s\n", liberty_file.c_str(), strerror(errno));
		LibertyParer libparser(f);
		fclose(f);

		find_cell(libparser.ast, "$_DFF_N_", false, false, false, false);
		find_cell(libparser.ast, "$_DFF_P_", true, false, false, false);
		find_cell(libparser.ast, "$_DFF_NN0_", false, true, false, false);
		find_cell(libparser.ast, "$_DFF_NN1_", false, true, false, true);
		find_cell(libparser.ast, "$_DFF_NP0_", false, true, true, false);
		find_cell(libparser.ast, "$_DFF_NP1_", false, true, true, true);
		find_cell(libparser.ast, "$_DFF_PN0_", true, true, false, false);
		find_cell(libparser.ast, "$_DFF_PN1_", true, true, false, true);
		find_cell(libparser.ast, "$_DFF_PP0_", true, true, true, false);
		find_cell(libparser.ast, "$_DFF_PP1_", true, true, true, true);

		bool keep_running = true;
		while (keep_running) {
			keep_running = false;
			keep_running |= expand_cellmap("$_DFF_*_", "C");
			keep_running |= expand_cellmap("$_DFF_*??_", "C");
			keep_running |= expand_cellmap("$_DFF_?*?_", "R");
			keep_running |= expand_cellmap("$_DFF_??*_", "DQ");
		}
 
 		log("  final dff cell mappings:\n");
 		logmap_all();

		for (auto &it : design->modules)
			if (design->selected(it.second))
				dfflibmap(design, it.second);

		cell_mappings.clear();
	}
} DfflibmapPass;
 
