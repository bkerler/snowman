/* The file is part of Snowman decompiler.             */
/* See doc/licenses.txt for the licensing information. */

#include "ArmInstructionAnalyzer.h"

#include <QCoreApplication>

#include <boost/range/size.hpp>

#include <nc/common/CheckedCast.h>
#include <nc/common/Foreach.h>
#include <nc/common/Unreachable.h>
#include <nc/common/make_unique.h>

#include <nc/core/ir/Program.h>
#include <nc/core/irgen/Expressions.h>
#include <nc/core/irgen/InvalidInstructionException.h>

#include "ArmArchitecture.h"
#include "ArmInstruction.h"
#include "ArmRegisters.h"
#include "CapstoneDisassembler.h"

namespace nc {
namespace arch {
namespace arm {

namespace {

class ArmExpressionFactory: public core::irgen::expressions::ExpressionFactory<ArmExpressionFactory> {
public:
    ArmExpressionFactory(const core::arch::Architecture *architecture):
        core::irgen::expressions::ExpressionFactory<ArmExpressionFactory>(architecture)
    {}
};

typedef core::irgen::expressions::ExpressionFactoryCallback<ArmExpressionFactory> ArmExpressionFactoryCallback;

NC_DEFINE_REGISTER_EXPRESSION(ArmRegisters, z)
NC_DEFINE_REGISTER_EXPRESSION(ArmRegisters, n)
NC_DEFINE_REGISTER_EXPRESSION(ArmRegisters, c)
NC_DEFINE_REGISTER_EXPRESSION(ArmRegisters, v)

NC_DEFINE_REGISTER_EXPRESSION(ArmRegisters, pseudo_flags)
NC_DEFINE_REGISTER_EXPRESSION(ArmRegisters, less)
NC_DEFINE_REGISTER_EXPRESSION(ArmRegisters, less_or_equal)
NC_DEFINE_REGISTER_EXPRESSION(ArmRegisters, below_or_equal)

} // anonymous namespace

class ArmInstructionAnalyzerImpl {
    Q_DECLARE_TR_FUNCTIONS(ArmInstructionAnalyzerImpl)

    CapstoneDisassembler disassembler_;
    ArmExpressionFactory factory_;
    core::ir::Program *program_;
    const ArmInstruction *instruction_;
    CapstoneInstructionPtr instr_;
    const cs_arm *detail_;

public:
    ArmInstructionAnalyzerImpl(const ArmArchitecture *architecture):
        disassembler_(CS_ARCH_ARM, CS_MODE_ARM), factory_(architecture)
    {}

    void createStatements(const ArmInstruction *instruction, core::ir::Program *program) {
        assert(instruction != NULL);
        assert(program != NULL);

        program_ = program;
        instruction_ = instruction;

        instr_ = disassemble(instruction);
        assert(instr_ != NULL);
        detail_ = &instr_->detail->arm;

        auto instructionBasicBlock = program_->getBasicBlockForInstruction(instruction_);

        if (detail_->cc == ARM_CC_AL) {
            createBody(instructionBasicBlock);
        } else {
            auto directSuccessor = program_->createBasicBlock(instruction_->endAddr());

            auto bodyBasicBlock = program_->createBasicBlock();
            createCondition(instructionBasicBlock, bodyBasicBlock, directSuccessor);
            createBody(bodyBasicBlock);

            if (!bodyBasicBlock->getTerminator()) {
                using namespace core::irgen::expressions;
                ArmExpressionFactoryCallback _(factory_, bodyBasicBlock, instruction);
                _[jump(directSuccessor)];
            }
        }
    }

private:
    CapstoneInstructionPtr disassemble(const ArmInstruction *instruction) {
        disassembler_.setMode(instruction->mode());
        return disassembler_.disassemble(instruction->addr(), instruction->bytes(), instruction->size());
    }

    void createCondition(core::ir::BasicBlock *conditionBasicBlock, core::ir::BasicBlock *bodyBasicBlock, core::ir::BasicBlock *directSuccessor) {
        using namespace core::irgen::expressions;

        ArmExpressionFactoryCallback _(factory_, conditionBasicBlock, instruction_);

        switch (detail_->cc) {
        case ARM_CC_INVALID:
            throw core::irgen::InvalidInstructionException(tr("Invalid condition code."));
        case ARM_CC_EQ:
            _[jump( z, bodyBasicBlock, directSuccessor)];
            break;
        case ARM_CC_NE:
            _[jump(~z, bodyBasicBlock, directSuccessor)];
            break;
        case ARM_CC_HS:
            _[jump( c, bodyBasicBlock, directSuccessor)];
            break;
        case ARM_CC_LO:
            _[jump(~c, bodyBasicBlock, directSuccessor)];
            break;
        case ARM_CC_MI:
            _[jump( n, bodyBasicBlock, directSuccessor)];
            break;
        case ARM_CC_PL:
            _[jump(~n, bodyBasicBlock, directSuccessor)];
            break;
        case ARM_CC_VS:
            _[jump( v, bodyBasicBlock, directSuccessor)];
            break;
        case ARM_CC_VC:
            _[jump(~v, bodyBasicBlock, directSuccessor)];
            break;
        case ARM_CC_HI:
            _[jump(choice(~below_or_equal, c & ~z), bodyBasicBlock, directSuccessor)];
            break;
        case ARM_CC_LS:
            _[jump(choice(below_or_equal, ~c | z), bodyBasicBlock, directSuccessor)];
            break;
        case ARM_CC_GE:
            _[jump(choice(~less, n == v), bodyBasicBlock, directSuccessor)];
            break;
        case ARM_CC_LT:
            _[jump(choice(less, ~(n == v)), bodyBasicBlock, directSuccessor)];
            break;
        case ARM_CC_GT:
            _[jump(choice(~less_or_equal, ~z & (n == v)), bodyBasicBlock, directSuccessor)];
            break;
        case ARM_CC_LE:
            _[jump(choice(less_or_equal, z | ~(n == v)), bodyBasicBlock, directSuccessor)];
            break;
        default:
            unreachable();
        };
    }

    void createBody(core::ir::BasicBlock *bodyBasicBlock) {
        using namespace core::irgen::expressions;

        ArmExpressionFactoryCallback _(factory_, bodyBasicBlock, instruction_);

        switch (instr_->id) {
        case ARM_INS_B:
            _[jump(operand(0))];
            break;
        case ARM_INS_BL: /* FALLTHROUGH */
        case ARM_INS_BLX:
            _[call(operand(0))];
            break;
        case ARM_INS_CMP: {
            _[
                n ^= intrinsic(),
                c ^= unsigned_(operand(0)) < operand(1),
                z ^= operand(0) == operand(1),
                v ^= intrinsic(),

                less             ^= signed_(operand(0)) < operand(1),
                less_or_equal    ^= signed_(operand(0)) <= operand(1),
                below_or_equal   ^= unsigned_(operand(0)) <= operand(1)
            ];
            break;
        }
        case ARM_INS_LDR:
        case ARM_INS_LDRT:
        case ARM_INS_LDREX: { // TODO: atomic
            _[operand(0) ^= operand(1)];
            handleWriteback(bodyBasicBlock);
            break;
        }
        case ARM_INS_LDRH:
        case ARM_INS_LDRHT:
        case ARM_INS_LDREXH: { // TODO: atomic
            _[operand(0) ^= zero_extend(operand(1, 16))];
            handleWriteback(bodyBasicBlock);
            break;
        }
        case ARM_INS_LDRSH:
        case ARM_INS_LDRSHT: {
            _[operand(0) ^= sign_extend(operand(1, 16))];
            handleWriteback(bodyBasicBlock);
            break;
        }
        case ARM_INS_LDRB:
        case ARM_INS_LDRBT:
        case ARM_INS_LDREXB: { // TODO: atomic
            _[operand(0) ^= zero_extend(operand(1, 8))];
            handleWriteback(bodyBasicBlock);
            break;
        }
        case ARM_INS_LDRSB:
        case ARM_INS_LDRSBT: {
            _[operand(0) ^= sign_extend(operand(1, 8))];
            handleWriteback(bodyBasicBlock);
            break;
        }
        // TODO case ARM_INS_LDRD:
        case ARM_INS_MOV: {
            if (detail_->operands[0].reg == ARM_REG_PC) {
                _[jump(operand(1))];
            } else {
                _[operand(0) ^= operand(1)];
                if (detail_->update_flags) {
                    // TODO
                }
            }
            break;
        }
        case ARM_INS_STR:
        case ARM_INS_STRT:
        case ARM_INS_STREX: { // TODO: atomic
            _[operand(1) ^= operand(0)];
            handleWriteback(bodyBasicBlock);
            break;
        }
        case ARM_INS_STRH:
        case ARM_INS_STRHT:
        case ARM_INS_STREXH: {
            _[operand(1, 16) ^= truncate(operand(0))];
            handleWriteback(bodyBasicBlock);
            break;
        }
        case ARM_INS_STRB:
        case ARM_INS_STRBT:
        case ARM_INS_STREXB: {
            _[operand(1, 8) ^= truncate(operand(0))];
            handleWriteback(bodyBasicBlock);
            break;
        }
        // TODO case ARM_INS_STRD:
        default: {
            _(std::make_unique<core::ir::InlineAssembly>());
            break;
        }
        } /* switch */
    }

    void handleWriteback(core::ir::BasicBlock *bodyBasicBlock) {
        if (detail_->op_count != 2 && detail_->op_count != 3) {
            throw core::irgen::InvalidInstructionException(tr("Expected either two or three operands."));
        }
        if (detail_->operands[0].type != ARM_OP_REG) {
            throw core::irgen::InvalidInstructionException(tr("Expected the first operand to be a register."));
        }
        if (detail_->operands[1].type != ARM_OP_MEM) {
            throw core::irgen::InvalidInstructionException(tr("Expected the second operand to be a memory operand."));
        }

        using namespace core::irgen::expressions;
        ArmExpressionFactoryCallback _(factory_, bodyBasicBlock, instruction_);

        if (detail_->op_count == 3) {
            auto base = regizter(getRegister(detail_->operands[1].mem.base));
            _[base ^= base + operand(2)];
        } else if (detail_->writeback) {
            auto base = regizter(getRegister(detail_->operands[1].mem.base));
            _[base ^= base + constant(detail_->operands[1].mem.disp)];
        }
    }

    core::irgen::expressions::TermExpression operand(std::size_t index, SmallBitSize sizeHint = 32) const {
        return core::irgen::expressions::TermExpression(createTermForOperand(index, sizeHint));
    }

    std::unique_ptr<core::ir::Term> createTermForOperand(std::size_t index, SmallBitSize sizeHint) const {
        assert(index < boost::size(detail_->operands));

        const auto &operand = detail_->operands[index];
        // TODO: shifts
        switch (operand.type) {
            case ARM_OP_REG: {
                auto reg = getRegister(operand.reg);
                if (sizeHint > reg->size()) {
                    throw core::irgen::InvalidInstructionException(tr("Size hint (%1) exceeds register size (%2).").arg(sizeHint).arg(reg->size()));
                }
                return std::make_unique<core::ir::MemoryLocationAccess>(reg->memoryLocation().resized(sizeHint));
            }
            case ARM_OP_CIMM:
                throw core::irgen::InvalidInstructionException(tr("Don't know how to deal with CIMM operands."));
            case ARM_OP_PIMM:
                throw core::irgen::InvalidInstructionException(tr("Don't know how to deal with PIMM operands."));
            case ARM_OP_IMM:
                return std::make_unique<core::ir::Constant>(SizedValue(sizeHint, operand.imm));
            case ARM_OP_FP:
                throw core::irgen::InvalidInstructionException(tr("Don't know how to deal with FP operands."));
            case ARM_OP_MEM:
                return std::make_unique<core::ir::Dereference>(createDereferenceAddress(index), core::ir::MemoryDomain::MEMORY, sizeHint);
            default:
                unreachable();
        }
    }

    std::unique_ptr<core::ir::Term> createDereferenceAddress(std::size_t index) const {
        assert(index < boost::size(detail_->operands));

        const auto &operand = detail_->operands[index];
        if (operand.type != ARM_OP_MEM) {
            throw core::irgen::InvalidInstructionException(tr("Expected the operand to be a memory operand"));
        }
        const auto &mem = operand.mem;

        auto result = createRegisterAccess(mem.base);

        if (mem.index != ARM_REG_INVALID) {
            assert(mem.scale == 1 || mem.scale == -1);

            result = std::make_unique<core::ir::BinaryOperator>(
                mem.scale == 1 ? core::ir::BinaryOperator::ADD : core::ir::BinaryOperator::SUB,
                std::move(result),
                createRegisterAccess(mem.index),
                result->size()
            );
        }

        if (mem.disp != 0) {
            result = std::make_unique<core::ir::BinaryOperator>(
                core::ir::BinaryOperator::ADD,
                std::move(result),
                std::make_unique<core::ir::Constant>(SizedValue(result->size(), mem.disp)),
                result->size()
            );
        }

        return result;
    }

    static std::unique_ptr<core::ir::Term> createRegisterAccess(int reg) {
        return ArmInstructionAnalyzer::createTerm(getRegister(reg));
    }

    static const core::arch::Register *getRegister(int reg) {
        switch (reg) {
        #define REG(lowercase, uppercase) \
            case ARM_REG_##uppercase: return ArmRegisters::lowercase();
        REG(apsr,       APSR)
        REG(apsr_nzcv,  APSR_NZCV)
        REG(cpsr,       CPSR)
        REG(fpexc,      FPEXC)
        REG(fpinst,     FPINST)
        REG(fpinst2,    FPINST2)
        REG(fpscr,      FPSCR)
        REG(fpscr_nzcv, FPSCR_NZCV)
        REG(fpsid,      FPSID)
        REG(itstate,    ITSTATE)
        REG(lr,         LR)
        REG(mvfr0,      MVFR0)
        REG(mvfr1,      MVFR1)
        REG(mvfr2,      MVFR2)
        REG(pc,         PC)
        REG(sp,         SP)
        REG(spsr,       SPSR)

        REG(r0,         R0)
        REG(r1,         R1)
        REG(r2,         R2)
        REG(r3,         R3)
        REG(r4,         R4)
        REG(r5,         R5)
        REG(r6,         R6)
        REG(r7,         R7)
        REG(r8,         R8)
        REG(r9,         R9)
        REG(r10,        R10)
        REG(r11,        R11)
        REG(r12,        R12)
        REG(s0,         S0)
        REG(s1,         S1)
        REG(s2,         S2)
        REG(s3,         S3)
        REG(s4,         S4)
        REG(s5,         S5)
        REG(s6,         S6)
        REG(s7,         S7)
        REG(s8,         S8)
        REG(s9,         S9)
        REG(s10,        S10)
        REG(s11,        S11)
        REG(s12,        S12)
        REG(s13,        S13)
        REG(s14,        S14)
        REG(s15,        S15)
        REG(s16,        S16)
        REG(s17,        S17)
        REG(s18,        S18)
        REG(s19,        S19)
        REG(s20,        S20)
        REG(s21,        S21)
        REG(s22,        S22)
        REG(s23,        S23)
        REG(s24,        S24)
        REG(s25,        S25)
        REG(s26,        S26)
        REG(s27,        S27)
        REG(s28,        S28)
        REG(s29,        S29)
        REG(s30,        S30)
        REG(s31,        S31)

        REG(d0,         D0)
        REG(d1,         D1)
        REG(d2,         D2)
        REG(d3,         D3)
        REG(d4,         D4)
        REG(d5,         D5)
        REG(d6,         D6)
        REG(d7,         D7)
        REG(d8,         D8)
        REG(d9,         D9)
        REG(d10,        D10)
        REG(d11,        D11)
        REG(d12,        D12)
        REG(d13,        D13)
        REG(d14,        D14)
        REG(d15,        D15)
        REG(d16,        D16)
        REG(d17,        D17)
        REG(d18,        D18)
        REG(d19,        D19)
        REG(d20,        D20)
        REG(d21,        D21)
        REG(d22,        D22)
        REG(d23,        D23)
        REG(d24,        D24)
        REG(d25,        D25)
        REG(d26,        D26)
        REG(d27,        D27)
        REG(d28,        D28)
        REG(d29,        D29)
        REG(d30,        D30)
        REG(d31,        D31)

        REG(q0,         Q0)
        REG(q1,         Q1)
        REG(q2,         Q2)
        REG(q3,         Q3)
        REG(q4,         Q4)
        REG(q5,         Q5)
        REG(q6,         Q6)
        REG(q7,         Q7)
        REG(q8,         Q8)
        REG(q9,         Q9)
        REG(q10,        Q10)
        REG(q11,        Q11)
        REG(q12,        Q12)
        REG(q13,        Q13)
        REG(q14,        Q14)
        REG(q15,        Q15)
        #undef REG

        default:
            throw core::irgen::InvalidInstructionException(tr("Invalid register number: %1").arg(reg));
        }
    }
};

ArmInstructionAnalyzer::ArmInstructionAnalyzer(const ArmArchitecture *architecture):
    impl_(std::make_unique<ArmInstructionAnalyzerImpl>(architecture))
{}

ArmInstructionAnalyzer::~ArmInstructionAnalyzer() {}

void ArmInstructionAnalyzer::doCreateStatements(const core::arch::Instruction *instruction, core::ir::Program *program) {
    impl_->createStatements(checked_cast<const ArmInstruction *>(instruction), program);
}

}}} // namespace nc::arch::arm

/* vim:set et sts=4 sw=4: */