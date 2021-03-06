#pragma once

#include <QRegularExpression>

#include "assembler_defines.h"
#include "directive.h"
#include "expreval.h"
#include "instruction.h"
#include "isa/isainfo.h"
#include "matcher.h"
#include "parserutilities.h"
#include "pseudoinstruction.h"
#include "relocation.h"
#include "ripes_types.h"

#include <cstdint>
#include <numeric>
#include <set>
#include <variant>

namespace Ripes {
namespace Assembler {

/**
 * A macro for running an assembler pass with error handling
 */
#define runPass(resName, resType, passFunction, ...)                               \
    auto passFunction##_res = passFunction(__VA_ARGS__);                           \
    if (auto* errors = std::get_if<Errors>(&passFunction##_res)) {                 \
        result.errors.insert(result.errors.end(), errors->begin(), errors->end()); \
        return result;                                                             \
    }                                                                              \
    auto resName = std::get<resType>(passFunction##_res);

/**
 * A macro for running an assembler operation which may throw an error or return a value (expressed through a variant
 * type) which runs inside a loop. In case of the operation throwing an error, the error is recorded and the next
 * iteration of the loop is executed. If not, the result value is provided through variable 'resName'.
 */
#define runOperation(resName, resType, operationFunction, ...)        \
    auto operationFunction##_res = operationFunction(__VA_ARGS__);    \
    if (auto* error = std::get_if<Error>(&operationFunction##_res)) { \
        errors.push_back(*error);                                     \
        continue;                                                     \
    }                                                                 \
    auto resName = std::get<resType>(operationFunction##_res);

#define runOperationNoRes(operationFunction, ...)                  \
    auto operationFunction##_res = operationFunction(__VA_ARGS__); \
    if (operationFunction##_res) {                                 \
        errors.push_back(operationFunction##_res.value());         \
        continue;                                                  \
    }

// Macro for defining type aliases for register/instruction width specific types of the assembler types
#define ASSEMBLER_TYPES(_Reg_T, _Instr_T)                           \
    using _InstrVec = InstrVec<_Reg_T, _Instr_T>;                   \
    using _InstrMap = InstrMap<_Reg_T, _Instr_T>;                   \
    using _PseudoInstrVec = PseudoInstrVec<_Reg_T, _Instr_T>;       \
    using _PseudoInstrMap = PseudoInstrMap<_Reg_T, _Instr_T>;       \
    using _Instruction = Instruction<_Reg_T, _Instr_T>;             \
    using _PseudoInstruction = PseudoInstruction<_Reg_T, _Instr_T>; \
    using _OpPart = OpPart<_Instr_T>;                               \
    using _Opcode = Opcode<_Reg_T, _Instr_T>;                       \
    using _Imm = Imm<_Reg_T, _Instr_T>;                             \
    using _ImmPart = ImmPart<_Instr_T>;                             \
    using _Reg = Reg<_Reg_T, _Instr_T>;                             \
    using _Matcher = Matcher<_Reg_T, _Instr_T>;                     \
    using _FieldLinkRequest = FieldLinkRequest<_Reg_T, _Instr_T>;   \
    using _RelocationsVec = RelocationsVec<_Reg_T, _Instr_T>;       \
    using _RelocationsMap = RelocationsMap<_Reg_T, _Instr_T>;       \
    using _AssembleRes = AssembleRes<_Reg_T, _Instr_T>;             \
    using _InstrRes = InstrRes<_Reg_T, Instr_T>;

class AssemblerBase {
public:
    virtual ~AssemblerBase() {}
    std::optional<Error> setCurrentSegment(Section seg) const {
        if (m_sectionBasePointers.count(seg) == 0) {
            return Error(0, "No base address set for segment '" + seg + +"'");
        }
        m_currentSection = seg;
        return {};
    }

    void setSegmentBase(Section seg, AInt base) { m_sectionBasePointers[seg] = base; }

    virtual AssembleResult assemble(const QStringList& programLines, const SymbolMap* symbols = nullptr) const = 0;
    AssembleResult assembleRaw(const QString& program, const SymbolMap* symbols = nullptr) const {
        const auto programLines = program.split(QRegExp("[\r\n]"));
        return assemble(programLines, symbols);
    }

    virtual DisassembleResult disassemble(const Program& program, const AInt baseAddress = 0) const = 0;
    virtual std::pair<QString, std::optional<Error>> disassemble(const VInt word, const ReverseSymbolMap& symbols,
                                                                 const AInt baseAddress = 0) const = 0;

    virtual std::set<QString> getOpcodes() const = 0;

    std::optional<Error> addSymbol(const TokenizedSrcLine& line, Symbol s, VInt v) const {
        return addSymbol(line.sourceLine, s, v);
    }

    std::optional<Error> addSymbol(const unsigned& line, Symbol s, VInt v) const {
        if (m_symbolMap.count(s)) {
            return {Error(line, "Multiple definitions of symbol '" + s.v + "'")};
        }
        m_symbolMap[s] = v;
        return {};
    }

    virtual ExprEvalRes evalExpr(const QString& expr) const = 0;

protected:
    virtual std::variant<Error, LineTokens> tokenize(const QString& line, const int sourceLine) const {
        // Regex: Split on all empty strings (\s+) and characters [, \[, \], \(, \)] except for quote-delimitered
        // substrings.
        const static auto splitter = QRegularExpression(R"((\s+|\,)(?=(?:[^"]*"[^"]*")*[^"]*$))");
        auto tokens = line.split(splitter);
        tokens.removeAll(QStringLiteral(""));
        auto joinedtokens = joinParentheses(tokens);
        if (auto* err = std::get_if<Error>(&joinedtokens)) {
            err->first = sourceLine;
            return *err;
        }
        return std::get<LineTokens>(joinedtokens);
    }

    virtual HandleDirectiveRes assembleDirective(const DirectiveArg& arg, bool& ok,
                                                 bool skipEarlyDirectives = true) const = 0;

    /**
     * @brief splitSymbolsFromLine
     * @returns a pair consisting of a symbol and the the input @p line tokens where the symbol has been removed.
     */
    virtual std::variant<Error, SymbolLinePair> splitSymbolsFromLine(const LineTokens& tokens, int sourceLine) const {
        if (tokens.size() == 0) {
            return {SymbolLinePair({}, tokens)};
        }

        // Handle the case where a token has been joined together with another token, ie "B:nop"
        LineTokens splitTokens;
        splitTokens.reserve(tokens.size());
        for (const auto& token : tokens) {
            Token buffer;
            for (const auto& ch : token) {
                buffer.append(ch);
                if (ch == ':') {
                    splitTokens.push_back(buffer);
                    buffer.clear();
                }
            }
            if (!buffer.isEmpty()) {
                splitTokens.push_back(buffer);
            }
        }

        LineTokens remainingTokens;
        remainingTokens.reserve(splitTokens.size());
        Symbols symbols;
        bool symbolStillAllowed = true;
        for (const auto& token : splitTokens) {
            if (token.endsWith(':')) {
                if (symbolStillAllowed) {
                    const Symbol cleanedSymbol = Symbol(token.left(token.length() - 1), Symbol::Type::Address);
                    if (symbols.count(cleanedSymbol.v) != 0) {
                        return {Error(sourceLine, "Multiple definitions of symbol '" + cleanedSymbol.v + "'")};
                    } else {
                        if (cleanedSymbol.v.isEmpty() || cleanedSymbol.v.contains(s_exprOperatorsRegex)) {
                            return {Error(sourceLine, "Invalid symbol '" + cleanedSymbol.v + "'")};
                        }

                        symbols.insert(cleanedSymbol);
                    }
                } else {
                    return {Error(sourceLine, QStringLiteral("Stray ':' in line"))};
                }
            } else {
                remainingTokens.push_back(token);
                symbolStillAllowed = false;
            }
        }
        return {SymbolLinePair(symbols, remainingTokens)};
    }

    virtual std::variant<Error, DirectiveLinePair> splitDirectivesFromLine(const LineTokens& tokens,
                                                                           int sourceLine) const {
        if (tokens.size() == 0) {
            return {DirectiveLinePair(QString(), tokens)};
        }

        LineTokens remainingTokens;
        remainingTokens.reserve(tokens.size());
        std::vector<QString> directives;
        bool directivesStillAllowed = true;
        for (const auto& token : tokens) {
            if (token.startsWith('.')) {
                if (directivesStillAllowed) {
                    directives.push_back(token);
                } else {
                    return {Error(sourceLine, QStringLiteral("Stray '.' in line"))};
                }
            } else {
                remainingTokens.push_back(token);
                directivesStillAllowed = false;
            }
        }
        if (directives.size() > 1) {
            return {Error(sourceLine, QStringLiteral("Illegal multiple directives"))};
        } else {
            return {DirectiveLinePair(directives.size() == 1 ? directives[0] : QString(), remainingTokens)};
        }
    }

    virtual std::variant<Error, LineTokens> splitCommentFromLine(const LineTokens& tokens) const {
        if (tokens.size() == 0) {
            return {tokens};
        }

        LineTokens preCommentTokens;
        preCommentTokens.reserve(tokens.size());
        for (const auto& token : tokens) {
            if (token.contains(commentDelimiter())) {
                break;
            } else {
                preCommentTokens.push_back(token);
            }
        }
        return {preCommentTokens};
    }

    virtual QChar commentDelimiter() const = 0;

    /**
     * @brief m_sectionBasePointers maintains the base position for the segments
     * annoted by the Segment enum class.
     */
    std::map<Section, AInt> m_sectionBasePointers;
    /**
     * @brief m_currentSegment maintains the current segment where the assembler emits information.
     * Marked mutable to allow for switching currently selected segment during assembling.
     */
    mutable Section m_currentSection;

    /**
     * @brief symbolMap maintains the symbols recorded during assembling. Marked mutable to allow for assembler
     * directives to add symbols during assembling.
     */
    mutable SymbolMap m_symbolMap;
};

/**
 *  Reg_T: type equal in size to the register width of the target
 *  Instr_T: type equal in size to the instruction width of the target
 */
template <typename Reg_T, typename Instr_T>
class Assembler : public AssemblerBase {
    static_assert(std::numeric_limits<Reg_T>::is_integer, "Register type must be integer");
    static_assert(std::numeric_limits<Instr_T>::is_integer, "Instruction type must be integer");

    ASSEMBLER_TYPES(Reg_T, Instr_T)

public:
    Assembler(const ISAInfoBase* isa) : m_isa(isa) {}

    AssembleResult assemble(const QStringList& programLines, const SymbolMap* symbols = nullptr) const override {
        AssembleResult result;

        // Per default, emit to .text until otherwise specified
        setCurrentSegment(".text");
        m_symbolMap.clear();
        if (symbols) {
            m_symbolMap = *symbols;
        }

        // Tokenize each source line and separate symbol from remainder of tokens
        runPass(tokenizedLines, SourceProgram, pass0, programLines);

        // Pseudo instruction expansion
        runPass(expandedLines, SourceProgram, pass1, tokenizedLines);

        /** Assemble. During assembly, we generate:
         * - linkageMap: Recording offsets of instructions which require linkage with symbols
         */
        LinkRequests needsLinkage;
        runPass(program, Program, pass2, expandedLines, needsLinkage);

        // Symbol linkage
        runPass(unused, NoPassResult, pass3, program, needsLinkage);
        Q_UNUSED(unused);

        result.program = program;
        result.program.entryPoint = m_sectionBasePointers.at(".text");
        return result;
    }

    DisassembleResult disassemble(const Program& program, const AInt baseAddress = 0) const override {
        size_t i = 0;
        DisassembleResult res;
        auto& programBits = program.getSection(".text")->data;
        while ((i + sizeof(Instr_T)) <= static_cast<VInt>(programBits.size())) {
            const Instr_T instructionWord = *reinterpret_cast<const Instr_T*>(programBits.data() + i);
            auto disres = disassemble(instructionWord, program.symbols, baseAddress + i);
            if (disres.second) {
                res.errors.push_back(disres.second.value());
            }
            res.program << disres.first;
            i += sizeof(Instr_T);
        }
        return res;
    }

    std::pair<QString, std::optional<Error>> disassemble(const VInt word, const ReverseSymbolMap& symbols,
                                                         const AInt baseAddress = 0) const override {
        auto match = m_matcher->matchInstruction(word);
        if (auto* error = std::get_if<Error>(&match)) {
            return {"unknown instruction", *error};
        }

        // Got match, disassemble
        auto tokensVar = std::get<const _Instruction*>(match)->disassemble(word, baseAddress, symbols);
        if (auto* error = std::get_if<Error>(&match)) {
            // Error during disassembling
            return {"invalid instruction", *error};
        }

        // Join tokens
        QString joinedLine;
        const auto tokens = std::get<LineTokens>(tokensVar);
        for (const auto& token : qAsConst(tokens)) {
            joinedLine += token + " ";
        }
        joinedLine.chop(1);  // remove trailing ' '

        return {joinedLine, {}};
    }

    const _Matcher& getMatcher() { return *m_matcher; }

    std::set<QString> getOpcodes() const override {
        std::set<QString> opcodes;
        for (const auto& iter : m_instructionMap) {
            opcodes.insert(iter.first);
        }
        for (const auto& iter : m_pseudoInstructionMap) {
            opcodes.insert(iter.first);
        }
        return opcodes;
    }

    void setRelocations(_RelocationsVec& relocations) {
        if (m_relocations.size() != 0) {
            throw std::runtime_error("Directives already set");
        }
        m_relocations = relocations;
        for (const auto& iter : m_relocations) {
            const auto relocation = iter.get()->name();
            if (m_relocationsMap.count(relocation) != 0) {
                throw std::runtime_error("Error: relocation " + relocation.toStdString() +
                                         " has already been registerred.");
            }
            m_relocationsMap[relocation] = iter;
        }
    }

protected:
    struct LinkRequest {
        unsigned sourceLine;  // Source location of code which resulted in the link request
        Reg_T offset;         // Offset of instruction in segment which needs link resolution
        Section section;      // Section which instruction was emitted in

        // Reference to the immediate field which resolves the symbol and the requested symbol
        _FieldLinkRequest fieldRequest;
    };

    Reg_T linkReqAddress(const LinkRequest& req) const { return req.offset + m_sectionBasePointers.at(req.section); }

    using LinkRequests = std::vector<LinkRequest>;

    /**
     * @brief pass0
     * Line tokenization and source line recording
     */
    std::variant<Errors, SourceProgram> pass0(const QStringList& program) const {
        Errors errors;
        SourceProgram tokenizedLines;
        tokenizedLines.reserve(program.size());
        Symbols symbols;

        /** @brief carry
         * A symbol should refer to the next following assembler line; whether an instruction or directive.
         * The carry is used to carry over symbol definitions from empty lines onto the next valid line.
         * Multiple symbol definitions is checked in this step given that a symbol may loose its source line
         * information if it is a blank symbol (no other information on line).
         */
        Symbols carry;
        for (int i = 0; i < program.size(); i++) {
            const auto& line = program.at(i);
            if (line.isEmpty())
                continue;
            TokenizedSrcLine tsl;
            tsl.sourceLine = i;
            runOperation(tokens, LineTokens, tokenize, program[i], i);

            runOperation(remainingTokens, LineTokens, splitCommentFromLine, tokens);

            // Symbols precede directives
            runOperation(symbolsAndRest, SymbolLinePair, splitSymbolsFromLine, remainingTokens, i);

            tsl.symbols = symbolsAndRest.first;

            bool uniqueSymbols = true;
            for (const auto& s : symbolsAndRest.first) {
                if (symbols.count(s) != 0) {
                    errors.push_back(Error(i, "Multiple definitions of symbol '" + s.v + "'"));
                    uniqueSymbols = false;
                    break;
                }
            }
            if (!uniqueSymbols) {
                continue;
            }
            symbols.insert(symbolsAndRest.first.begin(), symbolsAndRest.first.end());

            runOperation(directiveAndRest, DirectiveLinePair, splitDirectivesFromLine, symbolsAndRest.second, i);
            tsl.directive = directiveAndRest.first;

            // Parse (and remove) relocation hints from the tokens.
            runOperation(finalTokens, LineTokens, splitRelocationsFromLine, directiveAndRest.second);

            tsl.tokens = finalTokens;
            if (tsl.tokens.empty() && tsl.directive.isEmpty()) {
                if (!tsl.symbols.empty()) {
                    carry.insert(tsl.symbols.begin(), tsl.symbols.end());
                }
            } else {
                tsl.symbols.insert(carry.begin(), carry.end());
                carry.clear();
                tokenizedLines.push_back(tsl);
            }

            if (!tsl.directive.isEmpty() && m_earlyDirectives.count(tsl.directive)) {
                bool wasDirective;  // unused
                runOperation(directiveBytes, std::optional<QByteArray>, assembleDirective, DirectiveArg{tsl, nullptr},
                             wasDirective, false);
            }
        }
        if (errors.size() != 0) {
            return {errors};
        } else {
            return {tokenizedLines};
        }
    }

    /**
     * @brief pass1
     * Pseudo-op expansion. If @return errors is empty, pass succeeded.
     */
    std::variant<Errors, SourceProgram> pass1(const SourceProgram& tokenizedLines) const {
        Errors errors;
        SourceProgram expandedLines;
        expandedLines.reserve(tokenizedLines.size());

        for (unsigned i = 0; i < tokenizedLines.size(); i++) {
            const auto& tokenizedLine = tokenizedLines.at(i);
            runOperation(expandedOps, std::optional<std::vector<LineTokens>>, expandPseudoOp, tokenizedLine);
            if (expandedOps) {
                /** @note: Original source line is kept for all resulting lines after pseudo-op expantion.
                 * Labels and directives are only kept for the first expanded op.
                 */
                const auto& eops = expandedOps.value();
                for (size_t j = 0; j < eops.size(); j++) {
                    TokenizedSrcLine tsl;
                    tsl.tokens = eops.at(j);
                    tsl.sourceLine = tokenizedLine.sourceLine;
                    if (j == 0) {
                        tsl.directive = tokenizedLine.directive;
                        tsl.symbols = tokenizedLine.symbols;
                    }
                    expandedLines.push_back(tsl);
                }
            } else {
                // This was not a pseudoinstruction; just add line to the set of expanded lines
                expandedLines.push_back(tokenizedLine);
            }
        }

        if (errors.size() != 0) {
            return {errors};
        } else {
            return {expandedLines};
        }
    }

    /**
     * @brief pass2
     * Machine code translation. If @return errors is empty, pass succeeded.
     * In the following, current size of the program is used as an analog for the offset of the to-be-assembled
     * instruction in the program. This is then used for symbol resolution.
     */
    std::variant<Errors, Program> pass2(const SourceProgram& tokenizedLines, LinkRequests& needsLinkage) const {
        // Initialize program with initialized segments:
        Program program;
        for (const auto& iter : m_sectionBasePointers) {
            ProgramSection sec;
            sec.name = iter.first;
            sec.address = iter.second;
            sec.data = QByteArray();
            program.sections[iter.first] = sec;
        }

        Errors errors;
        ProgramSection* currentSection = &program.sections.at(m_currentSection);

        bool wasDirective;
        for (const auto& line : tokenizedLines) {
            // Get offset of currently emitting position in memory relative to section position
            VInt addr_offset = currentSection->data.size();
            for (const auto& s : line.symbols) {
                // Record symbol position as its absolute address in memory
                runOperationNoRes(addSymbol, line, s, addr_offset + program.sections.at(m_currentSection).address);
            }

            runOperation(directiveBytes, std::optional<QByteArray>, assembleDirective,
                         DirectiveArg{line, currentSection}, wasDirective);

            // Currently emitting segment may have changed during the assembler directive; refresh state
            currentSection = &program.sections.at(m_currentSection);
            addr_offset = currentSection->data.size();
            if (!wasDirective) {
                std::weak_ptr<_Instruction> assembledWith;
                runOperation(machineCode, _InstrRes, assembleInstruction, line, assembledWith);

                if (!machineCode.linksWithSymbol.symbol.isEmpty()) {
                    LinkRequest req;
                    req.sourceLine = line.sourceLine;
                    req.offset = addr_offset;
                    req.fieldRequest = machineCode.linksWithSymbol;
                    req.section = m_currentSection;
                    needsLinkage.push_back(req);
                }
                currentSection->data.append(
                    QByteArray(reinterpret_cast<char*>(&machineCode.instruction), sizeof(machineCode.instruction)));

            }
            // This was a directive; check if any bytes needs to be appended to the segment
            else if (directiveBytes) {
                currentSection->data.append(directiveBytes.value());
            }
        }
        if (errors.size() != 0) {
            return {errors};
        }

        // Register address symbols in program struct
        for (const auto& iter : m_symbolMap) {
            if (iter.first.is(Symbol::Type::Address)) {
                program.symbols[iter.second] = iter.first;
            }
        }

        return {program};
    }

    ExprEvalRes evalExpr(const QString& expr) const override {
        auto symbolValue = m_symbolMap.find(expr);
        if (symbolValue != m_symbolMap.end()) {
            return symbolValue->second;
        } else {
            return evaluate(expr, &m_symbolMap);
        }
    }

    std::variant<Errors, NoPassResult> pass3(Program& program, const LinkRequests& needsLinkage) const {
        Errors errors;
        for (const auto& linkRequest : needsLinkage) {
            const auto& symbol = linkRequest.fieldRequest.symbol;
            Reg_T symbolValue;

            // Add the special __address__ symbol indicating the address of the instruction itself. Not done through
            // addSymbol given that we redefine this symbol on each line.
            const Reg_T linkRequestAddress = linkReqAddress(linkRequest);
            m_symbolMap["__address__"] = linkRequestAddress;

            // Expression evaluation also performs symbol evaluation
            auto exprRes = evalExpr(symbol);
            if (auto* err = std::get_if<Error>(&exprRes)) {
                err->first = linkRequest.sourceLine;
                errors.push_back(*err);
                continue;
            } else {
                symbolValue = std::get<ExprEvalVT>(exprRes);
            }

            if (!linkRequest.fieldRequest.relocation.isEmpty()) {
                auto relocRes = m_relocationsMap.at(linkRequest.fieldRequest.relocation)
                                    .get()
                                    ->handle(symbolValue, linkRequestAddress);
                if (auto* error = std::get_if<Error>(&relocRes)) {
                    errors.push_back(*error);
                    continue;
                }
                symbolValue = std::get<Reg_T>(relocRes);
            }

            QByteArray& section = program.sections.at(linkRequest.section).data;

            // Decode instruction at link-request position
            assert(static_cast<unsigned>(section.size()) >= (linkRequest.offset + 4) &&
                   "Error: position of link request is not within program");
            Instr_T instr = *reinterpret_cast<Instr_T*>(section.data() + linkRequest.offset);

            // Re-apply immediate resolution using the value acquired from the symbol map
            if (auto* immField = dynamic_cast<const _Imm*>(linkRequest.fieldRequest.field)) {
                if (auto err = immField->applySymbolResolution(symbolValue, instr, linkReqAddress(linkRequest),
                                                               linkRequest.sourceLine)) {
                    errors.push_back(err.value());
                    continue;
                }
            } else {
                assert(false && "Something other than an immediate field has requested linkage?");
            }

            // Finally, overwrite the instruction in the section
            *reinterpret_cast<Instr_T*>(section.data() + linkRequest.offset) = instr;
        }
        if (errors.size() != 0) {
            return {errors};
        } else {
            return {NoPassResult()};
        }
    }

    virtual PseudoExpandRes expandPseudoOp(const TokenizedSrcLine& line) const {
        if (line.tokens.empty()) {
            return PseudoExpandRes(std::nullopt);
        }
        const auto& opcode = line.tokens.at(0);
        if (m_pseudoInstructionMap.count(opcode) == 0) {
            // Not a pseudo instruction
            return PseudoExpandRes(std::nullopt);
        }
        auto res = m_pseudoInstructionMap.at(opcode)->expand(line, m_symbolMap);
        if (auto* error = std::get_if<Error>(&res)) {
            Q_UNUSED(error);
            if (m_instructionMap.count(opcode) != 0) {
                // If this pseudo-instruction aliases with an instruction but threw an error (could arise if ie.
                // arguments provided were intended for the normal instruction and not the pseudoinstruction), then
                // return as if not a pseudo-instruction, falling to normal instruction handling
                return PseudoExpandRes(std::nullopt);
            }
        }

        // Return result (containing either a valid pseudo-instruction expand error or the expanded pseudo
        // instruction
        return {res};
    }

    virtual _AssembleRes assembleInstruction(const TokenizedSrcLine& line,
                                             std::weak_ptr<_Instruction>& assembledWith) const {
        if (line.tokens.empty()) {
            return {Error(line.sourceLine, "Empty source lines should be impossible at this point")};
        }
        const auto& opcode = line.tokens.at(0);
        if (m_instructionMap.count(opcode) == 0) {
            return {Error(line.sourceLine, "Unknown opcode '" + opcode + "'")};
        }
        assembledWith = m_instructionMap.at(opcode);
        return m_instructionMap.at(opcode)->assemble(line);
    }

    void setPseudoInstructions(_PseudoInstrVec& pseudoInstructions) {
        if (m_pseudoInstructions.size() != 0) {
            throw std::runtime_error("Pseudoinstructions already set");
        }
        m_pseudoInstructions = pseudoInstructions;

        for (const auto& iter : m_pseudoInstructions) {
            const auto instr_name = iter.get()->name();
            if (m_pseudoInstructionMap.count(instr_name) != 0) {
                throw std::runtime_error("Error: pseudo-instruction with opcode '" + instr_name.toStdString() +
                                         "' has already been registerred.");
            }
            m_pseudoInstructionMap[instr_name] = iter;
        }
    }

    /**
     * @brief splitRelocationsFromLine
     * Identify relocations in tokens; when found, add the relocation to the _following_ token. Relocations are _not_
     * added to the set of remaining tokens.
     */
    virtual std::variant<Error, LineTokens> splitRelocationsFromLine(LineTokens& tokens) const {
        if (tokens.size() == 0) {
            return {tokens};
        }

        LineTokens remainingTokens;
        remainingTokens.reserve(tokens.size());

        Token relocationForNextToken;
        for (auto& token : tokens) {
            if (m_relocationsMap.count(token)) {
                relocationForNextToken = token;
            } else {
                if (!relocationForNextToken.isEmpty()) {
                    token.setRelocation(relocationForNextToken);
                    relocationForNextToken.clear();
                }
                remainingTokens.push_back(token);
            }
        }

        return {remainingTokens};
    }

    void initialize(_InstrVec& instructions, _PseudoInstrVec& pseudoinstructions, DirectiveVec& directives,
                    _RelocationsVec& relocations) {
        setInstructions(instructions);
        setPseudoInstructions(pseudoinstructions);
        setDirectives(directives);
        setRelocations(relocations);
        m_matcher = std::make_unique<_Matcher>(m_instructions);
    }

    void setDirectives(DirectiveVec& directives) {
        if (m_directives.size() != 0) {
            throw std::runtime_error("Directives already set");
        }
        m_directives = directives;
        for (const auto& iter : m_directives) {
            const auto directive = iter.get()->name();
            if (m_directivesMap.count(directive) != 0) {
                throw std::runtime_error("Error: directive " + directive.toStdString() +
                                         " has already been registerred.");
            }
            m_directivesMap[directive] = iter;
            if (iter->early()) {
                m_earlyDirectives.insert(iter->name());
            }
        }
    }

    void setInstructions(_InstrVec& instructions) {
        if (m_instructions.size() != 0) {
            throw std::runtime_error("Instructions already set");
        }
        m_instructions = instructions;
        for (const auto& iter : m_instructions) {
            const auto instr_name = iter.get()->name();
            if (m_instructionMap.count(instr_name) != 0) {
                throw std::runtime_error("Error: instruction with opcode '" + instr_name.toStdString() +
                                         "' has already been registerred.");
            }
            m_instructionMap[instr_name] = iter;
        }
    }

    HandleDirectiveRes assembleDirective(const DirectiveArg& arg, bool& ok,
                                         bool skipEarlyDirectives = true) const override {
        ok = false;
        if (arg.line.directive.isEmpty()) {
            return std::nullopt;
        }
        ok = true;
        try {
            const auto& directive = m_directivesMap.at(arg.line.directive);
            if (directive->early() && skipEarlyDirectives) {
                return std::nullopt;
            }
            return directive->handle(this, arg);
        } catch (const std::out_of_range&) {
            return {Error(arg.line.sourceLine, "Unknown directive '" + arg.line.directive + "'")};
        }
    }

    /**
     * @brief m_instructions is the set of instructions which can be matched from an instruction string as well as
     * be disassembled from a program.
     */
    _InstrVec m_instructions;
    _InstrMap m_instructionMap;

    /**
     * @brief m_pseudoInstructions is the set of instructions which can be matched from an instruction string but
     * cannot be disassembled from a program. Typically, pseudoinstructions will expand to one or more non-pseudo
     * instructions.
     */
    _PseudoInstrVec m_pseudoInstructions;
    _PseudoInstrMap m_pseudoInstructionMap;

    /**
     * @brief m_relocations is the set of supported assembler relocation hints
     */
    _RelocationsVec m_relocations;
    _RelocationsMap m_relocationsMap;

    /**
     * @brief m_assemblerDirectives is the set of supported assembler directives.
     */
    DirectiveVec m_directives;
    DirectiveMap m_directivesMap;
    EarlyDirectives m_earlyDirectives;

    std::unique_ptr<_Matcher> m_matcher;

    const ISAInfoBase* m_isa;
};

}  // namespace Assembler

}  // namespace Ripes
