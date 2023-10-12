// Copyright 2023 Northern.tech AS
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//
//    Unless required by applicable law or agreed to in writing, software
//    distributed under the License is distributed on an "AS IS" BASIS,
//    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//    See the License for the specific language governing permissions and
//    limitations under the License.

#include <common/cli.hpp>

#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include <common/common.hpp>

namespace mender {
namespace common {
namespace cli {

using namespace std;

namespace common = mender::common;

const size_t max_width = 78;
const string indent = "   ";   // 3 spaces
const string separator = "  "; // 2 spaces

const Option help_option = {
	.long_option = "help",
	.short_option = "h",
	.description = "show help",
	.default_value = "false",
};

template <typename InputIterator>
using ColumnFormatter = function<string(typename iterator_traits<InputIterator>::value_type)>;

template <typename InputIterator>
void PrintInTwoColumns(
	InputIterator start,
	InputIterator end,
	ColumnFormatter<InputIterator> column_one_fmt,
	ColumnFormatter<InputIterator> column_two_fmt,
	ostream &stream) {
	// First pass to calculate the max size for the elements in the first column
	size_t column_one_size = 0;
	for (auto it = start; it != end; ++it) {
		if (column_one_fmt(*it).size() > column_one_size) {
			column_one_size = column_one_fmt(*it).size();
		}
	}

	// The total with will be the size of the largest element + indent + separator
	const size_t column_one_width {column_one_size + indent.size() + separator.size()};
	// The second column takes the rest of the available width
	const size_t column_two_width {max_width - column_one_width};
	for (auto it = start; it != end; ++it) {
		stream << indent << setw(column_one_size) << left << column_one_fmt(*it) << separator;
		// Wrap around and align the text for the second column
		auto lines = common::JoinStringsMaxWidth(
			common::SplitString(column_two_fmt(*it), " "), " ", column_two_width);
		stream << lines.front() << endl;
		for_each(lines.begin() + 1, lines.end(), [&stream, column_one_width](const string &l) {
			stream << setw(column_one_width) << left << " " << l << endl;
		});
	}
}

void PrintOptions(const vector<Option> &options, ostream &stream) {
	vector<Option> options_with_help = options;
	options_with_help.push_back(help_option);

	PrintInTwoColumns(
		options_with_help.begin(),
		options_with_help.end(),
		[](const Option &option) {
			// Format: --long-option[ PARAM][, -l[ PARAM]]
			string str = "--" + option.long_option;
			if (option.parameter != "") {
				str += " " + option.parameter;
			}
			if (option.short_option != "") {
				str += ", -" + option.short_option;
				if (option.parameter != "") {
					str += " " + option.parameter;
				}
			}
			return str;
		},
		[](const Option &option) {
			// Format: description[ (default: DEFAULT)]
			string str = option.description;
			if (option.default_value != "") {
				str += " (default: " + option.default_value + ")";
			}
			return str;
		},
		stream);
}

void PrintCommandHelp(const string &cli_name, const Command &command, ostream &stream) {
	stream << "NAME:" << endl;
	stream << indent << cli_name << " " << command.name;
	if (command.description != "") {
		stream << " - " << command.description;
	}
	stream << endl << endl;

	stream << "OPTIONS:" << endl;
	PrintOptions(command.options, stream);
}

void PrintCliHelp(const App &cli, ostream &stream) {
	stream << "NAME:" << endl;
	stream << indent << cli.name;
	if (cli.short_description != "") {
		stream << " - " << cli.short_description;
	}
	stream << endl << endl;

	stream << "USAGE:" << endl
		   << indent << cli.name << " [global options] command [command options] [arguments...]";
	stream << endl << endl;

	if (cli.version != "") {
		stream << "VERSION:" << endl << indent << cli.version;
		stream << endl << endl;
	}

	if (cli.long_description != "") {
		stream << "DESCRIPTION:" << endl << indent << cli.long_description;
		stream << endl << endl;
	}

	stream << "COMMANDS:" << endl;
	PrintInTwoColumns(
		cli.commands.begin(),
		cli.commands.end(),
		[](const Command &command) { return command.name; },
		[](const Command &command) { return command.description; },
		stream);
	stream << endl;

	stream << "GLOBAL OPTIONS:" << endl;
	PrintOptions(cli.global_options, stream);
}

void PrintCliCommandHelp(const App &cli, const string &command_name, ostream &stream) {
	auto match_on_name = [command_name](const Command &cmd) { return cmd.name == command_name; };

	auto cmd = std::find_if(cli.commands.begin(), cli.commands.end(), match_on_name);
	if (cmd != cli.commands.end()) {
		PrintCommandHelp(cli.name, *cmd, stream);
	} else {
		PrintCliHelp(cli, stream);
	}
}

} // namespace cli
} // namespace common
} // namespace mender
