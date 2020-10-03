// Copyright (C) 2017-2019 Jonathan M�ller <jonathanmueller.dev@gmail.com>
// This file is subject to the license terms in the LICENSE file
// found in the top-level directory of this distribution.
// 
#include <windows.h>
#undef min
#undef max

#include <iostream>
#include <filesystem>
#include <fstream>

#include <cxxopts.hpp>

#include <cppast/code_generator.hpp>         // for generate_code()
#include <cppast/cpp_entity_kind.hpp>        // for the cpp_entity_kind definition
#include <cppast/cpp_forward_declarable.hpp> // for is_definition()
#include <cppast/cpp_member_function.hpp>
#include <cppast/cpp_namespace.hpp>          // for cpp_namespace
#include <cppast/cpp_type_alias.hpp>
#include <cppast/libclang_parser.hpp> // for libclang_parser, libclang_compile_config, cpp_entity,...
#include <cppast/visitor.hpp>         // for visit()

class win_logger : public cppast::diagnostic_logger {
public:
    using diagnostic_logger::diagnostic_logger;

private:
    mutable char buf[8192];
    bool do_log(const char *source, const cppast::diagnostic &d) const override {
        auto loc = d.location.to_string();
        if (loc.empty())
            sprintf_s(buf, "[%s] [%s] %s\n", source, to_string(d.severity), d.message.c_str());
        else
            sprintf_s(buf, "[%s] [%s] %s %s\n", source, to_string(d.severity),
                         d.location.to_string().c_str(), d.message.c_str());

        OutputDebugStringA(buf);
        return true;
    }
};

// parse a file
std::unique_ptr<cppast::cpp_file> parse_file(const cppast::libclang_compile_config& config,
                                             const cppast::diagnostic_logger&       logger,
                                             cppast::cpp_entity_index&                 idx,
                                             std::string filename, bool fatal_error)
{
    // the parser is used to parse the entity
    // there can be multiple parser implementations
    cppast::libclang_parser parser(type_safe::ref(logger));
    // parse the file
    auto file = parser.parse(idx, filename, config);
    if (fatal_error && parser.error())
        return nullptr;
    return file;
}

std::string replace_backslashes(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

enum class type_information {
    unknown,
    is_float,
    is_integral,
    is_unsigned,
    is_string,
    is_pointer
};

const cppast::cpp_type& walk_typedefs(const cppast::cpp_type& base_type, const cppast::cpp_entity_index& idx) {
    if(base_type.kind() != cppast::cpp_type_kind::user_defined_t)
        return base_type;

    const auto& user_type = static_cast<const cppast::cpp_user_defined_type&>(base_type);
    const auto& type_entity = static_cast<const cppast::cpp_type_alias&>(user_type.entity().get(idx)[0u].get());
    const auto& underlying_type = type_entity.underlying_type();
    return walk_typedefs(underlying_type, idx);
}

type_information determine_type_information(const cppast::cpp_type& in_type, const cppast::cpp_entity_index& idx) {
    const auto& type = walk_typedefs(in_type, idx);
    if(type.kind() == cppast::cpp_type_kind::pointer_t) {
        const auto& ptr = static_cast<const cppast::cpp_pointer_type&>(type);
        if(ptr.pointee().kind() == cppast::cpp_type_kind::builtin_t) {
            const auto& builtin = static_cast<const cppast::cpp_builtin_type&>(ptr.pointee());
            if(builtin.builtin_type_kind() == cppast::cpp_char)
                return type_information::is_string;
        }
        return type_information::is_pointer;
    } else if(type.kind() == cppast::cpp_type_kind::builtin_t) {
        const auto& builtin = static_cast<const cppast::cpp_builtin_type&>(type);
        
        switch (builtin.builtin_type_kind()) {
        using namespace cppast;
        case cpp_bool:
        case cpp_schar:
        case cpp_short:
        case cpp_int:
        case cpp_long:
        case cpp_longlong:
            return type_information::is_integral;
        case cpp_uchar:
        case cpp_ushort:
        case cpp_uint:
        case cpp_ulong:
        case cpp_ulonglong:
            return type_information::is_unsigned;
        case cpp_float:
        case cpp_double:
            return type_information::is_float;
        default:
            return type_information::unknown;
        }
    }

    return type_information::unknown;
}

void generate_function_hook(const cppast::cpp_entity& entity, const std::string& context, const cppast::cpp_entity_index& idx, std::ofstream& output, std::ofstream& output_header) {
    size_t gen_idx = 0;

    const auto& func = static_cast<const cppast::cpp_function&>(entity);
    std::string func_name = context + func.name();
    
    std::string enum_context = context;
    std::replace(enum_context.begin(), enum_context.end(), ':', '_');
    std::string enum_val = "HookedCall::" + enum_context + func.name();

    bool no_plugins = has_attribute(entity, "NoPlugins").has_value();

    if(!no_plugins)
        output_header << "\t" << enum_context + func.name() << "," << std::endl;

    std::list<std::tuple<std::string, std::string, const cppast::cpp_function_parameter&>> args;
    for(const auto& arg : func.parameters()) {
        std::string arg_name = arg.name().empty() ? ("_genArg" + std::to_string(++gen_idx)) : arg.name();
        args.emplace_back(to_string(arg.type()), arg_name, arg);
    }

    output << to_string(func.return_type()) << " " << func_name << "(";
    for(auto& a : args) output << std::get<0>(a) << " " << std::get<1>(a) << ", ";
    if(!args.empty())
        output.seekp(-2, std::fstream::cur);
    output << ") {" << std::endl;

    output << "\tAddDebugLog(\"" << func_name << "(\\n";
    for(auto& a : args) {
        output << "\\t" << std::get<0>(a) << " " << std::get<1>(a) << " = %";
        auto type_info = determine_type_information(std::get<2>(a).type(), idx);
        switch(type_info) {
        case type_information::is_integral:
            output << "d";
            break;
        case type_information::is_unsigned:
            output << "u";
            break;
        case type_information::is_float:
            output << "f";
            break;
        case type_information::is_pointer:
            output << "p";
            break;
        case type_information::is_string:
        default:
            output << "s";
            break;
        }

        output << "\\n";
    }
    output << ")\"," << std::endl << "\t\t\t";
    for(auto& a : args) {
        output << std::get<1>(a) << ", ";
    }
    if(!args.empty())
        output.seekp(-2, std::fstream::cur);
    output << ");" << std::endl << std::endl;


    if(!no_plugins) {
        output << "\tauto [retVal, skip] = CallPluginsBefore<" << to_string(func.return_type()) << ">(" << enum_val << ",\n\t\t\t";
        for(auto& a : args) {
            output << std::get<1>(a) << ", ";
        }
        if(!args.empty())
            output.seekp(-2, std::fstream::cur);
        output << ");" << std::endl << std::endl;
    }

    output << "\t";
    if(!no_plugins)
        output << "if(!skip) retVal = ";
    else
        output << "return ";
    output << "CALL_CLIENT_METHOD(" << func.name() << "(";
    for(auto& a : args)
        output << std::get<1>(a) << ", ";
    if(!args.empty())
        output.seekp(-2, std::fstream::cur);
    output << "));" << std::endl;

    if(!no_plugins) {
        output << std::endl << "\tCallPluginsAfter(" << enum_val << ",\n\t\t\t";
        for(auto& a : args)
            output << std::get<1>(a) << ", ";
        if(!args.empty())
            output.seekp(-2, std::fstream::cur);
        output << ");" << std::endl << std::endl;

        output << "\treturn retVal;" << std::endl;
    }

    output << "}" << std::endl << std::endl;
}

void generate_hooks(const cppast::cpp_entity& e, const std::string& context, const cppast::cpp_entity_index& idx, std::ofstream& output, std::ofstream& output_header) {
    cppast::visit(e, [&](const cppast::cpp_entity& entity, cppast::visitor_info) {
        if(entity.kind() == cppast::cpp_entity_kind::member_function_t) {
            generate_function_hook(entity, context + e.name() + "::", idx, output, output_header);
        }
    });
}

void locate_hooks(const cppast::cpp_entity& base, const std::string& context, const cppast::cpp_entity_index& idx, std::ofstream& output, std::ofstream& output_header) {
    cppast::visit(base, [&](const cppast::cpp_entity& entity, cppast::visitor_info) {
        if(&entity == &base)
            return;

        OutputDebugStringA((entity.name() + "\n").c_str());

        switch(entity.kind()) {
        case cppast::cpp_entity_kind::class_t:
        case cppast::cpp_entity_kind::base_class_t:
            if(const auto& att = cppast::has_attribute(entity.attributes(), "Hook"))
                generate_hooks(entity, context, idx, output, output_header);
            break;
        case cppast::cpp_entity_kind::namespace_t:
            locate_hooks(entity, context + entity.name() + "::", idx, output, output_header);
            break;
        default:
            break;
        }
    });
}

int main(int argc, char* argv[])
{
    if(argc != 2)
        return 1;

    try {
        std::filesystem::path sln_dir = argv[1];
        std::filesystem::path gen_file = (sln_dir / "../source/__generated.cpp").lexically_normal();
        std::filesystem::path gen_header = (sln_dir / "../FLHookSDK/include/__generated.h").lexically_normal();
        std::ofstream output(gen_file.c_str());
        output << "//\n// WARNING: THIS IS AN AUTO-GENERATED FILE, DO NOT EDIT!\n//" << std::endl << std::endl;
        output << "#include <Hook.h>" << std::endl << std::endl;

        std::ofstream output_header(gen_header.c_str());
        output_header << "#pragma once\n\n//\n// WARNING: THIS IS AN AUTO-GENERATED FILE, DO NOT EDIT!\n//" << std::endl << std::endl;

        output_header << "enum class HookedCall {" << std::endl;

        cppast::libclang_compile_config config;
        config.set_flags(cppast::cpp_standard::cpp_latest, cppast::compile_flag::ms_extensions | cppast::compile_flag::ms_compatibility);
        config.define_macro("ST6_ALLOCATION_DEFINED", "1");
        config.define_macro("_X86_", "1");
        config.define_macro("IMPORT", "");
        config.define_macro("EXPORT", "");
        config.add_include_dir(replace_backslashes((sln_dir / "..\\FLHookSDK\\include").lexically_normal().string()));
        
        // the entity index is used to resolve cross references in the AST
        cppast::cpp_entity_index idx;

        // the logger is used to print diagnostics
        win_logger logger;

        auto core_defs = parse_file(config, logger, idx, replace_backslashes((sln_dir / "..\\FLHookSDK\\include\\FLCoreDefs.h").lexically_normal().string()), false);

        auto remote_client = parse_file(config, logger, idx, replace_backslashes((sln_dir / "..\\FLHookSDK\\include\\FLCoreRemoteClient.h").lexically_normal().string()), false);
        locate_hooks(*remote_client.get(), "", idx, output, output_header);

        output_header << "};";
    } catch(...) {
        return 1;
    }
}