//  This file is distributed under the BSD 3-Clause License. See LICENSE for details.

#include <format>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <queue>
#include <string>
#include <string_view>
#include <utility>

#include "absl/strings/match.h"
#include "firrtl.pb.h"
// #include "google/protobuf/util/time_util.h"
#include "inou_firrtl.hpp"
#include "perf_tracing.hpp"
#include "thread_pool.hpp"

// using google::protobuf::util::TimeUtil;

/* For help understanding FIRRTL/Protobuf:
 * 1) Semantics regarding FIRRTL language:
 * www2.eecs.berkeley.edu/Pubs/TechRpts/2019/EECS-2019-168.pdf
 * 2) Structure of FIRRTL Protobuf file:
 * github.com/freechipsproject/firrtl/blob/master/src/main/proto/firrtl.proto */

void Inou_firrtl::to_lnast(Eprp_var& var) {
  TRACE_EVENT("inou", "firrtl_tolnast");
  Inou_firrtl p(var);

  if (var.has_label("files")) {
    auto files = var.get("files");
    for (const auto& f_sv : absl::StrSplit(files, ',')) {
      std::string f(f_sv);

      // std::cout << std::format("FILE: {}\n", f);
      // firrtl::FirrtlPB firrtl_input;

      auto*        firrtl_input = new firrtl::FirrtlPB();
      std::fstream input(f.c_str(), std::ios::in | std::ios::binary);
      if (!firrtl_input->ParseFromIstream(&input)) {
        Pass::error("Failed to parse FIRRTL from protobuf format: {}", f);
        return;
      }
      p.iterate_circuits(var, *firrtl_input, f);
    }
  } else {
    std::cout << "No file provided. This requires a file input.\n";
    return;
  }

  // Optional:  Delete all global objects allocated by libprotobuf.
  // FIXME: dispatch to a new thread to overlap with ln2lg
  //        or defer to the end of lcompiler
  // google::protobuf::ShutdownProtobufLibrary();
}

//----------------Helper Functions--------------------------
std::string Inou_firrtl_module::create_tmp_var() { return absl::StrCat("___F", ++tmp_var_cnt); }

std::string Inou_firrtl_module::create_tmp_mut_var() { return absl::StrCat("_._M", ++dummy_expr_node_cnt); }

/* Determine if 'name' refers to any IO/reg/etc... If it does,
 * add the appropriate symbol and return the flattened version*/
std::string Inou_firrtl_module::name_prefix_modifier_flattener(std::string_view name, const bool is_rhs) {
  std::string flattened_name = std::string{name};
  std::replace(flattened_name.begin(), flattened_name.end(), '.', '_');

  if (output_names.count(name)) {
    return absl::StrCat("%", flattened_name);
  } else if (input_names.count(name)) {
    return absl::StrCat("$", flattened_name);
  } else if (reg2qpin.count(flattened_name)) {  // check if the register exists
    I(reg2qpin[flattened_name].substr(0, 3) == "_#_");
    if (is_rhs) {
      return reg2qpin[flattened_name];
    }
    // return absl::StrCat("#", flattened_name);
    return flattened_name;
  }

  return flattened_name;
}

std::string Inou_firrtl_module::get_runtime_idx_field_name(const firrtl::FirrtlPB_Expression& expr) {
  if (expr.has_sub_field()) {
    return get_runtime_idx_field_name(expr.sub_field().expression());
  } else if (expr.has_sub_access()) {
    bool dummy   = false;
    auto idx_str = get_expr_hier_name(expr.sub_access().index(), dummy);
    return idx_str;
  } else if (expr.has_sub_index()) {
    return get_runtime_idx_field_name(expr.sub_index().expression());
  } else if (expr.has_reference()) {
    I(false);
    return "";
  } else {
    I(false);
    return "";
  }
}

void Inou_firrtl_module::handle_lhs_runtime_idx(Lnast& lnast, Lnast_nid& parent_node, std::string_view hier_name_l_ori,
                                                std::string_view hier_name_r_ori, const firrtl::FirrtlPB_Expression& lhs_expr,
                                                const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  std::string rhs_flattened_name = name_prefix_modifier_flattener(hier_name_r_ori, true);
  // (1) get the runtime idx tuple field
  std::string rtidx_str = get_runtime_idx_field_name(lhs_expr);
  rtidx_str             = name_prefix_modifier_flattener(rtidx_str, true);

  bool        is_2d_vector    = false;
  std::string leaf_field_name = "";
  auto        pos             = hier_name_l_ori.find("..");
  std::string vec_name;
  if (pos != std::string::npos) {
    is_2d_vector    = true;
    leaf_field_name = hier_name_l_ori.substr(pos + 2);
    vec_name        = hier_name_l_ori.substr(0, pos);
  }

  auto pos2 = hier_name_l_ori.rfind('.');
  if (pos2 != std::string::npos && !is_2d_vector) {
    vec_name = hier_name_l_ori.substr(0, pos2);
  }

  // (2) know the vector size of this field
  auto rt_vec_size = get_vector_size(lnast, vec_name);

  vec_name = name_prefix_modifier_flattener(vec_name, true);
  // (3) create another function to create __fir_mux for 2^field_bits cases
  //     lhs <- __fir_mux(runtime_idx, value0, value1, ..., value2^filed_bits-1);
  std::vector<std::string> cond_strs;
  for (int i = 0; i < rt_vec_size; i++) {
    auto idx_eq   = lnast.add_child(parent_node, Lnast_node::create_func_call("", 0, line_pos, col_pos, fname));
    auto cond_str = create_tmp_var();
    lnast.add_child(idx_eq, Lnast_node::create_ref(cond_str, 0, line_pos, col_pos, fname));
    lnast.add_child(idx_eq, Lnast_node::create_const("__fir_eq", 0, line_pos, col_pos, fname));
    lnast.add_child(idx_eq, Lnast_node::create_ref(rtidx_str, 0, line_pos, col_pos, fname));
    lnast.add_child(idx_eq, Lnast_node::create_const(i));
    cond_strs.push_back(cond_str);
  }

  auto idx_mux = lnast.add_child(parent_node, Lnast_node::create_if("", 0, line_pos, col_pos, fname));
  for (int i = 0; i < rt_vec_size; i++) {
    lnast.add_child(idx_mux, Lnast_node::create_ref(cond_strs[i], 0, line_pos, col_pos, fname));
    auto idx_stmt_t = lnast.add_child(idx_mux, Lnast_node::create_stmts("", 0, line_pos, col_pos, fname));
    // auto rhs_flattened_name = name_prefix_modifier_flattener(tup_head, true);
    std::string lhs_flattened_name;
    if (is_2d_vector) {
      lhs_flattened_name = absl::StrCat(vec_name, ".", i, ".", leaf_field_name);
    } else {
      lhs_flattened_name = absl::StrCat(vec_name, ".", i);
    }
    lhs_flattened_name = name_prefix_modifier_flattener(lhs_flattened_name, false);

    add_lnast_assign(lnast, idx_stmt_t, lhs_flattened_name, rhs_flattened_name, stmt);
  }
  return;
}

void Inou_firrtl_module::handle_rhs_runtime_idx(Lnast& lnast, Lnast_nid& parent_node, std::string_view hier_name_l_ori,
                                                std::string_view hier_name_r_ori, const firrtl::FirrtlPB_Expression& rhs_expr,
                                                const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  std::string lhs_flattened_name = name_prefix_modifier_flattener(hier_name_l_ori, false);

  // idea:
  // (1) get the runtime idx tuple field
  // FIXME->sh: does not pass the RenameTable pattern .... check it later, but focus on Mul.fir now
  std::string rtidx_str = get_runtime_idx_field_name(rhs_expr);
  rtidx_str             = name_prefix_modifier_flattener(rtidx_str, true);

  std::string vec_name;
  bool        is_2d_vector    = false;
  std::string leaf_field_name = "";
  auto        pos             = hier_name_r_ori.find("..");
  if (pos != std::string::npos) {
    is_2d_vector    = true;
    leaf_field_name = hier_name_r_ori.substr(pos + 2);
    vec_name        = hier_name_r_ori.substr(0, pos);
  }

  auto pos2 = hier_name_r_ori.rfind('.');
  if (pos2 != std::string::npos && !is_2d_vector) {
    vec_name = hier_name_r_ori.substr(0, pos2);
  }

  // (2) know the vector size of this field
  auto rt_vec_size = get_vector_size(lnast, vec_name);

  vec_name = name_prefix_modifier_flattener(vec_name, true);
  // (3) create another function to create __fir_mux for 2^field_bits cases
  //     lhs <- __fir_mux(runtime_idx, value0, value1, ..., value2^filed_bits-1);

  std::vector<std::string> cond_strs;
  for (int i = 0; i < rt_vec_size; i++) {
    auto idx_eq   = lnast.add_child(parent_node, Lnast_node::create_func_call("", 0, line_pos, col_pos, fname));
    auto cond_str = create_tmp_var();
    lnast.add_child(idx_eq, Lnast_node::create_ref(cond_str, 0, line_pos, col_pos, fname));
    lnast.add_child(idx_eq, Lnast_node::create_const("__fir_eq", 0, line_pos, col_pos, fname));
    lnast.add_child(idx_eq, Lnast_node::create_ref(rtidx_str, 0, line_pos, col_pos, fname));
    lnast.add_child(idx_eq, Lnast_node::create_const(i));
    cond_strs.push_back(cond_str);
  }

  auto idx_mux = lnast.add_child(parent_node, Lnast_node::create_if("", 0, line_pos, col_pos, fname));
  for (int i = 0; i < rt_vec_size; i++) {
    lnast.add_child(idx_mux, Lnast_node::create_ref(cond_strs[i], 0, line_pos, col_pos, fname));
    auto idx_stmt_t = lnast.add_child(idx_mux, Lnast_node::create_stmts("", 0, line_pos, col_pos, fname));

    std::string rhs_flattened_name;
    if (is_2d_vector) {
      rhs_flattened_name = absl::StrCat(vec_name, ".", i, ".", leaf_field_name);
    } else {
      rhs_flattened_name = absl::StrCat(vec_name, ".", i);
    }
    rhs_flattened_name = name_prefix_modifier_flattener(rhs_flattened_name, true);

    add_lnast_assign(lnast, idx_stmt_t, lhs_flattened_name, rhs_flattened_name, stmt);
  }
  return;
}

uint16_t Inou_firrtl_module::get_vector_size(const Lnast& lnast, std::string_view vec_name) {
  if (var2vec_size.find(vec_name) == var2vec_size.end()) {
    auto module_name = lnast.get_top_module_name();
#ifndef NDEBUG
    Pass::warn(
        "Warning: the \"if\" statement below is to enable RocketTile LG generation. Remove the if block and use the assertion "
        "above it instead.\n");
#endif
    // I(Inou_firrtl::glob_info.module_var2vec_size.find(module_name) != Inou_firrtl::glob_info.module_var2vec_size.end());
    if (Inou_firrtl::glob_info.module_var2vec_size.find(module_name) == Inou_firrtl::glob_info.module_var2vec_size.end()) {
      return 1;
    }
    auto& io_var2vec_size = Inou_firrtl::glob_info.module_var2vec_size[module_name];

    // I(io_var2vec_size.find(vec_name) != io_var2vec_size.end());
    return io_var2vec_size[vec_name];
  } else {
    return var2vec_size[vec_name];
  }
}

int32_t Inou_firrtl_module::get_bit_count(const firrtl::FirrtlPB_Type& type) {
  switch (type.type_case()) {
    case firrtl::FirrtlPB_Type::kUintType: {  // UInt type
      return type.uint_type().width().value();
    }
    case firrtl::FirrtlPB_Type::kSintType: {  // SInt type
      return type.sint_type().width().value();
    }
    case firrtl::FirrtlPB_Type::kClockType: {  // Clock type
      return 1;
    }
    case firrtl::FirrtlPB_Type::kBundleType:    // Bundle type
    case firrtl::FirrtlPB_Type::kVectorType: {  // Vector type
      I(false);                                 // get_bit_count should never be called on these (no sense)
    }
    case firrtl::FirrtlPB_Type::kFixedType: {  // Fixed type
      I(false);                                // TODO: Not yet supported.
    }
    case firrtl::FirrtlPB_Type::kAnalogType: {  // Analog type
      return type.analog_type().width().value();
    }
    case firrtl::FirrtlPB_Type::kAsyncResetType: {  // AsyncReset type
      return 1;
    }
    case firrtl::FirrtlPB_Type::kResetType: {  // Reset type
      return 1;
    }
    default: Pass::error("Unknown port type.");
  }
  return -1;
}

void Inou_firrtl_module::handle_register(Lnast& lnast, const firrtl::FirrtlPB_Type& type, std::string id, Lnast_nid& parent_node,
                                         const firrtl::FirrtlPB_Statement& stmt) {
  switch (type.type_case()) {
    case firrtl::FirrtlPB_Type::kBundleType: {  // Bundle Type
      for (int i = 0; i < type.bundle_type().field_size(); i++) {
        handle_register(lnast,
                        type.bundle_type().field(i).type(),
                        absl::StrCat(id, ".", type.bundle_type().field(i).id()),
                        parent_node,
                        stmt);
      }
      break;
    }
    case firrtl::FirrtlPB_Type::kVectorType: {  // Vector Type
      var2vec_size.insert_or_assign(id, type.vector_type().size());
      for (uint32_t i = 0; i < type.vector_type().size(); i++) {
        handle_register(lnast, type.vector_type().type(), absl::StrCat(id, ".", i), parent_node, stmt);
      }
      break;
    }
    case firrtl::FirrtlPB_Type::kSintType:
    case firrtl::FirrtlPB_Type::kUintType: {
      add_local_flip_info(false, id);
      std::string head_chopped_hier_name;
      // if (id.find_first_of('.') != std::string::npos)
      if (absl::StrContains(id, '.')) {
        head_chopped_hier_name = id.substr(id.find_first_of('.') + 1);
      }

      std::replace(id.begin(), id.end(), '.', '_');

      auto reg_bits = get_bit_count(type);
      bool bits_set_done
          = reg_bits > 0 ? true
                         : false;  // some chirrtl code don't have bits set on register, but must have bits set on init expression
      auto diff_type = type.type_case() != firrtl::FirrtlPB_Type::kUintType;
      // setup_scalar_bits(lnast, absl::StrCat("#", id), reg_bits, parent_node, diff_type, stmt);
      setup_scalar_bits(lnast, id, reg_bits, parent_node, diff_type, stmt);

      setup_register_reset_init(lnast,
                                parent_node,
                                id,
                                stmt.register_().reset(),
                                stmt.register_().init(),
                                head_chopped_hier_name,
                                bits_set_done,
                                stmt);
      declare_register(lnast, parent_node, id, stmt);
      setup_register_q_pin(lnast, parent_node, id, stmt);
      break;
    }
    default: {
      I(false);
      break;
    }
  }
}

void Inou_firrtl_module::wire_init_flip_handling(Lnast& lnast, const firrtl::FirrtlPB_Type& type, std::string id, bool flipped_in,
                                                 Lnast_nid& parent_node, const firrtl::FirrtlPB_Statement& stmt) {
  switch (type.type_case()) {
    case firrtl::FirrtlPB_Type::kBundleType: {  // Bundle Type
      auto& btype = type.bundle_type();
      for (int i = 0; i < type.bundle_type().field_size(); i++) {
        if (btype.field(i).is_flipped()) {
          wire_init_flip_handling(lnast,
                                  type.bundle_type().field(i).type(),
                                  absl::StrCat(id, ".", type.bundle_type().field(i).id()),
                                  !flipped_in,
                                  parent_node,
                                  stmt);
        } else {
          wire_init_flip_handling(lnast,
                                  type.bundle_type().field(i).type(),
                                  absl::StrCat(id, ".", type.bundle_type().field(i).id()),
                                  flipped_in,
                                  parent_node,
                                  stmt);
        }
      }
      break;
    }
    case firrtl::FirrtlPB_Type::kVectorType: {  // Vector Type
      var2vec_size.insert_or_assign(id, type.vector_type().size());
      for (uint32_t i = 0; i < type.vector_type().size(); i++) {
        wire_init_flip_handling(lnast, type.vector_type().type(), absl::StrCat(id, ".", i), false, parent_node, stmt);
      }
      break;
    }
    case firrtl::FirrtlPB_Type::kFixedType: {  // Fixed Point Type
      I(false);                                // TODO: LNAST does not support fixed point yet.
      break;
    }
    case firrtl::FirrtlPB_Type::kAsyncResetType: {  // AsyncReset
      add_local_flip_info(flipped_in, id);
      std::replace(id.begin(), id.end(), '.', '_');
      Lnast_node zero_node = Lnast_node::create_const(0);
      create_default_value_for_scalar_var(lnast, parent_node, id, zero_node, stmt);

      break;
    }
    case firrtl::FirrtlPB_Type::kSintType: {  // signed
      add_local_flip_info(flipped_in, id);
      std::replace(id.begin(), id.end(), '.', '_');
      Lnast_node zero_node = Lnast_node::create_const(0);
      create_default_value_for_scalar_var(lnast, parent_node, id, zero_node, stmt);
      break;
    }
    case firrtl::FirrtlPB_Type::kUintType: {  // unsigned
      add_local_flip_info(flipped_in, id);

      std::replace(id.begin(), id.end(), '.', '_');
      Lnast_node zero_node = Lnast_node::create_const(0);
      create_default_value_for_scalar_var(lnast, parent_node, id, zero_node, stmt);
      break;
    }
    default: {
      // UInt Analog Reset Clock Types
      add_local_flip_info(flipped_in, id);
      break;
    }
  }
}

// When creating a register, we have to set the register's
// clock, reset, and init values using "dot" nodes in the LNAST.
// These functions create all of those when a reg is first declared.
void Inou_firrtl_module::setup_scalar_bits(Lnast& lnast, std::string_view id, uint32_t bits, Lnast_nid& parent_node, bool is_signed,
                                           const firrtl::FirrtlPB_Statement& stmt) {
  // Specify __bits, if bitwidth is explicit
  if (bits > 0) {
    auto value_node = Lnast_node::create_const(bits);
    auto extension  = is_signed ? ".__sbits" : ".__ubits";
    create_tuple_add_from_str(lnast, parent_node, absl::StrCat(id, extension), value_node, stmt);
  }
}

void Inou_firrtl_module::collect_memory_data_struct_hierarchy(std::string_view mem_name, const firrtl::FirrtlPB_Type& type_in,
                                                              std::string_view hier_fields_concats) {
  std::string new_hier_fields_concats;
  if (type_in.type_case() == firrtl::FirrtlPB_Type::kBundleType) {
    for (int i = 0; i < type_in.bundle_type().field_size(); i++) {
      if (hier_fields_concats.empty()) {
        new_hier_fields_concats = type_in.bundle_type().field(i).id();
      } else {
        new_hier_fields_concats = absl::StrCat(hier_fields_concats, ".", type_in.bundle_type().field(i).id());
      }

      auto type_sub_field = type_in.bundle_type().field(i).type();
      if (type_sub_field.type_case() == firrtl::FirrtlPB_Type::kBundleType
          || type_sub_field.type_case() == firrtl::FirrtlPB_Type::kVectorType) {
        collect_memory_data_struct_hierarchy(mem_name, type_sub_field, new_hier_fields_concats);
      } else {
        auto bits = get_bit_count(type_sub_field);
        absl::StrAppend(&new_hier_fields_concats, ".", bits);
        mem2din_fields[mem_name].emplace_back(new_hier_fields_concats);
      }
    }
  } else if (type_in.type_case() == firrtl::FirrtlPB_Type::kVectorType) {
    for (uint32_t i = 0; i < type_in.vector_type().size(); i++) {
      if (hier_fields_concats.empty()) {
        new_hier_fields_concats = std::to_string(i);
      } else {
        new_hier_fields_concats = absl::StrCat(hier_fields_concats, ".", std::to_string(i));
      }

      auto type_sub_field = type_in.vector_type().type();
      if (type_sub_field.type_case() == firrtl::FirrtlPB_Type::kBundleType
          || type_sub_field.type_case() == firrtl::FirrtlPB_Type::kVectorType) {
        collect_memory_data_struct_hierarchy(mem_name, type_sub_field, new_hier_fields_concats);
      } else {
        auto bits = get_bit_count(type_sub_field);
        absl::StrAppend(&new_hier_fields_concats, ".", bits);  // encode .bits at the end of hier-fields
        // new_hier_fields_concats = absl::StrCat(new_hier_fields_concats, ".", bits);
        mem2din_fields[mem_name].emplace_back(new_hier_fields_concats);
      }
    }
  }
}

void Inou_firrtl_module::init_cmemory(Lnast& lnast, Lnast_nid& parent_node, const firrtl::FirrtlPB_Statement_CMemory& cmem,
                                      const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  std::string depth_str;  // depth in firrtl = size in LiveHD
  uint8_t     wensize_init = 1;
  if (cmem.type_case() == firrtl::FirrtlPB_Statement_CMemory::kTypeAndDepth) {
    depth_str = Inou_firrtl::convert_bigint_to_str(cmem.type_and_depth().depth());
  } else {
    I(false, "happened somewhere in boom!");  // never happened?
  }

  firrtl::FirrtlPB_Type din_type = cmem.type_and_depth().data_type();
  if (din_type.type_case() == firrtl::FirrtlPB_Type::kBundleType || din_type.type_case() == firrtl::FirrtlPB_Type::kVectorType) {
    collect_memory_data_struct_hierarchy(cmem.id(), din_type, "");
    wensize_init = din_type.vector_type().size();
  } else if (din_type.type_case() == firrtl::FirrtlPB_Type::kVectorType) {
    I(false, "it's masked case!");
  } else if (din_type.type_case() == firrtl::FirrtlPB_Type::kUintType) {
    auto bits = get_bit_count(din_type);
    mem2din_fields[cmem.id()].emplace_back(absl::StrCat(".", bits));  // encode .bits at the end of hier-fields
  } else {
    I(false);
  }

  // Specify attributes
  bool fwd = false;
  if (cmem.read_under_write() == firrtl::FirrtlPB_Statement_ReadUnderWrite::FirrtlPB_Statement_ReadUnderWrite_NEW) {
    fwd = true;
  }

  // create foo_mem_res = __memory(foo_mem_aruments.__last_value)
  auto idx_attr_get  = lnast.add_child(parent_node, Lnast_node::create_attr_get("", 0, line_pos, col_pos, fname));
  auto temp_var_name = create_tmp_var();
  lnast.add_child(idx_attr_get, Lnast_node::create_ref(temp_var_name, 0, line_pos, col_pos, fname));
  lnast.add_child(idx_attr_get, Lnast_node::create_ref(absl::StrCat(cmem.id(), "_interface_args"), 0, line_pos, col_pos, fname));
  lnast.add_child(idx_attr_get, Lnast_node::create_const("__last_value", 0, line_pos, col_pos, fname));
  wire_names.insert(temp_var_name);

  auto idx_fncall = lnast.add_child(parent_node, Lnast_node::create_func_call("", 0, line_pos, col_pos, fname));
  lnast.add_child(idx_fncall, Lnast_node::create_ref(absl::StrCat(cmem.id(), "_res"), 0, line_pos, col_pos, fname));
  lnast.add_child(idx_fncall, Lnast_node::create_ref("__memory", 0, line_pos, col_pos, fname));
  lnast.add_child(idx_fncall, Lnast_node::create_ref(temp_var_name, 0, line_pos, col_pos, fname));

  // bare initialization of memory interfaces so that SSA can continue later
  auto idx_ta_maddr = lnast.add_child(parent_node, Lnast_node::create_tuple_add("", 0, line_pos, col_pos, fname));
  lnast.add_child(idx_ta_maddr, Lnast_node::create_ref(absl::StrCat(cmem.id(), "_addr"), 0, line_pos, col_pos, fname));

  auto idx_ta_mdin = lnast.add_child(parent_node, Lnast_node::create_tuple_add("", 0, line_pos, col_pos, fname));
  lnast.add_child(idx_ta_mdin, Lnast_node::create_ref(absl::StrCat(cmem.id(), "_din"), 0, line_pos, col_pos, fname));

  auto idx_ta_men = lnast.add_child(parent_node, Lnast_node::create_tuple_add("", 0, line_pos, col_pos, fname));
  lnast.add_child(idx_ta_men, Lnast_node::create_ref(absl::StrCat(cmem.id(), "_enable"), 0, line_pos, col_pos, fname));

  auto idx_asg_mfwd = lnast.add_child(parent_node, Lnast_node::create_assign("", 0, line_pos, col_pos, fname));
  lnast.add_child(idx_asg_mfwd, Lnast_node::create_ref(absl::StrCat(cmem.id(), "_fwd"), 0, line_pos, col_pos, fname));
  lnast.add_child(idx_asg_mfwd, Lnast_node::create_const(fwd));  // note: initialized

  auto idx_ta_mlat = lnast.add_child(parent_node, Lnast_node::create_assign("", 0, line_pos, col_pos, fname));
  lnast.add_child(idx_ta_mlat, Lnast_node::create_ref(absl::StrCat(cmem.id(), "_type"), 0, line_pos, col_pos, fname));
  lnast.add_child(idx_ta_mlat, Lnast_node::create_const(cmem.sync_read() ? 1 : 0));

  auto idx_asg_mwensize = lnast.add_child(parent_node, Lnast_node::create_assign("", 0, line_pos, col_pos, fname));
  lnast.add_child(idx_asg_mwensize, Lnast_node::create_ref(absl::StrCat(cmem.id(), "_wensize"), 0, line_pos, col_pos, fname));
  lnast.add_child(idx_asg_mwensize, Lnast_node::create_const(wensize_init));

  auto idx_asg_msize = lnast.add_child(parent_node, Lnast_node::create_assign("", 0, line_pos, col_pos, fname));
  lnast.add_child(idx_asg_msize, Lnast_node::create_ref(absl::StrCat(cmem.id(), "_size"), 0, line_pos, col_pos, fname));
  lnast.add_child(idx_asg_msize, Lnast_node::create_const(depth_str, 0, line_pos, col_pos, fname));  // note: initialized

  auto idx_ta_mrport = lnast.add_child(parent_node, Lnast_node::create_tuple_add("", 0, line_pos, col_pos, fname));
  lnast.add_child(idx_ta_mrport, Lnast_node::create_ref(absl::StrCat(cmem.id(), "_rdport"), 0, line_pos, col_pos, fname));

  // create if true scope so you have a inserted temporary stmt node
  // for foo_mem_din field variable initialization/declaration later
  auto idx_if = lnast.add_child(parent_node, Lnast_node::create_if("", 0, line_pos, col_pos, fname));
  lnast.add_child(idx_if, Lnast_node::create_ref("true", 0, line_pos, col_pos, fname));
  auto idx_stmts = lnast.add_child(idx_if, Lnast_node::create_stmts("", 0, line_pos, col_pos, fname));
  mem2initial_idx.insert_or_assign(cmem.id(), idx_stmts);

  mem2port_cnt.insert_or_assign(cmem.id(), -1);
}

void Inou_firrtl_module::handle_mport_declaration(Lnast& lnast, Lnast_nid& parent_node,
                                                  const firrtl::FirrtlPB_Statement_MemoryPort& mport,
                                                  const firrtl::FirrtlPB_Statement&            stmt) {
  // (void) parent_node;
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  const auto& mem_name = mport.memory_id();
  mport2mem.insert_or_assign(mport.id(), mem_name);

  mem2port_cnt[mem_name]++;
  auto clk_str         = get_expr_hier_name(lnast, parent_node, mport.expression(), stmt);
  auto adr_str         = get_expr_hier_name(lnast, parent_node, mport.memory_index(), stmt);
  clk_str              = name_prefix_modifier_flattener(clk_str, true);
  adr_str              = name_prefix_modifier_flattener(adr_str, true);
  auto port_cnt_str    = mem2port_cnt[mem_name];
  auto default_val_str = 0;

  // assign whatever adder/enable the mport variable comes with in the current scope, either top or scope
  auto idx_ta_maddr = lnast.add_child(parent_node, Lnast_node::create_tuple_add("", 0, line_pos, col_pos, fname));
  lnast.add_child(idx_ta_maddr, Lnast_node::create_ref(absl::StrCat(mem_name, "_addr"), 0, line_pos, col_pos, fname));
  lnast.add_child(idx_ta_maddr, Lnast_node::create_const(port_cnt_str));
  lnast.add_child(idx_ta_maddr, Lnast_node::create_ref(adr_str, 0, line_pos, col_pos, fname));

  // note: because any port might be declared inside a subscope but be used at upper scope, at the time you see a mport
  //       declaration, you must specify the port enable signal, even it's a masked write port. For the maksed write, a
  //       bit-vector wr_enable will be handled at the initialize_wr_mport_from_usage()
  auto idx_ta_men = lnast.add_child(parent_node, Lnast_node::create_tuple_add("", 0, line_pos, col_pos, fname));
  lnast.add_child(idx_ta_men, Lnast_node::create_ref(absl::StrCat(mem_name, "_enable"), 0, line_pos, col_pos, fname));
  lnast.add_child(idx_ta_men, Lnast_node::create_const(port_cnt_str));
  lnast.add_child(idx_ta_men, Lnast_node::create_const(1));

  // initialized port interfaces at the top scope
  I(mem2initial_idx.find(mem_name) != mem2initial_idx.end());
  auto& idx_initialize_stmts = mem2initial_idx[mem_name];

  auto idx_ta_mclk_ini = lnast.add_child(idx_initialize_stmts, Lnast_node::create_assign("", 0, line_pos, col_pos, fname));
  lnast.add_child(idx_ta_mclk_ini, Lnast_node::create_ref(absl::StrCat(mem_name, "_clock"), 0, line_pos, col_pos, fname));
  lnast.add_child(idx_ta_mclk_ini, Lnast_node::create_ref(clk_str, 0, line_pos, col_pos, fname));

  auto idx_ta_maddr_ini = lnast.add_child(idx_initialize_stmts, Lnast_node::create_tuple_add("", 0, line_pos, col_pos, fname));
  lnast.add_child(idx_ta_maddr_ini, Lnast_node::create_ref(absl::StrCat(mem_name, "_addr"), 0, line_pos, col_pos, fname));
  lnast.add_child(idx_ta_maddr_ini, Lnast_node::create_const(port_cnt_str));
  lnast.add_child(idx_ta_maddr_ini, Lnast_node::create_const(default_val_str));

  auto idx_ta_men_ini = lnast.add_child(idx_initialize_stmts, Lnast_node::create_tuple_add("", 0, line_pos, col_pos, fname));
  lnast.add_child(idx_ta_men_ini, Lnast_node::create_ref(absl::StrCat(mem_name, "_enable"), 0, line_pos, col_pos, fname));
  lnast.add_child(idx_ta_men_ini, Lnast_node::create_const(port_cnt_str));
  lnast.add_child(idx_ta_men_ini, Lnast_node::create_const(default_val_str));

  auto idx_ta_mrdport_ini = lnast.add_child(idx_initialize_stmts, Lnast_node::create_tuple_add("", 0, line_pos, col_pos));
  lnast.add_child(idx_ta_mrdport_ini, Lnast_node::create_ref(absl::StrCat(mem_name, "_rdport"), 0, line_pos, col_pos, fname));
  lnast.add_child(idx_ta_mrdport_ini, Lnast_node::create_const(port_cnt_str));
  lnast.add_child(idx_ta_mrdport_ini, Lnast_node::create_const("true", 0, line_pos, col_pos, fname));

  auto dir_case = mport.direction();
  if (dir_case
      == firrtl::FirrtlPB_Statement_MemoryPort_Direction::FirrtlPB_Statement_MemoryPort_Direction_MEMORY_PORT_DIRECTION_READ) {
    // rd_port: only need to initialize mem_res[rd_port] when you are sure it's a read mport
    init_mem_res(lnast, mem_name, std::to_string(port_cnt_str), stmt);
    // FIXME->sh:
    // if you already know it's a read mport,  you should let mport = mem_res[rd_port] here, or the mem_port_cnt might duplicately
    // count one more port_cnt, see the cases from ListBuffer.fir (search push_tail).
  } else if (dir_case
             == firrtl::FirrtlPB_Statement_MemoryPort_Direction::
                 FirrtlPB_Statement_MemoryPort_Direction_MEMORY_PORT_DIRECTION_WRITE) {
    // wr_port: noly need to initialize mem_din[wr_port] when you are sure it's a write mport
    init_mem_din(lnast, mem_name, std::to_string(port_cnt_str), stmt);
    I(mport2mask_bitvec.find(mport.id()) == mport2mask_bitvec.end());
    I(mport2mask_cnt.find(mport.id()) == mport2mask_cnt.end());
    mport2mask_bitvec.insert_or_assign(mport.id(), 1);
    mport2mask_cnt.insert_or_assign(mport.id(), 0);
  } else {
    // need to initialize both mem_din[wr_port] mem_res[res_port] when you are not sure the port type
    init_mem_res(lnast, mem_name, std::to_string(port_cnt_str), stmt);
    init_mem_din(lnast, mem_name, std::to_string(port_cnt_str), stmt);
  }
}

// we have to set the memory result bits so the later fir_bits pass could start propagate bits information from.
void Inou_firrtl_module::init_mem_res(Lnast& lnast, std::string_view mem_name, std::string_view port_cnt_str,
                                      const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  I(mem2initial_idx.find(mem_name) != mem2initial_idx.end());
  auto& idx_initialize_stmts = mem2initial_idx[mem_name];
  auto  it                   = mem2din_fields.find(mem_name);
  I(it != mem2din_fields.end());

  auto  mem_res_str     = absl::StrCat(mem_name, "_res");
  auto& hier_full_names = mem2din_fields[mem_name];
  for (const auto& hier_full_name : hier_full_names) {  // hier_full_name example: foo.bar.baz.20, the last field is bit
    std::vector<std::string> hier_sub_names;
    split_hier_name(hier_full_name, hier_sub_names);

    if (hier_sub_names.size() == 1) {  // it's a pure sclalar memory dout
      auto idx_ta = lnast.add_child(idx_initialize_stmts, Lnast_node::create_tuple_add("", 0, line_pos, col_pos, fname));
      lnast.add_child(idx_ta, Lnast_node::create_ref(mem_res_str, 0, line_pos, col_pos, fname));
      lnast.add_child(idx_ta, Lnast_node::create_const(port_cnt_str));
      lnast.add_child(idx_ta, Lnast_node::create_const("__ubits", 0, line_pos, col_pos, fname));
      lnast.add_child(idx_ta, Lnast_node::create_const(hier_sub_names.at(0)));
    } else {
      auto idx_ta = lnast.add_child(idx_initialize_stmts, Lnast_node::create_tuple_add("", 0, line_pos, col_pos, fname));
      lnast.add_child(idx_ta, Lnast_node::create_ref(mem_res_str, 0, line_pos, col_pos, fname));
      lnast.add_child(idx_ta, Lnast_node::create_const(port_cnt_str));
      uint8_t idx = 0;
      for (const auto& sub_name : hier_sub_names) {
        if (idx == hier_sub_names.size() - 1) {
          lnast.add_child(idx_ta, Lnast_node::create_const("__ubits", 0, line_pos, col_pos, fname));
        }
        lnast.add_child(idx_ta, Lnast_node::create_const(sub_name));
        idx++;
      }
    }
  }
}

void Inou_firrtl_module::init_mem_din(Lnast& lnast, std::string_view mem_name, std::string_view port_cnt_str,
                                      const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  auto default_val_str = 0;
  I(mem2initial_idx.find(mem_name) != mem2initial_idx.end());
  auto& idx_initialize_stmts = mem2initial_idx[mem_name];
  auto  it                   = mem2din_fields.find(mem_name);
  I(it != mem2din_fields.end());

  if (it->second.at(0) == ".") {  // din is scalar, the din_fields starts with something like .17
    auto idx_ta_mdin_ini = lnast.add_child(idx_initialize_stmts, Lnast_node::create_tuple_add("", 0, line_pos, col_pos, fname));
    lnast.add_child(idx_ta_mdin_ini, Lnast_node::create_ref(absl::StrCat(mem_name, "_din"), 0, line_pos, col_pos, fname));
    lnast.add_child(idx_ta_mdin_ini, Lnast_node::create_const(port_cnt_str));
    lnast.add_child(idx_ta_mdin_ini, Lnast_node::create_const(default_val_str));
  } else {  // din is tuple
    auto& hier_full_names = mem2din_fields[mem_name];
    for (const auto& hier_full_name : hier_full_names) {  // hier_full_name example: foo.bar.baz.20, the last field is bit
      std::vector<std::string> hier_sub_names;
      auto                     found = hier_full_name.rfind('.');  // get rid of last bit field
      split_hier_name(hier_full_name.substr(0, found), hier_sub_names);

      auto idx_ta_mdin_ini = lnast.add_child(idx_initialize_stmts, Lnast_node::create_tuple_add("", 0, line_pos, col_pos, fname));
      lnast.add_child(idx_ta_mdin_ini, Lnast_node::create_ref(absl::StrCat(mem_name, "_din"), 0, line_pos, col_pos, fname));
      lnast.add_child(idx_ta_mdin_ini, Lnast_node::create_const(port_cnt_str));

      for (const auto& sub_name : hier_sub_names) {
        lnast.add_child(idx_ta_mdin_ini, Lnast_node::create_const(sub_name));
      }

      lnast.add_child(idx_ta_mdin_ini, Lnast_node::create_const(default_val_str));
    }
  }
}

void Inou_firrtl_module::create_module_inst(Lnast& lnast, const firrtl::FirrtlPB_Statement_Instance& inst, Lnast_nid& parent_node,
                                            const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  auto temp_var_name2 = absl::StrCat("F", std::to_string(tmp_var_cnt));
  tmp_var_cnt++;
  auto inst_name = inst.id();
  if (inst.id().substr(0, 2) == "_T") {
    inst_name = absl::StrCat("_.", inst_name);
  }
  auto inp_name = absl::StrCat("itup_", inst_name);
  auto out_name = absl::StrCat("otup_", inst_name);

  // add_lnast_assign(lnast, parent_node, inp_name, "0", stmt); // the sub-input might be assigned within if-else subscope, so need
  // this trivial initialization for SSA to work

  auto idx_dot = lnast.add_child(parent_node, Lnast_node::create_attr_get("", 0, line_pos, col_pos, fname));
  lnast.add_child(idx_dot, Lnast_node::create_ref(temp_var_name2, 0, line_pos, col_pos, fname));
  lnast.add_child(idx_dot, Lnast_node::create_ref(inp_name, 0, line_pos, col_pos, fname));
  lnast.add_child(idx_dot, Lnast_node::create_const("__last_value", 0, line_pos, col_pos, fname));
  wire_names.insert(temp_var_name2);

  auto idx_fncall = lnast.add_child(parent_node, Lnast_node::create_func_call("", 0, line_pos, col_pos, fname));
  lnast.add_child(idx_fncall, Lnast_node::create_ref(out_name, 0, line_pos, col_pos, fname));
  lnast.add_child(idx_fncall, Lnast_node::create_ref(absl::StrCat("__firrtl_", inst.module_id()), 0, line_pos, col_pos, fname));
  lnast.add_child(idx_fncall, Lnast_node::create_ref(temp_var_name2, 0, line_pos, col_pos, fname));

  const auto& module_name = inst.module_id();
  inst2module[inst.id()]  = module_name;

  const auto& inputs_set = Inou_firrtl::glob_info.module2inputs[module_name];
  for (const auto& itr : inputs_set) {
    auto hier_name          = absl::StrCat(inst.id(), ".", itr);
    auto flattened_out_name = name_prefix_modifier_flattener(hier_name, false);
    create_tuple_add_for_instance_itup(lnast, parent_node, hier_name, flattened_out_name, stmt);
  }

  const auto& outputs_map = Inou_firrtl::glob_info.module2outputs[module_name];
  for (const auto& [key, val] : outputs_map) {
    auto hier_name_r        = absl::StrCat(inst.id(), ".", key);
    auto flattened_inp_name = name_prefix_modifier_flattener(hier_name_r, false);
    create_tuple_get_for_instance_otup(lnast, parent_node, hier_name_r, flattened_inp_name, stmt);
    setup_scalar_bits(lnast, flattened_inp_name, val.first, parent_node, val.second, stmt);
  }
}

/* No mux node type exists in LNAST. To support FIRRTL muxes, we instead
 * map a mux to an if-else statement whose condition is the same condition
 * as the first argument (the condition) of the mux. */
void Inou_firrtl_module::handle_mux_assign(Lnast& lnast, const firrtl::FirrtlPB_Expression& expr, Lnast_nid& parent_node,
                                           std::string_view lhs, const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  I(lnast.get_data(parent_node).type.is_stmts());

  bool is_runtime_idx_t = false;
  bool is_runtime_idx_f = false;

  const auto& expr_t         = expr.mux().t_value();
  const auto& expr_f         = expr.mux().f_value();
  std::string expr_str_tmp_t = get_expr_hier_name(expr_t, is_runtime_idx_t);  // FIXME: can we get the is_runtime_idx more elegent?
  std::string expr_str_tmp_f = get_expr_hier_name(expr_f, is_runtime_idx_f);

  std::string t_str;
  std::string f_str;

  if (is_runtime_idx_t) {
    auto tmp_var = create_tmp_var();
    handle_rhs_runtime_idx(lnast, parent_node, tmp_var, expr_str_tmp_t, expr_t, stmt);
    t_str = tmp_var;
    ;
  } else {
    t_str = get_expr_hier_name(lnast, parent_node, expr_t, stmt);
  }

  if (is_runtime_idx_f) {
    auto tmp_var = create_tmp_var();
    handle_rhs_runtime_idx(lnast, parent_node, tmp_var, expr_str_tmp_f, expr_f, stmt);
    f_str = tmp_var;
    ;
  } else {
    f_str = get_expr_hier_name(lnast, parent_node, expr_f, stmt);
  }

  // preparation: get head of the tuple name so you know entry for the var2flip table
  auto        p           = t_str.find_first_of('.');
  bool        is_instance = false;
  std::string tup_head_t;
  std::string tup_rest_t;
  std::string instance_name;
  auto        module_name = lnast.get_top_module_name();
  if (p != std::string::npos) {
    tup_head_t = t_str.substr(0, p);
    tup_rest_t = t_str.substr(p + 1);
    // note: get the sub-instance module_name, and use that to get the var2flip table
    if (inst2module.contains(tup_head_t)) {  // it's an instance io
      instance_name = tup_head_t;
      module_name   = inst2module[tup_head_t];
      is_instance   = true;
      t_str         = tup_rest_t;  // get rid of "inst_name." prefix
      auto p2       = tup_rest_t.find_first_of('.');
      tup_head_t    = tup_rest_t.substr(0, p2);
      tup_rest_t    = tup_rest_t.substr(p2 + 1);
    }
  } else {
    tup_head_t = t_str;
  }

  std::vector<std::string> head_chopped_hier_names;
  auto const&              module_var2flip = Inou_firrtl::glob_info.var2flip[module_name];
  auto                     found           = module_var2flip.find(tup_head_t) != module_var2flip.end();
  if (found) {  // check global io flip table
    for (auto& [var, set] : module_var2flip) {
      if (var == tup_head_t) {
        for (auto& [hier_name, flipped] : set) {
          auto pos = hier_name.find(t_str);
          if (pos != std::string::npos && hier_name != t_str && hier_name.at(t_str.size()) == '.') {
            head_chopped_hier_names.push_back(hier_name.substr(t_str.size() + 1));
          }
        }
      }
    }
  } else {  // check local wire flip table
    auto found2 = var2flip.find(tup_head_t) != var2flip.end();
    if (found2) {
      for (auto& [var, set] : var2flip) {
        if (var == tup_head_t) {
          for (auto& [hier_name, flipped] : set) {
            auto pos = hier_name.find(t_str);
            if (pos != std::string::npos && hier_name != t_str && hier_name.at(t_str.size()) == '.') {
              head_chopped_hier_names.push_back(hier_name.substr(t_str.size() + 1));
            }
          }
        }
      }
    }
  }

  // most cases
  if (head_chopped_hier_names.size() == 0) {
    auto idx_pre_asg = lnast.add_child(parent_node, Lnast_node::create_assign("", 0, line_pos, col_pos, fname));
    lnast.add_child(idx_pre_asg, Lnast_node::create_ref(lhs, 0, line_pos, col_pos, fname));
    lnast.add_child(idx_pre_asg, Lnast_node::create_const("0b?", 0, line_pos, col_pos, fname));
    wire_names.insert(std::string{lhs});

    auto cond_str = expr_str_flattened_or_tg(lnast, parent_node, expr.mux().condition(), stmt);

    auto idx_mux_if = lnast.add_child(parent_node, Lnast_node::create_if("", 0, line_pos, col_pos, fname));
    attach_expr_str2node(lnast, cond_str, idx_mux_if, stmt);

    auto idx_stmt_t = lnast.add_child(idx_mux_if, Lnast_node::create_stmts("", 0, line_pos, col_pos, fname));
    auto idx_stmt_f = lnast.add_child(idx_mux_if, Lnast_node::create_stmts("", 0, line_pos, col_pos, fname));

    t_str = name_prefix_modifier_flattener(t_str, true);
    f_str = name_prefix_modifier_flattener(f_str, true);
    if (var2last_value.find(t_str) != var2last_value.end()) {
      t_str = var2last_value[t_str];
    }
    if (var2last_value.find(f_str) != var2last_value.end()) {
      f_str = var2last_value[f_str];
    }

    if (is_instance) {
      t_str = absl::StrCat(instance_name, "_", t_str);
    }

    add_lnast_assign(lnast, idx_stmt_t, lhs, t_str, stmt);
    add_lnast_assign(lnast, idx_stmt_f, lhs, f_str, stmt);
    return;
  }

  // rare cases

  for (const auto& head_chopped_hier_name : head_chopped_hier_names) {
    auto new_lhs     = absl::StrCat(lhs, ".", head_chopped_hier_name);
    new_lhs          = name_prefix_modifier_flattener(new_lhs, false);
    auto idx_pre_asg = lnast.add_child(parent_node, Lnast_node::create_assign("", 0, line_pos, col_pos, fname));
    lnast.add_child(idx_pre_asg, Lnast_node::create_ref(new_lhs, 0, line_pos, col_pos, fname));
    lnast.add_child(idx_pre_asg, Lnast_node::create_const("0b?", 0, line_pos, col_pos, fname));
    wire_names.insert(new_lhs);

    auto cond_str = expr_str_flattened_or_tg(lnast, parent_node, expr.mux().condition(), stmt);

    auto idx_mux_if = lnast.add_child(parent_node, Lnast_node::create_if("", 0, line_pos, col_pos, fname));
    attach_expr_str2node(lnast, cond_str, idx_mux_if, stmt);

    auto idx_stmt_t = lnast.add_child(idx_mux_if, Lnast_node::create_stmts("", 0, line_pos, col_pos, fname));
    auto idx_stmt_f = lnast.add_child(idx_mux_if, Lnast_node::create_stmts("", 0, line_pos, col_pos, fname));

    std::string new_t_str;
    if (is_instance) {
      new_t_str = absl::StrCat(instance_name, ".", t_str, ".", head_chopped_hier_name);
    } else {
      new_t_str = absl::StrCat(t_str, ".", head_chopped_hier_name);
    }
    auto new_f_str = absl::StrCat(f_str, ".", head_chopped_hier_name);
    new_t_str      = name_prefix_modifier_flattener(new_t_str, true);
    new_f_str      = name_prefix_modifier_flattener(new_f_str, true);
    add_lnast_assign(lnast, idx_stmt_t, new_lhs, new_t_str, stmt);
    add_lnast_assign(lnast, idx_stmt_f, new_lhs, new_f_str, stmt);
  }

  if (!is_instance) {
    auto                                               lhs_str = std::string{lhs};
    absl::flat_hash_set<std::pair<std::string, bool>>* tup_set;
    if (var2flip.find(lhs_str) != var2flip.end()) {
      tup_set = &var2flip[lhs_str];
    } else {
      absl::flat_hash_set<std::pair<std::string, bool>> empty_set;
      var2flip.insert_or_assign(lhs_str, empty_set);
      tup_set = &var2flip[lhs_str];
    }

    for (const auto& head_chopped_hier_name : head_chopped_hier_names) {
      tup_set->insert(std::make_pair(absl::StrCat(lhs, ".", head_chopped_hier_name), false));
    }
  }
}

/* ValidIfs get detected as the RHS of an assign statement and we can't have a child of
 * an assign be an if-typed node. Thus, we have to detect ahead of time if it is a validIf
 * if we're doing an assign. If that is the case, do this instead of using ListExprType().*/
void Inou_firrtl_module::handle_valid_if_assign(Lnast& lnast, const firrtl::FirrtlPB_Expression& expr, Lnast_nid& parent_node,
                                                std::string_view lhs, const firrtl::FirrtlPB_Statement& stmt) {
  I(lnast.get_data(parent_node).type.is_stmts());
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  // FIXME->sh: do the trick to declare variable with the validif value, hope
  // this could make the validif to fit the role of "else mux"
  init_expr_add(lnast, expr.valid_if().value(), parent_node, lhs, stmt);

  auto cond_str = expr_str_flattened_or_tg(lnast, parent_node, expr.valid_if().condition(), stmt);
  auto idx_v_if = lnast.add_child(parent_node, Lnast_node::create_if("", 0, line_pos, col_pos, fname));
  lnast.add_child(idx_v_if, Lnast_node::create_ref(cond_str));

  auto idx_stmt_t = lnast.add_child(idx_v_if, Lnast_node::create_stmts("", 0, line_pos, col_pos, fname));

  init_expr_add(lnast, expr.valid_if().value(), idx_stmt_t, lhs, stmt);
}

// ----------------- primitive op start -------------------------------------------------------------------------------
void Inou_firrtl_module::handle_unary_op(Lnast& lnast, const firrtl::FirrtlPB_Expression_PrimOp& op, Lnast_nid& parent_node,
                                         std::string_view lhs, const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  I(lnast.get_data(parent_node).type.is_stmts());
  I(op.arg_size() == 1);

  auto lhs_str = lhs;
  auto e1_str  = expr_str_flattened_or_tg(lnast, parent_node, op.arg(0), stmt);
  auto idx_not = lnast.add_child(parent_node, Lnast_node::create_func_call("", 0, line_pos, col_pos, fname));  // "__fir_not"));
  lnast.add_child(idx_not, Lnast_node::create_ref(lhs_str, 0, line_pos, col_pos, fname));
  lnast.add_child(idx_not, Lnast_node::create_const("__fir_not", 0, line_pos, col_pos, fname));
  attach_expr_str2node(lnast, e1_str, idx_not, stmt);
}

void Inou_firrtl_module::handle_and_reduce_op(Lnast& lnast, const firrtl::FirrtlPB_Expression_PrimOp& op, Lnast_nid& parent_node,
                                              std::string_view lhs, const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  I(lnast.get_data(parent_node).type.is_stmts());
  I(op.arg_size() == 1);

  auto lhs_str  = lhs;
  auto e1_str   = expr_str_flattened_or_tg(lnast, parent_node, op.arg(0), stmt);
  auto idx_andr = lnast.add_child(parent_node, Lnast_node::create_func_call("", 0, line_pos, col_pos, fname));  // "__fir_andr"));
  lnast.add_child(idx_andr, Lnast_node::create_ref(lhs_str, 0, line_pos, col_pos, fname));
  lnast.add_child(idx_andr, Lnast_node::create_const("__fir_andr", 0, line_pos, col_pos, fname));
  // lnast.add_child(idx_andr, Lnast_node::create_ref(e1_str));
  attach_expr_str2node(lnast, e1_str, idx_andr, stmt);
}

void Inou_firrtl_module::handle_or_reduce_op(Lnast& lnast, const firrtl::FirrtlPB_Expression_PrimOp& op, Lnast_nid& parent_node,
                                             std::string_view lhs, const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  I(lnast.get_data(parent_node).type.is_stmts());
  I(op.arg_size() == 1);

  auto lhs_str = lhs;
  auto e1_str  = expr_str_flattened_or_tg(lnast, parent_node, op.arg(0), stmt);
  auto idx_orr = lnast.add_child(parent_node, Lnast_node::create_func_call("", 0, line_pos, col_pos, fname));  // "__fir_orr"));

  lnast.add_child(idx_orr, Lnast_node::create_ref(lhs_str, 0, line_pos, col_pos, fname));
  lnast.add_child(idx_orr, Lnast_node::create_const("__fir_orr", 0, line_pos, col_pos, fname));
  lnast.add_child(idx_orr, Lnast_node::create_ref(e1_str, 0, line_pos, col_pos, fname));
}

void Inou_firrtl_module::handle_xor_reduce_op(Lnast& lnast, const firrtl::FirrtlPB_Expression_PrimOp& op, Lnast_nid& parent_node,
                                              std::string_view lhs, const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  I(lnast.get_data(parent_node).type.is_stmts());
  I(op.arg_size() == 1);

  auto lhs_str  = lhs;
  auto e1_str   = expr_str_flattened_or_tg(lnast, parent_node, op.arg(0), stmt);
  auto idx_xorr = lnast.add_child(parent_node, Lnast_node::create_func_call("", 0, line_pos, col_pos, fname));  // "__fir_xorr"));
  lnast.add_child(idx_xorr, Lnast_node::create_ref(lhs_str, 0, line_pos, col_pos, fname));
  lnast.add_child(idx_xorr, Lnast_node::create_const("__fir_xorr", 0, line_pos, col_pos, fname));
  lnast.add_child(idx_xorr, Lnast_node::create_ref(e1_str, 0, line_pos, col_pos, fname));
}

void Inou_firrtl_module::handle_negate_op(Lnast& lnast, const firrtl::FirrtlPB_Expression_PrimOp& op, Lnast_nid& parent_node,
                                          std::string_view lhs, const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  I(lnast.get_data(parent_node).type.is_stmts());
  I(op.arg_size() == 1);

  auto lhs_str = lhs;
  auto e1_str  = expr_str_flattened_or_tg(lnast, parent_node, op.arg(0), stmt);
  auto idx_neg = lnast.add_child(parent_node, Lnast_node::create_func_call("", 0, line_pos, col_pos, fname));  // "__fir_neg"));
  lnast.add_child(idx_neg, Lnast_node::create_ref(lhs_str, 0, line_pos, col_pos, fname));
  lnast.add_child(idx_neg, Lnast_node::create_const("__fir_neg", 0, line_pos, col_pos, fname));
  lnast.add_child(idx_neg, Lnast_node::create_ref(e1_str, 0, line_pos, col_pos, fname));
}

void Inou_firrtl_module::handle_conv_op(Lnast& lnast, const firrtl::FirrtlPB_Expression_PrimOp& op, Lnast_nid& parent_node,
                                        std::string_view lhs, const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  I(lnast.get_data(parent_node).type.is_stmts());
  I(op.arg_size() == 1);

  auto lhs_str = lhs;
  auto e1_str  = expr_str_flattened_or_tg(lnast, parent_node, op.arg(0), stmt);
  auto idx_cvt = lnast.add_child(parent_node, Lnast_node::create_func_call("", 0, line_pos, col_pos, fname));  // "__fir_cvt"));
  lnast.add_child(idx_cvt, Lnast_node::create_ref(lhs_str, 0, line_pos, col_pos, fname));
  lnast.add_child(idx_cvt, Lnast_node::create_const("__fir_cvt", 0, line_pos, col_pos, fname));
  lnast.add_child(idx_cvt, Lnast_node::create_ref(e1_str, 0, line_pos, col_pos, fname));
}

void Inou_firrtl_module::handle_extract_bits_op(Lnast& lnast, const firrtl::FirrtlPB_Expression_PrimOp& op, Lnast_nid& parent_node,
                                                std::string_view lhs, const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  I(lnast.get_data(parent_node).type.is_stmts());
  I(op.arg_size() == 1 && op.const__size() == 2);

  auto lhs_str = lhs;
  auto e1_str  = expr_str_flattened_or_tg(lnast, parent_node, op.arg(0), stmt);
  auto idx_bits_exct
      = lnast.add_child(parent_node, Lnast_node::create_func_call("", 0, line_pos, col_pos, fname));  // "__fir_bits"));

  lnast.add_child(idx_bits_exct, Lnast_node::create_ref(lhs_str, 0, line_pos, col_pos, fname));
  lnast.add_child(idx_bits_exct, Lnast_node::create_const("__fir_bits", 0, line_pos, col_pos, fname));
  lnast.add_child(idx_bits_exct, Lnast_node::create_ref(e1_str, 0, line_pos, col_pos, fname));
  lnast.add_child(idx_bits_exct, Lnast_node::create_const(op.const_(0).value()));
  lnast.add_child(idx_bits_exct, Lnast_node::create_const(op.const_(1).value()));
}

void Inou_firrtl_module::handle_head_op(Lnast& lnast, const firrtl::FirrtlPB_Expression_PrimOp& op, Lnast_nid& parent_node,
                                        std::string_view lhs, const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  I(lnast.get_data(parent_node).type.is_stmts());
  I(op.arg_size() == 1 && op.const__size() == 1);

  auto lhs_str  = lhs;
  auto e1_str   = expr_str_flattened_or_tg(lnast, parent_node, op.arg(0), stmt);
  auto idx_head = lnast.add_child(parent_node, Lnast_node::create_func_call("", 0, line_pos, col_pos, fname));  // "__fir_head"));
  lnast.add_child(idx_head, Lnast_node::create_ref(lhs_str, 0, line_pos, col_pos, fname));
  lnast.add_child(idx_head, Lnast_node::create_const("__fir_head", 0, line_pos, col_pos, fname));
  lnast.add_child(idx_head, Lnast_node::create_ref(e1_str, 0, line_pos, col_pos, fname));
  lnast.add_child(idx_head, Lnast_node::create_const(op.const_(0).value()));
}

void Inou_firrtl_module::handle_tail_op(Lnast& lnast, const firrtl::FirrtlPB_Expression_PrimOp& op, Lnast_nid& parent_node,
                                        std::string_view lhs, const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  I(lnast.get_data(parent_node).type.is_stmts());
  I(op.arg_size() == 1 && op.const__size() == 1);
  auto lhs_str = lhs;
  auto e1_str  = expr_str_flattened_or_tg(lnast, parent_node, op.arg(0), stmt);

  auto idx_tail = lnast.add_child(parent_node, Lnast_node::create_func_call("", 0, line_pos, col_pos, fname));  // "__fir_tail"));
  lnast.add_child(idx_tail, Lnast_node::create_ref(lhs_str, 0, line_pos, col_pos, fname));
  lnast.add_child(idx_tail, Lnast_node::create_const("__fir_tail", 0, line_pos, col_pos, fname));
  lnast.add_child(idx_tail, Lnast_node::create_ref(e1_str, 0, line_pos, col_pos, fname));
  lnast.add_child(idx_tail, Lnast_node::create_const(op.const_(0).value()));
}

void Inou_firrtl_module::handle_concat_op(Lnast& lnast, const firrtl::FirrtlPB_Expression_PrimOp& op, Lnast_nid& parent_node,
                                          std::string_view lhs, const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  I(lnast.get_data(parent_node).type.is_stmts());
  I(op.arg_size() == 2);

  auto lhs_str = lhs;
  auto e1_str  = expr_str_flattened_or_tg(lnast, parent_node, op.arg(0), stmt);
  auto e2_str  = expr_str_flattened_or_tg(lnast, parent_node, op.arg(1), stmt);

  auto idx_concat = lnast.add_child(parent_node, Lnast_node::create_func_call("", 0, line_pos, col_pos, fname));  // "__fir_cat"));
  lnast.add_child(idx_concat, Lnast_node::create_ref(lhs_str, 0, line_pos, col_pos, fname));
  lnast.add_child(idx_concat, Lnast_node::create_const("__fir_cat", 0, line_pos, col_pos, fname));
  attach_expr_str2node(lnast, e1_str, idx_concat, stmt);
  attach_expr_str2node(lnast, e2_str, idx_concat, stmt);
}

void Inou_firrtl_module::handle_pad_op(Lnast& lnast, const firrtl::FirrtlPB_Expression_PrimOp& op, Lnast_nid& parent_node,
                                       std::string_view lhs, const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  I(lnast.get_data(parent_node).type.is_stmts());
  I(op.arg_size() == 1 && op.const__size() == 1);

  auto lhs_str = lhs;
  auto e1_str  = expr_str_flattened_or_tg(lnast, parent_node, op.arg(0), stmt);

  auto idx_pad = lnast.add_child(parent_node, Lnast_node::create_func_call("", 0, line_pos, col_pos, fname));  // "__fir_pad"));
  lnast.add_child(idx_pad, Lnast_node::create_ref(lhs_str, 0, line_pos, col_pos, fname));
  lnast.add_child(idx_pad, Lnast_node::create_const("__fir_pad", 0, line_pos, col_pos, fname));
  lnast.add_child(idx_pad, Lnast_node::create_ref(e1_str, 0, line_pos, col_pos, fname));
  lnast.add_child(idx_pad, Lnast_node::create_const(op.const_(0).value()));
}

void Inou_firrtl_module::handle_binary_op(Lnast& lnast, const firrtl::FirrtlPB_Expression_PrimOp& op, Lnast_nid& parent_node,
                                          std::string_view lhs, const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  I(lnast.get_data(parent_node).type.is_stmts());
  I(op.arg_size() == 2);

  auto e1_str = expr_str_flattened_or_tg(lnast, parent_node, op.arg(0), stmt);
  auto e2_str = expr_str_flattened_or_tg(lnast, parent_node, op.arg(1), stmt);

  Lnast_nid idx_primop;
  auto      sub_it = Inou_firrtl::op2firsub.find(op.op());
  I(sub_it != Inou_firrtl::op2firsub.end());

  idx_primop = lnast.add_child(parent_node, Lnast_node::create_func_call("", 0, line_pos, col_pos, fname));
  lnast.add_child(idx_primop, Lnast_node::create_ref(lhs, 0, line_pos, col_pos, fname));
  lnast.add_child(idx_primop, Lnast_node::create_const(sub_it->second));

  attach_expr_str2node(lnast, e1_str, idx_primop, stmt);
  attach_expr_str2node(lnast, e2_str, idx_primop, stmt);
}

void Inou_firrtl_module::handle_static_shift_op(Lnast& lnast, const firrtl::FirrtlPB_Expression_PrimOp& op, Lnast_nid& parent_node,
                                                std::string_view lhs, const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  I(lnast.get_data(parent_node).type.is_stmts());
  I(op.arg_size() == 1 || op.const__size() == 1);

  auto lhs_str = lhs;
  auto e1_str  = expr_str_flattened_or_tg(lnast, parent_node, op.arg(0), stmt);

  Lnast_nid idx_shift;
  auto      sub_it = Inou_firrtl::op2firsub.find(op.op());
  I(sub_it != Inou_firrtl::op2firsub.end());

  idx_shift = lnast.add_child(parent_node, Lnast_node::create_func_call("", 0, line_pos, col_pos, fname));

  lnast.add_child(idx_shift, Lnast_node::create_ref(lhs_str, 0, line_pos, col_pos, fname));
  lnast.add_child(idx_shift, Lnast_node::create_const(sub_it->second));
  attach_expr_str2node(lnast, e1_str, idx_shift, stmt);
  lnast.add_child(idx_shift, Lnast_node::create_const(op.const_(0).value()));
}

void Inou_firrtl_module::handle_as_usint_op(Lnast& lnast, const firrtl::FirrtlPB_Expression_PrimOp& op, Lnast_nid& parent_node,
                                            std::string_view lhs, const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  I(op.arg_size() == 1 && op.const__size() == 0);
  auto lhs_str = lhs;
  auto e1_str  = expr_str_flattened_or_tg(lnast, parent_node, op.arg(0), stmt);

  auto sub_it = Inou_firrtl::op2firsub.find(op.op());
  I(sub_it != Inou_firrtl::op2firsub.end());

  auto idx_conv = lnast.add_child(parent_node, Lnast_node::create_func_call("", 0, line_pos, col_pos, fname));

  lnast.add_child(idx_conv, Lnast_node::create_ref(lhs_str, 0, line_pos, col_pos, fname));
  lnast.add_child(idx_conv, Lnast_node::create_const(sub_it->second));
  attach_expr_str2node(lnast, e1_str, idx_conv, stmt);
}

void Inou_firrtl_module::handle_type_conv_op(Lnast& lnast, const firrtl::FirrtlPB_Expression_PrimOp& op, Lnast_nid& parent_node,
                                             std::string_view lhs, const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  I(op.arg_size() == 1 && op.const__size() == 0);
  auto lhs_str = lhs;
  auto e1_str  = expr_str_flattened_or_tg(lnast, parent_node, op.arg(0), stmt);
  auto sub_it  = Inou_firrtl::op2firsub.find(op.op());
  I(sub_it != Inou_firrtl::op2firsub.end());

  auto idx_conv = lnast.add_child(parent_node, Lnast_node::create_func_call("", 0, line_pos, col_pos, fname));

  lnast.add_child(idx_conv, Lnast_node::create_ref(lhs_str, 0, line_pos, col_pos, fname));
  lnast.add_child(idx_conv, Lnast_node::create_const(sub_it->second));
  // lnast.add_child(idx_conv, Lnast_node::create_const(e1_str));
  auto first_char = e1_str[0];
  if (isdigit(first_char) || first_char == '-' || first_char == '+') {
    lnast.add_child(idx_conv, Lnast_node::create_const(e1_str));
  } else {
    lnast.add_child(idx_conv, Lnast_node::create_ref(e1_str, 0, line_pos, col_pos, fname));
  }
}

// --------------------------------------- end of primitive op ----------------------------------------------

void Inou_firrtl_module::initialize_rd_mport_from_usage(Lnast& lnast, Lnast_nid& parent_node, std::string_view mport_name,
                                                        const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  auto mem_name     = mport2mem[mport_name];
  auto mem_port_str = mem2port_cnt[mem_name];

  auto it = mport_usage_visited.find(mport_name);
  if (it == mport_usage_visited.end()) {
    mport_usage_visited.emplace(mport_name);

    auto idx_ta_mrdport = lnast.add_child(parent_node, Lnast_node::create_tuple_add("", 0, line_pos, col_pos, fname));
    lnast.add_child(idx_ta_mrdport, Lnast_node::create_ref(absl::StrCat(mem_name, "_rdport"), 0, line_pos, col_pos, fname));
    lnast.add_child(idx_ta_mrdport, Lnast_node::create_const(mem_port_str));
    lnast.add_child(idx_ta_mrdport, Lnast_node::create_const("true", 0, line_pos, col_pos, fname));

    auto it2 = mem2rd_mports.find(mem_name);
    if (it2 == mem2rd_mports.end()) {
      mem2rd_mports.insert(std::pair<std::string, std::vector<std::pair<std::string, uint8_t>>>(
          mem_name,
          {std::pair(std::string(mport_name), mem2port_cnt[mem_name])}));
    } else {
      mem2rd_mports[mem_name].emplace_back(std::pair(mport_name, mem2port_cnt[mem_name]));
    }
    mem2rd_mport_loc.insert({std::string(mport_name), loc_info /*std::make_pair(line_pos, col_pos, fname)*/});

    // note: I defer the handling of rd_mport = mem_res[rd_port] to the interface connection phase.
    //       the reason is we need to do tuple field recovering mem_din, like
    //       rd_mport = mem_din[some_wr_port]
    //       rd_mport := mem_res[rd_port]
    //
    //       but the wr_port are not necessary happened before rd_mport

    // deprecated
    // I(mem2initial_idx.find(mem_name) != mem2initial_idx.end());
    // auto &idx_initialize_stmts = mem2initial_idx[mem_name];
    // auto idx_tg = lnast.add_child(idx_initialize_stmts, Lnast_node::create_tuple_get("" , 0, line_pos, col_pos, fname));
    // auto temp_var_name = create_tmp_var(lnast);
    // lnast.add_child(idx_tg, Lnast_node::create_ref(temp_var_name, 0, line_pos, col_pos, fname));
    // lnast.add_child(idx_tg, Lnast_node::create_ref(absl::StrCat(mem_name, "_res"), 0, line_pos, col_pos, fname)));
    // lnast.add_child(idx_tg, Lnast_node::create_const(mem_port_str));

    // auto idx_asg = lnast.add_child(idx_initialize_stmts, Lnast_node::create_assign("" , 0, line_pos, col_pos, fname));
    // lnast.add_child(idx_asg, Lnast_node::create_ref(lnast.add_string(mport_name), 0, line_pos, col_pos, fname));
    // lnast.add_child(idx_asg, Lnast_node::create_ref(temp_var_name, 0, line_pos, col_pos, fname));
  }
}

void Inou_firrtl_module::initialize_wr_mport_from_usage(Lnast& lnast, Lnast_nid& parent_node, std::string_view mport_name,
                                                        const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }
  auto mem_name = mport2mem[mport_name];
  auto port_cnt = mem2port_cnt[mem_name];

  //
  auto it = mport_usage_visited.find(mport_name);
  if (it == mport_usage_visited.end()) {
    auto& idx_initialize_stmts = mem2initial_idx[mem_name];
    mem2one_wr_mport.insert_or_assign(mem_name, port_cnt);

    mport_usage_visited.emplace(mport_name);

    auto idx_ta_mrdport = lnast.add_child(idx_initialize_stmts, Lnast_node::create_tuple_add("", 0, line_pos, col_pos, fname));
    lnast.add_child(idx_ta_mrdport, Lnast_node::create_ref(absl::StrCat(mem_name, "_rdport"), 0, line_pos, col_pos, fname));
    lnast.add_child(idx_ta_mrdport, Lnast_node::create_const(port_cnt));
    lnast.add_child(idx_ta_mrdport, Lnast_node::create_const("false", 0, line_pos, col_pos, fname));

    auto idx_attr_get     = lnast.add_child(idx_initialize_stmts, Lnast_node::create_attr_get("", 0, line_pos, col_pos, fname));
    auto mport_last_value = create_tmp_var();
    lnast.add_child(idx_attr_get, Lnast_node::create_ref(mport_last_value, 0, line_pos, col_pos, fname));
    lnast.add_child(idx_attr_get, Lnast_node::create_ref(mport_name, 0, line_pos, col_pos, fname));
    lnast.add_child(idx_attr_get, Lnast_node::create_const("__last_value", 0, line_pos, col_pos, fname));
    wire_names.insert(mport_last_value);

    auto idx_ta_mdin = lnast.add_child(idx_initialize_stmts, Lnast_node::create_tuple_add("", 0, line_pos, col_pos, fname));
    lnast.add_child(idx_ta_mdin, Lnast_node::create_ref(absl::StrCat(mem_name, "_din"), 0, line_pos, col_pos, fname));
    lnast.add_child(idx_ta_mdin, Lnast_node::create_const(port_cnt));
    lnast.add_child(idx_ta_mdin, Lnast_node::create_ref(mport_last_value, 0, line_pos, col_pos, fname));
  }

  (void)parent_node;
}

void Inou_firrtl_module::set_leaf_type(std::string_view subname, std::string_view full_name, size_t prev,
                                       std::vector<std::pair<std::string, Inou_firrtl_module::Leaf_type>>& hier_subnames) {
  if (prev == 0) {
    hier_subnames.emplace_back(std::pair(subname, Leaf_type::Ref));
  } else if (full_name[prev - 1] == '.') {
    auto first_char = subname[0];
    if (isdigit(first_char) || first_char == '-' || first_char == '+') {
      hier_subnames.emplace_back(std::pair(subname, Leaf_type::Const_num));
    } else {
      hier_subnames.emplace_back(std::pair(subname, Leaf_type::Const_str));
    }
  } else if (full_name[prev - 1] == '[') {
    auto first_char = subname[0];
    if (isdigit(first_char) || first_char == '-' || first_char == '+') {
      hier_subnames.emplace_back(std::pair(subname, Leaf_type::Const_num));
    } else {
      hier_subnames.emplace_back(std::pair(subname, Leaf_type::Ref));
    }
  }
}

void Inou_firrtl_module::split_hier_name(std::string_view full_name, std::vector<std::string>& hier_subnames) {
  std::size_t prev = 0;
  std::size_t pos  = full_name.find('.', prev);

  while (pos != std::string::npos) {
    if (pos > prev) {
      auto subname = full_name.substr(prev, pos - prev);
      if (subname.back() == ']') {
        subname = subname.substr(0, subname.size() - 1);  // exclude ']'
      }
      hier_subnames.emplace_back(subname);
    }
    prev = pos + 1;
    pos  = full_name.find('.', prev);
  }

  if (prev < full_name.size()) {
    auto subname = full_name.substr(prev);
    if (subname.back() == ']') {
      subname = subname.substr(0, subname.size() - 1);  // exclude ']'
    }
    hier_subnames.emplace_back(subname);
  }
}

void Inou_firrtl_module::split_hier_name(std::string_view                                                    full_name,
                                         std::vector<std::pair<std::string, Inou_firrtl_module::Leaf_type>>& hier_subnames) {
  std::size_t prev = 0;

  std::size_t pos = full_name.find('.', prev);
  while (pos != std::string::npos) {
    if (pos > prev) {
      auto subname = full_name.substr(prev, pos - prev);
      if (subname.back() == ']') {
        subname = subname.substr(0, subname.size() - 1);  // exclude ']'
      }
      set_leaf_type(subname, full_name, prev, hier_subnames);
    }
    prev = pos + 1;
    pos  = full_name.find('.', prev);
  }

  if (prev < full_name.size()) {
    auto subname = full_name.substr(prev);
    if (subname.back() == ']') {
      subname = subname.substr(0, subname.size() - 1);  // exclude ']'
    }
    set_leaf_type(subname, full_name, prev, hier_subnames);
  }
}

void Inou_firrtl_module::direct_instances_connection(Lnast& lnast, Lnast_nid& parent_node, std::string lhs_full_name,
                                                     std::string rhs_full_name, const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  I(str_tools::contains(lhs_full_name, '.'));
  I(str_tools::contains(rhs_full_name, '.'));

  // create TG for the rhs instances
  auto tg_node      = lnast.add_child(parent_node, Lnast_node::create_tuple_get("", 0, line_pos, col_pos, fname));
  auto temp_var_str = create_tmp_var();

  auto pos              = rhs_full_name.find('.');
  auto tg_head          = rhs_full_name.substr(0, pos);
  auto tg_merged_fields = std::string{rhs_full_name.substr(pos + 1)};
  std::replace(tg_merged_fields.begin(), tg_merged_fields.end(), '.', '_');
  lnast.add_child(tg_node, Lnast_node::create_ref(temp_var_str, 0, line_pos, col_pos, fname));
  lnast.add_child(tg_node, Lnast_node::create_ref(absl::StrCat("otup_", tg_head), 0, line_pos, col_pos, fname));
  lnast.add_child(tg_node, Lnast_node::create_const(tg_merged_fields));

  // create TA for the lhs instances
  auto ta_node          = lnast.add_child(parent_node, Lnast_node::create_tuple_add("", 0, line_pos, col_pos, fname));
  auto pos2             = lhs_full_name.find('.');
  auto ta_head          = lhs_full_name.substr(0, pos2);
  auto ta_merged_fields = std::string{lhs_full_name.substr(pos2 + 1)};
  std::replace(ta_merged_fields.begin(), ta_merged_fields.end(), '.', '_');
  lnast.add_child(ta_node, Lnast_node::create_ref(absl::StrCat("itup_", ta_head), 0, line_pos, col_pos, fname));
  lnast.add_child(ta_node, Lnast_node::create_const(ta_merged_fields));
  lnast.add_child(ta_node, Lnast_node::create_ref(temp_var_str, 0, line_pos, col_pos, fname));

  return;
}

void Inou_firrtl_module::create_tuple_add_for_instance_itup(Lnast& lnast, Lnast_nid& parent_node, std::string_view lhs_hier_name,
                                                            std::string                       rhs_flattened_name,
                                                            const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  auto attr_get_node = lnast.add_child(parent_node, Lnast_node::create_attr_get("", 0, line_pos, col_pos, fname));
  auto temp_var_str  = create_tmp_var();
  lnast.add_child(attr_get_node, Lnast_node::create_ref(temp_var_str, 0, line_pos, col_pos, fname));
  lnast.add_child(attr_get_node, Lnast_node::create_ref(rhs_flattened_name, 0, line_pos, col_pos, fname));
  lnast.add_child(attr_get_node, Lnast_node::create_const("__last_value", 0, line_pos, col_pos, fname));
  var2last_value.insert_or_assign(rhs_flattened_name, temp_var_str);
  wire_names.insert(temp_var_str);

  add_lnast_assign(
      lnast,
      parent_node,
      rhs_flattened_name,
      "0",
      stmt);  // the sub-input might be assigned within if-else subscope, so need this trivial initialization for SSA to work

  I(absl::StrContains(lhs_hier_name, '.'));
  auto selc_node         = lnast.add_child(parent_node, Lnast_node::create_tuple_add("", 0, line_pos, col_pos, fname));
  auto pos               = lhs_hier_name.find('.');
  auto tup_head          = lhs_hier_name.substr(0, pos);
  auto tup_merged_fields = std::string{lhs_hier_name.substr(pos + 1)};
  std::replace(tup_merged_fields.begin(), tup_merged_fields.end(), '.', '_');

  lnast.add_child(selc_node, Lnast_node::create_ref(absl::StrCat("itup_", tup_head), 0, line_pos, col_pos, fname));
  lnast.add_child(selc_node, Lnast_node::create_const(tup_merged_fields));
  lnast.add_child(selc_node, Lnast_node::create_ref(temp_var_str, 0, line_pos, col_pos, fname));
}

void Inou_firrtl_module::create_tuple_get_for_instance_otup(Lnast& lnast, Lnast_nid& parent_node, std::string_view rhs_full_name,
                                                            std::string lhs_full_name, const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  I(absl::StrContains(rhs_full_name, '.'));

  auto selc_node = lnast.add_child(parent_node, Lnast_node::create_tuple_get("", 0, line_pos, col_pos, fname));
  std::replace(lhs_full_name.begin(), lhs_full_name.end(), '.', '_');
  lnast.add_child(selc_node, Lnast_node::create_ref(lhs_full_name, 0, line_pos, col_pos, fname));
  auto pos               = rhs_full_name.find('.');
  auto tup_head          = rhs_full_name.substr(0, pos);
  auto tup_merged_fields = std::string{rhs_full_name.substr(pos + 1)};
  std::replace(tup_merged_fields.begin(), tup_merged_fields.end(), '.', '_');
  lnast.add_child(selc_node, Lnast_node::create_ref(absl::StrCat("otup_", tup_head), 0, line_pos, col_pos, fname));
  lnast.add_child(selc_node, Lnast_node::create_const(tup_merged_fields));
}

void Inou_firrtl_module::create_tuple_get_from_str(Lnast& ln, Lnast_nid& parent_node, std::string_view full_name,
                                                   const Lnast_node& target_node, const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  I(absl::StrContains(full_name, '.'));

  std::vector<std::pair<std::string, Inou_firrtl_module::Leaf_type>> hier_subnames;
  split_hier_name(full_name, hier_subnames);
  auto selc_node = ln.add_child(parent_node, Lnast_node::create_tuple_get("", 0, line_pos, col_pos, fname));
  ln.add_child(selc_node, target_node);

  for (const auto& subname : hier_subnames) {
    std::string field_name = subname.first;
    if (inst2module.count(subname.first)) {
      field_name = absl::StrCat("otup_", field_name);
    }
    switch (subname.second) {
      case Leaf_type::Ref: {
        ln.add_child(selc_node, Lnast_node::create_ref(field_name, 0, line_pos, col_pos, fname));
        break;
      }
      case Leaf_type::Const_num:
      case Leaf_type::Const_str: {
        ln.add_child(selc_node, Lnast_node::create_const(field_name));
        break;
      }
      default: Pass::error("Unknown port type.");
    }
  }
}

void Inou_firrtl_module::create_tuple_add_from_str(Lnast& ln, Lnast_nid& parent_node, std::string_view full_name,
                                                   const Lnast_node& value_node) {
  I(absl::StrContains(full_name, '.'));

  std::vector<std::pair<std::string, Inou_firrtl_module::Leaf_type>> hier_subnames;
  split_hier_name(full_name, hier_subnames);
  auto selc_node = ln.add_child(parent_node, Lnast_node::create_tuple_add());

  int i = 0;
  for (const auto& subname : hier_subnames) {
    std::string field_name = subname.first;
    if (inst2module.count(subname.first)) {
      field_name = absl::StrCat("itup_", field_name);
    }
    switch (subname.second) {
      case Leaf_type::Ref: {
        ln.add_child(selc_node, Lnast_node::create_ref(field_name));
        if (i == 0) {
          wire_names.insert(field_name);  // this is the flattened scalar vaiable (more likely a $input)
        }
        break;
      }
      case Leaf_type::Const_num:
      case Leaf_type::Const_str: {
        ln.add_child(selc_node, Lnast_node::create_const(field_name));
        break;
      }
      default: Pass::error("Unknown port type.");
    }
  }

  ln.add_child(selc_node, value_node);
}

void Inou_firrtl_module::create_tuple_add_from_str(Lnast& ln, Lnast_nid& parent_node, std::string_view full_name,
                                                   const Lnast_node& value_node, const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  I(absl::StrContains(full_name, '.'));

  std::vector<std::pair<std::string, Inou_firrtl_module::Leaf_type>> hier_subnames;
  split_hier_name(full_name, hier_subnames);
  auto selc_node = ln.add_child(parent_node, Lnast_node::create_tuple_add("", 0, line_pos, col_pos, fname));

  int i = 0;
  for (const auto& subname : hier_subnames) {
    std::string field_name = subname.first;
    if (inst2module.count(subname.first)) {
      field_name = absl::StrCat("itup_", field_name);
    }
    switch (subname.second) {
      case Leaf_type::Ref: {
        ln.add_child(selc_node, Lnast_node::create_ref(field_name, 0, line_pos, col_pos, fname));
        if (i == 0) {
          wire_names.insert(field_name);  // this is the flattened scalar vaiable (more likely a $input)
        }
        break;
      }
      case Leaf_type::Const_num:
      case Leaf_type::Const_str: {
        ln.add_child(selc_node, Lnast_node::create_const(field_name));
        break;
      }
      default: Pass::error("Unknown port type.");
    }
  }

  ln.add_child(selc_node, value_node);
}

//----------Ports-------------------------

/* This function iterates over the IO of a module and sets
 * the bitwidth + sign of each using a dot node in LNAST. */
void Inou_firrtl_module::list_port_info(Lnast& lnast, const firrtl::FirrtlPB_Port& port, Lnast_nid parent_node) {
  // Terms in port_list as follows: <name, direction, bits, sign>
  std::vector<std::tuple<std::string, uint8_t, uint32_t, bool>> port_list;
  Inou_firrtl::create_io_list(port.type(), port.direction(), port.id(), port_list);

  for (auto val : port_list) {
    auto port_name = std::get<0>(val);
    auto port_dir  = std::get<1>(val);
    auto port_bits = std::get<2>(val);
    auto port_sign = std::get<3>(val);

    std::string full_port_name;
    if (port_dir == firrtl::FirrtlPB_Port_Direction::FirrtlPB_Port_Direction_PORT_DIRECTION_IN) {
      record_all_input_hierarchy(port_name);
      full_port_name = absl::StrCat("$", port_name);
    } else if (port_dir == firrtl::FirrtlPB_Port_Direction::FirrtlPB_Port_Direction_PORT_DIRECTION_OUT) {
      record_all_output_hierarchy(port_name);
      full_port_name = absl::StrCat("%", port_name);
    } else {
      Pass::error("Found IO port {} specified with unknown direction in Protobuf message.", port_name);
    }

    std::replace(full_port_name.begin(), full_port_name.end(), '.', '_');

    if (port_bits > 0) {
      // set default value 0 for all module outputs
      if (full_port_name.substr(0, 1) == "%") {
        auto zero_node = Lnast_node::create_const(0);
        create_default_value_for_scalar_var(lnast, parent_node, full_port_name, zero_node);
      }

      // specify __bits for both inp/out
      auto value_node = Lnast_node::create_const(port_bits);
      auto extension  = port_sign ? ".__sbits" : ".__ubits";
      create_tuple_add_from_str(lnast, parent_node, absl::StrCat(full_port_name, extension), value_node);
    }
  }
}

void Inou_firrtl_module::create_default_value_for_scalar_var(Lnast& ln, Lnast_nid& parent_node, std::string_view sv,
                                                             const Lnast_node& value_node) {
  auto idx_asg = ln.add_child(parent_node, Lnast_node::create_assign());
  ln.add_child(idx_asg, Lnast_node::create_ref(sv));
  ln.add_child(idx_asg, value_node);
  wire_names.insert(std::string{sv});
}

void Inou_firrtl_module::create_default_value_for_scalar_var(Lnast& ln, Lnast_nid& parent_node, std::string_view sv,
                                                             const Lnast_node& value_node, const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);  // std::fromchars
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  auto idx_asg = ln.add_child(parent_node, Lnast_node::create_assign("", 0, line_pos, col_pos, fname));
  ln.add_child(idx_asg, Lnast_node::create_ref(sv, 0, line_pos, col_pos, fname));
  ln.add_child(idx_asg, value_node);
  wire_names.insert(std::string{sv});
}

void Inou_firrtl_module::record_all_input_hierarchy(std::string_view port_name) {
  std::size_t pos = port_name.size();
  while (pos != std::string::npos) {
    auto tmp = port_name.substr(0, pos);
    input_names.emplace(tmp);
    pos = tmp.rfind('.');
  }
}

void Inou_firrtl_module::record_all_output_hierarchy(std::string_view port_name) {
  std::size_t pos = port_name.size();
  while (pos != std::string::npos) {
    auto tmp = port_name.substr(0, pos);
    output_names.emplace(tmp);
    pos = tmp.rfind('.');
  }
}

//-----------Primitive Operations---------------------
/* TODO:
 * Rely upon intervals:
 *   Wrap
 *   Clip
 *   Squeeze
 *   As_Interval
 * Rely upon precision/fixed point:
 *   Increase_Precision
 *   Decrease_Precision
 *   Set_Precision
 *   As_Fixed_Point
 */
void Inou_firrtl_module::list_prime_op_info(Lnast& lnast, const firrtl::FirrtlPB_Expression_PrimOp& op, Lnast_nid& parent_node,
                                            std::string_view lhs, const firrtl::FirrtlPB_Statement& stmt) {
  switch (op.op()) {
    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_ADD:
    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_SUB:
    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_TIMES:
    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_DIVIDE:
    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_REM:
    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_DYNAMIC_SHIFT_LEFT:
    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_DYNAMIC_SHIFT_RIGHT:
    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_BIT_AND:
    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_BIT_OR:
    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_BIT_XOR:
    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_LESS:
    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_LESS_EQ:
    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_GREATER:
    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_GREATER_EQ:
    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_EQUAL:
    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_NOT_EQUAL: {
      handle_binary_op(lnast, op, parent_node, lhs, stmt);
      break;
    }

    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_TAIL: {  // take in some 'n', returns value with 'n' MSBs removed
      handle_tail_op(lnast, op, parent_node, lhs, stmt);
      break;
    }

    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_HEAD: {  // take in some 'n', returns 'n' MSBs of variable invoked on
      handle_head_op(lnast, op, parent_node, lhs, stmt);
      break;
    }

    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_SHIFT_LEFT:
    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_SHIFT_RIGHT: {
      handle_static_shift_op(lnast, op, parent_node, lhs, stmt);
      break;
    }

    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_BIT_NOT: {
      handle_unary_op(lnast, op, parent_node, lhs, stmt);
      break;
    }

    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_CONCAT: {
      handle_concat_op(lnast, op, parent_node, lhs, stmt);
      break;
    }

    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_PAD: {
      handle_pad_op(lnast, op, parent_node, lhs, stmt);
      break;
    }

    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_NEG: {  // takes a # (UInt or SInt) and returns it * -1
      handle_negate_op(lnast, op, parent_node, lhs, stmt);
      break;
    }
    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_CONVERT: {
      handle_conv_op(lnast, op, parent_node, lhs, stmt);
      break;
    }
    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_EXTRACT_BITS: {
      handle_extract_bits_op(lnast, op, parent_node, lhs, stmt);
      break;
    }

    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_AS_UINT:
    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_AS_SINT: {
      handle_as_usint_op(lnast, op, parent_node, lhs, stmt);
      break;
    }

    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_AS_CLOCK:
    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_AS_FIXED_POINT:
    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_AS_ASYNC_RESET: {
      handle_type_conv_op(lnast, op, parent_node, lhs, stmt);
      break;
    }
    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_XOR_REDUCE: {
      handle_xor_reduce_op(lnast, op, parent_node, lhs, stmt);
      break;
    }
    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_AND_REDUCE: {
      handle_and_reduce_op(lnast, op, parent_node, lhs, stmt);
      break;
    }
    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_OR_REDUCE: {
      handle_or_reduce_op(lnast, op, parent_node, lhs, stmt);
      break;
    }
    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_INCREASE_PRECISION:
    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_DECREASE_PRECISION:
    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_SET_PRECISION: {
      Pass::error("PrimOp: {} not yet supported (related to FloatingPoint type)", (int)op.op());
      break;
    }
    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_WRAP:
    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_CLIP:
    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_SQUEEZE:
    case firrtl::FirrtlPB_Expression_PrimOp_Op_OP_AS_INTERVAL: {
      Pass::error("PrimOp: {} not yet supported (related to Interavls)", (int)op.op());
      break;
    }
    default: Pass::error("Unknown PrimaryOp");
  }
}

//--------------Expressions-----------------------
void Inou_firrtl_module::init_expr_add(Lnast& lnast, const firrtl::FirrtlPB_Expression& rhs_expr, Lnast_nid& parent_node,
                                       std::string_view lhs_noprefixes, const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);  // std::fromchars
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  // Note: here, parent_node is the "stmt" node above where this expression will go.
  I(lnast.get_data(parent_node).type.is_stmts());
  auto lhs_str = name_prefix_modifier_flattener(lhs_noprefixes, false);
  wire_names.insert(lhs_str);
  switch (rhs_expr.expression_case()) {
    case firrtl::FirrtlPB_Expression::kReference: {  // Reference
      auto        tmp_rhs_str = rhs_expr.reference().id();
      std::string rhs_str;
      if (is_invalid_table.find(tmp_rhs_str) != is_invalid_table.end()) {
        // create __last_value
        auto idx_attr_get = lnast.add_child(parent_node, Lnast_node::create_attr_get("", 0, line_pos, col_pos, fname));
        auto temp_var_str = create_tmp_var();
        lnast.add_child(idx_attr_get, Lnast_node::create_ref(temp_var_str, 0, line_pos, col_pos, fname));
        lnast.add_child(idx_attr_get, Lnast_node::create_ref(tmp_rhs_str, 0, line_pos, col_pos, fname));
        lnast.add_child(idx_attr_get, Lnast_node::create_const("__last_value", 0, line_pos, col_pos, fname));
        rhs_str = temp_var_str;
        wire_names.insert(temp_var_str);
      } else {
        rhs_str = name_prefix_modifier_flattener(rhs_expr.reference().id(), true);
      }

      auto it = is_invalid_table.find(lhs_str);
      if (it != is_invalid_table.end()) {  // lhs is declared as invalid before
        auto idx_asg = lnast.add_child(parent_node, Lnast_node::create_assign("assign", 0, line_pos, col_pos, fname));
        lnast.add_child(idx_asg, Lnast_node::create_ref(lhs_str, 0, line_pos, col_pos, fname));
        lnast.add_child(idx_asg, Lnast_node::create_ref(rhs_str, 0, line_pos, col_pos, fname));
        is_invalid_table.erase(lhs_str);
        // } else if (lhs_str.substr(0, 1) == "_") {  // lhs is declared as kNode
      } else if (node_names.find(lhs_str) != node_names.end()) {  // lhs is declared as kNode
        auto idx_asg = lnast.add_child(parent_node, Lnast_node::create_assign("assign", 0, line_pos, col_pos, fname));
        lnast.add_child(idx_asg, Lnast_node::create_ref(lhs_str, 0, line_pos, col_pos, fname));
        lnast.add_child(idx_asg, Lnast_node::create_ref(rhs_str, 0, line_pos, col_pos, fname));
      }
      break;
    }
    case firrtl::FirrtlPB_Expression::kUintLiteral: {  // UIntLiteral
      Lnast_nid idx_asg;
      idx_asg = lnast.add_child(parent_node, Lnast_node::create_assign("assign", 0, line_pos, col_pos, fname));
      lnast.add_child(idx_asg, Lnast_node::create_ref(lhs_str, 0, line_pos, col_pos, fname));
      auto str_val = rhs_expr.uint_literal().value().value();
      lnast.add_child(idx_asg, Lnast_node::create_const(str_val, 0, line_pos, col_pos, fname));
      break;
    }
    case firrtl::FirrtlPB_Expression::kSintLiteral: {  // SIntLiteral
      Lnast_nid idx_conv;
      idx_conv = lnast.add_child(parent_node, Lnast_node::create_func_call("", 0, line_pos, col_pos, fname));

      auto temp_var_str = create_tmp_var();  // ___F6
      auto tmp_node     = Lnast_node::create_ref(temp_var_str, 0, line_pos, col_pos, fname);
      lnast.add_child(idx_conv, tmp_node);
      lnast.add_child(idx_conv, Lnast_node::create_const("__fir_as_sint", 0, line_pos, col_pos, fname));

      auto str_val = rhs_expr.sint_literal().value().value();
      lnast.add_child(idx_conv, Lnast_node::create_const(str_val, 0, line_pos, col_pos, fname));

      Lnast_nid idx_asg;
      idx_asg = lnast.add_child(parent_node, Lnast_node::create_assign("assign", 0, line_pos, col_pos, fname));
      lnast.add_child(idx_asg, Lnast_node::create_ref(lhs_str, 0, line_pos, col_pos, fname));
      lnast.add_child(idx_asg, Lnast_node::create_ref(temp_var_str, 0, line_pos, col_pos, fname));
      break;
    }
    case firrtl::FirrtlPB_Expression::kValidIf: {  // ValidIf
      handle_valid_if_assign(lnast, rhs_expr, parent_node, lhs_str, stmt);
      break;
    }
    case firrtl::FirrtlPB_Expression::kMux: {  // Mux
      handle_mux_assign(lnast, rhs_expr, parent_node, lhs_str, stmt);
      break;
    }
    case firrtl::FirrtlPB_Expression::kPrimOp: {  // PrimOp
      list_prime_op_info(lnast, rhs_expr.prim_op(), parent_node, lhs_str, stmt);
      break;
    }
    case firrtl::FirrtlPB_Expression::kSubField:
    case firrtl::FirrtlPB_Expression::kSubIndex:     // SubIndex
    case firrtl::FirrtlPB_Expression::kSubAccess: {  // SubAccess
      I(false);                                      // tuple/vector related stuff already handled in connect statement
      break;
    }
    case firrtl::FirrtlPB_Expression::kFixedLiteral: {  // FixedLiteral
      // FIXME: FixedPointLiteral not yet supported in LNAST
      I(false);
      break;
    }
    default: Pass::error("In init_expr_add, found unknown expression type: {}", (int)rhs_expr.expression_case());
  }
}

/* Given an expression that may or may
 * not have hierarchy, flatten it. */
std::string Inou_firrtl_module::get_expr_hier_name(Lnast& lnast, Lnast_nid& parent_node, const firrtl::FirrtlPB_Expression& expr,
                                                   const firrtl::FirrtlPB_Statement& stmt) {
  if (expr.has_sub_field()) {
    return absl::StrCat(get_expr_hier_name(lnast, parent_node, expr.sub_field().expression(), stmt), ".", expr.sub_field().field());
  } else if (expr.has_sub_access()) {
    auto idx_str = get_expr_hier_name(lnast, parent_node, expr.sub_access().index(), stmt);
    return absl::StrCat(get_expr_hier_name(lnast, parent_node, expr.sub_access().expression(), stmt), ".", idx_str);
  } else if (expr.has_sub_index()) {
    return absl::StrCat(get_expr_hier_name(lnast, parent_node, expr.sub_index().expression(), stmt),
                        ".",
                        expr.sub_index().index().value());
  } else if (expr.has_reference()) {
    return expr.reference().id();
  } else if (expr.has_prim_op()) {
    auto expr_string = create_tmp_var();
    list_prime_op_info(lnast, expr.prim_op(), parent_node, expr_string, stmt);
    return expr_string;
  } else if (expr.has_uint_literal()) {
    return absl::StrCat(expr.uint_literal().value().value(), "ubits", expr.uint_literal().width().value());
  } else {
    return "";
  }
}

std::string Inou_firrtl_module::get_expr_hier_name(const firrtl::FirrtlPB_Expression& expr, bool& is_runtime_idx) {
  if (expr.has_sub_field()) {
    return absl::StrCat(get_expr_hier_name(expr.sub_field().expression(), is_runtime_idx), ".", expr.sub_field().field());
  } else if (expr.has_sub_access()) {
    auto idx_str = expr.sub_access().index().uint_literal().value().value();
    if (!isdigit(idx_str[0])) {
      is_runtime_idx = true;
    }
    return absl::StrCat(get_expr_hier_name(expr.sub_access().expression(), is_runtime_idx), ".", idx_str);
  } else if (expr.has_sub_index()) {
    return absl::StrCat(get_expr_hier_name(expr.sub_index().expression(), is_runtime_idx), ".", expr.sub_index().index().value());
  } else if (expr.has_reference()) {
    return expr.reference().id();
  } else if (expr.has_uint_literal()) {
    return expr.uint_literal().value().value();
  } else if (expr.has_sint_literal()) {
    return expr.sint_literal().value().value();
  } else {
    return "";
  }
}

/* the new fir2lnast design want to have as many hierarchical flattened
 * wires as possible, the only exception comes from the connections of sub-module instance io.
 * The sub-module connections will be involved with TupleGet from the instance. This is handled
 * in the old fir2lnast. So,
 * (1) check if the operand comes from instance
 * (2) if yes, follow the old mechanism: create tuple_gets to retrieve the hierarchical field value
 * (3) if not, flattened the operand str and create a simple assignment to the prime_op/mux/valid_if ... etc
 * */
std::string Inou_firrtl_module::expr_str_flattened_or_tg(Lnast& lnast, Lnast_nid& parent_node,
                                                         const firrtl::FirrtlPB_Expression& operand_expr,
                                                         const firrtl::FirrtlPB_Statement&  stmt) {
  std::string expr_str;

  bool        is_runtime_idx_r = false;
  std::string expr_str_tmp     = get_expr_hier_name(operand_expr, is_runtime_idx_r);

  if (is_runtime_idx_r) {
    auto tmp_var = create_tmp_var();
    handle_rhs_runtime_idx(lnast, parent_node, tmp_var, expr_str_tmp, operand_expr, stmt);
    return tmp_var;
  }

  // here we only want to check the case of instance connection, so kSubField is sufficient
  auto expr_case = operand_expr.expression_case();
  if (expr_case == firrtl::FirrtlPB_Expression::kPrimOp) {
    expr_str = get_expr_hier_name(lnast, parent_node, operand_expr, stmt);
  } else if (expr_case == firrtl::FirrtlPB_Expression::kUintLiteral) {
    expr_str = absl::StrCat(operand_expr.uint_literal().value().value(), "ubits", operand_expr.uint_literal().width().value());
  } else {
    expr_str = name_prefix_modifier_flattener(expr_str_tmp, true);
  }

  if (var2last_value.find(expr_str) != var2last_value.end()) {
    expr_str = var2last_value[expr_str];
  }
  return expr_str;
}

void Inou_firrtl_module::add_lnast_assign(Lnast& lnast, Lnast_nid& parent_node, std::string_view lhs, std::string_view rhs,
                                          const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);  // std::fromchars
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  Lnast_nid idx_asg = lnast.add_child(parent_node, Lnast_node::create_assign("", 0, line_pos, col_pos, fname));
  lnast.add_child(idx_asg, Lnast_node::create_ref(lhs, 0, line_pos, col_pos, fname));
  auto first_char = rhs[0];
  if (isdigit(first_char) || first_char == '-' || first_char == '+') {
    lnast.add_child(idx_asg, Lnast_node::create_const(rhs));
  } else {
    lnast.add_child(idx_asg, Lnast_node::create_ref(rhs, 0, line_pos, col_pos, fname));
  }
  wire_names.insert(std::string(lhs));
}

/* This function takes in a string and adds it into the LNAST as
 * a child of the provided "parent_node". Note: the access_str should
 * already have any $/%/#/__q_pin added to it before this is called. */
void Inou_firrtl_module::attach_expr_str2node(Lnast& lnast, std::string_view access_str, Lnast_nid& parent_node,
                                              const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  I(!lnast.get_data(parent_node).type.is_stmts());

  auto first_char = access_str[0];
  if (isdigit(first_char) || first_char == '-' || first_char == '+') {
    // Represents an integer value.
    lnast.add_child(parent_node, Lnast_node::create_const(access_str));
  } else {
    // Represents a wire/variable/io.
    lnast.add_child(parent_node, Lnast_node::create_ref(access_str, 0, line_pos, col_pos, fname));
  }
}

//------------Statements----------------------
// TODO: Attach
//
void Inou_firrtl_module::setup_register_q_pin(Lnast& lnast, Lnast_nid& parent_node, std::string_view reg_name,
                                              const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  // auto flop_qpin_var = absl::StrCat("_#_", reg_name, "_q");
  auto flop_qpin_var = absl::StrCat("_#_", reg_name, "_q");
  auto idx_asg2      = lnast.add_child(parent_node, Lnast_node::create_assign("", 0, line_pos, col_pos, fname));
  lnast.add_child(idx_asg2, Lnast_node::create_ref(flop_qpin_var, 0, line_pos, col_pos, fname));
  // lnast.add_child(idx_asg2, Lnast_node::create_ref(absl::StrCat("#", reg_name), 0, line_pos, col_pos, fname));
  lnast.add_child(idx_asg2, Lnast_node::create_ref(reg_name, 0, line_pos, col_pos, fname));
  reg2qpin.insert_or_assign(reg_name, flop_qpin_var);
  wire_names.insert(flop_qpin_var);
}

void Inou_firrtl_module::declare_register(Lnast& lnast, Lnast_nid& parent_node, std::string_view reg_name,
                                          const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  auto idx_attget = lnast.add_child(parent_node, Lnast_node::create_attr_get("", 0, line_pos, col_pos, fname));
  // auto full_register_name = absl::StrCat("#", reg_name);
  auto full_register_name = reg_name;
  auto tmp_var_str        = create_tmp_var();
  lnast.add_child(idx_attget, Lnast_node::create_ref(tmp_var_str, 0, line_pos, col_pos, fname));
  lnast.add_child(idx_attget, Lnast_node::create_ref(full_register_name, 0, line_pos, col_pos, fname));
  lnast.add_child(idx_attget, Lnast_node::create_const("__create_flop", 0, line_pos, col_pos, fname));

  auto idx_asg = lnast.add_child(parent_node, Lnast_node::create_assign("", 0, line_pos, col_pos, fname));
  lnast.add_child(idx_asg, Lnast_node::create_ref(full_register_name, 0, line_pos, col_pos, fname));
  lnast.add_child(idx_asg, Lnast_node::create_ref(tmp_var_str, 0, line_pos, col_pos, fname));
  wire_names.insert(std::string(full_register_name));
}

void Inou_firrtl_module::setup_register_reset_init(Lnast& lnast, Lnast_nid& parent_node, std::string_view reg_raw_name,
                                                   const firrtl::FirrtlPB_Expression& resete,
                                                   const firrtl::FirrtlPB_Expression& inite,
                                                   std::string_view head_chopped_hier_name, bool bits_set_done,
                                                   const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  bool tied0_reset = false;
  auto resete_case = resete.expression_case();

  Lnast_node value_node;

  if (resete_case == firrtl::FirrtlPB_Expression::kUintLiteral || resete_case == firrtl::FirrtlPB_Expression::kSintLiteral) {
    auto str_val = resete.uint_literal().value().value();
    value_node   = Lnast_node::create_const(str_val);
    if (str_val == "0") {
      tied0_reset = true;
    }
  } else if (resete_case == firrtl::FirrtlPB_Expression::kReference) {
    auto ref_str = name_prefix_modifier_flattener(resete.reference().id(), true);
    value_node   = Lnast_node::create_ref(ref_str, 0, line_pos, col_pos, fname);
  }

  if (!value_node.is_invalid()) {
    // create_tuple_add_from_str(lnast, parent_node, absl::StrCat("#", reg_raw_name, ".__reset_pin"), value_node, stmt);
    create_tuple_add_from_str(lnast, parent_node, absl::StrCat(reg_raw_name, ".__reset_pin"), value_node, stmt);
  }

  if (tied0_reset) {
    return;
  }

  Lnast_node initial_node;

  auto inite_case = inite.expression_case();
  if (inite_case == firrtl::FirrtlPB_Expression::kUintLiteral) {
    auto str_val = inite.uint_literal().value().value();
    initial_node = Lnast_node::create_const(str_val);

    if (!bits_set_done) {
      auto bits = inite.uint_literal().width().value();
      // setup_scalar_bits(lnast, absl::StrCat("#", reg_raw_name), bits, parent_node, false, stmt);
      setup_scalar_bits(lnast, reg_raw_name, bits, parent_node, false, stmt);
    }
  } else if (inite_case == firrtl::FirrtlPB_Expression::kSintLiteral) {
    auto str_val = inite.uint_literal().value().value();
    initial_node = Lnast_node::create_const(str_val);

    if (!bits_set_done) {
      auto bits = inite.uint_literal().width().value();
      // setup_scalar_bits(lnast, absl::StrCat("#", reg_raw_name), bits, parent_node, true, stmt);
      setup_scalar_bits(lnast, reg_raw_name, bits, parent_node, true, stmt);
    }
  } else if (inite_case == firrtl::FirrtlPB_Expression::kReference) {
    // (void) head_chopped_hier_name;
    auto        ref_str_pre = inite.reference().id();
    std::string ref_str;
    if (head_chopped_hier_name != "") {
      ref_str = absl::StrCat(ref_str_pre, ".", head_chopped_hier_name);
      ref_str = name_prefix_modifier_flattener(ref_str, true);
    } else {
      ref_str = ref_str_pre;
    }

    initial_node = Lnast_node::create_ref(ref_str, 0, line_pos, col_pos, fname);
  }

  if (!initial_node.is_invalid()) {
    // create_tuple_add_from_str(lnast, parent_node, absl::StrCat("#", reg_raw_name, ".__initial"), initial_node, stmt);
    create_tuple_add_from_str(lnast, parent_node, absl::StrCat(reg_raw_name, ".__initial"), initial_node, stmt);
  }
}

void Inou_firrtl_module::dump_var2flip(
    const absl::flat_hash_map<std::string, absl::flat_hash_set<std::pair<std::string, bool>>>& module_var2flip) {
  (void)module_var2flip;
#ifndef NDEBUG
  for (auto& [var, set] : module_var2flip) {
    std::cout << std::format("var:{} \n", var);
    for (auto& set_itr : set) {
      std::cout << std::format("  hier_name:{:<20}, accu_flipped:{:<5}\n", set_itr.first, set_itr.second);
    }
  }
#endif
}

void Inou_firrtl_module::tuple_flattened_connections_instance_l(Lnast& lnast, Lnast_nid& parent_node,
                                                                std::string_view hier_name_l_ori, std::string_view hier_name_r_ori,
                                                                bool is_flipped, bool is_input,
                                                                const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  (void)is_flipped;
  // 0. swap lhs/rhs if needed
  if (/*is_flipped && */ !is_input) {
    auto tmp        = hier_name_l_ori;
    hier_name_l_ori = hier_name_r_ori;
    hier_name_r_ori = tmp;
  }

  std::string hier_name_l, hier_name_r;
  hier_name_l = name_prefix_modifier_flattener(hier_name_l_ori, false);
  hier_name_r = name_prefix_modifier_flattener(hier_name_r_ori, true);

  auto idx_asg = lnast.add_child(parent_node, Lnast_node::create_assign("", 0, line_pos, col_pos, fname));
  lnast.add_child(idx_asg, Lnast_node::create_ref(hier_name_l, 0, line_pos, col_pos, fname));
  attach_expr_str2node(lnast, hier_name_r, idx_asg, stmt);
  wire_names.insert(hier_name_l);
}

void Inou_firrtl_module::tuple_flattened_connections_instance_r(Lnast& lnast, Lnast_nid& parent_node,
                                                                std::string_view hier_name_l_ori, std::string_view hier_name_r_ori,
                                                                bool is_flipped, bool is_output,
                                                                const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  // 0. swap lhs/rhs if needed
  if (is_flipped && !is_output) {
    auto tmp        = hier_name_l_ori;
    hier_name_l_ori = hier_name_r_ori;
    hier_name_r_ori = tmp;
  }

  std::string hier_name_l, hier_name_r;
  hier_name_l = name_prefix_modifier_flattener(hier_name_l_ori, false);
  hier_name_r = name_prefix_modifier_flattener(hier_name_r_ori, true);

  auto idx_asg = lnast.add_child(parent_node, Lnast_node::create_assign("", 0, line_pos, col_pos, fname));
  lnast.add_child(idx_asg, Lnast_node::create_ref(hier_name_l, 0, line_pos, col_pos, fname));
  attach_expr_str2node(lnast, hier_name_r, idx_asg, stmt);
  wire_names.insert(hier_name_l);
}

// the sub-fields of lhs and rhs are the same, so we just pass the head of lhs
// (tup_l), and we could avoid traversing the big var2flip table again to get
// lhs flattened element.
// e.g. tup_l == foo, rhs == bar.a.b.c, is_flipped == true
// then: bar.a.b.c = foo.a.b.c
void Inou_firrtl_module::tuple_flattened_connections(Lnast& lnast, Lnast_nid& parent_node, std::string_view hier_name_l_ori,
                                                     std::string_view hier_name_r_ori, std::string_view flattened_element,
                                                     bool is_flipped, const firrtl::FirrtlPB_Statement& stmt) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }

  // 0. swap lhs/rhs if needed
  if (is_flipped) {
    std::swap(hier_name_l_ori, hier_name_r_ori);
  }

  // 1. decorate io prefix
  std::string hier_name_r, hier_name_l;
  if (input_names.find(hier_name_r_ori) != input_names.end()) {
    hier_name_r = absl::StrCat("$", hier_name_r_ori);
  } else if (reg2qpin.find(hier_name_r_ori) != reg2qpin.end()) {
    hier_name_r = name_prefix_modifier_flattener(hier_name_r_ori, true);
  } else {
    hier_name_r = hier_name_r_ori;
  }

  if (output_names.find(hier_name_l_ori) != output_names.end()) {
    hier_name_l = absl::StrCat("%", hier_name_l_ori);
  } else if (reg2qpin.find(hier_name_l_ori) != reg2qpin.end()) {
    hier_name_l = name_prefix_modifier_flattener(hier_name_l_ori, false);
  } else {
    hier_name_l = hier_name_l_ori;
  }

  std::string_view chop_head_flattened_element;
  if (is_flipped) {
    chop_head_flattened_element = flattened_element.substr(hier_name_r_ori.length());
  } else {
    chop_head_flattened_element = flattened_element.substr(hier_name_l_ori.length());
  }

  auto lhs_full_name = absl::StrCat(hier_name_l, chop_head_flattened_element);
  auto rhs_full_name = absl::StrCat(hier_name_r, chop_head_flattened_element);

  std::string rhs_wire_name;
  auto        pos = rhs_full_name.find('.');
  if (pos != std::string::npos) {
    rhs_wire_name = rhs_full_name.substr(0, pos);
  }

  lhs_full_name = name_prefix_modifier_flattener(lhs_full_name, false);
  rhs_full_name = name_prefix_modifier_flattener(rhs_full_name, true);

  // not trivial to prevent the unnecessary swap before, so swap again here if lhs is an module input
  if (lhs_full_name.at(0) == '$') {
    std::swap(lhs_full_name, rhs_full_name);
  }

  auto it              = wire_names.find(rhs_wire_name);
  bool rhs_is_wire_var = it != wire_names.end();
  if (rhs_is_wire_var) {
    auto temp_var_name = create_tmp_var();
    auto attr_get_node = lnast.add_child(parent_node, Lnast_node::create_attr_get("", 0, line_pos, col_pos, fname));
    lnast.add_child(attr_get_node, Lnast_node::create_ref(temp_var_name, 0, line_pos, col_pos, fname));
    lnast.add_child(attr_get_node, Lnast_node::create_ref(rhs_full_name, 0, line_pos, col_pos, fname));
    lnast.add_child(attr_get_node, Lnast_node::create_const("__last_value", 0, line_pos, col_pos, fname));
    wire_names.insert(temp_var_name);

    auto idx_asg = lnast.add_child(parent_node, Lnast_node::create_assign("", 0, line_pos, col_pos, fname));
    lnast.add_child(idx_asg, Lnast_node::create_ref(lhs_full_name, 0, line_pos, col_pos, fname));
    lnast.add_child(idx_asg, Lnast_node::create_ref(temp_var_name, 0, line_pos, col_pos, fname));
    wire_names.insert(lhs_full_name);
  } else if (node_names.find(lhs_full_name) != node_names.end()) {  // lhs is declared as kNode
    if (wire_names.find(rhs_full_name) == wire_names.end() && !std::isdigit(rhs_full_name.at(0))) {
      return;  // this lhs tuple field has no corresponding field in the rhs tuple, it's a partial_connect case, don't create
               // assignment for these lhs/rhs
    }

    auto idx_asg = lnast.add_child(parent_node, Lnast_node::create_assign("", 0, line_pos, col_pos, fname));
    lnast.add_child(idx_asg, Lnast_node::create_ref(lhs_full_name, 0, line_pos, col_pos, fname));
    lnast.add_child(idx_asg, Lnast_node::create_ref(rhs_full_name, 0, line_pos, col_pos, fname));
    wire_names.insert(lhs_full_name);
  } else {
    if (wire_names.find(rhs_full_name) == wire_names.end() && !std::isdigit(rhs_full_name.at(0))) {
      return;  // this lhs tuple field has no corresponding field in the rhs tuple, it's a partial_connect case, don't create
               // assignment for these lhs/rhs
    }

    auto idx_asg = lnast.add_child(parent_node, Lnast_node::create_assign("", 0, line_pos, col_pos, fname));
    lnast.add_child(idx_asg, Lnast_node::create_ref(lhs_full_name, 0, line_pos, col_pos, fname));
    attach_expr_str2node(lnast, rhs_full_name, idx_asg, stmt);
    wire_names.insert(lhs_full_name);
  }
  return;
}

void Inou_firrtl_module::list_statement_info(Lnast& lnast, const firrtl::FirrtlPB_Statement& stmt, Lnast_nid& parent_node) {
  const auto& loc_info = stmt.source_info().text();
  uint64_t    line_pos = 0;
  uint64_t    col_pos  = 0;
  std::string fname    = "";
  if (loc_info != "") {
    std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
    line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
    col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
    fname                             = subtrngs[0];
  }
  switch (stmt.statement_case()) {
    case firrtl::FirrtlPB_Statement::kWire: {
      auto stmt_wire_id = stmt.wire().id();
      wire_names.insert(stmt_wire_id);
      wire_init_flip_handling(lnast, stmt.wire().type(), stmt_wire_id, false, parent_node, stmt);
      break;
    }
    case firrtl::FirrtlPB_Statement::kRegister: {
      auto stmt_reg_id = stmt.register_().id();
      // step-I: recursively collect reg info into the var2flip-table
      handle_register(lnast, stmt.register_().type(), stmt_reg_id, parent_node, stmt);
      break;
    }
    case firrtl::FirrtlPB_Statement::kMemory: {
      I(false, "never happen in chirrtl");
      break;
    }
    case firrtl::FirrtlPB_Statement::kCmemory: {
      memory_names.insert(stmt.cmemory().id());
      memory_loc.insert({stmt.cmemory().id(), loc_info /*std::make_pair(line_pos, col_pos, fname)*/});
      init_cmemory(lnast, parent_node, stmt.cmemory(), stmt);
      break;
    }
    case firrtl::FirrtlPB_Statement::kMemoryPort: {
      handle_mport_declaration(lnast, parent_node, stmt.memory_port(), stmt);
      break;
    }
    case firrtl::FirrtlPB_Statement::kInstance: {  // Instance -- creating an instance of a module inside another
      create_module_inst(lnast, stmt.instance(), parent_node, stmt);
      break;
    }
    case firrtl::FirrtlPB_Statement::kNode: {  // Node -- nodes are simply named intermediates in a circuit
      // add_local_flip_info(false, stmt.node().id());
      node_names.insert(stmt.node().id());
      init_expr_add(lnast, stmt.node().expression(), parent_node, stmt.node().id(), stmt);
      break;
    }
    case firrtl::FirrtlPB_Statement::kWhen: {
      auto cond_str = expr_str_flattened_or_tg(lnast, parent_node, stmt.when().predicate(), stmt);
      auto idx_when = lnast.add_child(parent_node, Lnast_node::create_if("", 0, line_pos, col_pos, fname));
      // lnast.add_child(idx_when, Lnast_node::create_ref(cond_str));
      attach_expr_str2node(lnast, cond_str, idx_when, stmt);

      auto idx_stmts_t = lnast.add_child(idx_when, Lnast_node::create_stmts("", 0, line_pos, col_pos, fname));

      for (int i = 0; i < stmt.when().consequent_size(); i++) {
        list_statement_info(lnast, stmt.when().consequent(i), idx_stmts_t);
      }

      if (stmt.when().otherwise_size() > 0) {
        auto idx_stmts_f = lnast.add_child(idx_when, Lnast_node::create_stmts("", 0, line_pos, col_pos, fname));
        for (int j = 0; j < stmt.when().otherwise_size(); j++) {
          list_statement_info(lnast, stmt.when().otherwise(j), idx_stmts_f);
        }
      }
      break;
    }
    case firrtl::FirrtlPB_Statement::kStop:
    case firrtl::FirrtlPB_Statement::kPrintf:
    case firrtl::FirrtlPB_Statement::kSkip: {  // Skip
      // Nothing to do.
      break;
    }
    case firrtl::FirrtlPB_Statement::kConnect:
    case firrtl::FirrtlPB_Statement::kPartialConnect: {
      firrtl::FirrtlPB_Expression lhs_expr;
      firrtl::FirrtlPB_Expression rhs_expr;
      if (stmt.statement_case() == firrtl::FirrtlPB_Statement::kConnect) {
        lhs_expr = stmt.connect().location();
        rhs_expr = stmt.connect().expression();
      } else {
        lhs_expr = stmt.partial_connect().location();
        rhs_expr = stmt.partial_connect().expression();
      }

      // example: "_T <= io.in" means: hier_name_l == "_T", hier_name_r == "io.in"
      bool        is_runtime_idx_l = false;
      bool        is_runtime_idx_r = false;
      std::string hier_name_l      = get_expr_hier_name(lhs_expr, is_runtime_idx_l);
      std::string hier_name_r      = get_expr_hier_name(rhs_expr, is_runtime_idx_r);

      // case-I: runtime index
      // some facts:
      // (1) firrtl runtime index don't have any flipness issue
      // (2) it always be the flattened connection case action: we just flatten
      // the lhs and rhs and insert the multiplexers to handle the runtime
      // vector elements selection.
      if (is_runtime_idx_l) {
        handle_lhs_runtime_idx(lnast, parent_node, hier_name_l, hier_name_r, lhs_expr, stmt);
        return;
      }

      if (is_runtime_idx_r) {
        handle_rhs_runtime_idx(lnast, parent_node, hier_name_l, hier_name_r, rhs_expr, stmt);
        return;
      }

      // case-II: the rhs is a component w/o names, such as validif,
      // primitive_op, unsigned integers, and mux in this case, the hier_name_l
      // must already a leaf in the hierarchy, and must be non-flipped. We
      // could safely create a simple lnast assignment.
      if (hier_name_r.empty()) {
        init_expr_add(lnast, rhs_expr, parent_node, hier_name_l, stmt);
        return;
      }

      // preparation: get head of the tuple name so you know entry for the var2flip table
      auto        pos_l = hier_name_l.find_first_of('.');
      std::string tup_head_l, tup_head_r;
      std::string tup_rest_l, tup_rest_r;
      if (pos_l != std::string::npos) {
        tup_head_l = hier_name_l.substr(0, pos_l);
        tup_rest_l = hier_name_l.substr(pos_l + 1);
      } else {
        tup_head_l = hier_name_l;
      }

      auto pos_r = hier_name_r.find_first_of('.');
      if (pos_r != std::string::npos) {
        tup_head_r = hier_name_r.substr(0, pos_r);
        tup_rest_r = hier_name_r.substr(pos_r + 1);
      } else {
        tup_head_r = hier_name_r;
      }

      // case-IV: memory port connection
      bool is_wr_mport = mport2mem.find(tup_head_l) != mport2mem.end();
      bool is_rd_mport = mport2mem.find(tup_head_r) != mport2mem.end();

      if (is_rd_mport) {
        // std::cout << std::format("DEBUG BBB handle rd_mport hier_name_l:{}, hier_name_r:{}\n", hier_name_l, hier_name_r);
        initialize_rd_mport_from_usage(lnast, parent_node, tup_head_r, stmt);
        hier_name_l = name_prefix_modifier_flattener(hier_name_l, false);
        if (!absl::StrContains(hier_name_r, '.')) {
          add_lnast_assign(lnast, parent_node, hier_name_l, tup_head_r, stmt);
        } else {
          auto       target_var_str = create_tmp_var();
          Lnast_node target_node    = Lnast_node::create_ref(target_var_str, 0, line_pos, col_pos, fname);
          create_tuple_get_from_str(lnast, parent_node, hier_name_r, target_node, stmt);
          add_lnast_assign(lnast, parent_node, hier_name_l, target_var_str, stmt);
        }
        return;
      } else if (is_wr_mport) {
        // std::cout << std::format("DEBUG CCC handle wr_mport hier_name_l:{}, hier_name_r:{}\n", hier_name_l, hier_name_r);
        initialize_wr_mport_from_usage(lnast, parent_node, tup_head_l, stmt);
        hier_name_r = name_prefix_modifier_flattener(hier_name_r, true);
        if (!absl::StrContains(hier_name_l, '.')) {
          add_lnast_assign(lnast, parent_node, tup_head_l, hier_name_r, stmt);
        } else {
          Lnast_node value_node = Lnast_node::create_ref(hier_name_r, 0, line_pos, col_pos, fname);
          create_tuple_add_from_str(lnast, parent_node, hier_name_l, value_node, stmt);
        }
        return;
      }

      // case-VII: connections involves instances
      auto is_instance_l = inst2module.find(tup_head_l) != inst2module.end();
      auto is_instance_r = inst2module.find(tup_head_r) != inst2module.end();

      if (is_instance_l) {
        handle_lhs_instance_connections(lnast, parent_node, tup_head_l, hier_name_l, hier_name_r, stmt);
        return;
      } else if (is_instance_r) {
        handle_rhs_instance_connections(lnast, parent_node, tup_head_r, hier_name_l, hier_name_r, stmt);
        return;
      }

      // case-VIII: this is the normal case that involves firrtl kWire connection
      // could be (1) wire <- module_input (2) wire <- wire (3) module_output <- wire
      handle_normal_cases_wire_connections(lnast, parent_node, tup_head_l, hier_name_l, hier_name_r, stmt);
      break;
    }

    case firrtl::FirrtlPB_Statement::kIsInvalid: {
      // auto id = stmt.is_invalid().expression().reference().id();
      auto id = get_expr_hier_name(lnast, parent_node, stmt.is_invalid().expression(), stmt);
      auto it = wire_names.find(id);
      if (it != wire_names.end()) {
        is_invalid_table.insert(id);
      }
      auto found = id.find('.');
      if (found != std::string::npos) {
        id = name_prefix_modifier_flattener(id, false);
        if (id.at(0) != '$') {
          auto idx_asg = lnast.add_child(parent_node, Lnast_node::create_assign("", 0, line_pos, col_pos, fname));
          lnast.add_child(idx_asg, Lnast_node::create_ref(id, 0, line_pos, col_pos, fname));
          lnast.add_child(idx_asg,
                          Lnast_node::create_const("is_fir_invalid",
                                                   0,
                                                   line_pos,
                                                   col_pos,
                                                   fname));  // FIXME-> put ? then later cprop could collaps the mux
          wire_names.insert(id);
        }
      }
      break;
    }
    case firrtl::FirrtlPB_Statement::kAttach: {
      Pass::error("Attach statement not yet supported due to bidirectionality.");
      I(false);
      break;
    }
    default:
#ifndef NDEBUG
      Pass::warn("Warning: commented \"I(false)\" to enable RocketTile LG generation.");
#endif
      Pass::warn("Unknown statement type: {}, at line {} in file {}", (int)stmt.statement_case(), line_pos, fname);
      // I(false);
      return;
  }
  // TODO: Attach source info into node creation (line #, col #).
}

void Inou_firrtl_module::handle_normal_cases_wire_connections(Lnast& lnast, Lnast_nid& parent_node, std::string_view tup_head_l,
                                                              std::string_view hier_name_l, std::string_view hier_name_r,
                                                              const firrtl::FirrtlPB_Statement& stmt) {
  absl::flat_hash_set<std::pair<std::string, bool>>* tup_l_sets;
  auto                                               is_input  = input_names.find(tup_head_l) != input_names.end();
  auto                                               is_output = output_names.find(tup_head_l) != output_names.end();

  if (!is_input && !is_output) {  // it's local variable or wire or register
    tup_l_sets = &var2flip[tup_head_l];
  } else {  // it's module io, check the global table
    tup_l_sets = &Inou_firrtl::glob_info.var2flip[lnast.get_top_module_name()][tup_head_l];
  }

  for (const auto& it : *tup_l_sets) {
    auto pos = it.first.find(hier_name_l);
    bool hit = false;
    if (pos != std::string::npos) {
      I(pos == 0);
      auto pos2 = hier_name_l.size();
      if (it.first.size() > pos2 && it.first.at(pos2) == '.') {
        hit = true;
      }
    }

    if (it.first == hier_name_l || hit) {
      tuple_flattened_connections(lnast, parent_node, hier_name_l, hier_name_r, it.first, it.second, stmt);
    }
  }
  return;
}

void Inou_firrtl_module::handle_lhs_instance_connections(Lnast& lnast, Lnast_nid& parent_node, std::string_view tup_head_l,
                                                         std::string_view hier_name_l, std::string_view hier_name_r,
                                                         const firrtl::FirrtlPB_Statement& stmt) {
  // general cases: lhs/rhs are flattened, just connect
  auto sub_module_name          = inst2module[tup_head_l];
  auto pos                      = hier_name_l.find_first_of('.');
  auto head_chopped_hier_name_l = hier_name_l.substr(pos + 1);
  if (Inou_firrtl::glob_info.module2inputs[sub_module_name].contains(head_chopped_hier_name_l)) {
    tuple_flattened_connections_instance_l(lnast, parent_node, hier_name_l, hier_name_r, false, true, stmt);
    return;
  }

  // rare cases:
  // if the module2inputs table doesn't contains the head_chopped_hier_name_l,
  // it means the connection is a tuple connection, you'll need to expand each
  // of the field to connect lhs/rhs

  std::string_view inst_name = tup_head_l;
  auto             pos2      = head_chopped_hier_name_l.find_first_of('.');
  std::string      head_chopped_hier_name_l_2;
  if (pos2 != std::string::npos) {
    head_chopped_hier_name_l_2 = head_chopped_hier_name_l.substr(0, pos2);
  }

  const auto tup_l_sets = &Inou_firrtl::glob_info.var2flip[sub_module_name][head_chopped_hier_name_l_2];
  for (const auto& it : *tup_l_sets) {
    std::string tup_hier_name_l = it.first;

    auto        p   = tup_hier_name_l.find(head_chopped_hier_name_l);
    bool        hit = false;
    std::string leaf_field;
    if (p != std::string::npos) {
      I(p == 0);
      auto p2 = head_chopped_hier_name_l.size();
      if (tup_hier_name_l.size() > p2 && tup_hier_name_l.at(p2) == '.') {
        hit        = true;
        leaf_field = tup_hier_name_l.substr(p2);
      }
    }

    // bool is_input = false;
    // if (Inou_firrtl::glob_info.module2inputs[sub_module_name].contains(tup_hier_name_l))
    //   is_input = true;

    if (hit) {
      tup_hier_name_l           = absl::StrCat(inst_name, ".", tup_hier_name_l);
      auto concated_hier_name_r = absl::StrCat(hier_name_r, leaf_field);
      tuple_flattened_connections_instance_l(lnast, parent_node, tup_hier_name_l, concated_hier_name_r, it.second, true, stmt);
    }
  }
  return;
}

void Inou_firrtl_module::handle_rhs_instance_connections(Lnast& lnast, Lnast_nid& parent_node, std::string_view tup_head_r,
                                                         std::string_view hier_name_l, std::string_view hier_name_r,
                                                         const firrtl::FirrtlPB_Statement& stmt) {
  // general cases: lhs/rhs are flattened, just connect
  auto sub_module_name          = inst2module[tup_head_r];
  auto pos                      = hier_name_r.find_first_of('.');
  auto head_chopped_hier_name_r = hier_name_r.substr(pos + 1);

  if (Inou_firrtl::glob_info.module2outputs[sub_module_name].contains(head_chopped_hier_name_r)) {
    tuple_flattened_connections_instance_r(lnast, parent_node, hier_name_l, hier_name_r, false, true, stmt);
    return;
  }

  // rare cases:
  // if the module2outputs table doesn't contains the head_chopped_hier_name_r,
  // it means the connection is a tuple connection, you'll need to expand each
  // of the field to connect lhs/rhs

  const auto tup_r_sets = &Inou_firrtl::glob_info.var2flip[sub_module_name][head_chopped_hier_name_r];
  for (const auto& it : *tup_r_sets) {
    std::string tup_hier_name_r = it.first;
    auto        p               = tup_hier_name_r.find(head_chopped_hier_name_r);
    bool        hit             = false;
    std::string leaf_field;
    if (p != std::string::npos) {
      I(p == 0);
      auto p2 = head_chopped_hier_name_r.size();
      if (tup_hier_name_r.size() > p2 && tup_hier_name_r.at(p2) == '.') {
        hit        = true;
        leaf_field = tup_hier_name_r.substr(p2);
      }
    }

    // bool is_output = false;
    // if (Inou_firrtl::glob_info.module2outputs[sub_module_name].contains(tup_hier_name_r))
    //   is_output = true;

    if (hit) {
      auto concated_hier_name_l = absl::StrCat(hier_name_l, leaf_field);
      auto concated_hier_name_r = absl::StrCat(hier_name_r, leaf_field);
      tuple_flattened_connections_instance_r(lnast, parent_node, concated_hier_name_l, concated_hier_name_r, it.second, true, stmt);
    }
  }
}

void Inou_firrtl_module::final_mem_interface_assign(Lnast& lnast, Lnast_nid& parent_node) {
  // const auto &loc_info = stmt.source_info().text();
  // uint64_t line_pos = 0;
  // uint64_t col_pos = 0;
  // if (loc_info!="") {
  //   std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
  //   line_pos = std::stoul(subtrngs[1], nullptr, 0);
  //   col_pos= std::stoul(subtrngs[2], nullptr, 0);
  // }
  for (auto& mem_name : memory_names) {
    // try to recover tuplpe field from the mem_din
    auto& idx_initialize_stmts = mem2initial_idx[mem_name];

    for (auto& it : mem2rd_mports[mem_name]) {
      auto mport_name      = it.first;
      auto cnt_of_rd_mport = it.second;

      auto&       loc_info = mem2rd_mport_loc[mport_name];
      uint64_t    line_pos = 0;
      uint64_t    col_pos  = 0;
      std::string fname    = "";
      if (loc_info != "") {
        std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
        line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
        col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
        fname                             = subtrngs[0];
      }

      auto idx_tg2        = lnast.add_child(idx_initialize_stmts, Lnast_node::create_tuple_get("", 0, line_pos, col_pos, fname));
      auto temp_var_name2 = create_tmp_var();
      lnast.add_child(idx_tg2, Lnast_node::create_ref(temp_var_name2, 0, line_pos, col_pos, fname));
      lnast.add_child(idx_tg2, Lnast_node::create_ref(absl::StrCat(mem_name, "_res"), 0, line_pos, col_pos, fname));
      lnast.add_child(idx_tg2, Lnast_node::create_const(cnt_of_rd_mport));

      auto idx_asg2 = lnast.add_child(idx_initialize_stmts, Lnast_node::create_assign("", 0, line_pos, col_pos, fname));
      lnast.add_child(idx_asg2, Lnast_node::create_ref(mport_name, 0, line_pos, col_pos, fname));
      lnast.add_child(idx_asg2, Lnast_node::create_ref(temp_var_name2, 0, line_pos, col_pos, fname));
    }

    std::vector<std::string> tmp_flattened_fields_per_port;
    auto&                    loc_info = memory_loc[mem_name];
    uint64_t                 line_pos = 0;
    uint64_t                 col_pos  = 0;
    std::string              fname    = "";
    if (loc_info != "") {
      std::vector<std::string> subtrngs = absl::StrSplit(loc_info, absl::ByAnyChar(": "));
      line_pos                          = std::stoul(subtrngs[1], nullptr, 0);
      col_pos                           = std::stoul(subtrngs[2], nullptr, 0);
      fname                             = subtrngs[0];
    }
    auto idx_ta_margs = lnast.add_child(parent_node, Lnast_node::create_tuple_add("", 0, line_pos, col_pos, fname));
    auto temp_var_str = create_tmp_var();
    lnast.add_child(idx_ta_margs, Lnast_node::create_ref(temp_var_str, 0, line_pos, col_pos, fname));

    auto idx_asg_addr = lnast.add_child(idx_ta_margs, Lnast_node::create_assign("", 0, line_pos, col_pos, fname));
    lnast.add_child(idx_asg_addr, Lnast_node::create_const("addr", 0, line_pos, col_pos, fname));
    lnast.add_child(idx_asg_addr, Lnast_node::create_ref(absl::StrCat(mem_name, "_addr"), 0, line_pos, col_pos, fname));

    auto idx_asg_clock = lnast.add_child(idx_ta_margs, Lnast_node::create_assign("", 0, line_pos, col_pos, fname));
    lnast.add_child(idx_asg_clock, Lnast_node::create_const("clock", 0, line_pos, col_pos, fname));
    lnast.add_child(idx_asg_clock, Lnast_node::create_ref(absl::StrCat(mem_name, "_clock"), 0, line_pos, col_pos, fname));

    auto idx_asg_din = lnast.add_child(idx_ta_margs, Lnast_node::create_assign("", 0, line_pos, col_pos, fname));
    lnast.add_child(idx_asg_din, Lnast_node::create_const("din", 0, line_pos, col_pos, fname));
    lnast.add_child(idx_asg_din, Lnast_node::create_ref(absl::StrCat(mem_name, "_din"), 0, line_pos, col_pos, fname));

    auto idx_asg_enable = lnast.add_child(idx_ta_margs, Lnast_node::create_assign("", 0, line_pos, col_pos, fname));
    lnast.add_child(idx_asg_enable, Lnast_node::create_const("enable", 0, line_pos, col_pos, fname));
    lnast.add_child(idx_asg_enable, Lnast_node::create_ref(absl::StrCat(mem_name, "_enable"), 0, line_pos, col_pos, fname));

    auto idx_asg_fwd = lnast.add_child(idx_ta_margs, Lnast_node::create_assign("", 0, line_pos, col_pos, fname));
    lnast.add_child(idx_asg_fwd, Lnast_node::create_const("fwd", 0, line_pos, col_pos, fname));
    lnast.add_child(idx_asg_fwd, Lnast_node::create_ref(absl::StrCat(mem_name, "_fwd"), 0, line_pos, col_pos, fname));

    auto idx_asg_lat = lnast.add_child(idx_ta_margs, Lnast_node::create_assign("", 0, line_pos, col_pos, fname));
    lnast.add_child(idx_asg_lat, Lnast_node::create_const("type", 0, line_pos, col_pos, fname));
    lnast.add_child(idx_asg_lat, Lnast_node::create_ref(absl::StrCat(mem_name, "_type"), 0, line_pos, col_pos, fname));

    auto idx_asg_wensize = lnast.add_child(idx_ta_margs, Lnast_node::create_assign("", 0, line_pos, col_pos, fname));
    lnast.add_child(idx_asg_wensize, Lnast_node::create_const("wensize", 0, line_pos, col_pos, fname));
    lnast.add_child(idx_asg_wensize, Lnast_node::create_ref(absl::StrCat(mem_name, "_wensize"), 0, line_pos, col_pos, fname));

    auto idx_asg_size = lnast.add_child(idx_ta_margs, Lnast_node::create_assign("", 0, line_pos, col_pos, fname));
    lnast.add_child(idx_asg_size, Lnast_node::create_const("size", 0, line_pos, col_pos, fname));
    lnast.add_child(idx_asg_size, Lnast_node::create_ref(absl::StrCat(mem_name, "_size"), 0, line_pos, col_pos, fname));

    auto idx_asg_rdport = lnast.add_child(idx_ta_margs, Lnast_node::create_assign("", 0, line_pos, col_pos, fname));
    lnast.add_child(idx_asg_rdport, Lnast_node::create_const("rdport", 0, line_pos, col_pos, fname));
    lnast.add_child(idx_asg_rdport, Lnast_node::create_ref(absl::StrCat(mem_name, "_rdport"), 0, line_pos, col_pos, fname));

    auto idx_asg_margs = lnast.add_child(parent_node, Lnast_node::create_assign("", 0, line_pos, col_pos, fname));
    lnast.add_child(idx_asg_margs, Lnast_node::create_ref(absl::StrCat(mem_name, "_interface_args"), 0, line_pos, col_pos, fname));
    lnast.add_child(idx_asg_margs, Lnast_node::create_ref(temp_var_str, 0, line_pos, col_pos, fname));
  }
}

//--------------Modules/Circuits--------------------
// Create basis of LNAST tree. Set root to "top" and have "stmts" be top's child.
void Inou_firrtl::user_module_to_lnast(Eprp_var& var, const firrtl::FirrtlPB_Module& fmodule, std::string_view file_name) {
#ifndef NDEBUG
  std::cout << std::format("Module (user): {}\n", fmodule.user_module().id());
#endif

  Inou_firrtl_module firmod;

  std::unique_ptr<Lnast> lnast = std::make_unique<Lnast>(fmodule.user_module().id(), file_name);

  const firrtl::FirrtlPB_Module_UserModule& user_module = fmodule.user_module();

  lnast->set_root(Lnast_node::create_top());
  auto idx_stmts = lnast->add_child(lh::Tree_index::root(), Lnast_node::create_stmts());

  // Iterate over I/O of the module.
  for (int i = 0; i < user_module.port_size(); i++) {
    const firrtl::FirrtlPB_Port& port = user_module.port(i);
    firmod.list_port_info(*lnast, port, idx_stmts);
  }

  // Iterate over statements of the module.
  for (int j = 0; j < user_module.statement_size(); j++) {
    const firrtl::FirrtlPB_Statement& stmt = user_module.statement(j);
    // PreCheckForMem(*lnast, stmt, idx_stmts);
    firmod.list_statement_info(*lnast, stmt, idx_stmts);
  }

  // Inou_firrtl_module::dump_var2flip(firmod.var2flip);
  firmod.final_mem_interface_assign(*lnast, idx_stmts);

  std::lock_guard<std::mutex> guard(eprp_var_mutex);
  { var.add(std::move(lnast)); }
}

void Inou_firrtl::ext_module_to_lnast(Eprp_var& var, const firrtl::FirrtlPB_Module& fmodule, std::string_view file_name) {
#ifndef NDEBUG
  std::cout << std::format("Module (ext): {}\n", fmodule.external_module().id());
#endif

  Inou_firrtl_module                            firmod;
  std::unique_ptr<Lnast>                        lnast      = std::make_unique<Lnast>(fmodule.external_module().id(), file_name);
  const firrtl::FirrtlPB_Module_ExternalModule& ext_module = fmodule.external_module();

  lnast->set_root(Lnast_node::create_top());
  auto idx_stmts = lnast->add_child(lh::Tree_index::root(), Lnast_node::create_stmts());

  // Iterate over I/O of the module.
  for (int i = 0; i < ext_module.port_size(); i++) {
    const firrtl::FirrtlPB_Port& port = ext_module.port(i);
    firmod.list_port_info(*lnast, port, idx_stmts);
  }

  std::lock_guard<std::mutex> guard(eprp_var_mutex);
  { var.add(std::move(lnast)); }
}

void Inou_firrtl::populate_all_modules_io(Eprp_var& var, const firrtl::FirrtlPB_Circuit& circuit, std::string_view file_name) {
  // TRACE_EVENT("inou", "populate_all_modules_io");
  Graph_library* lib = Graph_library::instance(var.get("path", "lgdb"));

  for (int i = 0; i < circuit.module_size(); i++) {
    if (circuit.module(i).has_user_module()) {
      // populate_module_io(i, circuit, file_name, lib);
      thread_pool.add([this, &circuit, i, &file_name, &lib]() -> void {
        // TRACE_EVENT("inou", nullptr, [&i, &circuit](perfetto::EventContext ctx) {
        // ctx.event()->set_name("fir_tolnast:sub_module_io:" + circuit.module(i).user_module().id()); });
        this->populate_module_io(i, circuit, file_name, lib);
      });

    } else if (circuit.module(i).has_external_module()) {
      Pass::warn("ext_module have not implemented: {}", circuit.module(i).external_module().defined_name());
    } else {
      Pass::error("Module not set.");
    }
  }
  thread_pool.wait_all();
}

void Inou_firrtl::populate_module_io(int i, const firrtl::FirrtlPB_Circuit& circuit, std::string_view file_name,
                                     Graph_library* lib) {
  auto     module_i_user_module_id = circuit.module(i).user_module().id();
  auto*    sub                     = lib->create_sub(module_i_user_module_id, file_name);
  uint64_t inp_pos                 = 0;
  uint64_t out_pos                 = 0;

  for (int j = 0; j < circuit.module(i).user_module().port_size(); j++) {
    auto port        = circuit.module(i).user_module().port(j);
    auto initial_set = absl::flat_hash_set<std::pair<std::string, bool>>{};
    glob_info.var2flip[module_i_user_module_id].insert_or_assign(port.id(), initial_set);
    add_port_to_map(module_i_user_module_id, port.type(), port.direction(), false, port.id(), *sub, inp_pos, out_pos);
  }
}

void Inou_firrtl::initialize_global_tables(const firrtl::FirrtlPB_Circuit& circuit) {
  for (int i = 0; i < circuit.module_size(); i++) {
    auto module_i_user_module_id = circuit.module(i).user_module().id();
    absl::flat_hash_map<std::string, absl::flat_hash_set<std::pair<std::string, bool>>> empty_map;
    glob_info.var2flip.insert_or_assign(module_i_user_module_id, empty_map);
    auto empty_map2 = absl::flat_hash_map<std::string, std::pair<uint16_t, bool>>{};
    glob_info.module2outputs.insert_or_assign(module_i_user_module_id, empty_map2);
    auto empty_set = absl::flat_hash_set<std::string>{};
    glob_info.module2inputs.insert_or_assign(module_i_user_module_id, empty_set);
  }
}

/* Used to populate Sub_Nodes so that when Lgraphs are constructed,
 * all the Lgraphs will be able to populate regardless of order. */
void Inou_firrtl::add_port_sub(Sub_node& sub, uint64_t& inp_pos, uint64_t& out_pos, std::string_view port_id, uint8_t dir) {
  if (dir == 1) {                // PORT_DIRECTION_IN
    sub.add_input_pin(port_id);  //, inp_pos);
    inp_pos++;
  } else {
    sub.add_output_pin(port_id);  //, out_pos);
    out_pos++;
  }
}

void Inou_firrtl_module::add_local_flip_info(bool flipped_in, std::string_view id) {
  auto found = id.find_first_of('.');
  // case-I: scalar flop or scalar wire or firrtl node
  if (found == std::string::npos) {
    auto pair = std::make_pair(std::string(id), flipped_in);
    I(var2flip.find(id) == var2flip.end());

    auto new_set = absl::flat_hash_set<std::pair<std::string, bool>>{};
    new_set.insert(pair);
    var2flip.insert_or_assign(id, new_set);
    return;
  }

  // case-II: hier-flop or hier-wire
  I(found != std::string::npos);
  auto lnast_tupname = id.substr(0, found);
  auto pair          = std::make_pair(std::string(id), flipped_in);
  auto set_itr       = var2flip.find(lnast_tupname);
  if (set_itr == var2flip.end()) {
    auto new_set = absl::flat_hash_set<std::pair<std::string, bool>>{};
    new_set.insert(pair);
    var2flip.insert_or_assign(lnast_tupname, new_set);
  } else {
    set_itr->second.insert(pair);
  }
}

void Inou_firrtl::add_global_io_flipness(std::string_view mod_id, bool flipped_in, std::string_view port_id, uint8_t dir) {
  (void)dir;
  auto found = port_id.find_first_of('.');
  if (found == std::string::npos) {
    auto pair    = std::make_pair(std::string(port_id), flipped_in);
    auto new_set = absl::flat_hash_set<std::pair<std::string, bool>>{};

    new_set.insert(pair);
    glob_info.var2flip[mod_id].insert_or_assign(port_id, new_set);
    return;
  }

  auto lnast_tupname = port_id.substr(0, found);
  auto pair          = std::make_pair(std::string(port_id), flipped_in);
  auto set_itr       = glob_info.var2flip[mod_id].find(lnast_tupname);
  if (set_itr == glob_info.var2flip[mod_id].end()) {
    auto new_set = absl::flat_hash_set<std::pair<std::string, bool>>{};
    new_set.insert(pair);
    glob_info.var2flip[mod_id].insert_or_assign(lnast_tupname, new_set);
  } else {
    set_itr->second.insert(pair);
  }
}

void Inou_firrtl::add_port_to_map(std::string_view mod_id, const firrtl::FirrtlPB_Type& type, uint8_t dir, bool flipped_in,
                                  std::string_view port_id, Sub_node& sub, uint64_t& inp_pos, uint64_t& out_pos) {
  switch (type.type_case()) {
    case firrtl::FirrtlPB_Type::kBundleType: {  // Bundle type
      auto& btype = type.bundle_type();

      for (int i = 0; i < type.bundle_type().field_size(); i++) {
        auto concat_port_id = absl::StrCat(port_id, ".", btype.field(i).id());
        if (btype.field(i).is_flipped()) {
          uint8_t new_dir = 0;
          if (dir == 1) {  // PORT_DIRECTION_IN
            new_dir = 2;
          } else if (dir == 2) {
            new_dir = 1;
          }
          I(new_dir != 0);
          add_port_to_map(mod_id, btype.field(i).type(), new_dir, !flipped_in, concat_port_id, sub, inp_pos, out_pos);
        } else {
          add_port_to_map(mod_id,
                          btype.field(i).type(),
                          dir,
                          flipped_in,
                          absl::StrCat(port_id, ".", btype.field(i).id()),
                          sub,
                          inp_pos,
                          out_pos);
        }
      }
      break;
    }

    case firrtl::FirrtlPB_Type::kVectorType: {  // Vector type
      absl::flat_hash_map<std::string, uint16_t> var2vec_size;
      var2vec_size.insert_or_assign(port_id, type.vector_type().size());
      glob_info.module_var2vec_size.insert_or_assign(mod_id, var2vec_size);
      for (uint32_t i = 0; i < type.vector_type().size(); i++) {
        add_port_to_map(mod_id, type.vector_type().type(), dir, flipped_in, absl::StrCat(port_id, ".", i), sub, inp_pos, out_pos);
      }
      break;
    }
    case firrtl::FirrtlPB_Type::kUintType: {  // UInt type
      add_port_sub(sub, inp_pos, out_pos, port_id, dir);
      if (dir == 1) {
        glob_info.module2inputs[mod_id].insert({std::string{port_id}});
      } else if (dir == 2) {
        auto bits = type.uint_type().width().value();
        glob_info.module2outputs[mod_id].insert({std::string{port_id}, std::make_pair(bits, false)});
      }
      if (type.uint_type().width().value() != 0) {
        add_global_io_flipness(mod_id, flipped_in, port_id, dir);
      }
      break;
    }
    case firrtl::FirrtlPB_Type::kSintType: {  // SInt type
      add_port_sub(sub, inp_pos, out_pos, port_id, dir);
      if (dir == 1) {
        glob_info.module2inputs[mod_id].insert({std::string{port_id}});
      } else if (dir == 2) {
        auto bits = type.uint_type().width().value();
        glob_info.module2outputs[mod_id].insert({std::string{port_id}, std::make_pair(bits, true)});
      }
      if (type.sint_type().width().value() != 0) {
        add_global_io_flipness(mod_id, flipped_in, port_id, dir);
      }
      break;
    }
    case firrtl::FirrtlPB_Type::kResetType:
    case firrtl::FirrtlPB_Type::kAsyncResetType:
    case firrtl::FirrtlPB_Type::kClockType: {
      add_port_sub(sub, inp_pos, out_pos, port_id, dir);
      if (dir == 1) {
        glob_info.module2inputs[mod_id].insert({std::string{port_id}});
      } else if (dir == 2) {
        auto bits = type.uint_type().width().value();
        glob_info.module2outputs[mod_id].insert({std::string{port_id}, std::make_pair(bits, false)});
      }
      add_global_io_flipness(mod_id, flipped_in, port_id, dir);
      break;
    }
    case firrtl::FirrtlPB_Type::kFixedType: {  // Fixed type
      I(false);                                // TODO: Not yet supported.
      break;
    }
    case firrtl::FirrtlPB_Type::kAnalogType: {  // Analog type
      I(false);                                 // TODO: Not yet supported.
      break;
    }
    default: Pass::error("Unknown port type.");
  }
}

/* Not much to do here since this is just a Verilog
 * module that FIRRTL is going to use. Will have to
 * rely upon some Verilog pass to get the actual
 * contents of this into Lgraph form. */
void Inou_firrtl::grab_ext_module_info(const firrtl::FirrtlPB_Module_ExternalModule& emod) {
  // Figure out all of mods IO and their respective bw + dir.
  std::vector<std::tuple<std::string, uint8_t, uint32_t, bool>>
      port_list;  // Terms are as follows: name, direction, # of bits, sign.
  for (int i = 0; i < emod.port_size(); i++) {
    const auto& port = emod.port(i);
    create_io_list(port.type(), port.direction(), port.id(), port_list);
  }

  // Figure out what the value for each parameter is, add to map.
  for (int j = 0; j < emod.parameter_size(); j++) {
    std::string param_str;
    switch (emod.parameter(j).value_case()) {
      case firrtl::FirrtlPB_Module_ExternalModule_Parameter::kInteger:
        param_str = convert_bigint_to_str(emod.parameter(j).integer());
        break;
      case firrtl::FirrtlPB_Module_ExternalModule_Parameter::kDouble: param_str = emod.parameter(j).double_(); break;
      case firrtl::FirrtlPB_Module_ExternalModule_Parameter::kString: param_str = emod.parameter(j).string(); break;
      case firrtl::FirrtlPB_Module_ExternalModule_Parameter::kRawString: param_str = emod.parameter(j).raw_string(); break;
      default: I(false);
    }
    glob_info.ext_module2param[emod.defined_name()].insert({emod.parameter(j).id(), param_str});
  }

  // Add them to the map to let us know what ports exist in this module.
  // for (const auto& elem : port_list) {
  // glob_info.module2outputs[std::pair(emod.defined_name(), std::get<0>(elem))] = std::get<1>(elem);
  // }
}

/* This function is used for the following syntax rules in FIRRTL:
 * creating a wire, creating a register, instantiating an input/output (port),
 *
 * This function returns a pair which holds the full name of a wire/output/input/register
 * and the bitwidth of it (if the bw is 0, that means the bitwidth will be inferred later.
 */
void Inou_firrtl::create_io_list(const firrtl::FirrtlPB_Type& type, uint8_t dir, std::string_view port_id,
                                 std::vector<std::tuple<std::string, uint8_t, uint32_t, bool>>& vec) {
  switch (type.type_case()) {
    case firrtl::FirrtlPB_Type::kUintType: {  // UInt type
      vec.emplace_back(port_id, dir, type.uint_type().width().value(), false);
      break;
    }
    case firrtl::FirrtlPB_Type::kSintType: {  // SInt type
      vec.emplace_back(port_id, dir, type.sint_type().width().value(), true);
      break;
    }
    case firrtl::FirrtlPB_Type::kClockType: {  // Clock type
      // vec.emplace_back(port_id, dir, 1, false);
      vec.emplace_back(port_id, dir, 1, true);  // intentionally put 1 signed bits, LiveHD compiler will handle clock bits later
      break;
    }
    case firrtl::FirrtlPB_Type::kBundleType: {  // Bundle type
      const auto& btype = type.bundle_type();
      for (int i = 0; i < type.bundle_type().field_size(); i++) {
        if (btype.field(i).is_flipped()) {
          uint8_t new_dir = 0;
          if (dir == 1) {  // PORT_DIRECTION_IN
            new_dir = 2;
          } else if (dir == 2) {
            new_dir = 1;
          }
          I(new_dir != 0);
          create_io_list(btype.field(i).type(), new_dir, absl::StrCat(port_id, ".", btype.field(i).id()), vec);
        } else {
          create_io_list(btype.field(i).type(), dir, absl::StrCat(port_id, ".", btype.field(i).id()), vec);
        }
      }
      break;
    }
    case firrtl::FirrtlPB_Type::kVectorType: {  // Vector type
      for (uint32_t i = 0; i < type.vector_type().size(); i++) {
        vec.emplace_back(port_id, dir, 0, false);
        create_io_list(type.vector_type().type(), dir, absl::StrCat(port_id, ".", i), vec);
      }
      break;
    }
    case firrtl::FirrtlPB_Type::kFixedType: {  // Fixed type
      I(false);                                // FIXME: Not yet supported.
      break;
    }
    case firrtl::FirrtlPB_Type::kAnalogType: {  // Analog type
      I(false);                                 // FIXME: Not yet supported.
      break;
    }
    case firrtl::FirrtlPB_Type::kAsyncResetType: {  // AsyncReset type
      vec.emplace_back(port_id, dir, 1, false);
      // FIXME: handle it when encountered
      // async_rst_names.insert(port_id);
      break;
    }
    case firrtl::FirrtlPB_Type::kResetType: {  // Reset type
      vec.emplace_back(port_id, dir, 1, false);
      break;
    }
    default: Pass::error("Unknown port type.");
  }
}

std::string Inou_firrtl::convert_bigint_to_str(const firrtl::FirrtlPB_BigInt& bigint) {
  if (bigint.value().size() == 0) {
    return "0b0";
  }

  std::string bigint_val("0b");
  for (char bigint_char : bigint.value()) {
    std::string bit_str;
    for (int j = 0; j < 8; j++) {
      if (bigint_char % 2) {
        bit_str = absl::StrCat("1", bit_str);
      } else {
        bit_str = absl::StrCat("0", bit_str);
      }
      bigint_char >>= 1;
    }
    absl::StrAppend(&bigint_val, bit_str);
  }

  return bigint_val;
}

void Inou_firrtl::iterate_modules(Eprp_var& var, const firrtl::FirrtlPB_Circuit& circuit, std::string_view file_name) {
  if (circuit.top_size() > 1) {
    Pass::error("More than 1 top module specified.");
    I(false);
  }

  // Create ModuleName to I/O Pair List

  initialize_global_tables(circuit);
  populate_all_modules_io(var, circuit, file_name);

  for (int i = 0; i < circuit.module_size(); i++) {
    if (circuit.module(i).has_external_module()) {
      // circuit.module(i).external_module();
      grab_ext_module_info(circuit.module(i).external_module());
    }
  }
  // so far you collect all global table informations

  para_modules_to_lnasts(circuit, var, file_name);
}

void Inou_firrtl::para_modules_to_lnasts(const firrtl::FirrtlPB_Circuit& circuit, Eprp_var& var, std::string_view file_name) {
  // TRACE_EVENT("inou", "para_modules_to_lnasts");
  // parallelize the rest of firrtl modules -> lnasts
  for (int i = circuit.module_size() - 1; i >= 0; i--) {
    if (circuit.module(i).has_user_module()) {
      thread_pool.add([this, &var, &circuit, i, &file_name]() -> void {
        // TRACE_EVENT("inou", nullptr, [&i, &circuit](perfetto::EventContext ctx) { ctx.event()->set_name("fir_tolnast:module:" +
        // circuit.module(i).user_module().id()); });

        TRACE_EVENT("inou", nullptr, [&i, &circuit](perfetto::EventContext ctx) {
          std::string converted_str{(char)('A' + (trace_module_cnt++ % 25))};
          auto        str = "fir_tlnast:module:" + converted_str;
          ctx.event()->set_name(str + circuit.module(i).user_module().id());
        });

        this->user_module_to_lnast(var, circuit.module(i), file_name);
      });
    } else if (circuit.module(i).has_external_module()) {
      thread_pool.add([this, &var, &circuit, i, &file_name]() -> void {
        TRACE_EVENT("inou", nullptr, [&i, &circuit](perfetto::EventContext ctx) {
          ctx.event()->set_name("fir_tolnast:module:" + circuit.module(i).user_module().id());
        });
        this->ext_module_to_lnast(var, circuit.module(i), file_name);
      });
    } else {
      Pass::error("Module not set.");
    }
  }
  thread_pool.wait_all();
}

// Iterate over every FIRRTL circuit (design), each circuit can contain multiple modules.
void Inou_firrtl::iterate_circuits(Eprp_var& var, const firrtl::FirrtlPB& firrtl_input, std::string_view file_name) {
  for (int i = 0; i < firrtl_input.circuit_size(); i++) {
    Inou_firrtl::glob_info.module2outputs.clear();
    Inou_firrtl::glob_info.module2inputs.clear();
    Inou_firrtl::glob_info.ext_module2param.clear();
    Inou_firrtl::glob_info.var2flip.clear();

    const firrtl::FirrtlPB_Circuit& circuit = firrtl_input.circuit(i);
    iterate_modules(var, circuit, file_name);
  }
}
