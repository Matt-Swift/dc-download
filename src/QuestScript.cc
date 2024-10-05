#include "QuestScript.hh"

#include <stdint.h>
#include <string.h>

#include <array>
#include <deque>
#include <map>
#include <phosg/Math.hh>
#include <phosg/Strings.hh>
#include <set>
#include <unordered_map>
#include <vector>

#ifdef HAVE_RESOURCE_FILE
#include <resource_file/Emulators/PPC32Emulator.hh>
#include <resource_file/Emulators/SH4Emulator.hh>
#include <resource_file/Emulators/X86Emulator.hh>
#endif

#include "BattleParamsIndex.hh"
#include "CommandFormats.hh"
#include "Compression.hh"
#include "StaticGameData.hh"

using namespace std;

using AttackData = BattleParamsIndex::AttackData;
using ResistData = BattleParamsIndex::ResistData;
using MovementData = BattleParamsIndex::MovementData;

// bit_cast isn't in the standard place on macOS (it is apparently implicitly
// included by resource_dasm, but newserv can be built without resource_dasm)
// and I'm too lazy to go find the right header to include
template <typename ToT, typename FromT>
ToT as_type(const FromT& v) {
  static_assert(sizeof(FromT) == sizeof(ToT), "types are not the same size");
  ToT ret;
  memcpy(&ret, &v, sizeof(ToT));
  return ret;
}

static const char* name_for_header_episode_number(uint8_t episode) {
  static const array<const char*, 3> names = {"Episode1", "Episode2", "Episode4"};
  try {
    return names.at(episode);
  } catch (const out_of_range&) {
    return "Episode1  # invalid value in header";
  }
}

static TextEncoding encoding_for_language(uint8_t language) {
  return (language ? TextEncoding::ISO8859 : TextEncoding::SJIS);
}

static string escape_string(const string& data, TextEncoding encoding = TextEncoding::UTF8) {
  string decoded;
  try {
    switch (encoding) {
      case TextEncoding::UTF8:
        decoded = data;
        break;
      case TextEncoding::UTF16:
      case TextEncoding::UTF16_ALWAYS_MARKED:
        decoded = tt_utf16_to_utf8(data);
        break;
      case TextEncoding::SJIS:
        decoded = tt_sega_sjis_to_utf8(data);
        break;
      case TextEncoding::ISO8859:
        decoded = tt_8859_to_utf8(data);
        break;
      case TextEncoding::ASCII:
        decoded = tt_ascii_to_utf8(data);
        break;
      default:
        return phosg::format_data_string(data);
    }
  } catch (const runtime_error&) {
    return phosg::format_data_string(data);
  }

  string ret = "\"";
  for (char ch : decoded) {
    if (ch == '\n') {
      ret += "\\n";
    } else if (ch == '\r') {
      ret += "\\r";
    } else if (ch == '\t') {
      ret += "\\t";
    } else if (static_cast<uint8_t>(ch) < 0x20) {
      ret += phosg::string_printf("\\x%02hhX", ch);
    } else if (ch == '\'') {
      ret += "\\\'";
    } else if (ch == '\"') {
      ret += "\\\"";
    } else {
      ret += ch;
    }
  }
  ret += "\"";
  return ret;
}

static string format_and_indent_data(const void* data, size_t size, uint64_t start_address) {
  struct iovec iov;
  iov.iov_base = const_cast<void*>(data);
  iov.iov_len = size;

  string ret = "  ";
  phosg::format_data(
      [&ret](const void* vdata, size_t size) -> void {
        const char* data = reinterpret_cast<const char*>(vdata);
        for (size_t z = 0; z < size; z++) {
          if (data[z] == '\n') {
            ret += "\n  ";
          } else {
            ret.push_back(data[z]);
          }
        }
      },
      &iov, 1, start_address, nullptr, 0, phosg::PrintDataFlags::PRINT_ASCII);

  phosg::strip_trailing_whitespace(ret);
  return ret;
}

struct UnknownF8F2Entry {
  parray<le_float, 4> unknown_a1;
} __packed_ws__(UnknownF8F2Entry, 0x10);

struct QuestScriptOpcodeDefinition {
  struct Argument {
    enum class Type {
      LABEL16 = 0,
      LABEL16_SET,
      LABEL32,
      REG,
      REG_SET,
      REG_SET_FIXED, // Sequence of N consecutive regs
      REG32,
      REG32_SET_FIXED, // Sequence of N consecutive regs
      INT8,
      INT16,
      INT32,
      FLOAT32,
      CSTRING,
    };

    enum class DataType {
      NONE = 0,
      SCRIPT,
      DATA,
      CSTRING,
      PLAYER_STATS,
      PLAYER_VISUAL_CONFIG,
      RESIST_DATA,
      ATTACK_DATA,
      MOVEMENT_DATA,
      IMAGE_DATA,
      UNKNOWN_F8F2_DATA,
    };

    Type type;
    size_t count;
    DataType data_type;
    const char* name;

    Argument(Type type, size_t count = 0, const char* name = nullptr)
        : type(type),
          count(count),
          data_type(DataType::NONE),
          name(name) {}
    Argument(Type type, DataType data_type, const char* name = nullptr)
        : type(type),
          count(0),
          data_type(data_type),
          name(name) {}
  };

  uint16_t opcode;
  const char* name;
  const char* qedit_name;
  std::vector<Argument> args;
  uint16_t flags;

  QuestScriptOpcodeDefinition(
      uint16_t opcode,
      const char* name,
      const char* qedit_name,
      std::vector<Argument> args,
      uint16_t flags)
      : opcode(opcode),
        name(name),
        qedit_name(qedit_name),
        args(args),
        flags(flags) {}

  std::string str() const {
    string name_str = this->qedit_name ? phosg::string_printf("%s (qedit: %s)", this->name, this->qedit_name) : this->name;
    return phosg::string_printf("%04hX: %s flags=%04hX", this->opcode, name_str.c_str(), this->flags);
  }
};

constexpr uint16_t v_flag(Version v) {
  return (1 << static_cast<uint16_t>(v));
}

using Arg = QuestScriptOpcodeDefinition::Argument;

static_assert(NUM_VERSIONS == 14, "Don\'t forget to update the QuestScript flags and opcode definitions table");

static constexpr uint16_t F_PASS = 0x0001; // Version::PC_PATCH (unused for quests)
static constexpr uint16_t F_ARGS = 0x0002; // Version::BB_PATCH (unused for quests)
static constexpr uint16_t F_DC_NTE = 0x0004; // Version::DC_NTE
static constexpr uint16_t F_DC_112000 = 0x0008; // Version::DC_V1_11_2000_PROTOTYPE
static constexpr uint16_t F_DC_V1 = 0x0010; // Version::DC_V1
static constexpr uint16_t F_DC_V2 = 0x0020; // Version::DC_V2
static constexpr uint16_t F_PC_NTE = 0x0040; // Version::PC_NTE
static constexpr uint16_t F_PC_V2 = 0x0080; // Version::PC_V2
static constexpr uint16_t F_GC_NTE = 0x0100; // Version::GC_NTE
static constexpr uint16_t F_GC_V3 = 0x0200; // Version::GC_V3
static constexpr uint16_t F_GC_EP3TE = 0x0400; // Version::GC_EP3_NTE
static constexpr uint16_t F_GC_EP3 = 0x0800; // Version::GC_EP3
static constexpr uint16_t F_XB_V3 = 0x1000; // Version::XB_V3
static constexpr uint16_t F_BB_V4 = 0x2000; // Version::BB_V4
static constexpr uint16_t F_RET = 0x4000;
static constexpr uint16_t F_SET_EPISODE = 0x8000;

static_assert(F_DC_NTE == v_flag(Version::DC_NTE));
static_assert(F_DC_112000 == v_flag(Version::DC_V1_11_2000_PROTOTYPE));
static_assert(F_DC_V1 == v_flag(Version::DC_V1));
static_assert(F_DC_V2 == v_flag(Version::DC_V2));
static_assert(F_PC_NTE == v_flag(Version::PC_NTE));
static_assert(F_PC_V2 == v_flag(Version::PC_V2));
static_assert(F_GC_NTE == v_flag(Version::GC_NTE));
static_assert(F_GC_V3 == v_flag(Version::GC_V3));
static_assert(F_GC_EP3TE == v_flag(Version::GC_EP3_NTE));
static_assert(F_GC_EP3 == v_flag(Version::GC_EP3));
static_assert(F_XB_V3 == v_flag(Version::XB_V3));
static_assert(F_BB_V4 == v_flag(Version::BB_V4));

// clang-format off
static constexpr uint16_t F_V0_V2  = F_DC_NTE | F_DC_112000 | F_DC_V1 | F_DC_V2 | F_PC_NTE | F_PC_V2 | F_GC_NTE;
static constexpr uint16_t F_V0_V4  = F_DC_NTE | F_DC_112000 | F_DC_V1 | F_DC_V2 | F_PC_NTE | F_PC_V2 | F_GC_NTE | F_GC_V3 | F_GC_EP3TE | F_GC_EP3 | F_XB_V3 | F_BB_V4;
static constexpr uint16_t F_V05_V2 =            F_DC_112000 | F_DC_V1 | F_DC_V2 | F_PC_NTE | F_PC_V2 | F_GC_NTE;
static constexpr uint16_t F_V05_V4 =            F_DC_112000 | F_DC_V1 | F_DC_V2 | F_PC_NTE | F_PC_V2 | F_GC_NTE | F_GC_V3 | F_GC_EP3TE | F_GC_EP3 | F_XB_V3 | F_BB_V4;
static constexpr uint16_t F_V1_V2  =                          F_DC_V1 | F_DC_V2 | F_PC_NTE | F_PC_V2 | F_GC_NTE;
static constexpr uint16_t F_V1_V4  =                          F_DC_V1 | F_DC_V2 | F_PC_NTE | F_PC_V2 | F_GC_NTE | F_GC_V3 | F_GC_EP3TE | F_GC_EP3 | F_XB_V3 | F_BB_V4;
static constexpr uint16_t F_V2     =                                    F_DC_V2 | F_PC_NTE | F_PC_V2 | F_GC_NTE;
static constexpr uint16_t F_V2_V4  =                                    F_DC_V2 | F_PC_NTE | F_PC_V2 | F_GC_NTE | F_GC_V3 | F_GC_EP3TE | F_GC_EP3 | F_XB_V3 | F_BB_V4;
static constexpr uint16_t F_V3     =                                                                              F_GC_V3 | F_GC_EP3TE | F_GC_EP3 | F_XB_V3;
static constexpr uint16_t F_V3_V4  =                                                                              F_GC_V3 | F_GC_EP3TE | F_GC_EP3 | F_XB_V3 | F_BB_V4;
static constexpr uint16_t F_V4     =                                                                                                                          F_BB_V4;
// clang-format on
static constexpr uint16_t F_HAS_ARGS = F_V3_V4;

static constexpr auto LABEL16 = Arg::Type::LABEL16;
static constexpr auto LABEL16_SET = Arg::Type::LABEL16_SET;
static constexpr auto LABEL32 = Arg::Type::LABEL32;
static constexpr auto REG = Arg::Type::REG;
static constexpr auto REG_SET = Arg::Type::REG_SET;
static constexpr auto REG_SET_FIXED = Arg::Type::REG_SET_FIXED;
static constexpr auto REG32 = Arg::Type::REG32;
static constexpr auto REG32_SET_FIXED = Arg::Type::REG32_SET_FIXED;
static constexpr auto INT8 = Arg::Type::INT8;
static constexpr auto INT16 = Arg::Type::INT16;
static constexpr auto INT32 = Arg::Type::INT32;
static constexpr auto FLOAT32 = Arg::Type::FLOAT32;
static constexpr auto CSTRING = Arg::Type::CSTRING;

static const Arg SCRIPT16(LABEL16, Arg::DataType::SCRIPT);
static const Arg SCRIPT16_SET(LABEL16_SET, Arg::DataType::SCRIPT);
static const Arg SCRIPT32(LABEL32, Arg::DataType::SCRIPT);
static const Arg DATA16(LABEL16, Arg::DataType::DATA);
static const Arg CSTRING_LABEL16(LABEL16, Arg::DataType::CSTRING);

static const Arg CLIENT_ID(INT32, 0, "client_id");
static const Arg ITEM_ID(INT32, 0, "item_id");
static const Arg AREA(INT32, 0, "area");

static const QuestScriptOpcodeDefinition opcode_defs[] = {
    {0x0000, "nop", nullptr, {}, F_V0_V4}, // Does nothing
    {0x0001, "ret", nullptr, {}, F_V0_V4 | F_RET}, // Pops new PC off stack
    {0x0002, "sync", nullptr, {}, F_V0_V4}, // Stops execution for the current frame
    {0x0003, "exit", nullptr, {INT32}, F_V0_V4}, // Exits entirely
    {0x0004, "thread", nullptr, {SCRIPT16}, F_V0_V4}, // Starts a new thread
    {0x0005, "va_start", nullptr, {}, F_V3_V4}, // Pushes r1-r7 to the stack
    {0x0006, "va_end", nullptr, {}, F_V3_V4}, // Pops r7-r1 from the stack
    {0x0007, "va_call", nullptr, {SCRIPT16}, F_V3_V4}, // Replaces r1-r7 with the args stack, then calls the function
    {0x0008, "let", nullptr, {REG, REG}, F_V0_V4}, // Copies a value from regB to regA
    {0x0009, "leti", nullptr, {REG, INT32}, F_V0_V4}, // Sets register to a fixed value (int32)
    {0x000A, "leta", nullptr, {REG, REG}, F_V0_V2}, // Sets regA to the memory address of regB
    {0x000A, "letb", nullptr, {REG, INT8}, F_V3_V4}, // Sets register to a fixed value (int8)
    {0x000B, "letw", nullptr, {REG, INT16}, F_V3_V4}, // Sets register to a fixed value (int16)
    {0x000C, "leta", nullptr, {REG, REG}, F_V3_V4}, // Sets regA to the memory address of regB
    {0x000D, "leto", nullptr, {REG, SCRIPT16}, F_V3_V4}, // Sets register to the address of an entry in the quest function table
    {0x0010, "set", nullptr, {REG}, F_V0_V4}, // Sets a register to 1
    {0x0011, "clear", nullptr, {REG}, F_V0_V4}, // Sets a register to 0
    {0x0012, "rev", nullptr, {REG}, F_V0_V4}, // Sets a register to 0 if it's nonzero and vice versa
    {0x0013, "gset", nullptr, {INT16}, F_V0_V4}, // Sets a quest flag
    {0x0014, "gclear", nullptr, {INT16}, F_V0_V4}, // Clears a quest flag
    {0x0015, "grev", nullptr, {INT16}, F_V0_V4}, // Flips a quest flag
    {0x0016, "glet", nullptr, {INT16, REG}, F_V0_V4}, // Sets a quest flag to a specific value
    {0x0017, "gget", nullptr, {INT16, REG}, F_V0_V4}, // Gets a quest flag
    {0x0018, "add", nullptr, {REG, REG}, F_V0_V4}, // regA += regB
    {0x0019, "addi", nullptr, {REG, INT32}, F_V0_V4}, // regA += imm
    {0x001A, "sub", nullptr, {REG, REG}, F_V0_V4}, // regA -= regB
    {0x001B, "subi", nullptr, {REG, INT32}, F_V0_V4}, // regA -= imm
    {0x001C, "mul", nullptr, {REG, REG}, F_V0_V4}, // regA *= regB
    {0x001D, "muli", nullptr, {REG, INT32}, F_V0_V4}, // regA *= imm
    {0x001E, "div", nullptr, {REG, REG}, F_V0_V4}, // regA /= regB
    {0x001F, "divi", nullptr, {REG, INT32}, F_V0_V4}, // regA /= imm
    {0x0020, "and", nullptr, {REG, REG}, F_V0_V4}, // regA &= regB
    {0x0021, "andi", nullptr, {REG, INT32}, F_V0_V4}, // regA &= imm
    {0x0022, "or", nullptr, {REG, REG}, F_V0_V4}, // regA |= regB
    {0x0023, "ori", nullptr, {REG, INT32}, F_V0_V4}, // regA |= imm
    {0x0024, "xor", nullptr, {REG, REG}, F_V0_V4}, // regA ^= regB
    {0x0025, "xori", nullptr, {REG, INT32}, F_V0_V4}, // regA ^= imm
    {0x0026, "mod", nullptr, {REG, REG}, F_V3_V4}, // regA %= regB
    {0x0027, "modi", nullptr, {REG, INT32}, F_V3_V4}, // regA %= imm
    {0x0028, "jmp", nullptr, {SCRIPT16}, F_V0_V4}, // Jumps to function_table[fn_id]
    {0x0029, "call", nullptr, {SCRIPT16}, F_V0_V4}, // Pushes the offset after this opcode and jumps to function_table[fn_id]
    {0x002A, "jmp_on", nullptr, {SCRIPT16, REG_SET}, F_V0_V4}, // If all given registers are nonzero, jumps to function_table[fn_id]
    {0x002B, "jmp_off", nullptr, {SCRIPT16, REG_SET}, F_V0_V4}, // If all given registers are zero, jumps to function_table[fn_id]
    {0x002C, "jmp_eq", "jmp_=", {REG, REG, SCRIPT16}, F_V0_V4}, // If regA == regB, jumps to function_table[fn_id]
    {0x002D, "jmpi_eq", "jmpi_=", {REG, INT32, SCRIPT16}, F_V0_V4}, // If regA == regB, jumps to function_table[fn_id]
    {0x002E, "jmp_ne", "jmp_!=", {REG, REG, SCRIPT16}, F_V0_V4}, // If regA != regB, jumps to function_table[fn_id]
    {0x002F, "jmpi_ne", "jmpi_!=", {REG, INT32, SCRIPT16}, F_V0_V4}, // If regA != regB, jumps to function_table[fn_id]
    {0x0030, "ujmp_gt", "ujmp_>", {REG, REG, SCRIPT16}, F_V0_V4}, // If regA > regB, jumps to function_table[fn_id]
    {0x0031, "ujmpi_gt", "ujmpi_>", {REG, INT32, SCRIPT16}, F_V0_V4}, // If regA > regB, jumps to function_table[fn_id]
    {0x0032, "jmp_gt", "jmp_>", {REG, REG, SCRIPT16}, F_V0_V4}, // If regA > regB, jumps to function_table[fn_id]
    {0x0033, "jmpi_gt", "jmpi_>", {REG, INT32, SCRIPT16}, F_V0_V4}, // If regA > regB, jumps to function_table[fn_id]
    {0x0034, "ujmp_lt", "ujmp_<", {REG, REG, SCRIPT16}, F_V0_V4}, // If regA < regB, jumps to function_table[fn_id]
    {0x0035, "ujmpi_lt", "ujmpi_<", {REG, INT32, SCRIPT16}, F_V0_V4}, // If regA < regB, jumps to function_table[fn_id]
    {0x0036, "jmp_lt", "jmp_<", {REG, REG, SCRIPT16}, F_V0_V4}, // If regA < regB, jumps to function_table[fn_id]
    {0x0037, "jmpi_lt", "jmpi_<", {REG, INT32, SCRIPT16}, F_V0_V4}, // If regA < regB, jumps to function_table[fn_id]
    {0x0038, "ujmp_ge", "ujmp_>=", {REG, REG, SCRIPT16}, F_V0_V4}, // If regA >= regB, jumps to function_table[fn_id]
    {0x0039, "ujmpi_ge", "ujmpi_>=", {REG, INT32, SCRIPT16}, F_V0_V4}, // If regA >= regB, jumps to function_table[fn_id]
    {0x003A, "jmp_ge", "jmp_>=", {REG, REG, SCRIPT16}, F_V0_V4}, // If regA >= regB, jumps to function_table[fn_id]
    {0x003B, "jmpi_ge", "jmpi_>=", {REG, INT32, SCRIPT16}, F_V0_V4}, // If regA >= regB, jumps to function_table[fn_id]
    {0x003C, "ujmp_le", "ujmp_<=", {REG, REG, SCRIPT16}, F_V0_V4}, // If regA <= regB, jumps to function_table[fn_id]
    {0x003D, "ujmpi_le", "ujmpi_<=", {REG, INT32, SCRIPT16}, F_V0_V4}, // If regA <= regB, jumps to function_table[fn_id]
    {0x003E, "jmp_le", "jmp_<=", {REG, REG, SCRIPT16}, F_V0_V4}, // If regA <= regB, jumps to function_table[fn_id]
    {0x003F, "jmpi_le", "jmpi_<=", {REG, INT32, SCRIPT16}, F_V0_V4}, // If regA <= regB, jumps to function_table[fn_id]
    {0x0040, "switch_jmp", nullptr, {REG, SCRIPT16_SET}, F_V0_V4}, // Jumps to function_table[fn_ids[regA]]
    {0x0041, "switch_call", nullptr, {REG, SCRIPT16_SET}, F_V0_V4}, // Calls function_table[fn_ids[regA]]
    {0x0042, "nop_42", nullptr, {INT32}, F_V0_V2}, // Does nothing
    {0x0042, "stack_push", nullptr, {REG}, F_V3_V4}, // Pushes regA
    {0x0043, "stack_pop", nullptr, {REG}, F_V3_V4}, // Pops regA
    {0x0044, "stack_pushm", nullptr, {REG, INT32}, F_V3_V4}, // Pushes N regs in increasing order starting at regA
    {0x0045, "stack_popm", nullptr, {REG, INT32}, F_V3_V4}, // Pops N regs in decreasing order ending at regA
    {0x0048, "arg_pushr", nullptr, {REG}, F_V3_V4 | F_PASS}, // Pushes regA to the args list
    {0x0049, "arg_pushl", nullptr, {INT32}, F_V3_V4 | F_PASS}, // Pushes imm to the args list
    {0x004A, "arg_pushb", nullptr, {INT8}, F_V3_V4 | F_PASS}, // Pushes imm to the args list
    {0x004B, "arg_pushw", nullptr, {INT16}, F_V3_V4 | F_PASS}, // Pushes imm to the args list
    {0x004C, "arg_pusha", nullptr, {REG}, F_V3_V4 | F_PASS}, // Pushes memory address of regA to the args list
    {0x004D, "arg_pusho", nullptr, {LABEL16}, F_V3_V4 | F_PASS}, // Pushes function_table[fn_id] to the args list
    {0x004E, "arg_pushs", nullptr, {CSTRING}, F_V3_V4 | F_PASS}, // Pushes memory address of str to the args list
    {0x0050, "message", nullptr, {INT32, CSTRING}, F_V0_V4 | F_ARGS}, // Creates a dialogue with object/NPC N starting with message str
    {0x0051, "list", nullptr, {REG, CSTRING}, F_V0_V4 | F_ARGS}, // Prompts the player with a list of choices, returning the index of their choice in regA
    {0x0052, "fadein", nullptr, {}, F_V0_V4}, // Fades from black
    {0x0053, "fadeout", nullptr, {}, F_V0_V4}, // Fades to black
    {0x0054, "se", nullptr, {INT32}, F_V0_V4 | F_ARGS}, // Plays a sound effect
    {0x0055, "bgm", nullptr, {INT32}, F_V0_V4 | F_ARGS}, // Plays a fanfare (clear.adx or miniclear.adx)
    {0x0056, "nop_56", nullptr, {}, F_V0_V2}, // Does nothing
    {0x0057, "nop_57", nullptr, {}, F_V0_V2}, // Does nothing
    {0x0058, "nop_58", "enable", {INT32}, F_V0_V2}, // Does nothing
    {0x0059, "nop_59", "disable", {INT32}, F_V0_V2}, // Does nothing
    {0x005A, "window_msg", nullptr, {CSTRING}, F_V0_V4 | F_ARGS}, // Displays a message
    {0x005B, "add_msg", nullptr, {CSTRING}, F_V0_V4 | F_ARGS}, // Adds a message to an existing window
    {0x005C, "mesend", nullptr, {}, F_V0_V4}, // Closes a message box
    {0x005D, "gettime", nullptr, {REG}, F_V0_V4}, // Gets the current time
    {0x005E, "winend", nullptr, {}, F_V0_V4}, // Closes a window_msg
    {0x0060, "npc_crt", "npc_crt_V1", {INT32, INT32}, F_V0_V2 | F_ARGS}, // Creates an NPC
    {0x0060, "npc_crt", "npc_crt_V3", {INT32, INT32}, F_V3_V4 | F_ARGS}, // Creates an NPC
    {0x0061, "npc_stop", nullptr, {INT32}, F_V0_V4 | F_ARGS}, // Tells an NPC to stop following
    {0x0062, "npc_play", nullptr, {INT32}, F_V0_V4 | F_ARGS}, // Tells an NPC to follow the player
    {0x0063, "npc_kill", nullptr, {INT32}, F_V0_V4 | F_ARGS}, // Destroys an NPC
    {0x0064, "npc_nont", nullptr, {}, F_V0_V4},
    {0x0065, "npc_talk", nullptr, {}, F_V0_V4},
    {0x0066, "npc_crp", "npc_crp_V1", {{REG_SET_FIXED, 6}, INT32}, F_V0_V2}, // Creates an NPC. Second argument is ignored
    {0x0066, "npc_crp", "npc_crp_V3", {{REG_SET_FIXED, 6}}, F_V3_V4}, // Creates an NPC
    {0x0068, "create_pipe", nullptr, {INT32}, F_V0_V4 | F_ARGS}, // Creates a pipe
    {0x0069, "p_hpstat", "p_hpstat_V1", {REG, CLIENT_ID}, F_V0_V2 | F_ARGS}, // Compares player HP with a given value
    {0x0069, "p_hpstat", "p_hpstat_V3", {REG, CLIENT_ID}, F_V3_V4 | F_ARGS}, // Compares player HP with a given value
    {0x006A, "p_dead", "p_dead_V1", {REG, CLIENT_ID}, F_V0_V2 | F_ARGS}, // Checks if player is dead
    {0x006A, "p_dead", "p_dead_V3", {REG, CLIENT_ID}, F_V3_V4 | F_ARGS}, // Checks if player is dead
    {0x006B, "p_disablewarp", nullptr, {}, F_V0_V4}, // Disables telepipes/Ryuker
    {0x006C, "p_enablewarp", nullptr, {}, F_V0_V4}, // Enables telepipes/Ryuker
    {0x006D, "p_move", "p_move_v1", {{REG_SET_FIXED, 5}, INT32}, F_V0_V2}, // Moves player. Second argument is ignored
    {0x006D, "p_move", "p_move_V3", {{REG_SET_FIXED, 5}}, F_V3_V4}, // Moves player
    {0x006E, "p_look", nullptr, {CLIENT_ID}, F_V0_V4 | F_ARGS},
    {0x0070, "p_action_disable", nullptr, {}, F_V0_V4}, // Disables attacks for all players
    {0x0071, "p_action_enable", nullptr, {}, F_V0_V4}, // Enables attacks for all players
    {0x0072, "disable_movement1", nullptr, {CLIENT_ID}, F_V0_V4 | F_ARGS}, // Disables movement for the given player
    {0x0073, "enable_movement1", nullptr, {CLIENT_ID}, F_V0_V4 | F_ARGS}, // Enables movement for the given player
    {0x0074, "p_noncol", nullptr, {}, F_V0_V4},
    {0x0075, "p_col", nullptr, {}, F_V0_V4},
    {0x0076, "p_setpos", nullptr, {CLIENT_ID, {REG_SET_FIXED, 4}}, F_V0_V4 | F_ARGS},
    {0x0077, "p_return_guild", nullptr, {}, F_V0_V4},
    {0x0078, "p_talk_guild", nullptr, {CLIENT_ID}, F_V0_V4 | F_ARGS},
    {0x0079, "npc_talk_pl", "npc_talk_pl_V1", {{REG32_SET_FIXED, 8}}, F_V0_V2},
    {0x0079, "npc_talk_pl", "npc_talk_pl_V3", {{REG_SET_FIXED, 8}}, F_V3_V4},
    {0x007A, "npc_talk_kill", nullptr, {INT32}, F_V0_V4 | F_ARGS},
    {0x007B, "npc_crtpk", "npc_crtpk_V1", {INT32, INT32}, F_V0_V2 | F_ARGS}, // Creates attacker NPC
    {0x007B, "npc_crtpk", "npc_crtpk_V3", {INT32, INT32}, F_V3_V4 | F_ARGS}, // Creates attacker NPC
    {0x007C, "npc_crppk", "npc_crppk_V1", {{REG32_SET_FIXED, 7}, INT32}, F_V0_V2}, // Creates attacker NPC
    {0x007C, "npc_crppk", "npc_crppk_V3", {{REG_SET_FIXED, 7}}, F_V3_V4}, // Creates attacker NPC
    {0x007D, "npc_crptalk", "npc_crptalk_v1", {{REG32_SET_FIXED, 6}, INT32}, F_V0_V2},
    {0x007D, "npc_crptalk", "npc_crptalk_V3", {{REG_SET_FIXED, 6}}, F_V3_V4},
    {0x007E, "p_look_at", nullptr, {CLIENT_ID, CLIENT_ID}, F_V0_V4 | F_ARGS},
    {0x007F, "npc_crp_id", "npc_crp_id_V1", {{REG32_SET_FIXED, 7}, INT32}, F_V0_V2},
    {0x007F, "npc_crp_id", "npc_crp_id_v3", {{REG_SET_FIXED, 7}}, F_V3_V4},
    {0x0080, "cam_quake", nullptr, {}, F_V0_V4},
    {0x0081, "cam_adj", nullptr, {}, F_V0_V4},
    {0x0082, "cam_zmin", nullptr, {}, F_V0_V4},
    {0x0083, "cam_zmout", nullptr, {}, F_V0_V4},
    {0x0084, "cam_pan", "cam_pan_V1", {{REG32_SET_FIXED, 5}, INT32}, F_V0_V2},
    {0x0084, "cam_pan", "cam_pan_V3", {{REG_SET_FIXED, 5}}, F_V3_V4},
    {0x0085, "game_lev_super", nullptr, {}, F_V0_V2},
    {0x0085, "nop_85", nullptr, {}, F_V3_V4},
    {0x0086, "game_lev_reset", nullptr, {}, F_V0_V2},
    {0x0086, "nop_86", nullptr, {}, F_V3_V4},
    {0x0087, "pos_pipe", "pos_pipe_V1", {{REG32_SET_FIXED, 4}, INT32}, F_V0_V2},
    {0x0087, "pos_pipe", "pos_pipe_V3", {{REG_SET_FIXED, 4}}, F_V3_V4},
    {0x0088, "if_zone_clear", nullptr, {REG, {REG_SET_FIXED, 2}}, F_V0_V4},
    {0x0089, "chk_ene_num", nullptr, {REG}, F_V0_V4},
    {0x008A, "unhide_obj", nullptr, {{REG_SET_FIXED, 3}}, F_V0_V4},
    {0x008B, "unhide_ene", nullptr, {{REG_SET_FIXED, 3}}, F_V0_V4},
    {0x008C, "at_coords_call", nullptr, {{REG_SET_FIXED, 5}}, F_V0_V4},
    {0x008D, "at_coords_talk", nullptr, {{REG_SET_FIXED, 5}}, F_V0_V4},
    {0x008E, "npc_coords_call", nullptr, {{REG_SET_FIXED, 5}}, F_V0_V4},
    {0x008F, "party_coords_call", nullptr, {{REG_SET_FIXED, 6}}, F_V0_V4},
    {0x0090, "switch_on", nullptr, {INT32}, F_V0_V4 | F_ARGS},
    {0x0091, "switch_off", nullptr, {INT32}, F_V0_V4 | F_ARGS},
    {0x0092, "playbgm_epi", nullptr, {INT32}, F_V0_V4 | F_ARGS},
    {0x0093, "set_mainwarp", nullptr, {INT32}, F_V0_V4 | F_ARGS},
    {0x0094, "set_obj_param", nullptr, {{REG_SET_FIXED, 6}, REG}, F_V0_V4},
    {0x0095, "set_floor_handler", nullptr, {AREA, SCRIPT32}, F_V0_V2},
    {0x0095, "set_floor_handler", nullptr, {AREA, SCRIPT16}, F_V3_V4 | F_ARGS},
    {0x0096, "clr_floor_handler", nullptr, {AREA}, F_V0_V4 | F_ARGS},
    {0x0097, "npc_check_straggle", nullptr, {{REG_SET_FIXED, 9}}, F_V1_V4},
    {0x0098, "hud_hide", nullptr, {}, F_V0_V4},
    {0x0099, "hud_show", nullptr, {}, F_V0_V4},
    {0x009A, "cine_enable", nullptr, {}, F_V0_V4},
    {0x009B, "cine_disable", nullptr, {}, F_V0_V4},
    {0x00A0, "nop_A0_debug", nullptr, {INT32, CSTRING}, F_V0_V4 | F_ARGS}, // argA appears unused; game will softlock unless argB contains exactly 2 messages
    {0x00A1, "set_qt_failure", nullptr, {SCRIPT32}, F_V0_V2},
    {0x00A1, "set_qt_failure", nullptr, {SCRIPT16}, F_V3_V4},
    {0x00A2, "set_qt_success", nullptr, {SCRIPT32}, F_V0_V2},
    {0x00A2, "set_qt_success", nullptr, {SCRIPT16}, F_V3_V4},
    {0x00A3, "clr_qt_failure", nullptr, {}, F_V0_V4},
    {0x00A4, "clr_qt_success", nullptr, {}, F_V0_V4},
    {0x00A5, "set_qt_cancel", nullptr, {SCRIPT32}, F_V0_V2},
    {0x00A5, "set_qt_cancel", nullptr, {SCRIPT16}, F_V3_V4},
    {0x00A6, "clr_qt_cancel", nullptr, {}, F_V0_V4},
    {0x00A8, "pl_walk", "pl_walk_V1", {{REG32_SET_FIXED, 4}, INT32}, F_V0_V2},
    {0x00A8, "pl_walk", "pl_walk_V3", {{REG_SET_FIXED, 4}}, F_V3_V4},
    {0x00B0, "pl_add_meseta", nullptr, {CLIENT_ID, INT32}, F_V0_V4 | F_ARGS},
    {0x00B1, "thread_stg", nullptr, {SCRIPT16}, F_V0_V4},
    {0x00B2, "del_obj_param", nullptr, {REG}, F_V0_V4},
    {0x00B3, "item_create", nullptr, {{REG_SET_FIXED, 3}, REG}, F_V0_V4}, // Creates an item; regsA holds item data1[0-2], regB receives item ID
    {0x00B4, "item_create2", nullptr, {{REG_SET_FIXED, 12}, REG}, F_V0_V4}, // Like item_create but input regs each specify 1 byte (and can specify all of data1)
    {0x00B5, "item_delete", nullptr, {REG, {REG_SET_FIXED, 12}}, F_V0_V4},
    {0x00B6, "item_delete2", nullptr, {{REG_SET_FIXED, 3}, {REG_SET_FIXED, 12}}, F_V0_V4},
    {0x00B7, "item_check", nullptr, {{REG_SET_FIXED, 3}, REG}, F_V0_V4},
    {0x00B8, "setevt", nullptr, {INT32}, F_V05_V4 | F_ARGS},
    {0x00B9, "get_difficulty_level_v1", "get_difflvl", {REG}, F_V05_V4}, // Only returns 0-2, even in Ultimate (which results in 2 as well). Presumably all non-v1 quests should use get_difficulty_level_v2 instead.
    {0x00BA, "set_qt_exit", nullptr, {SCRIPT32}, F_V05_V2},
    {0x00BA, "set_qt_exit", nullptr, {SCRIPT16}, F_V3_V4},
    {0x00BB, "clr_qt_exit", nullptr, {}, F_V05_V4},
    {0x00BC, "nop_BC", nullptr, {CSTRING}, F_V05_V4},
    {0x00C0, "particle", "particle_V1", {{REG32_SET_FIXED, 5}, INT32}, F_V05_V2},
    {0x00C0, "particle", "particle_V3", {{REG_SET_FIXED, 5}}, F_V3_V4},
    {0x00C1, "npc_text", nullptr, {INT32, CSTRING}, F_V05_V4 | F_ARGS},
    {0x00C2, "npc_chkwarp", nullptr, {}, F_V05_V4},
    {0x00C3, "pl_pkoff", nullptr, {}, F_V05_V4},
    {0x00C4, "map_designate", nullptr, {{REG_SET_FIXED, 4}}, F_V05_V4},
    {0x00C5, "masterkey_on", nullptr, {}, F_V05_V4},
    {0x00C6, "masterkey_off", nullptr, {}, F_V05_V4},
    {0x00C7, "window_time", nullptr, {}, F_V05_V4},
    {0x00C8, "winend_time", nullptr, {}, F_V05_V4},
    {0x00C9, "winset_time", nullptr, {REG}, F_V05_V4},
    {0x00CA, "getmtime", nullptr, {REG}, F_V05_V4},
    {0x00CB, "set_quest_board_handler", nullptr, {INT32, SCRIPT32, CSTRING}, F_V05_V2},
    {0x00CB, "set_quest_board_handler", nullptr, {INT32, SCRIPT16, CSTRING}, F_V3_V4 | F_ARGS},
    {0x00CC, "clear_quest_board_handler", nullptr, {INT32}, F_V05_V4 | F_ARGS},
    {0x00CD, "particle_id", "particle_id_V1", {{REG32_SET_FIXED, 4}, INT32}, F_V05_V2},
    {0x00CD, "particle_id", "particle_id_V3", {{REG_SET_FIXED, 4}}, F_V3_V4},
    {0x00CE, "npc_crptalk_id", "npc_crptalk_id_V1", {{REG32_SET_FIXED, 7}, INT32}, F_V05_V2},
    {0x00CE, "npc_crptalk_id", "npc_crptalk_id_V3", {{REG_SET_FIXED, 7}}, F_V3_V4},
    {0x00CF, "npc_lang_clean", nullptr, {}, F_V05_V4},
    {0x00D0, "pl_pkon", nullptr, {}, F_V1_V4},
    {0x00D1, "pl_chk_item2", nullptr, {{REG_SET_FIXED, 4}, REG}, F_V1_V4}, // Presumably like item_check but also checks data2
    {0x00D2, "enable_mainmenu", nullptr, {}, F_V1_V4},
    {0x00D3, "disable_mainmenu", nullptr, {}, F_V1_V4},
    {0x00D4, "start_battlebgm", nullptr, {}, F_V1_V4},
    {0x00D5, "end_battlebgm", nullptr, {}, F_V1_V4},
    {0x00D6, "disp_msg_qb", nullptr, {CSTRING}, F_V1_V4 | F_ARGS},
    {0x00D7, "close_msg_qb", nullptr, {}, F_V1_V4},
    {0x00D8, "set_eventflag", "set_eventflag_v1", {INT32, INT32}, F_V1_V2 | F_ARGS},
    {0x00D8, "set_eventflag", "set_eventflag_v3", {INT32, INT32}, F_V3_V4 | F_ARGS},
    {0x00D9, "sync_register", "sync_leti", {INT32, INT32}, F_V1_V4 | F_ARGS},
    {0x00DA, "set_returnhunter", nullptr, {}, F_V1_V4},
    {0x00DB, "set_returncity", nullptr, {}, F_V1_V4},
    {0x00DC, "load_pvr", nullptr, {}, F_V1_V4},
    {0x00DD, "load_midi", nullptr, {}, F_V1_V4}, // Seems incomplete on V3 and BB - has some similar codepaths as load_pvr, but the function that actually process the data seems to do nothing
    {0x00DE, "item_detect_bank", "unknownDE", {{REG_SET_FIXED, 6}, REG}, F_V1_V4}, // regsA specifies the first 6 bytes of an ItemData (data1[0-5])
    {0x00DF, "npc_param", "npc_param_V1", {{REG32_SET_FIXED, 14}, INT32}, F_V1_V2},
    {0x00DF, "npc_param", "npc_param_V3", {{REG_SET_FIXED, 14}, INT32}, F_V3_V4 | F_ARGS},
    {0x00E0, "pad_dragon", nullptr, {}, F_V1_V4},
    {0x00E1, "clear_mainwarp", nullptr, {INT32}, F_V1_V4 | F_ARGS},
    {0x00E2, "pcam_param", "pcam_param_V1", {{REG32_SET_FIXED, 6}}, F_V1_V2},
    {0x00E2, "pcam_param", "pcam_param_V3", {{REG_SET_FIXED, 6}}, F_V3_V4},
    {0x00E3, "start_setevt", "start_setevt_v1", {INT32, INT32}, F_V1_V2 | F_ARGS},
    {0x00E3, "start_setevt", "start_setevt_v3", {INT32, INT32}, F_V3_V4 | F_ARGS},
    {0x00E4, "warp_on", nullptr, {}, F_V1_V4},
    {0x00E5, "warp_off", nullptr, {}, F_V1_V4},
    {0x00E6, "get_client_id", "get_slotnumber", {REG}, F_V1_V4},
    {0x00E7, "get_leader_id", "get_servernumber", {REG}, F_V1_V4},
    {0x00E8, "set_eventflag2", nullptr, {INT32, REG}, F_V1_V4 | F_ARGS},
    {0x00E9, "mod2", "res", {REG, REG}, F_V1_V4},
    {0x00EA, "modi2", "unknownEA", {REG, INT32}, F_V1_V4},
    {0x00EB, "enable_bgmctrl", nullptr, {INT32}, F_V1_V4 | F_ARGS},
    {0x00EC, "sw_send", nullptr, {{REG_SET_FIXED, 3}}, F_V1_V4},
    {0x00ED, "create_bgmctrl", nullptr, {}, F_V1_V4},
    {0x00EE, "pl_add_meseta2", nullptr, {INT32}, F_V1_V4 | F_ARGS},
    {0x00EF, "sync_register2", "sync_let", {INT32, REG32}, F_V1_V2},
    {0x00EF, "sync_register2", nullptr, {REG, INT32}, F_V3_V4 | F_ARGS},
    {0x00F0, "send_regwork", nullptr, {REG32, REG32}, F_V1_V2},
    {0x00F1, "leti_fixed_camera", "leti_fixed_camera_V1", {{REG32_SET_FIXED, 6}}, F_V2},
    {0x00F1, "leti_fixed_camera", "leti_fixed_camera_V3", {{REG_SET_FIXED, 6}}, F_V3_V4},
    {0x00F2, "default_camera_pos1", nullptr, {}, F_V2_V4},
    {0xF800, "debug_F800", nullptr, {}, F_V2}, // Same as 50, but uses fixed arguments - with a Japanese string that Google Translate translates as "I'm frugal!!"
    {0xF801, "set_chat_callback", "set_chat_callback?", {{REG32_SET_FIXED, 5}, CSTRING}, F_V2_V4 | F_ARGS},
    {0xF808, "get_difficulty_level_v2", "get_difflvl2", {REG}, F_V2_V4},
    {0xF809, "get_number_of_players", "get_number_of_player1", {REG}, F_V2_V4},
    {0xF80A, "get_coord_of_player", nullptr, {{REG_SET_FIXED, 3}, REG}, F_V2_V4},
    {0xF80B, "enable_map", nullptr, {}, F_V2_V4},
    {0xF80C, "disable_map", nullptr, {}, F_V2_V4},
    {0xF80D, "map_designate_ex", nullptr, {{REG_SET_FIXED, 5}}, F_V2_V4},
    {0xF80E, "disable_weapon_drop", "unknownF80E", {CLIENT_ID}, F_V2_V4 | F_ARGS},
    {0xF80F, "enable_weapon_drop", "unknownF80F", {CLIENT_ID}, F_V2_V4 | F_ARGS},
    {0xF810, "ba_initial_floor", nullptr, {AREA}, F_V2_V4 | F_ARGS},
    {0xF811, "set_ba_rules", nullptr, {}, F_V2_V4},
    {0xF812, "ba_set_tech_disk_mode", "ba_set_tech", {INT32}, F_V2_V4 | F_ARGS},
    {0xF813, "ba_set_weapon_and_armor_mode", "ba_set_equip", {INT32}, F_V2_V4 | F_ARGS},
    {0xF814, "ba_set_forbid_mags", "ba_set_mag", {INT32}, F_V2_V4 | F_ARGS},
    {0xF815, "ba_set_tool_mode", "ba_set_item", {INT32}, F_V2_V4 | F_ARGS},
    {0xF816, "ba_set_trap_mode", "ba_set_trapmenu", {INT32}, F_V2_V4 | F_ARGS},
    {0xF817, "ba_set_unused_F817", "unknownF817", {INT32}, F_V2_V4 | F_ARGS}, // This appears to be unused - the value is copied into the main battle rules struct, but then the field appears never to be read
    {0xF818, "ba_set_respawn", nullptr, {INT32}, F_V2_V4 | F_ARGS},
    {0xF819, "ba_set_replace_char", "ba_set_char", {INT32}, F_V2_V4 | F_ARGS},
    {0xF81A, "ba_dropwep", nullptr, {INT32}, F_V2_V4 | F_ARGS},
    {0xF81B, "ba_teams", nullptr, {INT32}, F_V2_V4 | F_ARGS},
    {0xF81C, "ba_start", "ba_disp_msg", {CSTRING}, F_V2_V4 | F_ARGS},
    {0xF81D, "death_lvl_up", nullptr, {INT32}, F_V2_V4 | F_ARGS},
    {0xF81E, "ba_set_meseta_drop_mode", "ba_set_meseta", {INT32}, F_V2_V4 | F_ARGS},
    {0xF820, "cmode_stage", nullptr, {INT32}, F_V2_V4 | F_ARGS},
    {0xF821, "nop_F821", nullptr, {{REG_SET_FIXED, 9}}, F_V2_V4}, // regsA[3-8] specify first 6 bytes of an ItemData. This opcode consumes an item ID, but does nothing else.
    {0xF822, "nop_F822", nullptr, {REG}, F_V2_V4},
    {0xF823, "set_cmode_char_template", nullptr, {INT32}, F_V2_V4 | F_ARGS},
    {0xF824, "set_cmode_difficulty", "set_cmode_diff", {INT32}, F_V2_V4 | F_ARGS},
    {0xF825, "exp_multiplication", nullptr, {{REG_SET_FIXED, 3}}, F_V2_V4},
    {0xF826, "if_player_alive_cm", "exp_division?", {REG}, F_V2_V4},
    {0xF827, "get_user_is_dead", "get_user_is_dead?", {REG}, F_V2_V4},
    {0xF828, "go_floor", nullptr, {REG, REG}, F_V2_V4},
    {0xF829, "get_num_kills", nullptr, {REG, REG}, F_V2_V4},
    {0xF82A, "reset_kills", nullptr, {REG}, F_V2_V4},
    {0xF82B, "unlock_door2", nullptr, {INT32, INT32}, F_V2_V4 | F_ARGS},
    {0xF82C, "lock_door2", nullptr, {INT32, INT32}, F_V2_V4 | F_ARGS},
    {0xF82D, "if_switch_not_pressed", nullptr, {{REG_SET_FIXED, 2}}, F_V2_V4},
    {0xF82E, "if_switch_pressed", nullptr, {{REG_SET_FIXED, 3}}, F_V2_V4},
    {0xF830, "control_dragon", nullptr, {REG}, F_V2_V4},
    {0xF831, "release_dragon", nullptr, {}, F_V2_V4},
    {0xF838, "shrink", nullptr, {REG}, F_V2_V4},
    {0xF839, "unshrink", nullptr, {REG}, F_V2_V4},
    {0xF83A, "set_shrink_cam1", nullptr, {{REG_SET_FIXED, 4}}, F_V2_V4},
    {0xF83B, "set_shrink_cam2", nullptr, {{REG_SET_FIXED, 4}}, F_V2_V4},
    {0xF83C, "display_clock2", "display_clock2?", {REG}, F_V2_V4},
    {0xF83D, "set_area_total", "unknownF83D", {INT32}, F_V2_V4 | F_ARGS},
    {0xF83E, "delete_area_title", "delete_area_title?", {INT32}, F_V2_V4 | F_ARGS},
    {0xF840, "load_npc_data", nullptr, {}, F_V2_V4},
    {0xF841, "get_npc_data", nullptr, {{LABEL16, Arg::DataType::PLAYER_VISUAL_CONFIG, "visual_config"}}, F_V2_V4},
    {0xF848, "give_damage_score", nullptr, {{REG_SET_FIXED, 3}}, F_V2_V4},
    {0xF849, "take_damage_score", nullptr, {{REG_SET_FIXED, 3}}, F_V2_V4},
    {0xF84A, "enemy_give_score", "unk_score_F84A", {{REG_SET_FIXED, 3}}, F_V2_V4}, // Actual value used is regsA[0] + (regsA[1] / regsA[2])
    {0xF84B, "enemy_take_score", "unk_score_F84B", {{REG_SET_FIXED, 3}}, F_V2_V4}, // Actual value used is regsA[0] + (regsA[1] / regsA[2])
    {0xF84C, "kill_score", nullptr, {{REG_SET_FIXED, 3}}, F_V2_V4},
    {0xF84D, "death_score", nullptr, {{REG_SET_FIXED, 3}}, F_V2_V4},
    {0xF84E, "enemy_kill_score", "unk_score_F84E", {{REG_SET_FIXED, 3}}, F_V2_V4}, // Actual value used is regsA[0] + (regsA[1] / regsA[2])
    {0xF84F, "enemy_death_score", nullptr, {{REG_SET_FIXED, 3}}, F_V2_V4},
    {0xF850, "meseta_score", nullptr, {{REG_SET_FIXED, 3}}, F_V2_V4},
    {0xF851, "ba_set_trap_count", "unknownF851", {{REG_SET_FIXED, 2}}, F_V2_V4}, // regsA is [trap_type, trap_count]
    {0xF852, "ba_set_target", "unknownF852", {INT32}, F_V2_V4 | F_ARGS},
    {0xF853, "reverse_warps", nullptr, {}, F_V2_V4},
    {0xF854, "unreverse_warps", nullptr, {}, F_V2_V4},
    {0xF855, "set_ult_map", nullptr, {}, F_V2_V4},
    {0xF856, "unset_ult_map", nullptr, {}, F_V2_V4},
    {0xF857, "set_area_title", nullptr, {CSTRING}, F_V2_V4 | F_ARGS},
    {0xF858, "ba_show_self_traps", "BA_Show_Self_Traps", {}, F_V2_V4},
    {0xF859, "ba_hide_self_traps", "BA_Hide_Self_Traps", {}, F_V2_V4},
    {0xF85A, "equip_item", "equip_item_v2", {{REG32_SET_FIXED, 4}}, F_V2}, // regsA are {client_id, item.data1[0-2]}
    {0xF85A, "equip_item", "equip_item_v3", {{REG_SET_FIXED, 4}}, F_V3_V4}, // regsA are {client_id, item.data1[0-2]}
    {0xF85B, "unequip_item", "unequip_item_V2", {CLIENT_ID, INT32}, F_V2 | F_ARGS},
    {0xF85B, "unequip_item", "unequip_item_V3", {CLIENT_ID, INT32}, F_V3_V4 | F_ARGS},
    {0xF85C, "qexit2", "QEXIT2", {INT32}, F_V2_V4},
    {0xF85D, "set_allow_item_flags", "unknownF85D", {INT32}, F_V2_V4 | F_ARGS}, // 0 = allow normal item usage (undoes all of the following), 1 = disallow weapons, 2 = disallow armors, 3 = disallow shields, 4 = disallow units, 5 = disallow mags, 6 = disallow tools
    {0xF85E, "ba_enable_sonar", "unknownF85E", {INT32}, F_V2_V4 | F_ARGS},
    {0xF85F, "ba_use_sonar", "unknownF85F", {INT32}, F_V2_V4 | F_ARGS},
    {0xF860, "clear_score_announce", "unknownF860", {}, F_V2_V4},
    {0xF861, "set_score_announce", "unknownF861", {INT32}, F_V2_V4 | F_ARGS},
    {0xF862, "give_s_rank_weapon", nullptr, {REG32, REG32, CSTRING}, F_V2},
    {0xF862, "give_s_rank_weapon", nullptr, {INT32, REG, CSTRING}, F_V3_V4 | F_ARGS},
    {0xF863, "get_mag_levels", nullptr, {{REG32_SET_FIXED, 4}}, F_V2},
    {0xF863, "get_mag_levels", nullptr, {{REG_SET_FIXED, 4}}, F_V3_V4},
    {0xF864, "set_cmode_rank_result", "cmode_rank", {INT32, CSTRING}, F_V2_V4 | F_ARGS},
    {0xF865, "award_item_name", "award_item_name?", {}, F_V2_V4},
    {0xF866, "award_item_select", "award_item_select?", {}, F_V2_V4},
    {0xF867, "award_item_give_to", "award_item_give_to?", {REG}, F_V2_V4}, // Sends 07DF on BB
    {0xF868, "set_cmode_rank_threshold", "set_cmode_rank", {REG, REG}, F_V2_V4},
    {0xF869, "check_rank_time", nullptr, {REG, REG}, F_V2_V4},
    {0xF86A, "item_create_cmode", nullptr, {{REG_SET_FIXED, 6}, REG}, F_V2_V4}, // regsA specifies item.data1[0-5]; sends 07DF on BB
    {0xF86B, "ba_set_box_drop_area", "ba_box_drops", {REG}, F_V2_V4}, // TODO: This sets override_area in TItemDropSub; use this in ItemCreator
    {0xF86C, "award_item_ok", "award_item_ok?", {REG}, F_V2_V4},
    {0xF86D, "ba_set_trapself", nullptr, {}, F_V2_V4},
    {0xF86E, "ba_clear_trapself", "unknownF86E", {}, F_V2_V4},
    {0xF86F, "ba_set_lives", nullptr, {INT32}, F_V2_V4 | F_ARGS},
    {0xF870, "ba_set_max_tech_level", "ba_set_tech_lvl", {INT32}, F_V2_V4 | F_ARGS},
    {0xF871, "ba_set_char_level", "ba_set_lvl", {INT32}, F_V2_V4 | F_ARGS},
    {0xF872, "ba_set_time_limit", nullptr, {INT32}, F_V2_V4 | F_ARGS},
    {0xF873, "dark_falz_is_dead", "boss_is_dead?", {REG}, F_V2_V4},
    {0xF874, "set_cmode_rank_override", nullptr, {INT32, CSTRING}, F_V2_V4 | F_ARGS}, // argA is an XRGB8888 color, argB is two strings separated by \t or \n: the rank text to check for, and the rank text that should replace it if found
    {0xF875, "enable_stealth_suit_effect", nullptr, {REG}, F_V2_V4},
    {0xF876, "disable_stealth_suit_effect", nullptr, {REG}, F_V2_V4},
    {0xF877, "enable_techs", nullptr, {REG}, F_V2_V4},
    {0xF878, "disable_techs", nullptr, {REG}, F_V2_V4},
    {0xF879, "get_gender", nullptr, {REG, REG}, F_V2_V4},
    {0xF87A, "get_chara_class", nullptr, {REG, {REG_SET_FIXED, 2}}, F_V2_V4},
    {0xF87B, "take_slot_meseta", nullptr, {{REG_SET_FIXED, 2}, REG}, F_V2_V4},
    {0xF87C, "get_guild_card_file_creation_time", nullptr, {REG}, F_V2_V4},
    {0xF87D, "kill_player", nullptr, {REG}, F_V2_V4},
    {0xF87E, "get_serial_number", nullptr, {REG}, F_V2_V4}, // Returns 0 on BB
    {0xF87F, "get_eventflag", "read_guildcard_flag", {REG, REG}, F_V2_V4},
    {0xF880, "set_trap_damage", "unknownF880", {{REG_SET_FIXED, 3}}, F_V2_V4}, // Normally trap damage is (700.0 * area_factor[area] * 2.0 * (0.01 * level + 0.1)); this overrides that computation. The value is specified with integer and fractional parts split up: the actual value is regsA[0] + (regsA[1] / regsA[2]).
    {0xF881, "get_pl_name", "get_pl_name?", {REG}, F_V2_V4},
    {0xF882, "get_pl_job", nullptr, {REG}, F_V2_V4},
    {0xF883, "get_player_proximity", "unknownF883", {{REG_SET_FIXED, 2}, REG}, F_V2_V4},
    {0xF884, "set_eventflag16", nullptr, {INT32, REG}, F_V2},
    {0xF884, "set_eventflag16", nullptr, {INT32, INT32}, F_V3_V4 | F_ARGS},
    {0xF885, "set_eventflag32", nullptr, {INT32, REG}, F_V2},
    {0xF885, "set_eventflag32", nullptr, {INT32, INT32}, F_V3_V4 | F_ARGS},
    {0xF886, "ba_get_place", nullptr, {REG, REG}, F_V2_V4},
    {0xF887, "ba_get_score", nullptr, {REG, REG}, F_V2_V4},
    {0xF888, "enable_win_pfx", "ba_close_msg", {}, F_V2_V4},
    {0xF889, "disable_win_pfx", nullptr, {}, F_V2_V4},
    {0xF88A, "get_player_status", nullptr, {REG, REG}, F_V2_V4},
    {0xF88B, "send_mail", nullptr, {REG, CSTRING}, F_V2_V4 | F_ARGS},
    {0xF88C, "get_game_version", nullptr, {REG}, F_V2_V4}, // Returns 2 on DCv2/PC, 3 on GC, 4 on XB and BB
    {0xF88D, "chl_set_timerecord", "chl_set_timerecord?", {REG}, F_V2 | F_V3},
    {0xF88D, "chl_set_timerecord", "chl_set_timerecord?", {REG, REG}, F_V4},
    {0xF88E, "chl_get_timerecord", "chl_get_timerecord?", {REG}, F_V2_V4},
    {0xF88F, "set_cmode_grave_rates", nullptr, {{REG_SET_FIXED, 20}}, F_V2_V4},
    {0xF890, "clear_mainwarp_all", "unknownF890", {}, F_V2_V4},
    {0xF891, "load_enemy_data", nullptr, {INT32}, F_V2_V4 | F_ARGS},
    {0xF892, "get_physical_data", nullptr, {{LABEL16, Arg::DataType::PLAYER_STATS, "stats"}}, F_V2_V4},
    {0xF893, "get_attack_data", nullptr, {{LABEL16, Arg::DataType::ATTACK_DATA, "attack_data"}}, F_V2_V4},
    {0xF894, "get_resist_data", nullptr, {{LABEL16, Arg::DataType::RESIST_DATA, "resist_data"}}, F_V2_V4},
    {0xF895, "get_movement_data", nullptr, {{LABEL16, Arg::DataType::MOVEMENT_DATA, "movement_data"}}, F_V2_V4},
    {0xF896, "get_eventflag16", nullptr, {REG, REG}, F_V2_V4},
    {0xF897, "get_eventflag32", nullptr, {REG, REG}, F_V2_V4},
    {0xF898, "shift_left", nullptr, {REG, REG}, F_V2_V4},
    {0xF899, "shift_right", nullptr, {REG, REG}, F_V2_V4},
    {0xF89A, "get_random", nullptr, {{REG_SET_FIXED, 2}, REG}, F_V2_V4},
    {0xF89B, "reset_map", nullptr, {}, F_V2_V4},
    {0xF89C, "disp_chl_retry_menu", nullptr, {REG}, F_V2_V4},
    {0xF89D, "chl_reverser", "chl_reverser?", {}, F_V2_V4},
    {0xF89E, "ba_forbid_scape_dolls", "unknownF89E", {INT32}, F_V2_V4 | F_ARGS},
    {0xF89F, "player_recovery", "unknownF89F", {REG}, F_V2_V4}, // regA = client ID
    {0xF8A0, "disable_bosswarp_option", "unknownF8A0", {}, F_V2_V4},
    {0xF8A1, "enable_bosswarp_option", "unknownF8A1", {}, F_V2_V4},
    {0xF8A2, "is_bosswarp_opt_disabled", nullptr, {REG}, F_V2_V4},
    {0xF8A3, "load_serial_number_to_flag_buf", "init_online_key?", {}, F_V2_V4}, // Loads 0 on BB
    {0xF8A4, "write_flag_buf_to_event_flags", "encrypt_gc_entry_auto", {REG}, F_V2_V4},
    {0xF8A5, "set_chat_callback_no_filter", nullptr, {{REG_SET_FIXED, 5}}, F_V2_V4},
    {0xF8A6, "set_symbol_chat_collision", nullptr, {{REG_SET_FIXED, 10}}, F_V2_V4},
    {0xF8A7, "set_shrink_size", nullptr, {REG, {REG_SET_FIXED, 3}}, F_V2_V4},
    {0xF8A8, "death_tech_lvl_up2", nullptr, {INT32}, F_V2_V4 | F_ARGS},
    {0xF8A9, "vol_opt_is_dead", "unknownF8A9", {REG}, F_V2_V4},
    {0xF8AA, "is_there_grave_message", nullptr, {REG}, F_V2_V4},
    {0xF8AB, "get_ba_record", nullptr, {{REG_SET_FIXED, 7}}, F_V2_V4},
    {0xF8AC, "get_cmode_prize_rank", nullptr, {REG}, F_V2_V4},
    {0xF8AD, "get_number_of_players2", nullptr, {REG}, F_V2_V4},
    {0xF8AE, "party_has_name", nullptr, {REG}, F_V2_V4},
    {0xF8AF, "someone_has_spoken", nullptr, {REG}, F_V2_V4},
    {0xF8B0, "read1", nullptr, {REG, REG}, F_V2},
    {0xF8B0, "read1", nullptr, {REG, INT32}, F_V3_V4 | F_ARGS},
    {0xF8B1, "read2", nullptr, {REG, REG}, F_V2},
    {0xF8B1, "read2", nullptr, {REG, INT32}, F_V3_V4 | F_ARGS},
    {0xF8B2, "read4", nullptr, {REG, REG}, F_V2},
    {0xF8B2, "read4", nullptr, {REG, INT32}, F_V3_V4 | F_ARGS},
    {0xF8B3, "write1", nullptr, {REG, REG}, F_V2},
    {0xF8B3, "write1", nullptr, {INT32, INT32}, F_V3_V4 | F_ARGS},
    {0xF8B4, "write2", nullptr, {REG, REG}, F_V2},
    {0xF8B4, "write2", nullptr, {INT32, INT32}, F_V3_V4 | F_ARGS},
    {0xF8B5, "write4", nullptr, {REG, REG}, F_V2},
    {0xF8B5, "write4", nullptr, {INT32, INT32}, F_V3_V4 | F_ARGS},
    {0xF8B6, "check_for_hacking", nullptr, {REG}, F_V2_V4}, // Returns a bitmask of 5 different types of detectable hacking. But it only works on DCv2 - it crashes on all other versions.
    {0xF8B7, "unknown_F8B7", nullptr, {REG}, F_V2_V4}, // TODO (DX) - Challenge mode. Appears to be timing-related; regA is expected to be in [60, 3600]. Encodes the value with encrypt_challenge_time even though it's never sent over the network and is only decrypted locally.
    {0xF8B8, "disable_retry_menu", "unknownF8B8", {}, F_V2_V4},
    {0xF8B9, "chl_recovery", "chl_recovery?", {}, F_V2_V4},
    {0xF8BA, "load_guild_card_file_creation_time_to_flag_buf", nullptr, {}, F_V2_V4},
    {0xF8BB, "write_flag_buf_to_event_flags2", nullptr, {REG}, F_V2_V4},
    {0xF8BC, "set_episode", nullptr, {INT32}, F_V3_V4 | F_SET_EPISODE},
    {0xF8C0, "file_dl_req", nullptr, {INT32, CSTRING}, F_V3 | F_ARGS}, // Sends D7
    {0xF8C0, "nop_F8C0", nullptr, {INT32, CSTRING}, F_V4 | F_ARGS},
    {0xF8C1, "get_dl_status", nullptr, {REG}, F_V3},
    {0xF8C1, "nop_F8C1", nullptr, {REG}, F_V4},
    {0xF8C2, "prepare_gba_rom_from_download", "gba_unknown4?", {}, F_GC_V3 | F_GC_EP3TE | F_GC_EP3}, // Prepares to load a GBA ROM from a previous file_dl_req opcode
    {0xF8C2, "nop_F8C2", nullptr, {}, F_XB_V3 | F_V4},
    {0xF8C3, "start_or_update_gba_joyboot", "get_gba_state?", {REG}, F_GC_V3 | F_GC_EP3TE | F_GC_EP3}, // One of F8C2 or F929 must be called before calling this, then this should be called repeatedly until it succeeds or fails. Return values are: 0 = not started, 1 = failed, 2 = timed out, 3 = in progress, 4 = complete
    {0xF8C3, "return_0_F8C3", nullptr, {REG}, F_XB_V3},
    {0xF8C3, "nop_F8C3", nullptr, {REG}, F_V4},
    {0xF8C4, "congrats_msg_multi_cm", "unknownF8C4", {REG}, F_V3},
    {0xF8C4, "nop_F8C4", nullptr, {REG}, F_V4},
    {0xF8C5, "stage_end_multi_cm", "unknownF8C5", {REG}, F_V3},
    {0xF8C5, "nop_F8C5", nullptr, {REG}, F_V4},
    {0xF8C6, "qexit", "QEXIT", {}, F_V3_V4},
    {0xF8C7, "use_animation", nullptr, {REG, REG}, F_V3_V4},
    {0xF8C8, "stop_animation", nullptr, {REG}, F_V3_V4},
    {0xF8C9, "run_to_coord", nullptr, {{REG_SET_FIXED, 4}, REG}, F_V3_V4},
    {0xF8CA, "set_slot_invincible", nullptr, {REG, REG}, F_V3_V4},
    {0xF8CB, "clear_slot_invincible", "unknownF8CB", {REG}, F_V3_V4},
    {0xF8CC, "set_slot_poison", nullptr, {REG}, F_V3_V4},
    {0xF8CD, "set_slot_paralyze", nullptr, {REG}, F_V3_V4},
    {0xF8CE, "set_slot_shock", nullptr, {REG}, F_V3_V4},
    {0xF8CF, "set_slot_freeze", nullptr, {REG}, F_V3_V4},
    {0xF8D0, "set_slot_slow", nullptr, {REG}, F_V3_V4},
    {0xF8D1, "set_slot_confuse", nullptr, {REG}, F_V3_V4},
    {0xF8D2, "set_slot_shifta", nullptr, {REG}, F_V3_V4},
    {0xF8D3, "set_slot_deband", nullptr, {REG}, F_V3_V4},
    {0xF8D4, "set_slot_jellen", nullptr, {REG}, F_V3_V4},
    {0xF8D5, "set_slot_zalure", nullptr, {REG}, F_V3_V4},
    {0xF8D6, "fleti_fixed_camera", nullptr, {{REG_SET_FIXED, 6}}, F_V3_V4 | F_ARGS},
    {0xF8D7, "fleti_locked_camera", nullptr, {INT32, {REG_SET_FIXED, 3}}, F_V3_V4 | F_ARGS},
    {0xF8D8, "default_camera_pos2", nullptr, {}, F_V3_V4},
    {0xF8D9, "set_motion_blur", nullptr, {}, F_V3_V4},
    {0xF8DA, "set_screen_bw", "set_screen_b&w", {}, F_V3_V4},
    {0xF8DB, "get_vector_from_path", "unknownF8DB", {INT32, FLOAT32, FLOAT32, INT32, {REG_SET_FIXED, 4}, SCRIPT16}, F_V3_V4 | F_ARGS},
    {0xF8DC, "npc_action_string", "NPC_action_string", {REG, REG, CSTRING_LABEL16}, F_V3_V4},
    {0xF8DD, "get_pad_cond", nullptr, {REG, REG}, F_V3_V4},
    {0xF8DE, "get_button_cond", nullptr, {REG, REG}, F_V3_V4},
    {0xF8DF, "freeze_enemies", nullptr, {}, F_V3_V4},
    {0xF8E0, "unfreeze_enemies", nullptr, {}, F_V3_V4},
    {0xF8E1, "freeze_everything", nullptr, {}, F_V3_V4},
    {0xF8E2, "unfreeze_everything", nullptr, {}, F_V3_V4},
    {0xF8E3, "restore_hp", nullptr, {REG}, F_V3_V4},
    {0xF8E4, "restore_tp", nullptr, {REG}, F_V3_V4},
    {0xF8E5, "close_chat_bubble", nullptr, {REG}, F_V3_V4},
    {0xF8E6, "move_coords_object", "unknownF8E6", {REG, {REG_SET_FIXED, 3}}, F_V3_V4},
    {0xF8E7, "at_coords_call_ex", "unknownF8E7", {{REG_SET_FIXED, 5}, REG}, F_V3_V4},
    {0xF8E8, "at_coords_talk_ex", "unknownF8E8", {{REG_SET_FIXED, 5}, REG}, F_V3_V4},
    {0xF8E9, "walk_to_coord_call_ex", "unknownF8E9", {{REG_SET_FIXED, 5}, REG}, F_V3_V4},
    {0xF8EA, "col_npcinr_ex", "unknownF8EA", {{REG_SET_FIXED, 6}, REG}, F_V3_V4},
    {0xF8EB, "set_obj_param_ex", "unknownF8EB", {{REG_SET_FIXED, 6}, REG}, F_V3_V4},
    {0xF8EC, "col_plinaw_ex", "unknownF8EC", {{REG_SET_FIXED, 9}, REG}, F_V3_V4},
    {0xF8ED, "animation_check", nullptr, {REG, REG}, F_V3_V4},
    {0xF8EE, "call_image_data", nullptr, {INT32, {LABEL16, Arg::DataType::IMAGE_DATA}}, F_V3_V4 | F_ARGS},
    {0xF8EF, "nop_F8EF", "unknownF8EF", {}, F_V3_V4},
    {0xF8F0, "turn_off_bgm_p2", nullptr, {}, F_V3_V4},
    {0xF8F1, "turn_on_bgm_p2", nullptr, {}, F_V3_V4},
    {0xF8F2, "unknown_F8F2", "load_unk_data", {INT32, FLOAT32, FLOAT32, INT32, {REG_SET_FIXED, 4}, {LABEL16, Arg::DataType::UNKNOWN_F8F2_DATA}}, F_V3_V4 | F_ARGS}, // TODO (DX)
    {0xF8F3, "particle2", nullptr, {{REG_SET_FIXED, 3}, INT32, FLOAT32}, F_V3_V4 | F_ARGS},
    {0xF901, "dec2float", nullptr, {REG, REG}, F_V3_V4},
    {0xF902, "float2dec", nullptr, {REG, REG}, F_V3_V4},
    {0xF903, "flet", nullptr, {REG, REG}, F_V3_V4},
    {0xF904, "fleti", nullptr, {REG, FLOAT32}, F_V3_V4},
    {0xF908, "fadd", nullptr, {REG, REG}, F_V3_V4},
    {0xF909, "faddi", nullptr, {REG, FLOAT32}, F_V3_V4},
    {0xF90A, "fsub", nullptr, {REG, REG}, F_V3_V4},
    {0xF90B, "fsubi", nullptr, {REG, FLOAT32}, F_V3_V4},
    {0xF90C, "fmul", nullptr, {REG, REG}, F_V3_V4},
    {0xF90D, "fmuli", nullptr, {REG, FLOAT32}, F_V3_V4},
    {0xF90E, "fdiv", nullptr, {REG, REG}, F_V3_V4},
    {0xF90F, "fdivi", nullptr, {REG, FLOAT32}, F_V3_V4},
    {0xF910, "get_total_deaths", "get_unknown_count?", {CLIENT_ID, REG}, F_V3_V4 | F_ARGS},
    {0xF911, "get_stackable_item_count", nullptr, {{REG_SET_FIXED, 4}, REG}, F_V3_V4}, // regsA[0] is client ID
    {0xF912, "freeze_and_hide_equip", nullptr, {}, F_V3_V4},
    {0xF913, "thaw_and_show_equip", nullptr, {}, F_V3_V4},
    {0xF914, "set_palettex_callback", "set_paletteX_callback", {CLIENT_ID, SCRIPT16}, F_V3_V4 | F_ARGS},
    {0xF915, "activate_palettex", "activate_paletteX", {CLIENT_ID}, F_V3_V4 | F_ARGS},
    {0xF916, "enable_palettex", "enable_paletteX", {CLIENT_ID}, F_V3_V4 | F_ARGS},
    {0xF917, "restore_palettex", "restore_paletteX", {CLIENT_ID}, F_V3_V4 | F_ARGS},
    {0xF918, "disable_palettex", "disable_paletteX", {CLIENT_ID}, F_V3_V4 | F_ARGS},
    {0xF919, "get_palettex_activated", "get_paletteX_activated", {CLIENT_ID, REG}, F_V3_V4 | F_ARGS},
    {0xF91A, "get_unknown_palettex_status", "get_unknown_paletteX_status?", {CLIENT_ID, INT32, REG}, F_V3_V4 | F_ARGS}, // Middle arg is unused
    {0xF91B, "disable_movement2", nullptr, {CLIENT_ID}, F_V3_V4 | F_ARGS},
    {0xF91C, "enable_movement2", nullptr, {CLIENT_ID}, F_V3_V4 | F_ARGS},
    {0xF91D, "get_time_played", nullptr, {REG}, F_V3_V4},
    {0xF91E, "get_guildcard_total", nullptr, {REG}, F_V3_V4},
    {0xF91F, "get_slot_meseta", nullptr, {REG}, F_V3_V4},
    {0xF920, "get_player_level", nullptr, {CLIENT_ID, REG}, F_V3_V4 | F_ARGS},
    {0xF921, "get_section_id", "get_Section_ID", {CLIENT_ID, REG}, F_V3_V4 | F_ARGS},
    {0xF922, "get_player_hp", nullptr, {CLIENT_ID, {REG_SET_FIXED, 4}}, F_V3_V4 | F_ARGS},
    {0xF923, "get_floor_number", nullptr, {CLIENT_ID, {REG_SET_FIXED, 2}}, F_V3_V4 | F_ARGS},
    {0xF924, "get_coord_player_detect", nullptr, {{REG_SET_FIXED, 3}, {REG_SET_FIXED, 4}}, F_V3_V4},
    {0xF925, "read_counter", "read_global_flag", {INT32, REG}, F_V3_V4 | F_ARGS},
    {0xF926, "write_counter", "write_global_flag", {INT32, INT32}, F_V3_V4 | F_ARGS},
    {0xF927, "item_detect_bank2", "unknownF927", {{REG_SET_FIXED, 4}, REG}, F_V3_V4},
    {0xF928, "floor_player_detect", nullptr, {{REG_SET_FIXED, 4}}, F_V3_V4},
    {0xF929, "prepare_gba_rom_from_disk", "read_disk_file?", {CSTRING}, F_V3 | F_ARGS}, // Prepares to load a GBA ROM from a local GSL file
    {0xF929, "nop_F929", nullptr, {CSTRING}, F_V4 | F_ARGS},
    {0xF92A, "open_pack_select", nullptr, {}, F_V3_V4},
    {0xF92B, "item_select", nullptr, {REG}, F_V3_V4},
    {0xF92C, "get_item_id", nullptr, {REG}, F_V3_V4},
    {0xF92D, "color_change", nullptr, {INT32, INT32, INT32, INT32, INT32}, F_V3_V4 | F_ARGS},
    {0xF92E, "send_statistic", "send_statistic?", {INT32, INT32, INT32, INT32, INT32, INT32, INT32, INT32}, F_V3_V4 | F_ARGS},
    {0xF92F, "gba_write_identifiers", "unknownF92F", {INT32, INT32}, F_V3 | F_ARGS}, // argA is ignored. If argB is 1, the game writes {system_file->creation_timestamp, current_time + rand(0, 100)} (8 bytes in total) to offset 0x2C0 in the GBA ROM data before sending it. current_time is in seconds since 1 January 2000.
    {0xF92F, "nop_F92F", nullptr, {INT32, INT32}, F_V4 | F_ARGS},
    {0xF930, "chat_box", nullptr, {INT32, INT32, INT32, INT32, INT32, CSTRING}, F_V3_V4 | F_ARGS},
    {0xF931, "chat_bubble", nullptr, {INT32, CSTRING}, F_V3_V4 | F_ARGS},
    {0xF932, "set_episode2", nullptr, {REG}, F_V3_V4},
    {0xF933, "item_create_multi_cm", "unknownF933", {{REG_SET_FIXED, 7}}, F_V3}, // regsA[1-6] form an ItemData's data1[0-5]
    {0xF933, "nop_F933", nullptr, {{REG_SET_FIXED, 7}}, F_V4},
    {0xF934, "scroll_text", nullptr, {INT32, INT32, INT32, INT32, INT32, FLOAT32, REG, CSTRING}, F_V3_V4 | F_ARGS},
    {0xF935, "gba_create_dl_graph", "gba_unknown1", {}, F_GC_V3 | F_GC_EP3TE | F_GC_EP3}, // Creates the GBA loading progress bar (same as the quest download progress bar)
    {0xF935, "nop_F935", nullptr, {}, F_XB_V3 | F_V4},
    {0xF936, "gba_destroy_dl_graph", "gba_unknown2", {}, F_GC_V3 | F_GC_EP3TE | F_GC_EP3}, // Destroys the GBA loading progress bar
    {0xF936, "nop_F936", nullptr, {}, F_XB_V3 | F_V4},
    {0xF937, "gba_update_dl_graph", "gba_unknown3", {}, F_GC_V3 | F_GC_EP3TE | F_GC_EP3}, // Updates the GBA loading progress bar
    {0xF937, "nop_F937", nullptr, {}, F_XB_V3 | F_V4},
    {0xF938, "add_damage_to", "add_damage_to?", {INT32, INT32}, F_V3_V4 | F_ARGS},
    {0xF939, "item_delete3", nullptr, {INT32}, F_V3_V4 | F_ARGS},
    {0xF93A, "get_item_info", nullptr, {ITEM_ID, {REG_SET_FIXED, 12}}, F_V3_V4 | F_ARGS}, // regsB are item.data1 (1 byte each)
    {0xF93B, "item_packing1", nullptr, {ITEM_ID}, F_V3_V4 | F_ARGS},
    {0xF93C, "item_packing2", nullptr, {ITEM_ID, INT32}, F_V3_V4 | F_ARGS}, // Sends 6xD6 on BB
    {0xF93D, "get_lang_setting", "get_lang_setting?", {REG}, F_V3_V4 | F_ARGS},
    {0xF93E, "prepare_statistic", "prepare_statistic?", {INT32, INT32, INT32}, F_V3_V4 | F_ARGS},
    {0xF93F, "keyword_detect", nullptr, {}, F_V3_V4},
    {0xF940, "keyword", nullptr, {REG, INT32, CSTRING}, F_V3_V4 | F_ARGS},
    {0xF941, "get_guildcard_num", nullptr, {CLIENT_ID, REG}, F_V3_V4 | F_ARGS},
    {0xF942, "get_recent_symbol_chat", nullptr, {INT32, {REG_SET_FIXED, 15}}, F_V3_V4 | F_ARGS}, // argA = client ID, regsB = symbol chat data (out)
    {0xF943, "create_symbol_chat_capture_buffer", nullptr, {}, F_V3_V4},
    {0xF944, "get_item_stackability", "get_wrap_status", {ITEM_ID, REG}, F_V3_V4 | F_ARGS},
    {0xF945, "initial_floor", nullptr, {INT32}, F_V3_V4 | F_ARGS},
    {0xF946, "sin", nullptr, {REG, INT32}, F_V3_V4 | F_ARGS},
    {0xF947, "cos", nullptr, {REG, INT32}, F_V3_V4 | F_ARGS},
    {0xF948, "tan", nullptr, {REG, INT32}, F_V3_V4 | F_ARGS},
    {0xF949, "atan2_int", nullptr, {REG, FLOAT32, FLOAT32}, F_V3_V4 | F_ARGS},
    {0xF94A, "olga_flow_is_dead", "boss_is_dead2?", {REG}, F_V3_V4},
    {0xF94B, "particle_effect_nc", "particle3", {{REG_SET_FIXED, 4}}, F_V3_V4},
    {0xF94C, "player_effect_nc", "unknownF94C", {{REG_SET_FIXED, 4}}, F_V3_V4},
    {0xF94D, "has_ep3_save_file", nullptr, {REG}, F_GC_V3 | F_ARGS}, // (PSO Plus only) Returns 1 if a file named PSO3_CHARACTER is present on either memory card
    {0xF94D, "give_card", "is_there_cardbattle?", {REG}, F_GC_EP3TE}, // regsA[0] is card_id; card is given if regsA[1] >= 0, otherwise it's taken
    {0xF94D, "give_or_take_card", "is_there_cardbattle?", {{REG_SET_FIXED, 2}}, F_GC_EP3}, // regsA[0] is card_id; card is given if regsA[1] >= 0, otherwise it's taken
    {0xF94D, "unknown_F94D", nullptr, {INT32, REG}, F_XB_V3 | F_ARGS}, // Related to voice chat. argA is a client ID; a value is read from that player's TVoiceChatClient object and (!!value) is placed in regB. This value is set by the 6xB3 command; TODO: figure out what that value represents and name this opcode appropriately
    {0xF94D, "nop_F94D", nullptr, {}, F_V4},
    {0xF94E, "nop_F94E", nullptr, {}, F_V4},
    {0xF94F, "nop_F94F", nullptr, {}, F_V4},
    {0xF950, "bb_p2_menu", "BB_p2_menu", {INT32}, F_V4 | F_ARGS},
    {0xF951, "bb_map_designate", "BB_Map_Designate", {INT8, INT8, INT8, INT8, INT8}, F_V4},
    {0xF952, "bb_get_number_in_pack", "BB_get_number_in_pack", {REG}, F_V4},
    {0xF953, "bb_swap_item", "BB_swap_item", {INT32, INT32, INT32, INT32, INT32, INT32, SCRIPT16, SCRIPT16}, F_V4 | F_ARGS}, // Sends 6xD5
    {0xF954, "bb_check_wrap", "BB_check_wrap", {INT32, REG}, F_V4 | F_ARGS},
    {0xF955, "bb_exchange_pd_item", "BB_exchange_PD_item", {INT32, INT32, INT32, LABEL16, LABEL16}, F_V4 | F_ARGS}, // Sends 6xD7
    {0xF956, "bb_exchange_pd_srank", "BB_exchange_PD_srank", {INT32, INT32, INT32, INT32, INT32, LABEL16, LABEL16}, F_V4 | F_ARGS}, // Sends 6xD8; argsA[0] is item ID; argsA[1]-[3] are item.data1[0]-[2]; argsA[4] is special type; argsA[5] is success label; argsA[6] is failure label
    {0xF957, "bb_exchange_pd_percent", "BB_exchange_PD_special", {INT32, INT32, INT32, INT32, INT32, INT32, LABEL16, LABEL16}, F_V4 | F_ARGS}, // Sends 6xDA
    {0xF958, "bb_exchange_ps_percent", "BB_exchange_PD_percent", {INT32, INT32, INT32, INT32, INT32, INT32, LABEL16, LABEL16}, F_V4 | F_ARGS}, // Sends 6xDA
    {0xF959, "bb_set_ep4_boss_can_escape", "unknownF959", {INT32}, F_V4 | F_ARGS},
    {0xF95A, "bb_is_ep4_boss_dying", nullptr, {REG}, F_V4},
    {0xF95B, "bb_send_6xD9", nullptr, {INT32, INT32, INT32, INT32, LABEL16, LABEL16}, F_V4 | F_ARGS}, // Sends 6xD9
    {0xF95C, "bb_exchange_slt", "BB_exchange_SLT", {INT32, INT32, INT32, INT32}, F_V4 | F_ARGS}, // Sends 6xDE
    {0xF95D, "bb_exchange_pc", "BB_exchange_PC", {}, F_V4}, // Sends 6xDF
    {0xF95E, "bb_box_create_bp", "BB_box_create_BP", {INT32, FLOAT32, FLOAT32}, F_V4 | F_ARGS}, // Sends 6xE0
    {0xF95F, "bb_exchange_pt", "BB_exchage_PT", {INT32, INT32, INT32, INT32, INT32}, F_V4 | F_ARGS}, // Sends 6xE1
    {0xF960, "bb_send_6xE2", "unknownF960", {INT32}, F_V4 | F_ARGS}, // Sends 6xE2
    {0xF961, "bb_get_6xE3_status", "unknownF961", {REG}, F_V4}, // Returns 0 if 6xE3 hasn't been received, 1 if the received item is valid, 2 if the received item is invalid
};

static const unordered_map<uint16_t, const QuestScriptOpcodeDefinition*>&
opcodes_for_version(Version v) {
  static array<
      unordered_map<uint16_t, const QuestScriptOpcodeDefinition*>,
      static_cast<size_t>(Version::BB_V4) + 1>
      indexes;

  auto& index = indexes.at(static_cast<size_t>(v));
  if (index.empty()) {
    uint16_t vf = v_flag(v);
    for (size_t z = 0; z < sizeof(opcode_defs) / sizeof(opcode_defs[0]); z++) {
      const auto& def = opcode_defs[z];
      if (!(def.flags & vf)) {
        continue;
      }
      if (!index.emplace(def.opcode, &def).second) {
        throw logic_error(phosg::string_printf("duplicate definition for opcode %04hX", def.opcode));
      }
    }
  }
  return index;
}

static const unordered_map<string, const QuestScriptOpcodeDefinition*>&
opcodes_by_name_for_version(Version v) {
  static array<
      unordered_map<string, const QuestScriptOpcodeDefinition*>,
      static_cast<size_t>(Version::BB_V4) + 1>
      indexes;

  auto& index = indexes.at(static_cast<size_t>(v));
  if (index.empty()) {
    uint16_t vf = v_flag(v);
    for (size_t z = 0; z < sizeof(opcode_defs) / sizeof(opcode_defs[0]); z++) {
      const auto& def = opcode_defs[z];
      if (!(def.flags & vf)) {
        continue;
      }
      if (def.name && !index.emplace(def.name, &def).second) {
        throw logic_error(phosg::string_printf("duplicate definition for opcode %04hX", def.opcode));
      }
      if (def.qedit_name && !index.emplace(def.qedit_name, &def).second) {
        throw logic_error(phosg::string_printf("duplicate definition for opcode %04hX", def.opcode));
      }
    }
  }
  return index;
}

void check_opcode_definitions() {
  static const array<Version, 12> versions = {
      Version::DC_NTE,
      Version::DC_V1_11_2000_PROTOTYPE,
      Version::DC_V1,
      Version::DC_V2,
      Version::PC_NTE,
      Version::PC_V2,
      Version::GC_NTE,
      Version::GC_V3,
      Version::GC_EP3_NTE,
      Version::GC_EP3,
      Version::XB_V3,
      Version::BB_V4,
  };
  for (Version v : versions) {
    const auto& opcodes_by_name = opcodes_by_name_for_version(v);
    const auto& opcodes = opcodes_for_version(v);
    phosg::log_info("Version %s has %zu opcodes with %zu mnemonics", phosg::name_for_enum(v), opcodes.size(), opcodes_by_name.size());
  }
}

std::string disassemble_quest_script(
    const void* data,
    size_t size,
    Version version,
    uint8_t override_language,
    bool reassembly_mode,
    bool use_qedit_names) {
  phosg::StringReader r(data, size);
  deque<string> lines;
  lines.emplace_back(phosg::string_printf(".version %s", phosg::name_for_enum(version)));

  bool use_wstrs = false;
  size_t code_offset = 0;
  size_t function_table_offset = 0;
  uint8_t language;
  switch (version) {
    case Version::DC_NTE: {
      const auto& header = r.get<PSOQuestHeaderDCNTE>();
      code_offset = header.code_offset;
      function_table_offset = header.function_table_offset;
      language = 0;
      lines.emplace_back(".name " + escape_string(header.name.decode(0)));
      break;
    }
    case Version::DC_V1_11_2000_PROTOTYPE:
    case Version::DC_V1:
    case Version::DC_V2: {
      const auto& header = r.get<PSOQuestHeaderDC>();
      code_offset = header.code_offset;
      function_table_offset = header.function_table_offset;
      if (override_language != 0xFF) {
        language = override_language;
      } else if (header.language < 5) {
        language = header.language;
      } else {
        language = 1;
      }
      lines.emplace_back(phosg::string_printf(".quest_num %hu", header.quest_number.load()));
      lines.emplace_back(phosg::string_printf(".language %hhu", header.language));
      lines.emplace_back(".name " + escape_string(header.name.decode(language)));
      lines.emplace_back(".short_desc " + escape_string(header.short_description.decode(language)));
      lines.emplace_back(".long_desc " + escape_string(header.long_description.decode(language)));
      break;
    }
    case Version::PC_NTE:
    case Version::PC_V2: {
      use_wstrs = true;
      const auto& header = r.get<PSOQuestHeaderPC>();
      code_offset = header.code_offset;
      function_table_offset = header.function_table_offset;
      if (override_language != 0xFF) {
        language = override_language;
      } else if (header.language < 8) {
        language = header.language;
      } else {
        language = 1;
      }
      lines.emplace_back(phosg::string_printf(".quest_num %hu", header.quest_number.load()));
      lines.emplace_back(phosg::string_printf(".language %hhu", header.language));
      lines.emplace_back(".name " + escape_string(header.name.decode(language)));
      lines.emplace_back(".short_desc " + escape_string(header.short_description.decode(language)));
      lines.emplace_back(".long_desc " + escape_string(header.long_description.decode(language)));
      break;
    }
    case Version::GC_NTE:
    case Version::GC_V3:
    case Version::GC_EP3_NTE:
    case Version::GC_EP3:
    case Version::XB_V3: {
      const auto& header = r.get<PSOQuestHeaderGC>();
      code_offset = header.code_offset;
      function_table_offset = header.function_table_offset;
      if (override_language != 0xFF) {
        language = override_language;
      } else if (header.language < 5) {
        language = header.language;
      } else {
        language = 1;
      }
      lines.emplace_back(phosg::string_printf(".quest_num %hhu", header.quest_number));
      lines.emplace_back(phosg::string_printf(".language %hhu", header.language));
      lines.emplace_back(phosg::string_printf(".episode %s", name_for_header_episode_number(header.episode)));
      lines.emplace_back(".name " + escape_string(header.name.decode(language)));
      lines.emplace_back(".short_desc " + escape_string(header.short_description.decode(language)));
      lines.emplace_back(".long_desc " + escape_string(header.long_description.decode(language)));
      break;
    }
    case Version::BB_V4: {
      use_wstrs = true;
      const auto& header = r.get<PSOQuestHeaderBB>();
      code_offset = header.code_offset;
      function_table_offset = header.function_table_offset;
      if (override_language != 0xFF) {
        language = override_language;
      } else {
        language = 1;
      }
      lines.emplace_back(phosg::string_printf(".quest_num %hu", header.quest_number.load()));
      lines.emplace_back(phosg::string_printf(".episode %s", name_for_header_episode_number(header.episode)));
      lines.emplace_back(phosg::string_printf(".max_players %hhu", header.max_players ? header.max_players : 4));
      if (header.joinable) {
        lines.emplace_back(".joinable");
      }
      lines.emplace_back(".name " + escape_string(header.name.decode(language)));
      lines.emplace_back(".short_desc " + escape_string(header.short_description.decode(language)));
      lines.emplace_back(".long_desc " + escape_string(header.long_description.decode(language)));
      break;
    }
    default:
      throw logic_error("invalid quest version");
  }

  const auto& opcodes = opcodes_for_version(version);
  phosg::StringReader cmd_r = r.sub(code_offset, function_table_offset - code_offset);

  struct Label {
    string name;
    uint32_t offset;
    uint32_t function_id; // 0xFFFFFFFF = no function ID
    uint64_t type_flags;
    set<size_t> references;

    Label(const string& name, uint32_t offset, int64_t function_id = -1, uint64_t type_flags = 0)
        : name(name),
          offset(offset),
          function_id(function_id),
          type_flags(type_flags) {}
    void add_data_type(Arg::DataType type) {
      this->type_flags |= (1 << static_cast<size_t>(type));
    }
    bool has_data_type(Arg::DataType type) const {
      return this->type_flags & (1 << static_cast<size_t>(type));
    }
  };

  vector<shared_ptr<Label>> function_table;
  multimap<size_t, shared_ptr<Label>> offset_to_label;
  phosg::StringReader function_table_r = r.sub(function_table_offset);
  while (!function_table_r.eof()) {
    try {
      uint32_t function_id = function_table.size();
      string name = (function_id == 0) ? "start" : phosg::string_printf("label%04" PRIX32, function_id);
      uint32_t offset = function_table_r.get_u32l();
      auto l = make_shared<Label>(name, offset, function_id);
      if (function_id == 0) {
        l->add_data_type(Arg::DataType::SCRIPT);
      }
      function_table.emplace_back(l);
      if (l->offset < cmd_r.size()) {
        offset_to_label.emplace(l->offset, l);
      }
    } catch (const out_of_range&) {
      function_table_r.skip(function_table_r.remaining());
    }
  }

  struct DisassemblyLine {
    string line;
    size_t next_offset;

    DisassemblyLine(string&& line, size_t next_offset)
        : line(std::move(line)),
          next_offset(next_offset) {}
  };

  struct ArgStackValue {
    enum class Type {
      REG,
      REG_PTR,
      LABEL,
      INT,
      CSTRING,
    };
    Type type;
    uint32_t as_int;
    std::string as_string;

    ArgStackValue(Type type, uint32_t value) {
      this->type = type;
      this->as_int = value;
    }
    ArgStackValue(const std::string& value) {
      this->type = Type::CSTRING;
      this->as_string = value;
    }
  };

  map<size_t, DisassemblyLine> dasm_lines;
  set<size_t> pending_dasm_start_offsets;
  for (const auto& l : function_table) {
    if (l->offset < cmd_r.size()) {
      pending_dasm_start_offsets.emplace(l->offset);
    }
  }

  bool version_has_args = F_HAS_ARGS & v_flag(version);
  while (!pending_dasm_start_offsets.empty()) {
    auto dasm_start_offset_it = pending_dasm_start_offsets.begin();
    cmd_r.go(*dasm_start_offset_it);
    pending_dasm_start_offsets.erase(dasm_start_offset_it);

    vector<ArgStackValue> arg_stack_values;
    while (!cmd_r.eof() && !dasm_lines.count(cmd_r.where())) {
      size_t opcode_start_offset = cmd_r.where();
      string dasm_line;
      try {
        uint16_t opcode = cmd_r.get_u8();
        if ((opcode & 0xFE) == 0xF8) {
          opcode = (opcode << 8) | cmd_r.get_u8();
        }

        const QuestScriptOpcodeDefinition* def = nullptr;
        try {
          def = opcodes.at(opcode);
        } catch (const out_of_range&) {
        }

        if (def == nullptr) {
          dasm_line = phosg::string_printf(".unknown %04hX", opcode);
        } else {
          const char* op_name = (use_qedit_names && def->qedit_name) ? def->qedit_name : def->name;
          dasm_line = op_name ? op_name : phosg::string_printf("[%04hX]", opcode);
          if (!version_has_args || !(def->flags & F_ARGS)) {
            dasm_line.resize(0x20, ' ');
            bool is_first_arg = true;
            for (const auto& arg : def->args) {
              using Type = QuestScriptOpcodeDefinition::Argument::Type;
              string dasm_arg;
              switch (arg.type) {
                case Type::LABEL16:
                case Type::LABEL32: {
                  uint32_t label_id = (arg.type == Type::LABEL32) ? cmd_r.get_u32l() : cmd_r.get_u16l();
                  if (def->flags & F_PASS) {
                    arg_stack_values.emplace_back(ArgStackValue::Type::LABEL, label_id);
                  }
                  if (label_id >= function_table.size()) {
                    dasm_arg = phosg::string_printf("label%04" PRIX32, label_id);
                  } else {
                    auto& l = function_table.at(label_id);
                    if (reassembly_mode) {
                      dasm_arg = phosg::string_printf("label%04" PRIX32, label_id);
                    } else {
                      dasm_arg = phosg::string_printf("label%04" PRIX32 " /* %04" PRIX32 " */", label_id, l->offset);
                    }
                    l->references.emplace(opcode_start_offset);
                    l->add_data_type(arg.data_type);
                    if (arg.data_type == Arg::DataType::SCRIPT) {
                      pending_dasm_start_offsets.emplace(l->offset);
                    }
                  }
                  break;
                }
                case Type::LABEL16_SET: {
                  if (def->flags & F_PASS) {
                    throw logic_error("LABEL16_SET cannot be pushed to arg stack");
                  }
                  uint8_t num_functions = cmd_r.get_u8();
                  for (size_t z = 0; z < num_functions; z++) {
                    dasm_arg += (dasm_arg.empty() ? "[" : ", ");
                    uint32_t label_id = cmd_r.get_u16l();
                    if (label_id >= function_table.size()) {
                      dasm_arg += phosg::string_printf("label%04" PRIX32, label_id);
                    } else {
                      auto& l = function_table.at(label_id);
                      if (reassembly_mode) {
                        dasm_arg += phosg::string_printf("label%04" PRIX32, label_id);
                      } else {
                        dasm_arg += phosg::string_printf("label%04" PRIX32 " /* %04" PRIX32 " */", label_id, l->offset);
                      }
                      l->references.emplace(opcode_start_offset);
                      l->add_data_type(arg.data_type);
                      if (arg.data_type == Arg::DataType::SCRIPT) {
                        pending_dasm_start_offsets.emplace(l->offset);
                      }
                    }
                  }
                  if (dasm_arg.empty()) {
                    dasm_arg = "[]";
                  } else {
                    dasm_arg += "]";
                  }
                  break;
                }
                case Type::REG: {
                  uint8_t reg = cmd_r.get_u8();
                  if (def->flags & F_PASS) {
                    arg_stack_values.emplace_back((def->opcode == 0x004C) ? ArgStackValue::Type::REG_PTR : ArgStackValue::Type::REG, reg);
                  }
                  dasm_arg = phosg::string_printf("r%hhu", reg);
                  break;
                }
                case Type::REG_SET: {
                  if (def->flags & F_PASS) {
                    throw logic_error("REG_SET cannot be pushed to arg stack");
                  }
                  uint8_t num_regs = cmd_r.get_u8();
                  for (size_t z = 0; z < num_regs; z++) {
                    dasm_arg += phosg::string_printf("%sr%hhu", (dasm_arg.empty() ? "[" : ", "), cmd_r.get_u8());
                  }
                  if (dasm_arg.empty()) {
                    dasm_arg = "[]";
                  } else {
                    dasm_arg += "]";
                  }
                  break;
                }
                case Type::REG_SET_FIXED: {
                  if (def->flags & F_PASS) {
                    throw logic_error("REG_SET_FIXED cannot be pushed to arg stack");
                  }
                  uint8_t first_reg = cmd_r.get_u8();
                  dasm_arg = phosg::string_printf("r%hhu-r%hhu", first_reg, static_cast<uint8_t>(first_reg + arg.count - 1));
                  break;
                }
                case Type::REG32_SET_FIXED: {
                  if (def->flags & F_PASS) {
                    throw logic_error("REG32_SET_FIXED cannot be pushed to arg stack");
                  }
                  uint32_t first_reg = cmd_r.get_u32l();
                  dasm_arg = phosg::string_printf("r%" PRIu32 "-r%" PRIu32, first_reg, static_cast<uint32_t>(first_reg + arg.count - 1));
                  break;
                }
                case Type::INT8: {
                  uint8_t v = cmd_r.get_u8();
                  if (def->flags & F_PASS) {
                    arg_stack_values.emplace_back(ArgStackValue::Type::INT, v);
                  }
                  dasm_arg = phosg::string_printf("0x%02hhX", v);
                  break;
                }
                case Type::INT16: {
                  uint16_t v = cmd_r.get_u16l();
                  if (def->flags & F_PASS) {
                    arg_stack_values.emplace_back(ArgStackValue::Type::INT, v);
                  }
                  dasm_arg = phosg::string_printf("0x%04hX", v);
                  break;
                }
                case Type::INT32: {
                  uint32_t v = cmd_r.get_u32l();
                  if (def->flags & F_PASS) {
                    arg_stack_values.emplace_back(ArgStackValue::Type::INT, v);
                  }
                  dasm_arg = phosg::string_printf("0x%08" PRIX32, v);
                  break;
                }
                case Type::FLOAT32: {
                  float v = cmd_r.get_f32l();
                  if (def->flags & F_PASS) {
                    arg_stack_values.emplace_back(ArgStackValue::Type::INT, as_type<uint32_t>(v));
                  }
                  dasm_arg = phosg::string_printf("%g", v);
                  break;
                }
                case Type::CSTRING:
                  if (use_wstrs) {
                    phosg::StringWriter w;
                    for (uint16_t ch = cmd_r.get_u16l(); ch; ch = cmd_r.get_u16l()) {
                      w.put_u16l(ch);
                    }
                    if (def->flags & F_PASS) {
                      arg_stack_values.emplace_back(tt_utf16_to_utf8(w.str()));
                    }
                    dasm_arg = escape_string(w.str(), TextEncoding::UTF16);
                  } else {
                    string s = cmd_r.get_cstr();
                    if (def->flags & F_PASS) {
                      arg_stack_values.emplace_back(language ? tt_8859_to_utf8(s) : tt_sega_sjis_to_utf8(s));
                    }
                    dasm_arg = escape_string(s, encoding_for_language(language));
                  }
                  break;
                default:
                  throw logic_error("invalid argument type");
              }
              if (!is_first_arg) {
                dasm_line += ", ";
              } else {
                is_first_arg = false;
              }
              dasm_line += dasm_arg;
            }

          } else { // (def->flags & F_ARGS)
            dasm_line.resize(0x20, ' ');
            if (reassembly_mode) {
              dasm_line += "...";
            } else {
              dasm_line += "... ";

              if (def->args.size() != arg_stack_values.size()) {
                dasm_line += phosg::string_printf("/* matching error: expected %zu arguments, received %zu arguments */",
                    def->args.size(), arg_stack_values.size());
              } else {
                bool is_first_arg = true;
                for (size_t z = 0; z < def->args.size(); z++) {
                  const auto& arg_def = def->args[z];
                  const auto& arg_value = arg_stack_values[z];

                  string dasm_arg;
                  switch (arg_def.type) {
                    case Arg::Type::LABEL16:
                    case Arg::Type::LABEL32:
                      switch (arg_value.type) {
                        case ArgStackValue::Type::REG:
                          dasm_arg = phosg::string_printf("r%" PRIu32 "/* warning: cannot determine label data type */", arg_value.as_int);
                          break;
                        case ArgStackValue::Type::LABEL:
                        case ArgStackValue::Type::INT:
                          dasm_arg = phosg::string_printf("label%04" PRIX32, arg_value.as_int);
                          try {
                            auto l = function_table.at(arg_value.as_int);
                            l->add_data_type(arg_def.data_type);
                            l->references.emplace(opcode_start_offset);
                          } catch (const out_of_range&) {
                          }
                          break;
                        default:
                          dasm_arg = "/* invalid-type */";
                      }
                      break;
                    case Arg::Type::REG:
                    case Arg::Type::REG32:
                      switch (arg_value.type) {
                        case ArgStackValue::Type::REG:
                          dasm_arg = phosg::string_printf("regs[r%" PRIu32 "]", arg_value.as_int);
                          break;
                        case ArgStackValue::Type::INT:
                          dasm_arg = phosg::string_printf("r%" PRIu32, arg_value.as_int);
                          break;
                        default:
                          dasm_arg = "/* invalid-type */";
                      }
                      break;
                    case Arg::Type::REG_SET_FIXED:
                    case Arg::Type::REG32_SET_FIXED:
                      switch (arg_value.type) {
                        case ArgStackValue::Type::REG:
                          dasm_arg = phosg::string_printf("regs[r%" PRIu32 "]-regs[r%" PRIu32 "+%hhu]", arg_value.as_int, arg_value.as_int, static_cast<uint8_t>(arg_def.count - 1));
                          break;
                        case ArgStackValue::Type::INT:
                          dasm_arg = phosg::string_printf("r%" PRIu32 "-r%hhu", arg_value.as_int, static_cast<uint8_t>(arg_value.as_int + arg_def.count - 1));
                          break;
                        default:
                          dasm_arg = "/* invalid-type */";
                      }
                      break;
                    case Arg::Type::INT8:
                    case Arg::Type::INT16:
                    case Arg::Type::INT32:
                      switch (arg_value.type) {
                        case ArgStackValue::Type::REG:
                          dasm_arg = phosg::string_printf("r%" PRIu32, arg_value.as_int);
                          break;
                        case ArgStackValue::Type::REG_PTR:
                          dasm_arg = phosg::string_printf("&r%" PRIu32, arg_value.as_int);
                          break;
                        case ArgStackValue::Type::INT:
                          dasm_arg = phosg::string_printf("0x%" PRIX32 " /* %" PRIu32 " */", arg_value.as_int, arg_value.as_int);
                          break;
                        default:
                          dasm_arg = "/* invalid-type */";
                      }
                      break;
                    case Arg::Type::FLOAT32:
                      switch (arg_value.type) {
                        case ArgStackValue::Type::REG:
                          dasm_arg = phosg::string_printf("f%" PRIu32, arg_value.as_int);
                          break;
                        case ArgStackValue::Type::INT:
                          dasm_arg = phosg::string_printf("%g", as_type<float>(arg_value.as_int));
                          break;
                        default:
                          dasm_arg = "/* invalid-type */";
                      }
                      break;
                    case Arg::Type::CSTRING:
                      if (arg_value.type == ArgStackValue::Type::CSTRING) {
                        dasm_arg = escape_string(arg_value.as_string);
                      } else {
                        dasm_arg = "/* invalid-type */";
                      }
                      break;
                    case Arg::Type::LABEL16_SET:
                    case Arg::Type::REG_SET:
                    default:
                      throw logic_error("set-type arg found on arg stack");
                  }

                  if (!is_first_arg) {
                    dasm_line += ", ";
                  } else {
                    is_first_arg = false;
                  }
                  dasm_line += dasm_arg;
                }
              }
            }
          }

          if (!(def->flags & F_PASS)) {
            arg_stack_values.clear();
          }
        }
      } catch (const exception& e) {
        dasm_line = phosg::string_printf(".failed (%s)", e.what());
      }
      phosg::strip_trailing_whitespace(dasm_line);

      string line_text;
      if (reassembly_mode) {
        line_text = phosg::string_printf("  %s", dasm_line.c_str());
      } else {
        string hex_data = phosg::format_data_string(cmd_r.preadx(opcode_start_offset, cmd_r.where() - opcode_start_offset), nullptr, phosg::FormatDataFlags::HEX_ONLY);
        if (hex_data.size() > 14) {
          hex_data.resize(12);
          hex_data += "...";
        }
        hex_data.resize(16, ' ');
        line_text = phosg::string_printf("  %04zX  %s  %s", opcode_start_offset, hex_data.c_str(), dasm_line.c_str());
      }
      dasm_lines.emplace(opcode_start_offset, DisassemblyLine(std::move(line_text), cmd_r.where()));
    }
  }

  auto label_it = offset_to_label.begin();
  while (label_it != offset_to_label.end()) {
    auto l = label_it->second;
    label_it++;
    size_t size = ((label_it == offset_to_label.end()) ? cmd_r.size() : label_it->second->offset) - l->offset;
    if (size > 0) {
      lines.emplace_back();
    }
    if (reassembly_mode) {
      lines.emplace_back(phosg::string_printf("%s@0x%04" PRIX32 ":", l->name.c_str(), l->function_id));
    } else {
      lines.emplace_back(phosg::string_printf("%s:", l->name.c_str()));
      if (l->references.size() == 1) {
        lines.emplace_back(phosg::string_printf("  // Referenced by instruction at %04zX", *l->references.begin()));
      } else if (!l->references.empty()) {
        vector<string> tokens;
        tokens.reserve(l->references.size());
        for (size_t reference_offset : l->references) {
          tokens.emplace_back(phosg::string_printf("%04zX", reference_offset));
        }
        lines.emplace_back("  // Referenced by instructions at " + phosg::join(tokens, ", "));
      }
    }

    if (l->type_flags == 0) {
      lines.emplace_back(phosg::string_printf("  // Could not determine data type; disassembling as code"));
      l->add_data_type(Arg::DataType::SCRIPT);
    }

    auto add_disassembly_lines = [&](size_t start_offset, size_t size) -> void {
      for (size_t z = start_offset; z < start_offset + size;) {
        const auto& l = dasm_lines.at(z);
        lines.emplace_back(l.line);
        if (l.next_offset <= z) {
          throw logic_error("line points backward or to itself");
        }
        z = l.next_offset;
      }
    };

    // Print data interpretations of the label (if any)
    if (reassembly_mode) {
      if (l->has_data_type(Arg::DataType::SCRIPT)) {
        add_disassembly_lines(l->offset, size);
      } else {
        lines.emplace_back(".data " + phosg::format_data_string(cmd_r.pgetv(l->offset, size), size));
      }

    } else {
      auto print_as_struct = [&]<Arg::DataType data_type, typename StructT>(function<void(const StructT&)> print_fn) {
        if (l->has_data_type(data_type)) {
          if (size >= sizeof(StructT)) {
            print_fn(cmd_r.pget<StructT>(l->offset));
            if (size > sizeof(StructT)) {
              size_t struct_end_offset = l->offset + sizeof(StructT);
              size_t remaining_size = size - sizeof(StructT);
              lines.emplace_back("  // Extra data after structure");
              lines.emplace_back(format_and_indent_data(cmd_r.pgetv(struct_end_offset, remaining_size), remaining_size, struct_end_offset));
            }
          } else {
            lines.emplace_back(phosg::string_printf("  // As raw data (0x%zX bytes; too small for referenced type)", size));
            lines.emplace_back(format_and_indent_data(cmd_r.pgetv(l->offset, size), size, l->offset));
          }
        }
      };

      if (l->has_data_type(Arg::DataType::DATA)) {
        lines.emplace_back(phosg::string_printf("  // As raw data (0x%zX bytes)", size));
        lines.emplace_back(format_and_indent_data(cmd_r.pgetv(l->offset, size), size, l->offset));
      }
      if (l->has_data_type(Arg::DataType::CSTRING)) {
        lines.emplace_back(phosg::string_printf("  // As C string (0x%zX bytes)", size));
        string str_data = cmd_r.pread(l->offset, size);
        phosg::strip_trailing_zeroes(str_data);
        if (use_wstrs) {
          if (str_data.size() & 1) {
            str_data.push_back(0);
          }
          str_data = tt_utf16_to_utf8(str_data);
        }
        string formatted = escape_string(str_data, use_wstrs ? TextEncoding::UTF16 : encoding_for_language(language));
        lines.emplace_back(phosg::string_printf("  %04" PRIX32 "  %s", l->offset, formatted.c_str()));
      }
      print_as_struct.template operator()<Arg::DataType::PLAYER_VISUAL_CONFIG, PlayerVisualConfig>([&](const PlayerVisualConfig& visual) -> void {
        lines.emplace_back("  // As PlayerVisualConfig");
        string name = escape_string(visual.name.decode(language));
        lines.emplace_back(phosg::string_printf("  %04zX  name              %s", l->offset + offsetof(PlayerVisualConfig, name), name.c_str()));
        lines.emplace_back(phosg::string_printf("  %04zX  name_color        %08" PRIX32, l->offset + offsetof(PlayerVisualConfig, name_color), visual.name_color.load()));
        string a2_str = phosg::format_data_string(visual.unknown_a2.data(), sizeof(visual.unknown_a2));
        lines.emplace_back(phosg::string_printf("  %04zX  a2                %s", l->offset + offsetof(PlayerVisualConfig, unknown_a2), a2_str.c_str()));
        lines.emplace_back(phosg::string_printf("  %04zX  extra_model       %02hhX", l->offset + offsetof(PlayerVisualConfig, extra_model), visual.extra_model));
        string unused = phosg::format_data_string(visual.unused.data(), visual.unused.bytes());
        lines.emplace_back(phosg::string_printf("  %04zX  unused            %s", l->offset + offsetof(PlayerVisualConfig, unused), unused.c_str()));
        lines.emplace_back(phosg::string_printf("  %04zX  name_color_cs     %08" PRIX32, l->offset + offsetof(PlayerVisualConfig, name_color_checksum), visual.name_color_checksum.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  section_id        %02hhX (%s)", l->offset + offsetof(PlayerVisualConfig, section_id), visual.section_id, name_for_section_id(visual.section_id)));
        lines.emplace_back(phosg::string_printf("  %04zX  char_class        %02hhX (%s)", l->offset + offsetof(PlayerVisualConfig, char_class), visual.char_class, name_for_char_class(visual.char_class)));
        lines.emplace_back(phosg::string_printf("  %04zX  validation_flags  %02hhX", l->offset + offsetof(PlayerVisualConfig, validation_flags), visual.validation_flags));
        lines.emplace_back(phosg::string_printf("  %04zX  version           %02hhX", l->offset + offsetof(PlayerVisualConfig, version), visual.version));
        lines.emplace_back(phosg::string_printf("  %04zX  class_flags       %08" PRIX32, l->offset + offsetof(PlayerVisualConfig, class_flags), visual.class_flags.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  costume           %04hX", l->offset + offsetof(PlayerVisualConfig, costume), visual.costume.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  skin              %04hX", l->offset + offsetof(PlayerVisualConfig, skin), visual.skin.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  face              %04hX", l->offset + offsetof(PlayerVisualConfig, face), visual.face.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  head              %04hX", l->offset + offsetof(PlayerVisualConfig, head), visual.head.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  hair              %04hX", l->offset + offsetof(PlayerVisualConfig, hair), visual.hair.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  hair_color        %04hX, %04hX, %04hX", l->offset + offsetof(PlayerVisualConfig, hair_r), visual.hair_r.load(), visual.hair_g.load(), visual.hair_b.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  proportion        %g, %g", l->offset + offsetof(PlayerVisualConfig, proportion_x), visual.proportion_x.load(), visual.proportion_y.load()));
      });
      print_as_struct.template operator()<Arg::DataType::PLAYER_STATS, PlayerStats>([&](const PlayerStats& stats) -> void {
        lines.emplace_back("  // As PlayerStats");
        lines.emplace_back(phosg::string_printf("  %04zX  atp               %04hX /* %hu */", l->offset + offsetof(PlayerStats, char_stats.atp), stats.char_stats.atp.load(), stats.char_stats.atp.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  mst               %04hX /* %hu */", l->offset + offsetof(PlayerStats, char_stats.mst), stats.char_stats.mst.load(), stats.char_stats.mst.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  evp               %04hX /* %hu */", l->offset + offsetof(PlayerStats, char_stats.evp), stats.char_stats.evp.load(), stats.char_stats.evp.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  hp                %04hX /* %hu */", l->offset + offsetof(PlayerStats, char_stats.hp), stats.char_stats.hp.load(), stats.char_stats.hp.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  dfp               %04hX /* %hu */", l->offset + offsetof(PlayerStats, char_stats.dfp), stats.char_stats.dfp.load(), stats.char_stats.dfp.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  ata               %04hX /* %hu */", l->offset + offsetof(PlayerStats, char_stats.ata), stats.char_stats.ata.load(), stats.char_stats.ata.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  lck               %04hX /* %hu */", l->offset + offsetof(PlayerStats, char_stats.lck), stats.char_stats.lck.load(), stats.char_stats.lck.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  esp               %04hX /* %hu */", l->offset + offsetof(PlayerStats, esp), stats.esp.load(), stats.esp.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  height            %08" PRIX32 " /* %g */", l->offset + offsetof(PlayerStats, height), stats.height.load_raw(), stats.height.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  a3                %08" PRIX32 " /* %g */", l->offset + offsetof(PlayerStats, unknown_a3), stats.unknown_a3.load_raw(), stats.unknown_a3.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  level             %08" PRIX32 " /* level %" PRIu32 " */", l->offset + offsetof(PlayerStats, level), stats.level.load(), stats.level.load() + 1));
        lines.emplace_back(phosg::string_printf("  %04zX  experience        %08" PRIX32 " /* %" PRIu32 " */", l->offset + offsetof(PlayerStats, experience), stats.experience.load(), stats.experience.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  meseta            %08" PRIX32 " /* %" PRIu32 " */", l->offset + offsetof(PlayerStats, meseta), stats.meseta.load(), stats.meseta.load()));
      });
      print_as_struct.template operator()<Arg::DataType::RESIST_DATA, ResistData>([&](const ResistData& resist) -> void {
        lines.emplace_back("  // As ResistData");
        lines.emplace_back(phosg::string_printf("  %04zX  evp_bonus         %04hX /* %hu */", l->offset + offsetof(ResistData, evp_bonus), resist.evp_bonus.load(), resist.evp_bonus.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  efr               %04hX /* %hu */", l->offset + offsetof(ResistData, efr), resist.efr.load(), resist.efr.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  eic               %04hX /* %hu */", l->offset + offsetof(ResistData, eic), resist.eic.load(), resist.eic.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  eth               %04hX /* %hu */", l->offset + offsetof(ResistData, eth), resist.eth.load(), resist.eth.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  elt               %04hX /* %hu */", l->offset + offsetof(ResistData, elt), resist.elt.load(), resist.elt.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  edk               %04hX /* %hu */", l->offset + offsetof(ResistData, edk), resist.edk.load(), resist.edk.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  a6                %08" PRIX32 " /* %" PRIu32 " */", l->offset + offsetof(ResistData, unknown_a6), resist.unknown_a6.load(), resist.unknown_a6.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  a7                %08" PRIX32 " /* %" PRIu32 " */", l->offset + offsetof(ResistData, unknown_a7), resist.unknown_a7.load(), resist.unknown_a7.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  a8                %08" PRIX32 " /* %" PRIu32 " */", l->offset + offsetof(ResistData, unknown_a8), resist.unknown_a8.load(), resist.unknown_a8.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  a9                %08" PRIX32 " /* %" PRIu32 " */", l->offset + offsetof(ResistData, unknown_a9), resist.unknown_a9.load(), resist.unknown_a9.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  dfp_bonus         %08" PRIX32 " /* %" PRIu32 " */", l->offset + offsetof(ResistData, dfp_bonus), resist.dfp_bonus.load(), resist.dfp_bonus.load()));
      });
      print_as_struct.template operator()<Arg::DataType::ATTACK_DATA, AttackData>([&](const AttackData& attack) -> void {
        lines.emplace_back("  // As AttackData");
        lines.emplace_back(phosg::string_printf("  %04zX  a1                %04hX /* %hd */", l->offset + offsetof(AttackData, unknown_a1), attack.unknown_a1.load(), attack.unknown_a1.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  atp               %04hX /* %hd */", l->offset + offsetof(AttackData, atp), attack.atp.load(), attack.atp.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  ata_bonus         %04hX /* %hd */", l->offset + offsetof(AttackData, ata_bonus), attack.ata_bonus.load(), attack.ata_bonus.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  a4                %04hX /* %hu */", l->offset + offsetof(AttackData, unknown_a4), attack.unknown_a4.load(), attack.unknown_a4.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  distance_x        %08" PRIX32 " /* %g */", l->offset + offsetof(AttackData, distance_x), attack.distance_x.load_raw(), attack.distance_x.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  angle_x           %08" PRIX32 " /* %" PRIu32 "/65536 */", l->offset + offsetof(AttackData, angle_x), attack.angle_x.load_raw(), attack.angle_x.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  distance_y        %08" PRIX32 " /* %g */", l->offset + offsetof(AttackData, distance_y), attack.distance_y.load_raw(), attack.distance_y.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  a8                %04hX /* %hu */", l->offset + offsetof(AttackData, unknown_a8), attack.unknown_a8.load(), attack.unknown_a8.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  a9                %04hX /* %hu */", l->offset + offsetof(AttackData, unknown_a9), attack.unknown_a9.load(), attack.unknown_a9.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  a10               %04hX /* %hu */", l->offset + offsetof(AttackData, unknown_a10), attack.unknown_a10.load(), attack.unknown_a10.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  a11               %04hX /* %hu */", l->offset + offsetof(AttackData, unknown_a11), attack.unknown_a11.load(), attack.unknown_a11.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  a12               %08" PRIX32 " /* %" PRIu32 " */", l->offset + offsetof(AttackData, unknown_a12), attack.unknown_a12.load(), attack.unknown_a12.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  a13               %08" PRIX32 " /* %" PRIu32 " */", l->offset + offsetof(AttackData, unknown_a13), attack.unknown_a13.load(), attack.unknown_a13.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  a14               %08" PRIX32 " /* %" PRIu32 " */", l->offset + offsetof(AttackData, unknown_a14), attack.unknown_a14.load(), attack.unknown_a14.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  a15               %08" PRIX32 " /* %" PRIu32 " */", l->offset + offsetof(AttackData, unknown_a15), attack.unknown_a15.load(), attack.unknown_a15.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  a16               %08" PRIX32 " /* %" PRIu32 " */", l->offset + offsetof(AttackData, unknown_a16), attack.unknown_a16.load(), attack.unknown_a16.load()));
      });
      print_as_struct.template operator()<Arg::DataType::MOVEMENT_DATA, MovementData>([&](const MovementData& movement) -> void {
        lines.emplace_back("  // As MovementData");
        lines.emplace_back(phosg::string_printf("  %04zX  idle_move_speed   %08" PRIX32 " /* %g */", l->offset + offsetof(MovementData, idle_move_speed), movement.idle_move_speed.load_raw(), movement.idle_move_speed.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  idle_anim_speed   %08" PRIX32 " /* %g */", l->offset + offsetof(MovementData, idle_animation_speed), movement.idle_animation_speed.load_raw(), movement.idle_animation_speed.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  move_speed        %08" PRIX32 " /* %g */", l->offset + offsetof(MovementData, move_speed), movement.move_speed.load_raw(), movement.move_speed.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  animation_speed   %08" PRIX32 " /* %g */", l->offset + offsetof(MovementData, animation_speed), movement.animation_speed.load_raw(), movement.animation_speed.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  a1                %08" PRIX32 " /* %g */", l->offset + offsetof(MovementData, unknown_a1), movement.unknown_a1.load_raw(), movement.unknown_a1.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  a2                %08" PRIX32 " /* %g */", l->offset + offsetof(MovementData, unknown_a2), movement.unknown_a2.load_raw(), movement.unknown_a2.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  a3                %08" PRIX32 " /* %" PRIu32 " */", l->offset + offsetof(MovementData, unknown_a3), movement.unknown_a3.load(), movement.unknown_a3.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  a4                %08" PRIX32 " /* %" PRIu32 " */", l->offset + offsetof(MovementData, unknown_a4), movement.unknown_a4.load(), movement.unknown_a4.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  a5                %08" PRIX32 " /* %" PRIu32 " */", l->offset + offsetof(MovementData, unknown_a5), movement.unknown_a5.load(), movement.unknown_a5.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  a6                %08" PRIX32 " /* %" PRIu32 " */", l->offset + offsetof(MovementData, unknown_a6), movement.unknown_a6.load(), movement.unknown_a6.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  a7                %08" PRIX32 " /* %" PRIu32 " */", l->offset + offsetof(MovementData, unknown_a7), movement.unknown_a7.load(), movement.unknown_a7.load()));
        lines.emplace_back(phosg::string_printf("  %04zX  a8                %08" PRIX32 " /* %" PRIu32 " */", l->offset + offsetof(MovementData, unknown_a8), movement.unknown_a8.load(), movement.unknown_a8.load()));
      });
      if (l->has_data_type(Arg::DataType::IMAGE_DATA)) {
        const void* data = cmd_r.pgetv(l->offset, size);
        auto decompressed = prs_decompress_with_meta(data, size);
        lines.emplace_back(phosg::string_printf("  // As decompressed image data (0x%zX bytes)", decompressed.data.size()));
        lines.emplace_back(format_and_indent_data(decompressed.data.data(), decompressed.data.size(), 0));
        if (decompressed.input_bytes_used < size) {
          size_t compressed_end_offset = l->offset + decompressed.input_bytes_used;
          size_t remaining_size = size - decompressed.input_bytes_used;
          lines.emplace_back("  // Extra data after compressed data");
          lines.emplace_back(format_and_indent_data(cmd_r.pgetv(compressed_end_offset, remaining_size), remaining_size, compressed_end_offset));
        }
      }
      if (l->has_data_type(Arg::DataType::UNKNOWN_F8F2_DATA)) {
        phosg::StringReader r = cmd_r.sub(l->offset, size);
        lines.emplace_back("  // As F8F2 entries");
        while (r.remaining() >= sizeof(UnknownF8F2Entry)) {
          size_t offset = l->offset + cmd_r.where();
          const auto& e = r.get<UnknownF8F2Entry>();
          lines.emplace_back(phosg::string_printf("  %04zX  entry        %g, %g, %g, %g", offset, e.unknown_a1[0].load(), e.unknown_a1[1].load(), e.unknown_a1[2].load(), e.unknown_a1[3].load()));
        }
        if (r.remaining() > 0) {
          size_t struct_end_offset = l->offset + r.where();
          size_t remaining_size = r.remaining();
          lines.emplace_back("  // Extra data after structures");
          lines.emplace_back(format_and_indent_data(r.getv(remaining_size), remaining_size, struct_end_offset));
        }
      }
      if (l->has_data_type(Arg::DataType::SCRIPT)) {
        add_disassembly_lines(l->offset, size);
      }
    }
  }

  lines.emplace_back(); // Add a \n on the end
  return phosg::join(lines, "\n");
}

Episode find_quest_episode_from_script(const void* data, size_t size, Version version) {
  phosg::StringReader r(data, size);

  bool use_wstrs = false;
  size_t code_offset = 0;
  size_t function_table_offset = 0;
  Episode header_episode = Episode::NONE;
  switch (version) {
    case Version::DC_NTE:
    case Version::DC_V1_11_2000_PROTOTYPE:
    case Version::DC_V1:
    case Version::DC_V2:
    case Version::PC_NTE:
    case Version::PC_V2:
      return Episode::EP1;
    case Version::GC_NTE:
    case Version::GC_V3:
    case Version::GC_EP3_NTE:
    case Version::GC_EP3:
    case Version::XB_V3: {
      const auto& header = r.get<PSOQuestHeaderGC>();
      code_offset = header.code_offset;
      function_table_offset = header.function_table_offset;
      header_episode = episode_for_quest_episode_number(header.episode);
      break;
    }
    case Version::BB_V4: {
      use_wstrs = true;
      const auto& header = r.get<PSOQuestHeaderBB>();
      code_offset = header.code_offset;
      function_table_offset = header.function_table_offset;
      header_episode = episode_for_quest_episode_number(header.episode);
      break;
    }
    default:
      throw logic_error("invalid quest version");
  }

  unordered_set<Episode> found_episodes;

  try {
    const auto& opcodes = opcodes_for_version(version);
    // The set_episode opcode should always be in the first function (0)
    phosg::StringReader cmd_r = r.sub(code_offset + r.pget_u32l(function_table_offset));

    while (!cmd_r.eof()) {
      uint16_t opcode = cmd_r.get_u8();
      if ((opcode & 0xFE) == 0xF8) {
        opcode = (opcode << 8) | cmd_r.get_u8();
      }

      const QuestScriptOpcodeDefinition* def = nullptr;
      try {
        def = opcodes.at(opcode);
      } catch (const out_of_range&) {
      }

      if (def == nullptr) {
        throw runtime_error(phosg::string_printf("unknown quest opcode %04hX", opcode));
      }

      if (def->flags & F_RET) {
        break;
      }

      if (!(def->flags & F_ARGS)) {
        for (const auto& arg : def->args) {
          using Type = QuestScriptOpcodeDefinition::Argument::Type;
          string dasm_arg;
          switch (arg.type) {
            case Type::LABEL16:
              cmd_r.skip(2);
              break;
            case Type::LABEL32:
              cmd_r.skip(4);
              break;
            case Type::LABEL16_SET:
              if (def->flags & F_PASS) {
                throw logic_error("LABEL16_SET cannot be pushed to arg stack");
              }
              cmd_r.skip(cmd_r.get_u8() * 2);
              break;
            case Type::REG:
              cmd_r.skip(1);
              break;
            case Type::REG_SET:
              if (def->flags & F_PASS) {
                throw logic_error("REG_SET cannot be pushed to arg stack");
              }
              cmd_r.skip(cmd_r.get_u8());
              break;
            case Type::REG_SET_FIXED:
              if (def->flags & F_PASS) {
                throw logic_error("REG_SET_FIXED cannot be pushed to arg stack");
              }
              cmd_r.skip(1);
              break;
            case Type::REG32_SET_FIXED:
              if (def->flags & F_PASS) {
                throw logic_error("REG32_SET_FIXED cannot be pushed to arg stack");
              }
              cmd_r.skip(4);
              break;
            case Type::INT8:
              cmd_r.skip(1);
              break;
            case Type::INT16:
              cmd_r.skip(2);
              break;
            case Type::INT32:
              if (def->flags & F_SET_EPISODE) {
                found_episodes.emplace(episode_for_quest_episode_number(cmd_r.get_u32l()));
              } else {
                cmd_r.skip(4);
              }
              break;
            case Type::FLOAT32:
              cmd_r.skip(4);
              break;
            case Type::CSTRING:
              if (use_wstrs) {
                for (uint16_t ch = cmd_r.get_u16l(); ch; ch = cmd_r.get_u16l()) {
                }
              } else {
                for (uint8_t ch = cmd_r.get_u8(); ch; ch = cmd_r.get_u8()) {
                }
              }
              break;
            default:
              throw logic_error("invalid argument type");
          }
        }
      }
    }
  } catch (const exception& e) {
    phosg::log_warning("Cannot determine episode from quest script (%s)", e.what());
  }

  if (found_episodes.size() > 1) {
    throw runtime_error("multiple episodes found");
  } else if (found_episodes.size() == 1) {
    return *found_episodes.begin();
  } else {
    return header_episode;
  }
}

Episode episode_for_quest_episode_number(uint8_t episode_number) {
  switch (episode_number) {
    case 0x00:
    case 0xFF:
      return Episode::EP1;
    case 0x01:
      return Episode::EP2;
    case 0x02:
      return Episode::EP4;
    default:
      throw runtime_error(phosg::string_printf("invalid episode number %02hhX", episode_number));
  }
}

struct RegisterAssigner {
  struct Register {
    string name;
    int16_t number = -1; // -1 = unassigned (any number)
    shared_ptr<Register> prev;
    shared_ptr<Register> next;
    unordered_set<size_t> offsets;

    std::string str() const {
      return phosg::string_printf("Register(%p, name=\"%s\", number=%hd)", this, this->name.c_str(), this->number);
    }
  };

  ~RegisterAssigner() {
    for (auto& it : this->named_regs) {
      it.second->prev.reset();
      it.second->next.reset();
    }
    for (auto& reg : this->numbered_regs) {
      if (reg) {
        reg->prev.reset();
        reg->next.reset();
      }
    }
  }

  shared_ptr<Register> get_or_create(const string& name, int16_t number) {
    if ((number < -1) || (number >= 0x100)) {
      throw runtime_error("invalid register number");
    }

    shared_ptr<Register> reg;
    if (!name.empty()) {
      try {
        reg = this->named_regs.at(name);
      } catch (const out_of_range&) {
      }
    }
    if (!reg && number >= 0) {
      reg = this->numbered_regs.at(number);
    }

    if (!reg) {
      reg = make_shared<Register>();
    }

    if (number >= 0) {
      if (reg->number < 0) {
        reg->number = number;
        auto& numbered_reg = this->numbered_regs.at(reg->number);
        if (numbered_reg) {
          throw runtime_error(reg->str() + " cannot be assigned due to conflict with " + numbered_reg->str());
        }
        this->numbered_regs.at(reg->number) = reg;
      } else if (reg->number != number) {
        throw runtime_error(phosg::string_printf("register %s is assigned multiple numbers", reg->name.c_str()));
      }
    }

    if (!name.empty()) {
      if (reg->name.empty()) {
        reg->name = name;
        if (!this->named_regs.emplace(reg->name, reg).second) {
          throw runtime_error(phosg::string_printf("name %s is already assigned to a different register", reg->name.c_str()));
        }
      } else if (reg->name != name) {
        throw runtime_error(phosg::string_printf("register %hd is assigned multiple names", reg->number));
      }
    }

    return reg;
  }

  void assign_number(shared_ptr<Register> reg, uint8_t number) {
    if (reg->number < 0) {
      reg->number = number;
      if (this->numbered_regs.at(reg->number)) {
        throw logic_error(phosg::string_printf("register number %hd assigned multiple times", reg->number));
      }
      this->numbered_regs.at(reg->number) = reg;
    } else if (reg->number != static_cast<int16_t>(number)) {
      throw runtime_error(phosg::string_printf("assigning different register number %hhu over existing register number %hd", number, reg->number));
    }
  }

  void constrain(shared_ptr<Register> first_reg, shared_ptr<Register> second_reg) {
    if (!first_reg->next) {
      first_reg->next = second_reg;
    } else if (first_reg->next != second_reg) {
      throw runtime_error(phosg::string_printf("register %s must come after %s, but is already constrained to another register", second_reg->name.c_str(), first_reg->name.c_str()));
    }
    if (!second_reg->prev) {
      second_reg->prev = first_reg;
    } else if (second_reg->prev != first_reg) {
      throw runtime_error(phosg::string_printf("register %s must come before %s, but is already constrained to another register", first_reg->name.c_str(), second_reg->name.c_str()));
    }
    if ((first_reg->number >= 0) && (second_reg->number >= 0) && (first_reg->number != ((second_reg->number - 1) & 0xFF))) {
      throw runtime_error(phosg::string_printf("register %s must come before %s, but both registers already have non-consecutive numbers", first_reg->name.c_str(), second_reg->name.c_str()));
    }
  }

  void assign_all() {
    // TODO: Technically, we should assign the biggest blocks first to minimize
    // fragmentation. I am lazy and haven't implemented this yet.
    vector<shared_ptr<Register>> unassigned;
    for (auto it : this->named_regs) {
      if (it.second->number < 0) {
        unassigned.emplace_back(it.second);
      }
    }

    for (auto reg : unassigned) {
      // If this register is already assigned, skip it
      if (reg->number >= 0) {
        continue;
      }

      // If any next register is assigned, assign this register
      size_t next_delta = 1;
      for (auto next_reg = reg->next; next_reg; next_reg = next_reg->next, next_delta++) {
        if (next_reg->number >= 0) {
          this->assign_number(reg, (next_reg->number - next_delta) & 0xFF);
          break;
        }
      }
      if (reg->number >= 0) {
        continue;
      }

      // If any prev register is assigned, assign this register
      size_t prev_delta = 1;
      for (auto prev_reg = reg->prev; prev_reg; prev_reg = prev_reg->prev, prev_delta++) {
        if (prev_reg->number >= 0) {
          this->assign_number(reg, (prev_reg->number + prev_delta) & 0xFF);
          break;
        }
      }
      if (reg->number >= 0) {
        continue;
      }

      // No prev or next register is assigned; find an interval in the register
      // number space that fits this block of registers. The total number of
      // register numbers needed is (prev_delta - 1) + (next_delta - 1) + 1.
      size_t num_regs = prev_delta + next_delta - 1;
      this->assign_number(reg, (this->find_register_number_space(num_regs) + (prev_delta - 1)) & 0xFF);

      // We don't need to assign the prev and next registers; they should also
      // be in the unassigned set and will be assigned by the above logic
    }

    // At this point, all registers should be assigned
    for (const auto& it : this->named_regs) {
      if (it.second->number < 0) {
        throw logic_error(phosg::string_printf("register %s was not assigned", it.second->name.c_str()));
      }
    }
    for (size_t z = 0; z < 0x100; z++) {
      auto reg = this->numbered_regs[z];
      if (reg && (reg->number != static_cast<int16_t>(z))) {
        throw logic_error(phosg::string_printf("register %zu has incorrect number %hd", z, reg->number));
      }
    }
  }

  uint8_t find_register_number_space(size_t num_regs) const {
    for (size_t candidate = 0; candidate < 0x100; candidate++) {
      size_t z;
      for (z = 0; z < num_regs; z++) {
        if (this->numbered_regs[candidate + z]) {
          break;
        }
      }
      if (z == num_regs) {
        return candidate;
      }
    }
    throw runtime_error("not enough space to assign registers");
  }

  map<string, shared_ptr<Register>> named_regs;
  array<shared_ptr<Register>, 0x100> numbered_regs;
};

std::string assemble_quest_script(const std::string& text, const std::string& include_directory) {
  auto lines = phosg::split(text, '\n');

  // Strip comments and whitespace
  for (auto& line : lines) {
    size_t comment_start = line.find("/*");
    while (comment_start != string::npos) {
      size_t comment_end = line.find("*/", comment_start + 2);
      if (comment_end == string::npos) {
        throw runtime_error("unterminated inline comment");
      }
      line.erase(comment_start, comment_end + 2 - comment_start);
      comment_start = line.find("/*");
    }
    comment_start = line.find("//");
    if (comment_start != string::npos) {
      line.resize(comment_start);
    }
    phosg::strip_trailing_whitespace(line);
    phosg::strip_leading_whitespace(line);
  }

  // Collect metadata directives
  Version quest_version = Version::UNKNOWN;
  string quest_name;
  string quest_short_desc;
  string quest_long_desc;
  int64_t quest_num = -1;
  uint8_t quest_language = 1;
  Episode quest_episode = Episode::EP1;
  uint8_t quest_max_players = 4;
  bool quest_joinable = false;
  for (const auto& line : lines) {
    if (line.empty()) {
      continue;
    }
    if (line[0] == '.') {
      if (phosg::starts_with(line, ".version ")) {
        string name = line.substr(9);
        quest_version = phosg::enum_for_name<Version>(name.c_str());
      } else if (phosg::starts_with(line, ".name ")) {
        quest_name = phosg::parse_data_string(line.substr(6));
      } else if (phosg::starts_with(line, ".short_desc ")) {
        quest_short_desc = phosg::parse_data_string(line.substr(12));
      } else if (phosg::starts_with(line, ".long_desc ")) {
        quest_long_desc = phosg::parse_data_string(line.substr(11));
      } else if (phosg::starts_with(line, ".quest_num ")) {
        quest_num = stoul(line.substr(11), nullptr, 0);
      } else if (phosg::starts_with(line, ".language ")) {
        quest_language = stoul(line.substr(10), nullptr, 0);
      } else if (phosg::starts_with(line, ".episode ")) {
        quest_episode = episode_for_token_name(line.substr(9));
      } else if (phosg::starts_with(line, ".max_players ")) {
        quest_max_players = stoul(line.substr(12), nullptr, 0);
      } else if (phosg::starts_with(line, ".joinable ")) {
        quest_joinable = true;
      }
    }
  }
  if (quest_version == Version::PC_PATCH || quest_version == Version::BB_PATCH || quest_version == Version::UNKNOWN) {
    throw runtime_error(".version directive is missing or invalid");
  }
  if (quest_num < 0) {
    throw runtime_error(".quest_num directive is missing or invalid");
  }
  if (quest_name.empty()) {
    throw runtime_error(".name directive is missing or invalid");
  }

  // Find all label names
  struct Label {
    std::string name;
    ssize_t index = -1;
    ssize_t offset = -1;
  };
  map<string, shared_ptr<Label>> labels_by_name;
  map<ssize_t, shared_ptr<Label>> labels_by_index;
  for (size_t line_num = 1; line_num <= lines.size(); line_num++) {
    const auto& line = lines[line_num - 1];
    if (phosg::ends_with(line, ":")) {
      auto label = make_shared<Label>();
      label->name = line.substr(0, line.size() - 1);
      size_t at_offset = label->name.find('@');
      if (at_offset != string::npos) {
        try {
          label->index = stoul(label->name.substr(at_offset + 1), nullptr, 0);
        } catch (const exception& e) {
          throw runtime_error(phosg::string_printf("(line %zu) invalid index in label (%s)", line_num, e.what()));
        }
        label->name.resize(at_offset);
        if (label->name == "start" && label->index != 0) {
          throw runtime_error("start label cannot have a nonzero label ID");
        }
      } else if (label->name == "start") {
        label->index = 0;
      }
      if (!labels_by_name.emplace(label->name, label).second) {
        throw runtime_error(phosg::string_printf("(line %zu) duplicate label name: %s", line_num, label->name.c_str()));
      }
      if (label->index >= 0) {
        auto index_emplace_ret = labels_by_index.emplace(label->index, label);
        if (label->index >= 0 && !index_emplace_ret.second) {
          throw runtime_error(phosg::string_printf("(line %zu) duplicate label index: %zd (0x%zX) from %s and %s", line_num, label->index, label->index, label->name.c_str(), index_emplace_ret.first->second->name.c_str()));
        }
      }
    }
  }
  if (!labels_by_name.count("start")) {
    throw runtime_error("start label is not defined");
  }

  // Assign indexes to labels without explicit indexes
  {
    size_t next_index = 0;
    for (auto& it : labels_by_name) {
      if (it.second->index >= 0) {
        continue;
      }
      while (labels_by_index.count(next_index)) {
        next_index++;
      }
      it.second->index = next_index++;
      labels_by_index.emplace(it.second->index, it.second);
    }
  }

  // Prepare to collect named registers
  RegisterAssigner reg_assigner;
  auto parse_reg = [&reg_assigner](const string& arg, bool allow_unnumbered = true) -> shared_ptr<RegisterAssigner::Register> {
    if (arg.size() < 2) {
      throw runtime_error("register argument is too short");
    }
    if ((arg[0] != 'r') && (arg[0] != 'f')) {
      throw runtime_error("a register is required");
    }
    string name;
    ssize_t number = -1;
    if (arg[1] == ':') {
      auto tokens = phosg::split(arg.substr(2), '@');
      if (tokens.size() == 1) {
        name = std::move(tokens[0]);
      } else if (tokens.size() == 2) {
        name = std::move(tokens[0]);
        number = stoull(tokens[1], nullptr, 0);
      } else {
        throw runtime_error("invalid register specification");
      }
    } else {
      number = stoull(arg.substr(1), nullptr, 0);
    }
    if (!allow_unnumbered && (number < 0)) {
      throw runtime_error("a numbered register is required");
    }
    if (number > 0xFF) {
      throw runtime_error("invalid register number");
    }
    return reg_assigner.get_or_create(name, number);
  };
  auto parse_reg_set_fixed = [&reg_assigner, &parse_reg](const string& name, size_t expected_count) -> vector<shared_ptr<RegisterAssigner::Register>> {
    if (expected_count == 0) {
      throw logic_error("REG_SET_FIXED argument expects no registers");
    }
    if (name.empty()) {
      throw runtime_error("no register specified for REG_SET_FIXED argument");
    }
    vector<shared_ptr<RegisterAssigner::Register>> regs;
    if ((name[0] == '(') && (name.back() == ')')) {
      auto tokens = phosg::split(name.substr(1, name.size() - 2), ',');
      if (tokens.size() != expected_count) {
        throw runtime_error("incorrect number of registers in REG_SET_FIXED");
      }
      for (auto& token : tokens) {
        phosg::strip_trailing_whitespace(token);
        phosg::strip_leading_whitespace(token);
        regs.emplace_back(parse_reg(token));
        if (regs.size() > 1) {
          reg_assigner.constrain(regs.at(regs.size() - 2), regs.back());
        }
      }
    } else {
      auto tokens = phosg::split(name, '-');
      if (tokens.size() == 1) {
        regs.emplace_back(parse_reg(tokens[0], false));
        while (regs.size() < expected_count) {
          regs.emplace_back(parse_reg("", (regs.back()->number + 1) & 0xFF));
          reg_assigner.constrain(regs.at(regs.size() - 2), regs.back());
        }
      } else if (tokens.size() == 2) {
        regs.emplace_back(parse_reg(tokens[0], false));
        while (regs.size() < expected_count - 1) {
          regs.emplace_back(reg_assigner.get_or_create("", (regs.back()->number + 1) & 0xFF));
          reg_assigner.constrain(regs.at(regs.size() - 2), regs.back());
        }
        regs.emplace_back(parse_reg(tokens[1], false));
        if (static_cast<size_t>(regs.back()->number - regs.front()->number + 1) != expected_count) {
          throw runtime_error("incorrect number of registers used");
        }
        reg_assigner.constrain(regs.at(regs.size() - 2), regs.back());
      } else {
        throw runtime_error("invalid fixed register set syntax");
      }
    }
    if (regs.empty() || regs.size() != expected_count) {
      throw logic_error("incorrect register count in REG_SET_FIXED after parsing");
    }
    return regs;
  };

  // Assemble code segment
  bool version_has_args = F_HAS_ARGS & v_flag(quest_version);
  const auto& opcodes = opcodes_by_name_for_version(quest_version);
  phosg::StringWriter code_w;
  for (size_t line_num = 1; line_num <= lines.size(); line_num++) {
    try {
      const auto& line = lines[line_num - 1];
      if (line.empty()) {
        continue;
      }

      if (phosg::ends_with(line, ":")) {
        size_t at_offset = line.find('@');
        string label_name = line.substr(0, (at_offset == string::npos) ? (line.size() - 1) : at_offset);
        labels_by_name.at(label_name)->offset = code_w.size();
        continue;
      }

      if (line[0] == '.') {
        if (phosg::starts_with(line, ".data ")) {
          code_w.write(phosg::parse_data_string(line.substr(6)));
        } else if (phosg::starts_with(line, ".zero ")) {
          size_t size = stoull(line.substr(6), nullptr, 0);
          code_w.extend_by(size, 0x00);
        } else if (phosg::starts_with(line, ".zero_until ")) {
          size_t size = stoull(line.substr(12), nullptr, 0);
          code_w.extend_to(size, 0x00);
        } else if (phosg::starts_with(line, ".align ")) {
          size_t alignment = stoull(line.substr(7), nullptr, 0);
          while (code_w.size() % alignment) {
            code_w.put_u8(0);
          }
        } else if (phosg::starts_with(line, ".include_bin ")) {
          string filename = line.substr(13);
          phosg::strip_whitespace(filename);
          code_w.write(phosg::load_file(include_directory + "/" + filename));
        } else if (phosg::starts_with(line, ".include_native ")) {
#ifdef HAVE_RESOURCE_FILE
          string filename = line.substr(16);
          phosg::strip_whitespace(filename);
          string native_text = phosg::load_file(include_directory + "/" + filename);
          string code;
          if (is_ppc(quest_version)) {
            code = std::move(ResourceDASM::PPC32Emulator::assemble(native_text).code);
          } else if (is_x86(quest_version)) {
            code = std::move(ResourceDASM::X86Emulator::assemble(native_text).code);
          } else if (is_sh4(quest_version)) {
            code = std::move(ResourceDASM::SH4Emulator::assemble(native_text).code);
          } else {
            throw runtime_error("unknown architecture");
          }
          code_w.write(code);
#else
          throw runtime_error("native code cannot be compiled; rebuild newserv with libresource_file");
#endif
        }
        continue;
      }

      auto line_tokens = phosg::split(line, ' ', 1);
      const auto& opcode_def = opcodes.at(line_tokens.at(0));

      bool use_args = version_has_args && (opcode_def->flags & F_ARGS);
      if (!use_args) {
        if ((opcode_def->opcode & 0xFF00) == 0x0000) {
          code_w.put_u8(opcode_def->opcode);
        } else {
          code_w.put_u16b(opcode_def->opcode);
        }
      }

      if (opcode_def->args.empty()) {
        if (line_tokens.size() > 1) {
          throw runtime_error(phosg::string_printf("(line %zu) arguments not allowed for %s", line_num, opcode_def->name));
        }
        continue;
      }

      if (line_tokens.size() < 2) {
        throw runtime_error(phosg::string_printf("(line %zu) arguments required for %s", line_num, opcode_def->name));
      }
      phosg::strip_trailing_whitespace(line_tokens[1]);
      phosg::strip_leading_whitespace(line_tokens[1]);

      if (phosg::starts_with(line_tokens[1], "...")) {
        if (!use_args) {
          throw runtime_error(phosg::string_printf("(line %zu) \'...\' can only be used with F_ARGS opcodes", line_num));
        }

      } else { // Not "..."
        auto args = phosg::split_context(line_tokens[1], ',');
        if (args.size() != opcode_def->args.size()) {
          throw runtime_error(phosg::string_printf("(line %zu) incorrect argument count for %s", line_num, opcode_def->name));
        }

        for (size_t z = 0; z < args.size(); z++) {
          using Type = QuestScriptOpcodeDefinition::Argument::Type;

          string& arg = args[z];
          const auto& arg_def = opcode_def->args[z];
          phosg::strip_trailing_whitespace(arg);
          phosg::strip_leading_whitespace(arg);

          try {
            auto add_cstr = [&](const string& text, bool bin) -> void {
              switch (quest_version) {
                case Version::DC_NTE:
                  code_w.write(bin ? text : tt_utf8_to_sega_sjis(text));
                  code_w.put_u8(0);
                  break;
                case Version::DC_V1_11_2000_PROTOTYPE:
                case Version::DC_V1:
                case Version::DC_V2:
                case Version::GC_NTE:
                case Version::GC_V3:
                case Version::GC_EP3_NTE:
                case Version::GC_EP3:
                case Version::XB_V3:
                  code_w.write(bin ? text : (quest_language ? tt_utf8_to_8859(text) : tt_utf8_to_sega_sjis(text)));
                  code_w.put_u8(0);
                  break;
                case Version::PC_NTE:
                case Version::PC_V2:
                case Version::BB_V4:
                  code_w.write(bin ? text : tt_utf8_to_utf16(text));
                  code_w.put_u16(0);
                  break;
                default:
                  throw logic_error("invalid game version");
              }
            };

            if (use_args) {
              auto label_it = labels_by_name.find(arg);
              if (arg.empty()) {
                throw runtime_error("argument is empty");
              } else if (label_it != labels_by_name.end()) {
                code_w.put_u8(0x4B); // arg_pushw
                code_w.put_u16l(label_it->second->index);
              } else if ((arg[0] == 'r') || (arg[0] == 'f') || ((arg[0] == '(') && (arg.back() == ')'))) {
                // If the corresponding argument is a REG or REG_SET_FIXED, push
                // the register number, not the register's value, since it's an
                // out-param
                if ((arg_def.type == Type::REG) || (arg_def.type == Type::REG32)) {
                  code_w.put_u8(0x4A); // arg_pushb
                  auto reg = parse_reg(arg);
                  reg->offsets.emplace(code_w.size());
                  code_w.put_u8(reg->number);
                } else if (
                    (arg_def.type == Type::REG_SET_FIXED) ||
                    (arg_def.type == Type::REG32_SET_FIXED)) {
                  auto regs = parse_reg_set_fixed(arg, arg_def.count);
                  code_w.put_u8(0x4A); // arg_pushb
                  regs[0]->offsets.emplace(code_w.size());
                  code_w.put_u8(regs[0]->number);
                } else {
                  code_w.put_u8(0x48); // arg_pushr
                  auto reg = parse_reg(arg);
                  reg->offsets.emplace(code_w.size());
                  code_w.put_u8(reg->number);
                }
              } else if ((arg[0] == '@') && ((arg[1] == 'r') || (arg[1] == 'f'))) {
                code_w.put_u8(0x4C); // arg_pusha
                auto reg = parse_reg(arg.substr(1));
                reg->offsets.emplace(code_w.size());
                code_w.put_u8(reg->number);
              } else if ((arg[0] == '@') && labels_by_name.count(arg.substr(1))) {
                code_w.put_u8(0x4D); // arg_pusho
                code_w.put_u16(labels_by_name.at(arg.substr(1))->index);
              } else {
                bool write_as_str = false;
                try {
                  size_t end_offset;
                  uint64_t value = stoll(arg, &end_offset, 0);
                  if (end_offset != arg.size()) {
                    write_as_str = true;
                  } else if (value > 0xFFFF) {
                    code_w.put_u8(0x49); // arg_pushl
                    code_w.put_u32l(value);
                  } else if (value > 0xFF) {
                    code_w.put_u8(0x4B); // arg_pushw
                    code_w.put_u16l(value);
                  } else {
                    code_w.put_u8(0x4A); // arg_pushb
                    code_w.put_u8(value);
                  }
                } catch (const exception&) {
                  write_as_str = true;
                }
                if (write_as_str) {
                  if (arg[0] == '\"') {
                    code_w.put_u8(0x4E); // arg_pushs
                    if (phosg::starts_with(arg, "bin:")) {
                      add_cstr(phosg::parse_data_string(arg.substr(4)), true);
                    } else {
                      add_cstr(phosg::parse_data_string(arg), false);
                    }
                  } else {
                    throw runtime_error("invalid argument syntax");
                  }
                }
              }

            } else { // Not use_args
              auto add_label = [&](const string& name, bool is32) -> void {
                if (!labels_by_name.count(name)) {
                  throw runtime_error("label not defined: " + name);
                }
                if (is32) {
                  code_w.put_u32(labels_by_name.at(name)->index);
                } else {
                  code_w.put_u16(labels_by_name.at(name)->index);
                }
              };
              auto add_reg = [&](shared_ptr<RegisterAssigner::Register> reg, bool is32) -> void {
                reg->offsets.emplace(code_w.size());
                if (is32) {
                  code_w.put_u32l(reg->number & 0xFF);
                } else {
                  code_w.put_u8(reg->number);
                }
              };

              auto split_set = [&](const string& text) -> vector<string> {
                if (!phosg::starts_with(text, "[") || !phosg::ends_with(text, "]")) {
                  throw runtime_error("incorrect syntax for set-valued argument");
                }
                auto values = phosg::split(text.substr(1, text.size() - 2), ',');
                if (values.size() > 0xFF) {
                  throw runtime_error("too many labels in set-valued argument");
                }
                return values;
              };

              switch (arg_def.type) {
                case Type::LABEL16:
                case Type::LABEL32:
                  add_label(arg, arg_def.type == Type::LABEL32);
                  break;
                case Type::LABEL16_SET: {
                  auto label_names = split_set(arg);
                  code_w.put_u8(label_names.size());
                  for (auto name : label_names) {
                    phosg::strip_trailing_whitespace(name);
                    phosg::strip_leading_whitespace(name);
                    add_label(name, false);
                  }
                  break;
                }
                case Type::REG:
                case Type::REG32:
                  add_reg(parse_reg(arg), arg_def.type == Type::REG32);
                  break;
                case Type::REG_SET_FIXED:
                case Type::REG32_SET_FIXED: {
                  auto regs = parse_reg_set_fixed(arg, arg_def.count);
                  add_reg(regs[0], arg_def.type == Type::REG32_SET_FIXED);
                  break;
                }
                case Type::REG_SET: {
                  auto regs = split_set(arg);
                  code_w.put_u8(regs.size());
                  for (auto reg_arg : regs) {
                    phosg::strip_trailing_whitespace(reg_arg);
                    phosg::strip_leading_whitespace(reg_arg);
                    add_reg(parse_reg(reg_arg), false);
                  }
                  break;
                }
                case Type::INT8:
                  code_w.put_u8(stol(arg, nullptr, 0));
                  break;
                case Type::INT16:
                  code_w.put_u16l(stol(arg, nullptr, 0));
                  break;
                case Type::INT32:
                  code_w.put_u32l(stol(arg, nullptr, 0));
                  break;
                case Type::FLOAT32:
                  code_w.put_u32l(stof(arg, nullptr));
                  break;
                case Type::CSTRING:
                  if (phosg::starts_with(arg, "bin:")) {
                    add_cstr(phosg::parse_data_string(arg.substr(4)), true);
                  } else {
                    add_cstr(phosg::parse_data_string(arg), false);
                  }
                  break;
                default:
                  throw logic_error("unknown argument type");
              }
            }
          } catch (const exception& e) {
            throw runtime_error(phosg::string_printf("(arg %zu) %s", z + 1, e.what()));
          }
        }
      }

      if (use_args) {
        if ((opcode_def->opcode & 0xFF00) == 0x0000) {
          code_w.put_u8(opcode_def->opcode);
        } else {
          code_w.put_u16b(opcode_def->opcode);
        }
      }

    } catch (const exception& e) {
      throw runtime_error(phosg::string_printf("(line %zu) %s", line_num, e.what()));
    }
  }
  while (code_w.size() & 3) {
    code_w.put_u8(0);
  }

  // Assign all register numbers and patch the code section if needed
  reg_assigner.assign_all();
  for (size_t z = 0; z < 0x100; z++) {
    auto reg = reg_assigner.numbered_regs[z];
    if (!reg) {
      continue;
    }
    for (size_t offset : reg->offsets) {
      code_w.pput_u8(offset, reg->number);
    }
  }

  // Generate function table
  ssize_t function_table_size = labels_by_index.rbegin()->first + 1;
  vector<le_uint32_t> function_table;
  function_table.reserve(function_table_size);
  {
    auto it = labels_by_index.begin();
    for (ssize_t z = 0; z < function_table_size; z++) {
      if (it == labels_by_index.end()) {
        throw logic_error("function table size exceeds maximum function ID");
      } else if (it->first > z) {
        function_table.emplace_back(0xFFFFFFFF);
      } else if (it->first == z) {
        if (it->second->offset < 0) {
          throw runtime_error("label " + it->second->name + " does not have a valid offset");
        }
        function_table.emplace_back(it->second->offset);
        it++;
      } else if (it->first < z) {
        throw logic_error("missed label " + it->second->name + " when compiling function table");
      }
    }
  }

  // Generate header
  phosg::StringWriter w;
  switch (quest_version) {
    case Version::DC_NTE: {
      PSOQuestHeaderDCNTE header;
      header.code_offset = sizeof(header);
      header.function_table_offset = sizeof(header) + code_w.size();
      header.size = header.function_table_offset + function_table.size() * sizeof(function_table[0]);
      header.unused = 0;
      header.name.encode(quest_name, 0);
      w.put(header);
      break;
    }
    case Version::DC_V1_11_2000_PROTOTYPE:
    case Version::DC_V1:
    case Version::DC_V2: {
      PSOQuestHeaderDC header;
      header.code_offset = sizeof(header);
      header.function_table_offset = sizeof(header) + code_w.size();
      header.size = header.function_table_offset + function_table.size() * sizeof(function_table[0]);
      header.unused = 0;
      header.language = quest_language;
      header.unknown1 = 0;
      header.quest_number = quest_num;
      header.name.encode(quest_name, quest_language);
      header.short_description.encode(quest_short_desc, quest_language);
      header.long_description.encode(quest_long_desc, quest_language);
      w.put(header);
      break;
    }
    case Version::PC_NTE:
    case Version::PC_V2: {
      PSOQuestHeaderPC header;
      header.code_offset = sizeof(header);
      header.function_table_offset = sizeof(header) + code_w.size();
      header.size = header.function_table_offset + function_table.size() * sizeof(function_table[0]);
      header.unused = 0;
      header.language = quest_language;
      header.unknown1 = 0;
      header.quest_number = quest_num;
      header.name.encode(quest_name, quest_language);
      header.short_description.encode(quest_short_desc, quest_language);
      header.long_description.encode(quest_long_desc, quest_language);
      w.put(header);
      break;
    }
    case Version::GC_NTE:
    case Version::GC_V3:
    case Version::GC_EP3_NTE:
    case Version::GC_EP3:
    case Version::XB_V3: {
      PSOQuestHeaderGC header;
      header.code_offset = sizeof(header);
      header.function_table_offset = sizeof(header) + code_w.size();
      header.size = header.function_table_offset + function_table.size() * sizeof(function_table[0]);
      header.unused = 0;
      header.language = quest_language;
      header.unknown1 = 0;
      header.quest_number = quest_num;
      header.episode = (quest_episode == Episode::EP2) ? 1 : 0;
      header.name.encode(quest_name, quest_language);
      header.short_description.encode(quest_short_desc, quest_language);
      header.long_description.encode(quest_long_desc, quest_language);
      w.put(header);
      break;
    }
    case Version::BB_V4: {
      PSOQuestHeaderBB header;
      header.code_offset = sizeof(header);
      header.function_table_offset = sizeof(header) + code_w.size();
      header.size = header.function_table_offset + function_table.size() * sizeof(function_table[0]);
      header.unused = 0;
      header.quest_number = quest_num;
      header.unused2 = 0;
      if (quest_episode == Episode::EP4) {
        header.episode = 2;
      } else if (quest_episode == Episode::EP2) {
        header.episode = 1;
      } else {
        header.episode = 0;
      }
      header.max_players = quest_max_players;
      header.joinable = quest_joinable ? 1 : 0;
      header.unknown = 0;
      header.name.encode(quest_name, quest_language);
      header.short_description.encode(quest_short_desc, quest_language);
      header.long_description.encode(quest_long_desc, quest_language);
      w.put(header);
      break;
    }
    default:
      throw logic_error("invalid quest version");
  }
  w.write(code_w.str());
  w.write(function_table.data(), function_table.size() * sizeof(function_table[0]));
  return std::move(w.str());
}
