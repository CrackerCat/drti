// -*- mode:c++ -*-
//
// Module drti-decorate.cpp
//
// Copyright (c) 2019, 2020 Raoul M. Gough
//
// This file is part of DRTI.
//
// DRTI is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, version 3 only.
//
// DRTI is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// History
// =======
// 2019/11/06   rmg     File creation
//

#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

#include <drti/runtime.hpp>
#include <drti/drti-common.hpp>

#include <fstream>
#include <istream>
#include <sstream>
#include <unordered_set>

// These are generated by objcopy from bitcode
extern const char _binary_drti_inline_bc_start[];
extern const char _binary_drti_inline_bc_end[];

namespace drti
{
    struct Decorate : public llvm::ModulePass
    {
        static char ID;

        Decorate();

        bool runOnModule(llvm::Module&) override;
    };
}

char drti::Decorate::ID = 0;

static llvm::RegisterPass<drti::Decorate> registrar1(
    "drti-decorate",
    "Dynamic runtime inlining decoration pass",
    true /* Does modify CFG */,
    false /* Is not an analysis pass */);

static void registerDecorate(
    const llvm::PassManagerBuilder&,
    llvm::legacy::PassManagerBase& PM)
{
    PM.add(new drti::Decorate());
}

// TODO - finding the right place in the pipeline is tricky. Ideally
// we want to run after optimizations so that we serialized
// already-optimized bitcode to help the JIT run faster, but on the
// other hand we almost certainly want another round of inlining after
// we're done. Refer to llvm/lib/Transforms/IPO/PassManagerBuilder.cpp
// for the options details.
static llvm::RegisterStandardPasses registrar2(
    llvm::PassManagerBuilder::EP_OptimizerLast, // EP_ModuleOptimizerEarly, // EP_Peephole, EP_CGSCCOptimizerLate, EP_Peephole, EP_ScalarOptimizerLate, EP_OptimizerLast?
    registerDecorate);

drti::Decorate::Decorate() :
    ModulePass(ID)
{
}

// Check member is at a specific offset
#define CHECK_MEMBER(CLASS, MEMBER, TYPE, OFFSET) \
    static_assert(std::is_same<decltype(std::declval<CLASS>().MEMBER), TYPE>::value); \
    static_assert(offsetof(CLASS, MEMBER) == OFFSET);

// Check adjacency between previous and next members
#define CHECK_MEMBER_P(CLASS, MEMBER, TYPE, PREVIOUS) \
    CHECK_MEMBER(CLASS, MEMBER, TYPE, offsetof(CLASS, PREVIOUS) + sizeof(CLASS::PREVIOUS))

namespace drti
{
    struct InlineHelpers
    {
        InlineHelpers(llvm::Module&);
        bool ok() const;

        llvm::StructType* m_drti_landing_site_type;
        llvm::StructType* m_drti_callsite_type;
        llvm::StructType* m_drti_treenode_type;
        llvm::StructType* m_drti_reflect_type;
        llvm::Function* m_drti_landed;
        llvm::Function* m_drti_call_from;
    };

    class DecoratePass
    {
    public:
        DecoratePass(llvm::Module& module);

        //! Find any target functions
        bool find_target_functions();

        //! Link our inlinable support functions from bitcode
        bool add_helpers();

        bool lookup_helpers();

        void create_self();
        void add_landing_globals();
        llvm::GlobalVariable* create_landing_global(llvm::Function* const);
        llvm::GlobalVariable* create_callsite_global(
            llvm::Function* const,
            llvm::GlobalVariable* landing_global,
            unsigned call_number);

    private:
        llvm::SmallVector<llvm::GlobalValue*, 10> collect_globals();
        llvm::SmallVector<char, 0> raw_bitcode();

        llvm::Value* add_landing_update(
            llvm::Function*, llvm::GlobalVariable*);
        void decorate_call(
            llvm::Value*, llvm::CallBase*, llvm::GlobalVariable*);

        std::vector<std::pair<unsigned, llvm::CallBase*>> collect_calls(
            llvm::Function* function);

        void decorate_calls(
            const std::vector<std::pair<unsigned, llvm::CallBase*>>& collected,
            llvm::Value*, llvm::GlobalVariable*);

        static std::unordered_set<std::string> targets_from_environment();
        static void split_stream(std::istream&, std::unordered_set<std::string>&);

        //! The names of functions we want to decorate for landing
        //! purposes, as well as the names of call targets that we
        //! decorate from within those targets.
        std::unordered_set<std::string> m_target_function_names;
        llvm::Module& m_module;
        //! Function declarations and definitions in this module from
        //! our set of target names
        llvm::DenseSet<llvm::Function*> m_target_functions;
        //! For now we decorate outgoing calls to named functions and
        //! also via function pointers of the type of any function
        //! types we found function declarations
        llvm::DenseSet<llvm::Type*> m_target_function_types;
        std::optional<InlineHelpers> m_inline;
        llvm::GlobalVariable* m_reflect_global;
    };
};

void drti::DecoratePass::split_stream(
    std::istream& stream, std::unordered_set<std::string>& symbols)
{
    std::string symbol;
    while(stream >> symbol)
    {
        symbols.insert(symbol);
    }
}

std::unordered_set<std::string> drti::DecoratePass::targets_from_environment()
{
    std::unordered_set<std::string> result;

    const char* target_names = getenv("DRTI_TARGET_NAMES");
    if(target_names)
    {
        DEBUG_WITH_TYPE(
            "drti", llvm::dbgs() << "drti: parsing DRTI_TARGET_NAMES environment variable\n");
        std::istringstream stream{std::string(target_names)};
        split_stream(stream, result);
    }

    const char* targets_file = getenv("DRTI_TARGETS_FILE");
    if(targets_file)
    {
        DEBUG_WITH_TYPE(
            "drti", llvm::dbgs()
            << "drti: parsing from DRTI_TARGETS_FILE "
            << targets_file
            << "\n");
        std::ifstream stream(targets_file);
        split_stream(stream, result);
    }

    if(result.empty())
    {
        llvm::report_fatal_error(
            "No target functions found. "
            "Have you set DRTI_TARGET_NAMES and/or DRTI_TARGETS_FILE?");
    }

    return result;
}

drti::DecoratePass::DecoratePass(llvm::Module& module) :
    m_target_function_names(targets_from_environment()),
    m_module(module),
    m_target_functions(),
    m_target_function_types(),
    m_inline(),
    m_reflect_global(nullptr)
{
}

llvm::SmallVector<llvm::GlobalValue*, 10> drti::DecoratePass::collect_globals()
{
    llvm::SmallVector<llvm::GlobalValue*, 10> result;

    visit_listed_globals(
        m_module,
        [&result](llvm::GlobalVariable& variable) {
            DEBUG_WITH_TYPE(
                "drti", llvm::dbgs() << "drti: noting extern " << variable.getName() << "\n");

            result.push_back(&variable);
        });

    for(llvm::Function& function: m_module.functions())
    {
        if(function.isIntrinsic())
        {
            // Ignore
        }
        else if(function.isDeclaration())
        {
            // Save declarations for runtime global resolution
            // IMPORTANT - filtering here must match the same
            // functions as in globalsMap from runtime.cpp
            DEBUG_WITH_TYPE("drti", llvm::dbgs() << "drti: noting extern " << function.getName() << "\n");
            result.push_back(&function);
        }
        else
        {
            // Make sure all function definitions can be optimized and
            // potentially inlined. This is currently necessary
            // because we run clang with no optimizations and this marks
            // the functions in the bitcode
            function.removeFnAttr(llvm::Attribute::OptimizeNone);
            function.removeFnAttr(llvm::Attribute::NoInline);
        }
    }

    return result;
}

bool drti::DecoratePass::find_target_functions()
{
    for(llvm::Function& function: m_module.functions())
    {
        if(m_target_function_names.find(function.getName().str()) !=
           m_target_function_names.end())
        {
            if(!function.isDeclaration())
            {
                DEBUG_WITH_TYPE(
                    "drti",
                    llvm::dbgs() << "drti: found target function definition "
                    << function.getName() << "\n");
            }

            m_target_functions.insert(&function);
            m_target_function_types.insert(function.getFunctionType());
        }
    }

    if(m_target_functions.empty())
    {
        DEBUG_WITH_TYPE(
            "drti",
            llvm::dbgs() << "drti: No target functions found in module\n");
        return false;
    }
    else
    {
        DEBUG_WITH_TYPE(
            "drti",
            llvm::dbgs() << "drti: "
            << m_target_functions.size() << " target functions\n");
        return true;
    }
}

bool drti::DecoratePass::add_helpers()
{
    llvm::StringRef string(
        _binary_drti_inline_bc_start,
        _binary_drti_inline_bc_end - _binary_drti_inline_bc_start);

    auto buffer(
        llvm::MemoryBuffer::getMemBuffer(string, "bitcode", false));

    llvm::Expected<std::unique_ptr<llvm::Module>> maybeModule(
        llvm::parseBitcodeFile(*buffer, m_module.getContext()));

    if(!maybeModule)
    {
        llvm::report_fatal_error(maybeModule.takeError());
    }

    llvm::Linker linker(m_module);

    if(linker.linkInModule(std::move(*maybeModule)))
    {
        // ERROR. We assume linkInModule wrote some diagnostics to
        // stderr or such
        return false;
    }
    else
    {
        return true;
    }
}

drti::InlineHelpers::InlineHelpers(llvm::Module& module) :
    m_drti_landing_site_type(
        module.getTypeByName("struct.drti::landing_site")),
    m_drti_callsite_type(
        module.getTypeByName("struct.drti::static_callsite")),
    m_drti_treenode_type(
        module.getTypeByName("struct.drti::treenode")),
    m_drti_reflect_type(
        module.getTypeByName("struct.drti::reflect")),
    m_drti_landed(
        module.getFunction("_drti_landed")),
    m_drti_call_from(
        module.getFunction("_drti_call_from"))
{
    // Check that the compile-time structure types in tree.hpp haven't
    // changed since we hard-coded their setup here
    CHECK_MEMBER(reflect, module, const char*, 0);
    CHECK_MEMBER_P(reflect, module_size, size_t, module);
    CHECK_MEMBER_P(reflect, globals, void* const*, module_size);
    CHECK_MEMBER_P(reflect, globals_size, size_t, globals);

    CHECK_MEMBER(landing_site, total_called, counter_t, 0);
    CHECK_MEMBER_P(landing_site, global_name, const char*, total_called);
    CHECK_MEMBER_P(landing_site, function_name, const char*, global_name);
    CHECK_MEMBER_P(landing_site, self, reflect*, function_name);
}

bool drti::InlineHelpers::ok() const
{
    if(!m_drti_landing_site_type ||
       !m_drti_callsite_type ||
       !m_drti_treenode_type ||
       !m_drti_reflect_type)
    {
        DEBUG_WITH_TYPE(
            "drti", llvm::dbgs() << "drti: type(s) not found in module\n");
        return false;
    }
    else if (!m_drti_landed || !m_drti_call_from)
    {
        DEBUG_WITH_TYPE(
            "drti", llvm::dbgs() << "drti: support function(s) not found in module\n");
        return false;
    }
    else
    {
        return true;
    }
}

bool drti::DecoratePass::lookup_helpers()
{
    m_inline.emplace(m_module);

    return m_inline->ok();
}

llvm::SmallVector<char, 0> drti::DecoratePass::raw_bitcode()
{
    llvm::SmallVector<char, 0> buffer;
    llvm::BitcodeWriter writer(buffer);
    writer.writeModule(m_module);
    writer.writeStrtab();
    return buffer;
}

void drti::DecoratePass::create_self()
{
    // We need to collect the globals after linking the helpers
    // because we need to match (exactly) the dumped bitcode
    llvm::SmallVector<llvm::GlobalValue*, 10> globals(collect_globals());

    // Dump the module as bitcode in its current state (before actual
    // decoration) and save this in a global variable in the module so
    // it can be deserialized at runtime.
    llvm::SmallVector<char, 0> buffer = raw_bitcode();
    llvm::Constant* as_constant(
        llvm::ConstantDataArray::get(
            m_module.getContext(),
            llvm::makeArrayRef(
                buffer.data(), buffer.size())));

    auto bitcode_global = new llvm::GlobalVariable(
        m_module,
        as_constant->getType(), true, llvm::GlobalValue::InternalLinkage,
        as_constant, "__drti_bitcode");

    llvm::PointerType* void_star = llvm::IntegerType::get(
        m_module.getContext(), 8)->getPointerTo();

    llvm::Constant* cast_bitcode = llvm::ConstantExpr::getBitCast(
        bitcode_global, void_star);

    // Create void* pointers for all the globals (variables and functions)
    llvm::SmallVector<llvm::Constant*, 0> extern_addresses;
    extern_addresses.reserve(globals.size());
    for(llvm::GlobalValue* extern_: globals)
    {
        extern_addresses.push_back(
            llvm::ConstantExpr::getBitCast(extern_, void_star));
    }

    llvm::Constant* globals_array = llvm::ConstantArray::get(
        llvm::ArrayType::get(
            void_star, extern_addresses.size()),
        llvm::makeArrayRef(
            extern_addresses.data(), extern_addresses.size()));

    // Create a global array to store the addresses
    auto globals_variable = new llvm::GlobalVariable(
        m_module,
        globals_array->getType(), true, llvm::GlobalValue::InternalLinkage,
        globals_array, "__drti_globals");

    llvm::Constant* cast_globals = llvm::ConstantExpr::getBitCast(
        globals_variable, void_star->getPointerTo());

    llvm::Constant* reflect_members[4] = {
        cast_bitcode,
        llvm::ConstantInt::get(
            llvm::IntegerType::get(
                m_module.getContext(), 64), buffer.size()),
        cast_globals,
        llvm::ConstantInt::get(
            llvm::IntegerType::get(
                m_module.getContext(), 64), extern_addresses.size()),
    };

    llvm::Constant* reflect_constant =
        llvm::ConstantStruct::get(m_inline->m_drti_reflect_type, reflect_members);

    m_reflect_global = new llvm::GlobalVariable(
        m_module,
        m_inline->m_drti_reflect_type, true, llvm::GlobalValue::InternalLinkage,
        reflect_constant, "__drti_self");

    DEBUG_WITH_TYPE(
        "drti",
        llvm::dbgs() << "drti: inserted __drti_self of size "
        << buffer.size() << "\n");
}

llvm::Value* drti::DecoratePass::add_landing_update(
    llvm::Function* function,
    llvm::GlobalVariable* landing_global)
{
    // Split the entry basic block after any alloca instructions and
    // insert logic that checks for an incoming treenode pointer and
    // updates the landing global.

    // We arrive at code like this:
    // entry:
    //    alloca instruction(s)
    //    drtiRetAddress = __builtin_return_address(0)
    //    aligned = (drtiRetAddress % (DRTI_RETALIGN - 1)) == 0
    //    br i1 aligned, drti_land2, drti_land1
    //
    // drti_land1:
    //    caller = phi [ nullptr, entry ], [ nullptr, drti_land2 ], [ treenode, drti_land3 ]
    //    original entry terminator
    //    <remaining function body>
    //
    // drti_land2:
    //    maybe_magic = ((const uint64_t*)drtiRetAddress)[-DRTI_RETALIGN / 8]
    //    has_magic = maybe_magic == DRTI_MAGIC
    //    br i1 has_magic drti_land3, drti_land1
    //
    // drti_land3:
    //    treenode = _drti_caller()
    //    call _drti_landed(landing_global, treenode)
    //    br drti_land1

    llvm::BasicBlock* entryBlock = &function->getEntryBlock();
    llvm::Instruction* splitPoint = entryBlock->getTerminator();
    for(llvm::Instruction& instruction: *entryBlock)
    {
        if(!llvm::isa<llvm::AllocaInst>(instruction))
        {
            splitPoint = &instruction;
            break;
        }
    }
    if(!splitPoint)
    {
        llvm::report_fatal_error(
            "drti-decorate: Malformed entry block in function "
            + function->getName().str());
    }

    llvm::BasicBlock* land1 = entryBlock->splitBasicBlock(
        splitPoint, "drti_land1");
    llvm::BasicBlock* land2 = llvm::BasicBlock::Create(
        m_module.getContext(), "drti_land2", function, nullptr);
    llvm::BasicBlock* land3 = llvm::BasicBlock::Create(
        m_module.getContext(), "drti_land3", function, nullptr);

    // Remove the unconditional branch inserted by splitBasicBlock
    llvm::IRBuilder<> builder(
        entryBlock, entryBlock->back().eraseFromParent());

    //    drtiRetAddress = __builtin_return_address(0)
    //    aligned = (drtiRetAddress % (DRTI_RETALIGN - 1)) == 0
    //    br i1 aligned, drti_land2, drti_land1
    llvm::Constant* zero32 = llvm::ConstantInt::get(
        llvm::IntegerType::get(m_module.getContext(), 32), 0);
    llvm::Constant* zero64 = llvm::ConstantInt::get(
        llvm::IntegerType::get(m_module.getContext(), 64), 0);
    llvm::Value* zeroArg[] = { zero32 };
    llvm::Type* voidpType = llvm::IntegerType::get(
        m_module.getContext(), 8)->getPointerTo();
    llvm::FunctionCallee builtinRet = m_module.getOrInsertFunction(
        "llvm.returnaddress", voidpType, zero32->getType());
    llvm::Value* returnAddress = builder.CreateCall(
        builtinRet, zeroArg, "drtiRetAddress");

    llvm::Constant* drtiRetAlign = llvm::ConstantInt::get(
        llvm::IntegerType::get(m_module.getContext(), 64), DRTI_RETALIGN - 1);

    returnAddress = builder.CreatePtrToInt(
        returnAddress, drtiRetAlign->getType(), "drtiRetAddressCast");
    llvm::Value* mod = builder.CreateAnd(
        returnAddress, drtiRetAlign, "drtiAndRetalign");

    llvm::Value* isZero = builder.CreateICmpEQ(mod, zero64, "drtiRetIsAligned");
    builder.CreateCondBr(isZero, land2, land1);

    // drti_land1:
    //    caller = phi [ nullptr, entry ], [ nullptr, drti_land2 ], [ treenode, drti_land3 ]
    builder.SetInsertPoint(land1, land1->begin());
    llvm::PointerType* treenode_pointer_type =
        m_inline->m_drti_treenode_type->getPointerTo();
    llvm::Constant* null_treenode_pointer = llvm::ConstantPointerNull::get(
        treenode_pointer_type);
    llvm::PHINode* caller = builder.CreatePHI(
        treenode_pointer_type, 3, "drtiCallerTreenode");

    // drti_land2:
    //    maybe_magic = ((const uint64_t*)drtiRetAddress)[-DRTI_RETALIGN / 8]
    //    has_magic = maybe_magic == DRTI_MAGIC
    //    br i1 has_magic drti_land3, drti_land1
    builder.SetInsertPoint(land2);
    llvm::Type* ptr_int64_t =
        llvm::IntegerType::get(m_module.getContext(), 64)->getPointerTo();
    llvm::Value* returnAddressCast = builder.CreateIntToPtr(
        returnAddress, ptr_int64_t, "drtiRetAddressCast");
    static_assert(DRTI_RETALIGN % sizeof(int64_t) == 0, "Invalid DRTI_RETALIGN");
    llvm::Value* indexes[] = {
        llvm::ConstantInt::get(
            llvm::IntegerType::get(m_module.getContext(), 64),
            -DRTI_RETALIGN / sizeof(int64_t))
    };
    llvm::Value* gep = builder.CreateGEP(returnAddressCast, indexes, "drtiGep");
    llvm::Value* maybeMagic = builder.CreateLoad(gep, "drtiMaybeMagic");
    llvm::Constant* magic = llvm::ConstantInt::get(
        llvm::IntegerType::get(m_module.getContext(), 64), DRTI_MAGIC);
    llvm::Value* matches = builder.CreateICmpEQ(maybeMagic, magic, "drtiMatches");
    builder.CreateCondBr(matches, land3, land1);

    // drti_land3:
    //    call _drti_landed(landing_global, _drti_caller())
    //    br drti_land1
    builder.SetInsertPoint(land3);
    llvm::FunctionCallee drtiCaller(
        m_module.getOrInsertFunction("_drti_caller", treenode_pointer_type));
    llvm::Value* treenode = builder.CreateCall(
        drtiCaller, llvm::None, "drtiTreenode");

    llvm::Value* arguments[] = { landing_global, treenode };

    DEBUG_WITH_TYPE(
        "drti",
        llvm::dbgs()
        << "drti: adding call to " << m_inline->m_drti_landed->getName()
        << " from " << function->getName() << "\n");

    builder.CreateCall(m_inline->m_drti_landed, arguments);
    builder.CreateBr(land1);

    caller->addIncoming(null_treenode_pointer, entryBlock);
    caller->addIncoming(null_treenode_pointer, land2);
    caller->addIncoming(treenode, land3);

    return caller;
}

void drti::DecoratePass::decorate_call(
    llvm::Value* caller,
    llvm::CallBase* callInst,
    llvm::GlobalVariable* callsite)
{
    // Insert new code before the original call
    llvm::IRBuilder<> builder(callInst);

    // Cast the original target to void* for passing to
    // _drti_call_from. i8* is equivalent to void*
    llvm::Value* oldTarget = builder.CreateBitCast(
        callInst->getCalledOperand(),
        llvm::IntegerType::get(m_module.getContext(), 8)->getPointerTo(),
        "castOldTarget");

    llvm::Value* callFromArgs[] = {
        callsite, caller, oldTarget
    };

    llvm::Value* treenode = builder.CreateCall(
        m_inline->m_drti_call_from, callFromArgs, "treenode");

    // We do two things here - replace the target of the call with
    // the (casted) treenode's resolved_target function pointer and
    // replace the first argument with the treenode

    llvm::Value* resolved_target = builder.CreateStructGEP(
        treenode, 5, "resolved_target");

    llvm::Value* newTarget = builder.CreateBitCast(
        builder.CreateLoad(resolved_target),
        callInst->getCalledOperand()->getType(),
        "castResolvedTarget");

    // This has to go immediately before the target call, and gets
    // rewritten in our machine code pass
    llvm::FunctionCallee drtiSetCaller(
        m_module.getOrInsertFunction(
            "_drti_set_caller",
            llvm::Type::getVoidTy(m_module.getContext()),
            treenode->getType()));
    llvm::Value* setCallerArgs[] = { treenode };
    builder.CreateCall(drtiSetCaller, setCallerArgs);

    // reset the call target
    callInst->setCalledOperand(newTarget);

    // Prevent tail-call optimisations on the "decorated" call. We
    // need it to be a genuine call for the hidden DRTI argument
    // passing, which is based on return address magic and implemented
    // by drti-target.cpp, to work.
    if(auto* downcast = llvm::dyn_cast<llvm::CallInst>(callInst))
    {
        downcast->setTailCallKind(llvm::CallInst::TCK_NoTail);
    }
}

std::vector<std::pair<unsigned, llvm::CallBase*>> drti::DecoratePass::collect_calls(
    llvm::Function* function)
{
    std::vector<std::pair<unsigned, llvm::CallBase*>> result;

    // For each onward call we decide whether it needs decoration or
    // not. We must do this before modifying the function so that the
    // numbering can be reproduced at runtime from the saved bitcode
    unsigned call_number = 0;
    for(llvm::BasicBlock& block: *function)
    {
        for(llvm::Instruction& instruction: block)
        {
            auto callInst = llvm::dyn_cast<llvm::CallBase>(&instruction);
            if(callInst && !callInst->isInlineAsm())
            {
                llvm::Value* callee = callInst->getCalledOperand();
                auto global = llvm::dyn_cast<llvm::Function>(callee);

                const llvm::Type* type = callee->getType();
                if(auto pointer = llvm::dyn_cast<llvm::PointerType>(type))
                {
                    type = pointer->getElementType();
                }

                DEBUG_WITH_TYPE(
                    "drti",
                    llvm::dbgs()
                    << "drti: "
                    << function->getName().str()
                    << " call_number "
                    << call_number
                    << " "
                    << (global ?
                        global->getName().str() :
                        std::string("pointer"))
                    << "\n");

                if(m_target_function_types.find(type) != m_target_function_types.end())
                {
                    if(global && (m_target_functions.find(global) == m_target_functions.end()))
                    {
                        // Call is direct to a function declaration
                        // and it's not one of our targets, so we
                        // don't decorate it
                    }
                    else
                    {
                        // Otherwise it's direct to one of our targets
                        // or via a function pointer and we decorate
                        // it in either case.
                        // TODO - handle direct calls more simply than pointer calls
                        DEBUG_WITH_TYPE(
                            "drti",
                            llvm::dbgs()
                            << "drti: collecting call to "
                            << m_inline->m_drti_call_from->getName()
                            << " from " << function->getName()
                            << " call_number "
                            << call_number
                            << "\n");

                        result.push_back({call_number, callInst});
                    }
                }

                ++call_number;
            }
        }
    }
    return result;
}

void drti::DecoratePass::decorate_calls(
    const std::vector<std::pair<unsigned, llvm::CallBase*>>& collected,
    llvm::Value* caller,
    llvm::GlobalVariable* landing_global)
{
    // For each onward call to be decorated we need to create a static
    // callsite and invoke _drti_call_from(callsite, caller,
    // call_target) and replace the call target with the return
    // value. Our caller is determined from our own landing site code.

    for(const auto& [call_number, callInst]: collected)
    {
        llvm::GlobalVariable* callsite_global(
            create_callsite_global(
                callInst->getParent()->getParent(),
                landing_global,
                call_number));

        decorate_call(caller, callInst, callsite_global);
    }
}

void drti::DecoratePass::add_landing_globals()
{
    // For each target function definition we add a static landing
    // site object and decorate any suitable outgoing calls
    for(llvm::Function* function: m_target_functions)
    {
        if(!function->isDeclaration())
        {
            // Find any outgoing calls to be decorated, before
            // modifying the function
            std::vector<std::pair<unsigned, llvm::CallBase*>> calls(
                collect_calls(function));

            llvm::GlobalVariable* landing_global = create_landing_global(function);

            llvm::Value* caller = add_landing_update(function, landing_global);

            decorate_calls(calls, caller, landing_global);

            // prints to dbgs()
            llvm::FunctionAnalysisManager DummyFAM;
            llvm::PrintFunctionPass().run(*function, DummyFAM);
        }
    }
}

llvm::GlobalVariable* drti::DecoratePass::create_landing_global(
    llvm::Function* const function)
{
    std::string variableName = "_drti_landing_" + function->getName().str();

    llvm::Constant* name_initializer = llvm::ConstantDataArray::getString(
        m_module.getContext(), variableName);

    auto name_global = new llvm::GlobalVariable(
        m_module,
        name_initializer->getType(), true, llvm::GlobalValue::InternalLinkage,
        name_initializer, "__drti_landing_site_name");

    llvm::Constant* function_name_initializer = llvm::ConstantDataArray::getString(
        m_module.getContext(), function->getName());

    // TODO - probably we don't need the function or global names, we
    // could just use ordinal, assuming function and global variable
    // iteration order is stable across the bitcode write/read cycle.
    auto function_name_global = new llvm::GlobalVariable(
        m_module,
        function_name_initializer->getType(),
        true, llvm::GlobalValue::InternalLinkage,
        function_name_initializer, "__drti_landing_site_function_name",
        name_global);

    llvm::Constant* zero =
        llvm::ConstantInt::get(
            llvm::IntegerType::get(m_module.getContext(), 64), 0);

    llvm::Constant* landing_site_members[] = {
        // total_called
        zero,
        // global_name (cast to remove the array type)
        llvm::ConstantExpr::getBitCast(
            name_global,
            llvm::IntegerType::get(m_module.getContext(), 8)->getPointerTo()),
        // function_name (cast to remove the array type)
        llvm::ConstantExpr::getBitCast(
            function_name_global,
            llvm::IntegerType::get(m_module.getContext(), 8)->getPointerTo()),
        // self
        m_reflect_global
    };

    llvm::Constant* landing_site_constant =
        llvm::ConstantStruct::get(
            m_inline->m_drti_landing_site_type, landing_site_members);

    auto variable = new llvm::GlobalVariable(
        m_module,
        landing_site_constant->getType(),
        false, llvm::GlobalValue::InternalLinkage,
        landing_site_constant, variableName,
        function_name_global);

    return variable;
}

llvm::GlobalVariable* drti::DecoratePass::create_callsite_global(
    llvm::Function* const function,
    llvm::GlobalVariable* landing_global,
    unsigned call_number)
{
    llvm::Constant* zero =
        llvm::ConstantInt::get(
            llvm::IntegerType::get(m_module.getContext(), 64), 0);

    static_assert(sizeof(unsigned) == 4, "32-bit integer unsigned representation expected");
    llvm::Constant* callsite_members[] = {
        // total_calls
        zero,
        // &landing_site
        landing_global,
        llvm::ConstantInt::get(
            llvm::IntegerType::get(m_module.getContext(), 32), call_number),
        // vector
        llvm::ConstantAggregateZero::get(
            m_inline->m_drti_callsite_type->getElementType(3))
    };

    llvm::Constant* callsite_constant =
        llvm::ConstantStruct::get(
            m_inline->m_drti_callsite_type, callsite_members);

    auto variable = new llvm::GlobalVariable(
        m_module,
        callsite_constant->getType(),
        false, llvm::GlobalValue::InternalLinkage,
        callsite_constant,
        "_drti_callsite_" + function->getName().str());

    return variable;
}

bool drti::Decorate::runOnModule(llvm::Module& module)
{
    const std::string& targetTriple(module.getTargetTriple());
    if(targetTriple != "x86_64-unknown-linux-gnu")
    {
        llvm::dbgs() << "drti: skipping module for target " << targetTriple << "\n";
        return false;
    }

    DecoratePass decorator(module);

    if(!decorator.find_target_functions())
    {
        // Module is not of interest to us
        return false;
    }

    // Link in our support module
    if(!decorator.add_helpers())
    {
        return false;
    }

    if(!decorator.lookup_helpers())
    {
        return true;
    }

    // Unfortunately this will include the support module which we
    // really don't want in the JIT-time compilation
    decorator.create_self();

    decorator.add_landing_globals();
//    decorator.set_initializers();

    // This lets our machine code passes run on the module as well
    module.setTargetTriple("x86_64_drti-unknown-linux-gnu");

    return true;
}
