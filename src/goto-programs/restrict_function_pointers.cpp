/*******************************************************************\

Module: Restrict function pointers

Author: Diffblue Ltd.

\*******************************************************************/

#include "restrict_function_pointers.h"

#include <ansi-c/expr2c.h>

#include <json/json_parser.h>

#include <util/cmdline.h>
#include <util/options.h>
#include <util/pointer_expr.h>
#include <util/string_utils.h>

#include "goto_model.h"
#include "remove_function_pointers.h"

#include <algorithm>
#include <fstream>

namespace
{
template <typename Handler, typename GotoFunctionT>
void for_each_function_call(GotoFunctionT &&goto_function, Handler handler)
{
  using targett = decltype(goto_function.body.instructions.begin());
  for_each_instruction_if(
    goto_function,
    [](targett target) { return target->is_function_call(); },
    handler);
}

static void restrict_function_pointer(
  message_handlert &message_handler,
  symbol_tablet &symbol_table,
  goto_programt &goto_program,
  const irep_idt &function_id,
  const function_pointer_restrictionst &restrictions,
  const goto_programt::targett &location)
{
  PRECONDITION(location->is_function_call());

  // for each function call, we check if it is using a symbol we have
  // restrictions for, and if so branch on its value and insert concrete calls
  // to the restriction functions

  // Check if this is calling a function pointer, and if so if it is one
  // we have a restriction for
  const auto &original_function = location->call_function();

  if(!can_cast_expr<dereference_exprt>(original_function))
    return;

  // because we run the label function pointer calls transformation pass before
  // this stage a dereference can only dereference a symbol expression
  auto const &called_function_pointer =
    to_dereference_expr(original_function).pointer();
  PRECONDITION(can_cast_expr<symbol_exprt>(called_function_pointer));
  auto const &pointer_symbol = to_symbol_expr(called_function_pointer);
  auto const restriction_iterator =
    restrictions.restrictions.find(pointer_symbol.get_identifier());

  if(restriction_iterator == restrictions.restrictions.end())
    return;

  const namespacet ns(symbol_table);
  std::unordered_set<symbol_exprt, irep_hash> candidates;
  for(const auto &candidate : restriction_iterator->second)
    candidates.insert(ns.lookup(candidate).symbol_expr());

  remove_function_pointer(
    message_handler,
    symbol_table,
    goto_program,
    function_id,
    location,
    candidates,
    true);
}
} // namespace

function_pointer_restrictionst::invalid_restriction_exceptiont::
  invalid_restriction_exceptiont(std::string reason, std::string correct_format)
  : reason(std::move(reason)), correct_format(std::move(correct_format))
{
}

std::string
function_pointer_restrictionst::invalid_restriction_exceptiont::what() const
{
  std::string res;

  res += "Invalid restriction";
  res += "\nReason: " + reason;

  if(!correct_format.empty())
  {
    res += "\nFormat: " + correct_format;
  }

  return res;
}

void function_pointer_restrictionst::typecheck_function_pointer_restrictions(
  const goto_modelt &goto_model,
  const function_pointer_restrictionst::restrictionst &restrictions)
{
  for(auto const &restriction : restrictions)
  {
    auto const function_pointer_sym =
      goto_model.symbol_table.lookup(restriction.first);
    if(function_pointer_sym == nullptr)
    {
      throw invalid_restriction_exceptiont{id2string(restriction.first) +
                                           " not found in the symbol table"};
    }
    auto const &function_pointer_type = function_pointer_sym->type;
    if(function_pointer_type.id() != ID_pointer)
    {
      throw invalid_restriction_exceptiont{"not a function pointer: " +
                                           id2string(restriction.first)};
    }
    auto const &function_type =
      to_pointer_type(function_pointer_type).subtype();
    if(function_type.id() != ID_code)
    {
      throw invalid_restriction_exceptiont{"not a function pointer: " +
                                           id2string(restriction.first)};
    }
    auto const &ns = namespacet{goto_model.symbol_table};
    for(auto const &function_pointer_target : restriction.second)
    {
      auto const function_pointer_target_sym =
        goto_model.symbol_table.lookup(function_pointer_target);
      if(function_pointer_target_sym == nullptr)
      {
        throw invalid_restriction_exceptiont{
          "symbol not found: " + id2string(function_pointer_target)};
      }
      auto const &function_pointer_target_type =
        function_pointer_target_sym->type;
      if(function_pointer_target_type.id() != ID_code)
      {
        throw invalid_restriction_exceptiont{
          "not a function: " + id2string(function_pointer_target)};
      }

      if(!function_is_type_compatible(
           true,
           to_code_type(function_type),
           to_code_type(function_pointer_target_type),
           ns))
      {
        throw invalid_restriction_exceptiont{
          "type mismatch: `" + id2string(restriction.first) + "' points to `" +
          type2c(function_type, ns) + "', but restriction `" +
          id2string(function_pointer_target) + "' has type `" +
          type2c(function_pointer_target_type, ns) + "'"};
      }
    }
  }
}

void restrict_function_pointers(
  message_handlert &message_handler,
  goto_modelt &goto_model,
  const function_pointer_restrictionst &restrictions)
{
  for(auto &function_item : goto_model.goto_functions.function_map)
  {
    goto_functiont &goto_function = function_item.second;

    for_each_function_call(goto_function, [&](const goto_programt::targett it) {
      restrict_function_pointer(
        message_handler,
        goto_model.symbol_table,
        goto_function.body,
        function_item.first,
        restrictions,
        it);
    });
  }
}

void parse_function_pointer_restriction_options_from_cmdline(
  const cmdlinet &cmdline,
  optionst &options)
{
  if(cmdline.isset(RESTRICT_FUNCTION_POINTER_OPT))
  {
    options.set_option(
      RESTRICT_FUNCTION_POINTER_OPT,
      cmdline.get_values(RESTRICT_FUNCTION_POINTER_OPT));
  }

  if(cmdline.isset(RESTRICT_FUNCTION_POINTER_FROM_FILE_OPT))
  {
    options.set_option(
      RESTRICT_FUNCTION_POINTER_FROM_FILE_OPT,
      cmdline.get_values(RESTRICT_FUNCTION_POINTER_FROM_FILE_OPT));
  }

  if(cmdline.isset(RESTRICT_FUNCTION_POINTER_BY_NAME_OPT))
  {
    options.set_option(
      RESTRICT_FUNCTION_POINTER_BY_NAME_OPT,
      cmdline.get_values(RESTRICT_FUNCTION_POINTER_BY_NAME_OPT));
  }
}

function_pointer_restrictionst::restrictionst
function_pointer_restrictionst::merge_function_pointer_restrictions(
  function_pointer_restrictionst::restrictionst lhs,
  const function_pointer_restrictionst::restrictionst &rhs)
{
  auto &result = lhs;

  for(auto const &restriction : rhs)
  {
    auto emplace_result = result.emplace(restriction.first, restriction.second);
    if(!emplace_result.second)
    {
      for(auto const &target : restriction.second)
      {
        emplace_result.first->second.insert(target);
      }
    }
  }

  return result;
}

function_pointer_restrictionst::restrictionst
function_pointer_restrictionst::parse_function_pointer_restrictions(
  const std::list<std::string> &restriction_opts,
  const std::string &option)
{
  auto function_pointer_restrictions =
    function_pointer_restrictionst::restrictionst{};

  for(const std::string &restriction_opt : restriction_opts)
  {
    const auto restriction =
      parse_function_pointer_restriction(restriction_opt, option);

    const bool inserted = function_pointer_restrictions
                            .emplace(restriction.first, restriction.second)
                            .second;

    if(!inserted)
    {
      throw invalid_restriction_exceptiont{
        "function pointer restriction for `" + id2string(restriction.first) +
        "' was specified twice"};
    }
  }

  return function_pointer_restrictions;
}

function_pointer_restrictionst::restrictionst function_pointer_restrictionst::
  parse_function_pointer_restrictions_from_command_line(
    const std::list<std::string> &restriction_opts)
{
  return parse_function_pointer_restrictions(
    restriction_opts, "--" RESTRICT_FUNCTION_POINTER_OPT);
}

function_pointer_restrictionst::restrictionst
function_pointer_restrictionst::parse_function_pointer_restrictions_from_file(
  const std::list<std::string> &filenames,
  message_handlert &message_handler)
{
  auto merged_restrictions = function_pointer_restrictionst::restrictionst{};

  for(auto const &filename : filenames)
  {
    auto const restrictions = read_from_file(filename, message_handler);

    merged_restrictions = merge_function_pointer_restrictions(
      std::move(merged_restrictions), restrictions.restrictions);
  }

  return merged_restrictions;
}

function_pointer_restrictionst::restrictiont
function_pointer_restrictionst::parse_function_pointer_restriction(
  const std::string &restriction_opt,
  const std::string &option)
{
  // the format for restrictions is <pointer_name>/<target[,more_targets]*>
  // exactly one pointer and at least one target
  auto const pointer_name_end = restriction_opt.find('/');
  auto const restriction_format_message =
    "the format for restrictions is "
    "<pointer_name>/<target[,more_targets]*>";

  if(pointer_name_end == std::string::npos)
  {
    throw invalid_restriction_exceptiont{"couldn't find '/' in `" +
                                           restriction_opt + "'",
                                         restriction_format_message};
  }

  if(pointer_name_end == restriction_opt.size())
  {
    throw invalid_restriction_exceptiont{
      "couldn't find names of targets after '/' in `" + restriction_opt + "'",
      restriction_format_message};
  }

  if(pointer_name_end == 0)
  {
    throw invalid_restriction_exceptiont{
      "couldn't find target name before '/' in `" + restriction_opt + "'"};
  }

  auto const pointer_name = restriction_opt.substr(0, pointer_name_end);
  auto const target_names_substring =
    restriction_opt.substr(pointer_name_end + 1);
  auto const target_name_strings = split_string(target_names_substring, ',');

  if(target_name_strings.size() == 1 && target_name_strings[0].empty())
  {
    throw invalid_restriction_exceptiont{
      "missing target list for function pointer restriction " + pointer_name,
      restriction_format_message};
  }

  std::unordered_set<irep_idt> target_names;
  target_names.insert(target_name_strings.begin(), target_name_strings.end());

  for(auto const &target_name : target_names)
  {
    if(target_name == ID_empty_string)
    {
      throw invalid_restriction_exceptiont(
        "leading or trailing comma in restrictions for `" + pointer_name + "'",
        restriction_format_message);
    }
  }

  return std::make_pair(pointer_name, target_names);
}

optionalt<function_pointer_restrictionst::restrictiont>
function_pointer_restrictionst::get_by_name_restriction(
  const goto_functiont &goto_function,
  const function_pointer_restrictionst::restrictionst &by_name_restrictions,
  const goto_programt::const_targett &location)
{
  PRECONDITION(location->is_function_call());

  const exprt &function = location->call_function();

  if(!can_cast_expr<dereference_exprt>(function))
    return {};

  // the function pointer is guaranteed to be a symbol expression, as the
  // label_function_pointer_call_sites() pass (which must be run before the
  // function pointer restriction) replaces calls via complex pointer
  // expressions by calls to a function pointer variable
  auto const &function_pointer_call_site =
    to_symbol_expr(to_dereference_expr(function).pointer());

  const goto_programt::const_targett it = std::prev(location);

  INVARIANT(
    to_symbol_expr(it->assign_lhs()).get_identifier() ==
      function_pointer_call_site.get_identifier(),
    "called function pointer must have been assigned at the previous location");

  if(!can_cast_expr<symbol_exprt>(it->assign_rhs()))
    return {};

  const auto &rhs = to_symbol_expr(it->assign_rhs());

  const auto restriction = by_name_restrictions.find(rhs.get_identifier());

  if(restriction != by_name_restrictions.end())
  {
    return optionalt<function_pointer_restrictionst::restrictiont>(
      std::make_pair(
        function_pointer_call_site.get_identifier(), restriction->second));
  }

  return {};
}

function_pointer_restrictionst function_pointer_restrictionst::from_options(
  const optionst &options,
  const goto_modelt &goto_model,
  message_handlert &message_handler)
{
  auto const restriction_opts =
    options.get_list_option(RESTRICT_FUNCTION_POINTER_OPT);

  restrictionst commandline_restrictions;
  try
  {
    commandline_restrictions =
      parse_function_pointer_restrictions_from_command_line(restriction_opts);
    typecheck_function_pointer_restrictions(
      goto_model, commandline_restrictions);
  }
  catch(const invalid_restriction_exceptiont &e)
  {
    throw invalid_command_line_argument_exceptiont{
      e.reason, "--" RESTRICT_FUNCTION_POINTER_OPT, e.correct_format};
  }

  restrictionst file_restrictions;
  try
  {
    auto const restriction_file_opts =
      options.get_list_option(RESTRICT_FUNCTION_POINTER_FROM_FILE_OPT);
    file_restrictions = parse_function_pointer_restrictions_from_file(
      restriction_file_opts, message_handler);
    typecheck_function_pointer_restrictions(goto_model, file_restrictions);
  }
  catch(const invalid_restriction_exceptiont &e)
  {
    throw deserialization_exceptiont{e.what()};
  }

  restrictionst name_restrictions;
  try
  {
    auto const restriction_name_opts =
      options.get_list_option(RESTRICT_FUNCTION_POINTER_BY_NAME_OPT);
    name_restrictions = get_function_pointer_by_name_restrictions(
      restriction_name_opts, goto_model);
    typecheck_function_pointer_restrictions(goto_model, name_restrictions);
  }
  catch(const invalid_restriction_exceptiont &e)
  {
    throw invalid_command_line_argument_exceptiont{
      e.reason, "--" RESTRICT_FUNCTION_POINTER_BY_NAME_OPT, e.correct_format};
  }

  return {merge_function_pointer_restrictions(
    commandline_restrictions,
    merge_function_pointer_restrictions(file_restrictions, name_restrictions))};
}

function_pointer_restrictionst
function_pointer_restrictionst::from_json(const jsont &json)
{
  function_pointer_restrictionst::restrictionst restrictions;

  if(!json.is_object())
  {
    throw deserialization_exceptiont{"top level item is not an object"};
  }

  for(auto const &restriction : to_json_object(json))
  {
    restrictions.emplace(irep_idt{restriction.first}, [&] {
      if(!restriction.second.is_array())
      {
        throw deserialization_exceptiont{"Value of " + restriction.first +
                                         " is not an array"};
      }
      auto possible_targets = std::unordered_set<irep_idt>{};
      auto const &array = to_json_array(restriction.second);
      std::transform(
        array.begin(),
        array.end(),
        std::inserter(possible_targets, possible_targets.end()),
        [&](const jsont &array_element) {
          if(!array_element.is_string())
          {
            throw deserialization_exceptiont{
              "Value of " + restriction.first +
              "contains a non-string array element"};
          }
          return irep_idt{to_json_string(array_element).value};
        });
      return possible_targets;
    }());
  }

  return function_pointer_restrictionst{restrictions};
}

function_pointer_restrictionst function_pointer_restrictionst::read_from_file(
  const std::string &filename,
  message_handlert &message_handler)
{
  auto inFile = std::ifstream{filename};
  jsont json;

  if(parse_json(inFile, filename, message_handler, json))
  {
    throw deserialization_exceptiont{
      "failed to read function pointer restrictions from " + filename};
  }

  return from_json(json);
}

jsont function_pointer_restrictionst::to_json() const
{
  auto function_pointer_restrictions_json = jsont{};
  auto &restrictions_json_object =
    function_pointer_restrictions_json.make_object();

  for(auto const &restriction : restrictions)
  {
    auto &targets_array =
      restrictions_json_object[id2string(restriction.first)].make_array();
    for(auto const &target : restriction.second)
    {
      targets_array.push_back(json_stringt{target});
    }
  }

  return function_pointer_restrictions_json;
}

void function_pointer_restrictionst::write_to_file(
  const std::string &filename) const
{
  auto function_pointer_restrictions_json = to_json();

  auto outFile = std::ofstream{filename};

  if(!outFile)
  {
    throw system_exceptiont{"cannot open " + filename +
                            " for writing function pointer restrictions"};
  }

  function_pointer_restrictions_json.output(outFile);
  // Ensure output file ends with a newline character.
  outFile << '\n';
}

function_pointer_restrictionst::restrictionst
function_pointer_restrictionst::get_function_pointer_by_name_restrictions(
  const std::list<std::string> &restriction_name_opts,
  const goto_modelt &goto_model)
{
  function_pointer_restrictionst::restrictionst by_name_restrictions =
    parse_function_pointer_restrictions(
      restriction_name_opts, "--" RESTRICT_FUNCTION_POINTER_BY_NAME_OPT);

  function_pointer_restrictionst::restrictionst restrictions;
  for(auto const &goto_function : goto_model.goto_functions.function_map)
  {
    for_each_function_call(
      goto_function.second, [&](const goto_programt::const_targett it) {
        const auto restriction = get_by_name_restriction(
          goto_function.second, by_name_restrictions, it);

        if(restriction)
        {
          restrictions.insert(*restriction);
        }
      });
  }

  return restrictions;
}
