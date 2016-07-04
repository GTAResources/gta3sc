///
/// Disassembler
///
/// The disassembler is responsible for converting the SCM bytecode into a vector of pseudo-instructions,
/// which can be easily parsed by our program.
///
#pragma once
#include "stdinc.h"
#include "commands.hpp"
#include "compiler.hpp" // for Compiled* types
#include "program.hpp"

struct BinaryFetcher;
struct Disassembler;

//using EOAL = EOAL;

// contrasts to CompiledVar
struct DecompiledVar
{
    bool     global;
    uint32_t offset; // if global, i*1, if local, i*4;

    bool operator==(const DecompiledVar& rhs) const
    {
        return this->global == rhs.global && this->offset == rhs.offset;
    }
};

// contrasts to CompiledVar
struct DecompiledVarArray
{
    DecompiledVar base;
    DecompiledVar index;
};

// contrasts to CompiledString
using DecompiledString = CompiledString;

// contrasts to ArgVariant
// TODO rename
using ArgVariant2 = variant<EOAL, int8_t, int16_t, int32_t, float, /*LABEL,*/ DecompiledVar, DecompiledVarArray, DecompiledString>;

// contrasts to CompiledCommand
struct DecompiledCommand
{
    uint16_t                 id;
    std::vector<ArgVariant2> args; 
};

// contrasts to CompiledLabelDef
struct DecompiledLabelDef
{
    size_t offset;      //< Local offset (relative to self-mission-base or main-base)
};

// contrasts to CompiledHex
using DecompiledHex = CompiledHex;

// contrasts to CompiledScmHeader
struct DecompiledScmHeader
{
    enum class Version : uint8_t
    {
        Liberty,
        Miami,
    };

    Version                               version;
    uint32_t                              size_global_vars_space; // including the 8 bytes of GOTO at the top
    std::vector<std::string>              models;
    uint32_t                              main_size;
    std::vector<uint32_t>                 mission_offsets;

    DecompiledScmHeader(Version version, uint32_t size_globals, std::vector<std::string> models,
        uint32_t main_size, std::vector<uint32_t> mission_offsets) :
        version(version), size_global_vars_space(size_globals), main_size(main_size),
        models(std::move(models)), mission_offsets(std::move(mission_offsets))
    {
    }

    static optional<DecompiledScmHeader> from_bytecode(const uint8_t* bytecode, size_t bytecode_size, Version version);
};

// contrasts to CompiledData
struct DecompiledData
{
    size_t                                                        offset;   //< Local offset of this piece of data
    variant<DecompiledLabelDef, DecompiledCommand, DecompiledHex> data;

    DecompiledData(size_t offset, DecompiledCommand x)
        : offset(offset), data(std::move(x))
    {}

    DecompiledData(size_t offset, std::vector<uint8_t> x)
        : offset(offset), data(DecompiledHex{ std::move(x) })
    {}

    DecompiledData(DecompiledLabelDef x)
        : offset(x.offset), data(std::move(x))
    {}
};


/// Gets the immediate 32 bits value of the value inside the variant, or nullopt if not possible.
template<typename T>
static optional<int32_t> get_imm32(const T&);
static optional<int32_t> get_imm32(const ArgVariant2&);

/// Gets the immediate string / text label value of the value inside the variant, or nullopt if not possible.
template<typename T>
static optional<std::string> get_immstr(const T&);
static optional<std::string> get_immstr(const ArgVariant2&);

/// Returns a vector of { bytecode, size } for each mission in the 
std::vector<BinaryFetcher> mission_segment_fetcher(const uint8_t* bytecode, size_t bytecode_size,
                                                   const DecompiledScmHeader& header, ProgramContext& program);


/// Interface to fetch little-endian bytes from a sequence of bytes in a easy and safe way.
// TODO move to a utility header?
// TODO try doing a similar interface in codegen.hpp?
struct BinaryFetcher
{
    const uint8_t* const bytecode;
    const size_t         size;

    optional<uint8_t> fetch_u8(size_t offset)
    {
        if(offset + 1 <= size)
        {
             return this->bytecode[offset];
        }
        return nullopt;
    }

    optional<uint16_t> fetch_u16(size_t offset)
    {
        if(offset + 2 <= size)
        {
            return uint16_t(this->bytecode[offset+0]) << 0
                 | uint16_t(this->bytecode[offset+1]) << 8;
        }
        return nullopt;
    }

    optional<uint32_t> fetch_u32(size_t offset)
    {
        if(offset + 4 <= size)
        {
            return uint32_t(this->bytecode[offset+0]) << 0
                 | uint32_t(this->bytecode[offset+1]) << 8
                 | uint32_t(this->bytecode[offset+2]) << 16
                 | uint32_t(this->bytecode[offset+3]) << 24;
        }
        return nullopt;
    }

    optional<int8_t> fetch_i8(size_t offset)
    {
        if(auto opt = fetch_u8(offset))
        {
            return reinterpret_cast<int8_t&>(*opt);
        }
        return nullopt;
    }

    optional<int16_t> fetch_i16(size_t offset)
    {
        if(auto opt = fetch_u16(offset))
        {
            return reinterpret_cast<int16_t&>(*opt);
        }
        return nullopt;
    }

    optional<int32_t> fetch_i32(size_t offset)
    {
        if(auto opt = fetch_u32(offset))
        {
            return reinterpret_cast<int32_t&>(*opt);
        }
        return nullopt;
    }

    optional<char*> fetch_chars(size_t offset, size_t count, char* output)
    {
        if(offset + count <= size)
        {
            std::strncpy(output, reinterpret_cast<const char*>(&this->bytecode[offset]), count);
            return output;
        }
        return nullopt;
    }

    optional<std::string> fetch_chars(size_t offset, size_t count)
    {
        std::string str(count, '\0');
        if(fetch_chars(offset, count, &str[0]))
            return str;
        return nullopt;
    }
};

///
struct Disassembler
{
private:
    ProgramContext&     program;
    const Commands&     commands;

    /// Bytecode being analyzed.
    BinaryFetcher       bf;

    /// The local offset of the labels in the analyzed bytecode.
    std::set<size_t>    label_offsets;

    /// A bitset of the offsets explored and unexplored. Explored offsets are confirmed to be code.
    dynamic_bitset      offset_explored;

    /// LIFO structure of offsets [mostly confirmed to be code] which still needs to be explored.
    std::stack<size_t>  to_explore;

    /// A hint (for efficient memory allocation) of how many opcodes are in the analyzed bytecode.
    std::size_t         hint_num_ops = 0;

    /// Used internally to process the SWITCH_START/SWITCH_CONTINUED commands.
    std::size_t         switch_cases_left = 0;

    /// Reference to the Disassembler of the main code segment.
    /// This reference may be pointing to *this.
    Disassembler&       main_asm;

    /// The result of disassemblying
    std::vector<DecompiledData> decompiled;

public:
    /// Constructs assuming `main_asm` to be the main code segment.
    ///
    /// It's undefined what happens with the analyzer if data inside `fetcher.bytecode` is changed while
    /// this Disassembler object is still alive.
    Disassembler(ProgramContext& program, const Commands& commands, BinaryFetcher fetcher, Disassembler& main_asm) :
        bf(std::move(fetcher)), program(program), commands(commands), main_asm(main_asm)
    {
        // This constructor **ALWAYS** run, put all common initialization here.
        this->offset_explored.resize(bf.size);
    }

    /// Constructs assuming `*this` to be the main code segment.
    ///
    /// Also see the warning regarding modifying `fetcher.bytecode` on the other constructor.
    Disassembler(ProgramContext& program, const Commands& commands, BinaryFetcher fetcher) :
        Disassembler(program, commands, std::move(fetcher), *this)
    {
    }

    ///
    Disassembler(const Disassembler&) = delete;

    ///
    Disassembler(Disassembler&&) = default;

    /// Is this Disassembler the main code segment?
    bool is_main_segment() const
    {
        return this == &main_asm;
    }

    /// Step 1. Analyze the code.
    void run_analyzer();

    /// Step 2. After analyzes, disassembly into a vector of pseudo-instructions.
    void disassembly();

    /// Step 3. Get reference to output.
    const std::vector<DecompiledData>& get_data() const { return this->decompiled; }

    /// After Step 3. the following is available also.
    /// Gets index on get_data() vector based on a local offset.
    optional<size_t> get_dataindex(uint32_t local_offset) const;

private:

    void analyze();

    void explore(size_t offset);


    /// Tries to explore the `offset` assuming it contains the specified `command`.
    ///
    /// Returns the number of bytes explored (the size of the compiled command),
    /// or `nullopt` if impossible to explore this opcode.
    optional<size_t> explore_opcode(size_t offset, const Command& command, bool not_flag);

    /// Returns a `DecompiledData` containing a `DecompiledCommand` by interpreting `offset`.
    ///
    /// The `offset` **must** have been successfully explored previosly by `explore_opcode`.
    /// For this reason, this call never fails.
    DecompiledData opcode_to_data(size_t& offset);
};


inline optional<int32_t> get_imm32(const EOAL&)
{
    return nullopt;
}

inline optional<int32_t> get_imm32(const DecompiledVar&)
{
    return nullopt;
}

inline optional<int32_t> get_imm32(const DecompiledVarArray&)
{
    return nullopt;
}

inline optional<int32_t> get_imm32(const DecompiledString&)
{
    return nullopt;
}

inline optional<int32_t> get_imm32(const int8_t& i8)
{
    return static_cast<int32_t>(i8);
}

inline optional<int32_t> get_imm32(const int16_t& i16)
{
    return static_cast<int32_t>(i16);
}

inline optional<int32_t> get_imm32(const int32_t& i32)
{
    return static_cast<int32_t>(i32);
}

inline optional<int32_t> get_imm32(const float& flt)
{
    // TODO floating point format static assert
    return reinterpret_cast<const int32_t&>(flt);
}

inline optional<int32_t> get_imm32(const ArgVariant2& varg)
{
    return visit_one(varg, [&](const auto& arg) { return ::get_imm32(arg); });
}

inline optional<std::string> get_immstr(const EOAL&)
{
    return nullopt;
}

inline optional<std::string> get_immstr(const DecompiledVar&)
{
    return nullopt;
}

inline optional<std::string> get_immstr(const DecompiledVarArray&)
{
    return nullopt;
}

inline optional<std::string> get_immstr(const DecompiledString& s)
{
    // TODO unescape storage
    // TODO for varlen strings don't search for \0, just unescape
    auto nul_it = std::find(s.storage.begin(), s.storage.end(), '\0');
    return std::string(s.storage.begin(), nul_it);
}

inline optional<std::string> get_immstr(const int8_t&)
{
    return nullopt;
}

inline optional<std::string> get_immstr(const int16_t&)
{
    return nullopt;
}

inline optional<std::string> get_immstr(const int32_t&)
{
    return nullopt;
}

inline optional<std::string> get_immstr(const float&)
{
    return nullopt;
}

inline optional<std::string> get_immstr(const ArgVariant2& varg)
{
    return visit_one(varg, [&](const auto& arg) { return ::get_immstr(arg); });
}
