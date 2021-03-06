#pragma once

#include <iostream>
#include <memory>
#include <numeric>

#include "instruction.h"

namespace Ripes {
namespace Assembler {

template <typename Reg_T, typename Instr_T>
class Matcher {
    struct MatchNode {
        MatchNode(OpPart<Instr_T> _matcher) : matcher(_matcher) {}
        OpPart<Instr_T> matcher;
        std::vector<MatchNode> children;
        std::shared_ptr<Instruction<Reg_T, Instr_T>> instruction;

        void print(unsigned depth = 0) const {
            if (depth == 0) {
                std::cout << "root";
            } else {
                for (unsigned i = 0; i < depth; i++) {
                    std::cout << "-";
                }
                QString matchField =
                    QString::number(matcher.range.stop) + "[" +
                    QStringLiteral("%1").arg(matcher.value, matcher.range.width(), 2, QLatin1Char('0')) + "]" +
                    QString::number(matcher.range.start);
                std::cout << matchField.toStdString() << " -> ";
            }

            if (instruction) {
                std::cout << instruction.get()->name().toStdString() << std::endl;
            } else {
                std::cout << std::endl;
                for (const auto& child : children) {
                    child.print(depth + 1);
                }
            }
        }
    };

public:
    Matcher(const std::vector<std::shared_ptr<Instruction<Reg_T, Instr_T>>>& instructions)
        : m_matchRoot(buildMatchTree(instructions, 1)) {}
    void print() const { m_matchRoot.print(); }

    std::variant<Error, const Instruction<Reg_T, Instr_T>*> matchInstruction(const Instr_T& instruction) const {
        auto match = matchInstructionRec(instruction, m_matchRoot, true);
        if (match == nullptr) {
            return Error(0, "Unknown instruction");
        }
        return match;
    }

private:
    const Instruction<Reg_T, Instr_T>* matchInstructionRec(const Instr_T& instruction, const MatchNode& node,
                                                           bool isRoot) const {
        if (isRoot || node.matcher.matches(instruction)) {
            if (node.children.size() > 0) {
                for (const auto& child : node.children) {
                    if (auto matchedInstr = matchInstructionRec(instruction, child, false)) {
                        return matchedInstr;
                    }
                }
            } else {
                return &(*node.instruction);
            }
        }
        return nullptr;
    }

    MatchNode buildMatchTree(const std::vector<std::shared_ptr<Instruction<Reg_T, Instr_T>>>& instructions,
                             const unsigned fieldDepth = 1,
                             OpPart<Instr_T> matcher = OpPart<Instr_T>(0, BitRange<Instr_T>(0, 0, 2))) {
        std::map<OpPart<Instr_T>, InstrVec<Reg_T, Instr_T>> instrsWithEqualOpPart;

        for (const auto& instr : instructions) {
            if (auto instrRef = instr.get()) {
                const size_t nOpParts = instrRef->getOpcode().opParts.size();
                if (nOpParts < fieldDepth) {
                    QString err =
                        "Instruction '" + instr->name() +
                        "' cannot be decoded; aliases with other instruction (Needs more discernable parts)\n";
                    throw std::runtime_error(err.toStdString().c_str());
                }
                const OpPart opPart = instrRef->getOpcode().opParts[fieldDepth - 1];
                if (nOpParts == fieldDepth && instrsWithEqualOpPart.count(opPart) != 0) {
                    QString err;
                    err +=
                        "Instruction cannot be decoded; aliases with other instruction (Identical to other "
                        "instruction)\n";
                    err += instr->name() + " is equal to " + instrsWithEqualOpPart.at(opPart).at(0)->name();
                    throw std::runtime_error(err.toStdString().c_str());
                }
                instrsWithEqualOpPart[opPart].push_back(instr);
            }
        }

        MatchNode node(matcher);
        for (const auto& iter : instrsWithEqualOpPart) {
            if (iter.second.size() == 1) {
                // Uniquely identifiable instruction
                MatchNode child(iter.first);
                child.instruction = iter.second[0];
                node.children.push_back(child);
            } else {
                // Match branch; recursively continue match tree
                node.children.push_back(buildMatchTree(iter.second, fieldDepth + 1, iter.first));
            }
        }

        return node;
    }

    MatchNode m_matchRoot;
};

}  // namespace Assembler
}  // namespace Ripes
