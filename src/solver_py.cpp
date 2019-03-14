// Include-Gardener
//
// Copyright (C) 2019  Christian Haettich [feddischson]
//
// This program is free software; you can redistribute it
// and/or modify it under the terms of the GNU General Public
// License as published by the Free Software Foundation;
// either version 3 of the License, or (at your option)
// any later version.
//
// This program is distributed in the hope that it will
// be useful, but WITHOUT ANY WARRANTY; without even the
// implied warranty of MERCHANTABILITY or FITNESS FOR A
// PARTICULAR PURPOSE. See the GNU General Public License
// for more details.
//
// You should have received a copy of the GNU General
// Public License along with this program; if not, see
// <http://www.gnu.org/licenses/>.
//
#include "solver_py.h"

#include <string>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/range/algorithm_ext/erase.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/find.hpp>

namespace INCLUDE_GARDENER {

namespace po = boost::program_options;
using std::mutex;
using std::string;
using std::unique_lock;
using std::vector;

const int py_import_regex_idx = 0;
const int py_from_import_regex_idx = 1;

vector<string> Solver_Py::get_statement_regex() const {
  vector<string> regex_str = {R"(^[ \t]*import[ \t]+([^\d\W](?:[\w,\.])*)[ \t]*$)",
                             R"(^[ \t]*from[ \t]+([^\d\W](?:[\w\.]*)[ \t]+import[ \t]+(?:\*|[^\d\W](?:[\w,\. ]*)))[ \t]*$)"};
  return regex_str;
}

string Solver_Py::get_file_regex() const { return string(R"(^(?:.*[\/\\])?[^\d\W]\w*\.py[3w]?$)"); }

void Solver_Py::add_options(po::options_description *options __attribute__((unused))) {

}

void Solver_Py::extract_options(const po::variables_map &vm __attribute__((unused))) {

}

void Solver_Py::add_edge(const string &src_path,
                         const string &statement,
                         unsigned int idx,
                         unsigned int line_no) {
  using boost::filesystem::path;
  using boost::filesystem::operator/;

  BOOST_LOG_TRIVIAL(trace) << "add_edge: " << src_path << " -> " << statement
                             << ", idx = " << idx << ", line_no = " << line_no;

  if (boost::contains(statement, "*")) return; // Import * is not supported yet

  // from (x import y)
  if (idx == py_from_import_regex_idx){

    if (boost::contains(statement, ",")){

      std::string before_import = get_first_substring(statement, " import");
      std::string after_import = get_final_substring(statement, "import ");
      vector<string> comma_separated_statements;

      // Commas are not allowed before "import" if it's done "from"
      boost::split(comma_separated_statements, after_import, [](char c){ return c == ','; });

      // Handle each comma-separated part separately with the from-statement
      for (auto comma_separated_statement : comma_separated_statements){
        add_edge(src_path, before_import + comma_separated_statement, 99, line_no);
      }

      return;
    }

  // import (x)
  } else if (idx == py_import_regex_idx){
    if (boost::contains(statement, ",")){
      vector<string> comma_separated_statements;
      boost::split(comma_separated_statements, statement, [](char c){ return c == ','; });
      add_edges(src_path, comma_separated_statements, 99, line_no);
      return;
  }

  std::string import_to_path;
  if (boost::contains(statement, " import ")){
    import_to_path = from_import_statement_to_path(statement);
  } else {
    import_to_path = import_statement_to_path(statement);
  }

  path likely_path = path(src_path).parent_path() / path(import_to_path);
  std::string likely_module_name = likely_path.stem().string();
  path likely_parent_path = likely_path.parent_path();

  unique_lock<mutex> glck(graph_mutex);

  for (const string &file_extension : file_extensions){
    std::string module_with_file_extension = likely_module_name + '.' + file_extension;
    path dst_path = likely_parent_path / module_with_file_extension;

    if (exists(dst_path)){
      dst_path = canonical(dst_path);
      BOOST_LOG_TRIVIAL(trace) << "   |>> Relative Edge";
      insert_edge(src_path, dst_path.string(), import_to_path, line_no);
      return;
    }
  }

  // search in preconfigured list of standard system directories
  for (const auto &i_path : include_paths) {
    path dst_path = i_path / statement;
    if (exists(dst_path)) {
      dst_path = canonical(dst_path);
      BOOST_LOG_TRIVIAL(trace) << "   |>> Absolute Edge";
      insert_edge(src_path, dst_path.string(), statement, line_no);
      return;
    }
  }

  // if none of the cases above found a file:
  // -> add an dummy entry
  insert_edge(src_path, "", statement, line_no); //module_name
  }
}
void Solver_Py::add_edges(const std::string &src_path,
                          const std::vector<std::string> &statements,
                          unsigned int idx,
                          unsigned int line_no){  
  for (auto statement: statements){
    add_edge(src_path, statement, idx, line_no);
  }
}

std::string Solver_Py::dots_to_system_slash(const std::string &statement)
{
  std::string separator(1, boost::filesystem::path::preferred_separator);
  return boost::replace_all_copy(statement, ".", separator);
}

std::string Solver_Py::from_import_statement_to_path(const std::string &statement)
{
  std::string from_field = get_first_substring(statement, " ");
  std::string import_field = get_final_substring(statement, " ");
  boost::erase_all(from_field, " ");

  std::string path_concatenation = dots_to_system_slash(
    from_field
    + boost::filesystem::path::preferred_separator
    + import_field);

  boost::erase_all(path_concatenation, " ");

  return path_concatenation;
}

std::string Solver_Py::import_statement_to_path(const std::string &statement){
  std::string import_field = statement;

  if (boost::contains(import_field, " as ")){
      import_field = import_field.substr(0, import_field.find(" as "));
  }

  std::string path_concatenation = dots_to_system_slash(
              boost::filesystem::path::preferred_separator
                                + import_field);

  boost::erase_all(path_concatenation, " ");
  return path_concatenation;
}


void Solver_Py::insert_edge(const std::string &src_path,
                            const std::string &dst_path,
                            const std::string &name,
                            unsigned int line_no) {
  add_vertex(name, dst_path);
  string key;

  Edge_Descriptor edge;
  bool b;

  // Does the same edge already exist?
  if (boost::edge_by_label(src_path, name, graph).second
          || boost::edge_by_label(src_path, dst_path, graph).second){
    BOOST_LOG_TRIVIAL(trace) << "Duplicate in insert_edge: "
                               << "\n"
                               << "   src = " << src_path << "\n"
                               << "   dst = " << name << "\n"
                               << "   name = " << name;
    return;
  }

  if (0 == dst_path.length()) {
    BOOST_LOG_TRIVIAL(trace) << "insert_edge: "
                               << "\n"
                               << "   src = " << src_path << "\n"
                               << "   dst = " << name << "\n"
                               << "   name = " << name;
    boost::tie(edge, b) = boost::add_edge_by_label(src_path, name, graph);
  } else {
    BOOST_LOG_TRIVIAL(trace) << "insert_edge: "
                               << "\n"
                               << "   src = " << src_path << "\n"
                               << "   dst = " << dst_path << "\n"
                               << "   name = " << name;
    boost::tie(edge, b) = boost::add_edge_by_label(src_path, dst_path, graph);
  }

  graph[edge] = Edge{static_cast<int>(line_no)};
}

std::string Solver_Py::get_first_substring(const std::string &statement, const std::string &delimiter)
{
  return statement.substr(0, statement.find_first_of(delimiter));
}

std::string Solver_Py::get_final_substring(const std::string &statement,
                                           const std::string &delimiter){
  size_t pos_of_last_delim = statement.find_last_of(delimiter);
  return statement.substr(pos_of_last_delim+1);
}


}  // namespace INCLUDE_GARDENER

// vim: filetype=cpp et ts=2 sw=2 sts=2
