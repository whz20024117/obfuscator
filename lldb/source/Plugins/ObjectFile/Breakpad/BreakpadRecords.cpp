//===-- BreakpadRecords.cpp -----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Plugins/ObjectFile/Breakpad/BreakpadRecords.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FormatVariadic.h"

using namespace lldb_private;
using namespace lldb_private::breakpad;

namespace {
enum class Token {
  Unknown,
  Module,
  Info,
  CodeID,
  File,
  Func,
  Public,
  Stack,
  CFI,
  Init,
  Win,
};
}

template<typename T>
static T stringTo(llvm::StringRef Str);

template <> Token stringTo<Token>(llvm::StringRef Str) {
  return llvm::StringSwitch<Token>(Str)
      .Case("MODULE", Token::Module)
      .Case("INFO", Token::Info)
      .Case("CODE_ID", Token::CodeID)
      .Case("FILE", Token::File)
      .Case("FUNC", Token::Func)
      .Case("PUBLIC", Token::Public)
      .Case("STACK", Token::Stack)
      .Case("CFI", Token::CFI)
      .Case("INIT", Token::Init)
      .Case("WIN", Token::Win)
      .Default(Token::Unknown);
}

template <>
llvm::Triple::OSType stringTo<llvm::Triple::OSType>(llvm::StringRef Str) {
  using llvm::Triple;
  return llvm::StringSwitch<Triple::OSType>(Str)
      .Case("Linux", Triple::Linux)
      .Case("mac", Triple::MacOSX)
      .Case("windows", Triple::Win32)
      .Default(Triple::UnknownOS);
}

template <>
llvm::Triple::ArchType stringTo<llvm::Triple::ArchType>(llvm::StringRef Str) {
  using llvm::Triple;
  return llvm::StringSwitch<Triple::ArchType>(Str)
      .Case("arm", Triple::arm)
      .Cases("arm64", "arm64e", Triple::aarch64)
      .Case("mips", Triple::mips)
      .Case("ppc", Triple::ppc)
      .Case("ppc64", Triple::ppc64)
      .Case("s390", Triple::systemz)
      .Case("sparc", Triple::sparc)
      .Case("sparcv9", Triple::sparcv9)
      .Case("x86", Triple::x86)
      .Cases("x86_64", "x86_64h", Triple::x86_64)
      .Default(Triple::UnknownArch);
}

template<typename T>
static T consume(llvm::StringRef &Str) {
  llvm::StringRef Token;
  std::tie(Token, Str) = getToken(Str);
  return stringTo<T>(Token);
}

/// Return the number of hex digits needed to encode an (POD) object of a given
/// type.
template <typename T> static constexpr size_t hex_digits() {
  return 2 * sizeof(T);
}

static UUID parseModuleId(llvm::Triple::OSType os, llvm::StringRef str) {
  struct data_t {
    using uuid_t = uint8_t[16];
    uuid_t uuid;
    llvm::support::ubig32_t age;
  } data;
  static_assert(sizeof(data) == 20, "");
  // The textual module id encoding should be between 33 and 40 bytes long,
  // depending on the size of the age field, which is of variable length.
  // The first three chunks of the id are encoded in big endian, so we need to
  // byte-swap those.
  if (str.size() <= hex_digits<data_t::uuid_t>() ||
      str.size() > hex_digits<data_t>())
    return UUID();
  if (!all_of(str, llvm::isHexDigit))
    return UUID();

  llvm::StringRef uuid_str = str.take_front(hex_digits<data_t::uuid_t>());
  llvm::StringRef age_str = str.drop_front(hex_digits<data_t::uuid_t>());

  llvm::copy(fromHex(uuid_str), data.uuid);
  uint32_t age;
  bool success = to_integer(age_str, age, 16);
  assert(success);
  (void)success;
  data.age = age;

  // On non-windows, the age field should always be zero, so we don't include to
  // match the native uuid format of these platforms.
  return UUID::fromData(&data, os == llvm::Triple::Win32 ? sizeof(data)
                                                         : sizeof(data.uuid));
}

llvm::Optional<Record::Kind> Record::classify(llvm::StringRef Line) {
  Token Tok = consume<Token>(Line);
  switch (Tok) {
  case Token::Module:
    return Record::Module;
  case Token::Info:
    return Record::Info;
  case Token::File:
    return Record::File;
  case Token::Func:
    return Record::Func;
  case Token::Public:
    return Record::Public;
  case Token::Stack:
    Tok = consume<Token>(Line);
    switch (Tok) {
    case Token::CFI:
      return Record::StackCFI;
    case Token::Win:
      return Record::StackWin;
    default:
      return llvm::None;
    }

  case Token::Unknown:
    // Optimistically assume that any unrecognised token means this is a line
    // record, those don't have a special keyword and start directly with a
    // hex number.
    return Record::Line;

  case Token::CodeID:
  case Token::CFI:
  case Token::Init:
  case Token::Win:
    // These should never appear at the start of a valid record.
    return llvm::None;
  }
  llvm_unreachable("Fully covered switch above!");
}

llvm::Optional<ModuleRecord> ModuleRecord::parse(llvm::StringRef Line) {
  // MODULE Linux x86_64 E5894855C35DCCCCCCCCCCCCCCCCCCCC0 a.out
  if (consume<Token>(Line) != Token::Module)
    return llvm::None;

  llvm::Triple::OSType OS = consume<llvm::Triple::OSType>(Line);
  if (OS == llvm::Triple::UnknownOS)
    return llvm::None;

  llvm::Triple::ArchType Arch = consume<llvm::Triple::ArchType>(Line);
  if (Arch == llvm::Triple::UnknownArch)
    return llvm::None;

  llvm::StringRef Str;
  std::tie(Str, Line) = getToken(Line);
  UUID ID = parseModuleId(OS, Str);
  if (!ID)
    return llvm::None;

  return ModuleRecord(OS, Arch, std::move(ID));
}

llvm::raw_ostream &breakpad::operator<<(llvm::raw_ostream &OS,
                                        const ModuleRecord &R) {
  return OS << "MODULE " << llvm::Triple::getOSTypeName(R.OS) << " "
            << llvm::Triple::getArchTypeName(R.Arch) << " "
            << R.ID.GetAsString();
}

llvm::Optional<InfoRecord> InfoRecord::parse(llvm::StringRef Line) {
  // INFO CODE_ID 554889E55DC3CCCCCCCCCCCCCCCCCCCC [a.exe]
  if (consume<Token>(Line) != Token::Info)
    return llvm::None;

  if (consume<Token>(Line) != Token::CodeID)
    return llvm::None;

  llvm::StringRef Str;
  std::tie(Str, Line) = getToken(Line);
  // If we don't have any text following the code ID (e.g. on linux), we should
  // use this as the UUID. Otherwise, we should revert back to the module ID.
  UUID ID;
  if (Line.trim().empty()) {
    if (Str.empty() || !ID.SetFromStringRef(Str))
      return llvm::None;
  }
  return InfoRecord(std::move(ID));
}

llvm::raw_ostream &breakpad::operator<<(llvm::raw_ostream &OS,
                                        const InfoRecord &R) {
  return OS << "INFO CODE_ID " << R.ID.GetAsString();
}

llvm::Optional<FileRecord> FileRecord::parse(llvm::StringRef Line) {
  // FILE number name
  if (consume<Token>(Line) != Token::File)
    return llvm::None;

  llvm::StringRef Str;
  size_t Number;
  std::tie(Str, Line) = getToken(Line);
  if (!to_integer(Str, Number))
    return llvm::None;

  llvm::StringRef Name = Line.trim();
  if (Name.empty())
    return llvm::None;

  return FileRecord(Number, Name);
}

llvm::raw_ostream &breakpad::operator<<(llvm::raw_ostream &OS,
                                        const FileRecord &R) {
  return OS << "FILE " << R.Number << " " << R.Name;
}

static bool parsePublicOrFunc(llvm::StringRef Line, bool &Multiple,
                              lldb::addr_t &Address, lldb::addr_t *Size,
                              lldb::addr_t &ParamSize, llvm::StringRef &Name) {
  // PUBLIC [m] address param_size name
  // or
  // FUNC [m] address size param_size name

  Token Tok = Size ? Token::Func : Token::Public;

  if (consume<Token>(Line) != Tok)
    return false;

  llvm::StringRef Str;
  std::tie(Str, Line) = getToken(Line);
  Multiple = Str == "m";

  if (Multiple)
    std::tie(Str, Line) = getToken(Line);
  if (!to_integer(Str, Address, 16))
    return false;

  if (Tok == Token::Func) {
    std::tie(Str, Line) = getToken(Line);
    if (!to_integer(Str, *Size, 16))
      return false;
  }

  std::tie(Str, Line) = getToken(Line);
  if (!to_integer(Str, ParamSize, 16))
    return false;

  Name = Line.trim();
  if (Name.empty())
    return false;

  return true;
}

llvm::Optional<FuncRecord> FuncRecord::parse(llvm::StringRef Line) {
  bool Multiple;
  lldb::addr_t Address, Size, ParamSize;
  llvm::StringRef Name;

  if (parsePublicOrFunc(Line, Multiple, Address, &Size, ParamSize, Name))
    return FuncRecord(Multiple, Address, Size, ParamSize, Name);

  return llvm::None;
}

bool breakpad::operator==(const FuncRecord &L, const FuncRecord &R) {
  return L.Multiple == R.Multiple && L.Address == R.Address &&
         L.Size == R.Size && L.ParamSize == R.ParamSize && L.Name == R.Name;
}
llvm::raw_ostream &breakpad::operator<<(llvm::raw_ostream &OS,
                                        const FuncRecord &R) {
  return OS << llvm::formatv("FUNC {0}{1:x-} {2:x-} {3:x-} {4}",
                             R.Multiple ? "m " : "", R.Address, R.Size,
                             R.ParamSize, R.Name);
}

llvm::Optional<LineRecord> LineRecord::parse(llvm::StringRef Line) {
  lldb::addr_t Address;
  llvm::StringRef Str;
  std::tie(Str, Line) = getToken(Line);
  if (!to_integer(Str, Address, 16))
    return llvm::None;

  lldb::addr_t Size;
  std::tie(Str, Line) = getToken(Line);
  if (!to_integer(Str, Size, 16))
    return llvm::None;

  uint32_t LineNum;
  std::tie(Str, Line) = getToken(Line);
  if (!to_integer(Str, LineNum))
    return llvm::None;

  size_t FileNum;
  std::tie(Str, Line) = getToken(Line);
  if (!to_integer(Str, FileNum))
    return llvm::None;

  return LineRecord(Address, Size, LineNum, FileNum);
}

bool breakpad::operator==(const LineRecord &L, const LineRecord &R) {
  return L.Address == R.Address && L.Size == R.Size && L.LineNum == R.LineNum &&
         L.FileNum == R.FileNum;
}
llvm::raw_ostream &breakpad::operator<<(llvm::raw_ostream &OS,
                                        const LineRecord &R) {
  return OS << llvm::formatv("{0:x-} {1:x-} {2} {3}", R.Address, R.Size,
                             R.LineNum, R.FileNum);
}

llvm::Optional<PublicRecord> PublicRecord::parse(llvm::StringRef Line) {
  bool Multiple;
  lldb::addr_t Address, ParamSize;
  llvm::StringRef Name;

  if (parsePublicOrFunc(Line, Multiple, Address, nullptr, ParamSize, Name))
    return PublicRecord(Multiple, Address, ParamSize, Name);

  return llvm::None;
}

bool breakpad::operator==(const PublicRecord &L, const PublicRecord &R) {
  return L.Multiple == R.Multiple && L.Address == R.Address &&
         L.ParamSize == R.ParamSize && L.Name == R.Name;
}
llvm::raw_ostream &breakpad::operator<<(llvm::raw_ostream &OS,
                                        const PublicRecord &R) {
  return OS << llvm::formatv("PUBLIC {0}{1:x-} {2:x-} {3}",
                             R.Multiple ? "m " : "", R.Address, R.ParamSize,
                             R.Name);
}

llvm::Optional<StackCFIRecord> StackCFIRecord::parse(llvm::StringRef Line) {
  // STACK CFI INIT address size reg1: expr1 reg2: expr2 ...
  // or
  // STACK CFI address reg1: expr1 reg2: expr2 ...
  // No token in exprN ends with a colon.

  if (consume<Token>(Line) != Token::Stack)
    return llvm::None;
  if (consume<Token>(Line) != Token::CFI)
    return llvm::None;

  llvm::StringRef Str;
  std::tie(Str, Line) = getToken(Line);

  bool IsInitRecord = stringTo<Token>(Str) == Token::Init;
  if (IsInitRecord)
    std::tie(Str, Line) = getToken(Line);

  lldb::addr_t Address;
  if (!to_integer(Str, Address, 16))
    return llvm::None;

  llvm::Optional<lldb::addr_t> Size;
  if (IsInitRecord) {
    Size.emplace();
    std::tie(Str, Line) = getToken(Line);
    if (!to_integer(Str, *Size, 16))
      return llvm::None;
  }

  return StackCFIRecord(Address, Size, Line.trim());
}

bool breakpad::operator==(const StackCFIRecord &L, const StackCFIRecord &R) {
  return L.Address == R.Address && L.Size == R.Size &&
         L.UnwindRules == R.UnwindRules;
}

llvm::raw_ostream &breakpad::operator<<(llvm::raw_ostream &OS,
                                        const StackCFIRecord &R) {
  OS << "STACK CFI ";
  if (R.Size)
    OS << "INIT ";
  OS << llvm::formatv("{0:x-} ", R.Address);
  if (R.Size)
    OS << llvm::formatv("{0:x-} ", *R.Size);
  return OS << " " << R.UnwindRules;
}

llvm::Optional<StackWinRecord> StackWinRecord::parse(llvm::StringRef Line) {
  // STACK WIN type rva code_size prologue_size epilogue_size parameter_size
  //     saved_register_size local_size max_stack_size has_program_string
  //     program_string_OR_allocates_base_pointer

  if (consume<Token>(Line) != Token::Stack)
    return llvm::None;
  if (consume<Token>(Line) != Token::Win)
    return llvm::None;

  llvm::StringRef Str;
  uint8_t Type;
  std::tie(Str, Line) = getToken(Line);
  // Right now we only support the "FrameData" frame type.
  if (!to_integer(Str, Type) || FrameType(Type) != FrameType::FrameData)
    return llvm::None;

  lldb::addr_t RVA;
  std::tie(Str, Line) = getToken(Line);
  if (!to_integer(Str, RVA, 16))
    return llvm::None;

  lldb::addr_t CodeSize;
  std::tie(Str, Line) = getToken(Line);
  if (!to_integer(Str, CodeSize, 16))
    return llvm::None;

  // Skip fields which we aren't using right now.
  std::tie(Str, Line) = getToken(Line); // prologue_size
  std::tie(Str, Line) = getToken(Line); // epilogue_size

  lldb::addr_t ParameterSize;
  std::tie(Str, Line) = getToken(Line);
  if (!to_integer(Str, ParameterSize, 16))
    return llvm::None;

  lldb::addr_t SavedRegisterSize;
  std::tie(Str, Line) = getToken(Line);
  if (!to_integer(Str, SavedRegisterSize, 16))
    return llvm::None;

  lldb::addr_t LocalSize;
  std::tie(Str, Line) = getToken(Line);
  if (!to_integer(Str, LocalSize, 16))
    return llvm::None;

  std::tie(Str, Line) = getToken(Line); // max_stack_size

  uint8_t HasProgramString;
  std::tie(Str, Line) = getToken(Line);
  if (!to_integer(Str, HasProgramString))
    return llvm::None;
  // FrameData records should always have a program string.
  if (!HasProgramString)
    return llvm::None;

  return StackWinRecord(RVA, CodeSize, ParameterSize, SavedRegisterSize,
                        LocalSize, Line.trim());
}

bool breakpad::operator==(const StackWinRecord &L, const StackWinRecord &R) {
  return L.RVA == R.RVA && L.CodeSize == R.CodeSize &&
         L.ParameterSize == R.ParameterSize &&
         L.SavedRegisterSize == R.SavedRegisterSize &&
         L.LocalSize == R.LocalSize && L.ProgramString == R.ProgramString;
}

llvm::raw_ostream &breakpad::operator<<(llvm::raw_ostream &OS,
                                        const StackWinRecord &R) {
  return OS << llvm::formatv(
             "STACK WIN 4 {0:x-} {1:x-} ? ? {2} {3} {4} ? 1 {5}", R.RVA,
             R.CodeSize, R.ParameterSize, R.SavedRegisterSize, R.LocalSize,
             R.ProgramString);
}

llvm::StringRef breakpad::toString(Record::Kind K) {
  switch (K) {
  case Record::Module:
    return "MODULE";
  case Record::Info:
    return "INFO";
  case Record::File:
    return "FILE";
  case Record::Func:
    return "FUNC";
  case Record::Line:
    return "LINE";
  case Record::Public:
    return "PUBLIC";
  case Record::StackCFI:
    return "STACK CFI";
  case Record::StackWin:
    return "STACK WIN";
  }
  llvm_unreachable("Unknown record kind!");
}
