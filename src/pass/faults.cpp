/***********************************************************************************************/
/* This file is licensed under the University of Illinois/NCSA Open Source License.            */
/* See LICENSE.TXT for details.                                                                */
/***********************************************************************************************/


/***********************************************************************************************/
/*                                                                                             */
/* Name: faults.cpp                                                                            */
/*                                                                                             */
/* Description: LLVM IR compiler pass. This pass add calls to corrupting functions that will   */
/*              corrupt (single bit-flip) the result of LLVM instructions with a certain       */
/*              probability.                                                                   */
/*                                                                                             */
/***********************************************************************************************/

#include "./faults.h"
#include <algorithm>
#include <vector>
#include <string>


DynamicFaults::DynamicFaults() : ModulePass(ID) {
    func_corruptIntData_8bit = NULL;
    func_corruptIntData_16bit = NULL;
    func_corruptIntData_32bit = NULL;
    func_corruptIntData_64bit = NULL;
    func_corruptPtr2Int_64bit = NULL;
    func_corruptFloatData_32bit = NULL;
    func_corruptFloatData_64bit = NULL;
    func_corruptIntAdr_8bit = NULL;
    func_corruptIntAdr_16bit = NULL;
    func_corruptIntAdr_32bit = NULL;
    func_corruptIntAdr_64bit = NULL;
    func_corruptFloatAdr_32bit = NULL;
    func_corruptFloatAdr_64bit = NULL;
}

bool DynamicFaults::runOnModule(Module &M) {
    srand(time(NULL));
    if (byte_val < -1 || byte_val > 7)
        byte_val = rand() % 8;

    /* Check for assertion violation(s) */
    assert(byte_val <= 7 && byte_val >= -1);
    assert(siteProb >= 0. && siteProb < 1.);
    assert(ijo == 1 || ijo == 0);
    assert(ptr_err == 1 || ptr_err == 0);
    assert(arith_err == 1 || arith_err == 0);
    assert(ptr_err == 1 || ptr_err == 0);


    Module::FunctionListType &functionList = M.getFunctionList();
    vector<string> flist = splitAtSpace(func_list);
    unsigned faultIdx = 0;
    unsigned displayIdx = 0;


    init(faultIdx, displayIdx);

    /*Cache function references of the function defined in Corrupt.c to all inserting of
     *call instructions to them */
    cacheFunctions(functionList);

    /*Cache instructions from all the targetable functions for fault injection in case the
     * list of functions is not defined default to inject into all function inside the file. */
    for (Module::iterator it = functionList.begin(); it != functionList.end(); ++it) {

        /* extract the pure function name, i.e. demangle if using c++*/
        string cstr = demangle(it->getName().str());

        if (cstr.find("corruptIntData_8bit") != std::string::npos  ||
            cstr.find("corruptIntData_16bit") != std::string::npos   ||
            cstr.find("corruptIntData_32bit") != std::string::npos   ||
            cstr.find("corruptIntData_64bit") != std::string::npos   ||
            cstr.find("corruptPtr2Int_64bit") != std::string::npos   ||
            cstr.find("corruptFloatData_32bit") != std::string::npos ||
            cstr.find("corruptFloatData_64bit") != std::string::npos ||
            cstr.find("corruptIntAdr_8bit") != std::string::npos    ||
            cstr.find("corruptIntAdr_16bit") != std::string::npos    ||
            cstr.find("corruptIntAdr_32bit") != std::string::npos    ||
            cstr.find("corruptIntAdr_64bit") != std::string::npos    ||
            cstr.find("corruptFloatAdr_32bit") != std::string::npos  ||
            cstr.find("corruptFloatAdr_64bit") != std::string::npos  ||
            !cstr.compare("main"))
            continue;

         if (funcProbs.find(cstr) != funcProbs.end())
            if (funcProbs[cstr] == 0)
                continue;

        Function* F = NULL;
        /*if the user defined function list is empty or the currently selected function is in the list of
         * user defined function list then consider the function for fault injection*/
        if (func_list.length() == 0 || std::find(flist.begin(), flist.end(), cstr) != flist.end()) {
            F = it;
        } else {
            continue;
        }
        if (F->begin() == F->end())
            continue;

        /*Cache instruction references with in a function to be considered for fault injection*/
        std::vector<Instruction*> ilist;
        errs() << "\n\nFunction Name: " << cstr
            << "\n------------------------------------------------------------------------------\n";
        enumerateSites(ilist, F, displayIdx);


        /*If the list of instruction to corrupt is not empty add code for fault injection */
        if (!ilist.empty())
            injectFaults(ilist, faultIdx);
    }/*end for*/


    finalize(faultIdx, displayIdx);
    return false;
}

string DynamicFaults::demangle(string name)
{
    int status;
    string demangled;
    char* tmp = abi::__cxa_demangle(name.c_str(), NULL, NULL, &status);
    if (tmp == NULL)
        return name;
    
    demangled = tmp;
    free(tmp);
    /* drop the parameter list as we only need the function name */
    return demangled.find("(") == string::npos ? demangled : demangled.substr(0, demangled.find("("));
}

void  DynamicFaults::init(unsigned int& faultIdx, unsigned int& displayIdx) {
    ifstream infile;
    string path(getenv("HOME"));
    path += "/.FlipItState";

    infile.open(path.c_str());
    if (infile.is_open()) {
        infile >> faultIdx >> displayIdx;
    } else {
        faultIdx = 0;
        displayIdx = 0;
    }
    infile.close();

    readConfig(configPath);
}

void DynamicFaults::readConfig(string path) {
    ifstream infile;
    string line;
    infile.open(path.c_str());

    // read inst probs
    getline(infile, line); // INSTRUCTIONS:
    getline(infile, line);
    while (line != "FUNCTIONS:" && infile) {
        unsigned long found = line.find("=");

        if (found != string::npos)
            instProbs[line.substr(0, found)] = atof(line.substr(found+1).c_str());
        getline(infile, line);
    }

    // read func probs
    while (infile) {
        unsigned long found = line.find("=");

        if (found != string::npos && line[0] != '#')
            funcProbs[line.substr(0, found)] = atof(line.substr(found+1).c_str());
        getline(infile, line);
    }
    infile.close();
}


void  DynamicFaults::finalize(unsigned int& faultIdx, unsigned int& displayIdx) {
    ofstream outfile;
    string path(getenv("HOME"));
    path += "/.FlipItState";

    outfile.open(path.c_str() , ios::trunc);
    if (!outfile.is_open()) {
        errs() << "WARNING: unable to update injector state file at path: " << path << "\n";
    } else {
        outfile << faultIdx << " " << displayIdx;
    }
    outfile.close();
}

vector<string> DynamicFaults::splitAtSpace(string spltStr) {
    std::vector<std::string> strLst;
    std::istringstream isstr(spltStr);
    copy(std::istream_iterator<std::string>(isstr), std::istream_iterator<std::string>(),
	   std::back_inserter<std::vector<std::string> >(strLst));

    return strLst;
}

double DynamicFaults::getInstProb(Instruction* I) {
    /*First check if it is a call to a function listed in the config file*/
    if (CallInst *callInst = dyn_cast<CallInst>(I)) {
        if (callInst->getCalledFunction() == NULL) /* function pointers will be null */
            return 0;

        string funcName = callInst->getCalledFunction()->getName().str();
        if (funcProbs.find(funcName) != funcProbs.end())
            return funcProbs[funcName];
    }

    /* Get the probability from the instruction's type from the config
    file or the default probabilty given as a command line argument */
    string type = I->getOpcodeName();
    return instProbs.find(type) != instProbs.end() ? instProbs[type] : siteProb;
}

bool DynamicFaults::injectControl(Instruction* I, int faultIndex) {
    if (I == NULL)
        return false;

    /* Locate the instruction I in the basic block BB */
    BasicBlock *BB = I->getParent();
    BasicBlock::iterator BI;
    for (BI = BB->begin(); BI != BB->end(); BI++)
        if (BI == *I)
            break;

    /* Build argument list before calling Corrupt function */
    CallInst* CallI = NULL;
    std::vector<Value*> args;
    args.push_back(ConstantInt::get(IntegerType::getInt32Ty(getGlobalContext()), faultIndex));
    args.push_back(ConstantInt::get(IntegerType::getInt32Ty(getGlobalContext()), ijo));
    args.push_back(ConstantFP::get(Type::getDoubleTy(getGlobalContext()), getInstProb(I)));
    args.push_back(ConstantInt::get(IntegerType::getInt32Ty(getGlobalContext()), byte_val));


    /* Choose a fault site in CmpInst and insert Corrupt function call */
    if (isa<CmpInst>(I))
        return inject_Compare(I, args, CallI);


    /* check to see if instruction modifies a looping variable such as i++
        if so we need to inject into it and mark the injection type 'control' */
    if (!isa<CmpInst>(I) && !isa<StoreInst>(I))
        if (I->getName().str().find("indvars") == 0
            || I->getName().str().substr(0, 3) == "inc")
            return inject_Generic(I, args, CallI, BB);

    return false;
}


bool DynamicFaults::injectArithmetic(Instruction* I, int faultIndex) {
    if (I == NULL)
        return false;

    /* Locate the instruction I in the basic block BB */
    BasicBlock *BB = I->getParent();
    BasicBlock::iterator BI;
    for (BI = BB->begin(); BI != BB->end(); BI++)
        if (BI == *I)
            break;

    /* Build argument list before calling Corrupt function */
    CallInst* CallI = NULL;
    std::vector<Value*> args;
    args.push_back(ConstantInt::get(IntegerType::getInt32Ty(getGlobalContext()), faultIndex));
    args.push_back(ConstantInt::get(IntegerType::getInt32Ty(getGlobalContext()), ijo));
    args.push_back(ConstantFP::get(Type::getDoubleTy(getGlobalContext()), getInstProb(I)));
    args.push_back(ConstantInt::get(IntegerType::getInt32Ty(getGlobalContext()), byte_val));


    if (isa<CallInst>(I))
        return false;
    /* store instruction required differnt injection logic than binary operators
    *   (add mul div) and loads */
    if (isa<StoreInst>(I))
        return inject_Store_Data(I, args, CallI);

    if (!isa<CmpInst>(I) && !isa<StoreInst>(I)) {
        return inject_Generic(I, args, CallI, BB);
    }
    return false;
}


bool DynamicFaults::injectPointer(Instruction* I, int faultIndex) {
    if (I == NULL)
        return false;

    CallInst* CallI = NULL;
    /*Build argument list before calling Corrupt function*/
    std::vector<Value*> args;
    args.push_back(ConstantInt::get(IntegerType::getInt32Ty(getGlobalContext()), faultIndex));
    args.push_back(ConstantInt::get(IntegerType::getInt32Ty(getGlobalContext()), ijo));
    args.push_back(ConstantFP::get(Type::getDoubleTy(getGlobalContext()), getInstProb(I)));
    args.push_back(ConstantInt::get(IntegerType::getInt32Ty(getGlobalContext()), byte_val));

    /*Locate the instruction I in the basic block BB*/
    BasicBlock *BB = I->getParent();
    BasicBlock::iterator BI, BINext;
    for (BI = BB->begin(); BI != BB->end(); BI++)
        if (BI == *I)
            break;

    if (isa<StoreInst>(I))
        return inject_Store_Ptr(I, args, CallI);

    if (isa<LoadInst>(I))
        return inject_Load_Ptr(I, args, CallI, BI, BB);

    if (isa<AllocaInst>(I))
       return inject_Alloc_Ptr(I, args, CallI, BI, BB);

    if (isa<GetElementPtrInst>(I))
        return inject_GetElementPtr_Ptr(I, args, CallI, BI, BB);

    return false;
}

bool DynamicFaults::injectCall(Instruction* I, int faultIndex) {
    if (I == NULL)
        return false;

    CallInst* CallI = NULL;
    /*Build argument list before calling Corrupt function*/
    std::vector<Value*> args;
    args.push_back(ConstantInt::get(IntegerType::getInt32Ty(getGlobalContext()), faultIndex));
    args.push_back(ConstantInt::get(IntegerType::getInt32Ty(getGlobalContext()), ijo));
    args.push_back(ConstantFP::get(Type::getDoubleTy(getGlobalContext()), getInstProb(I)));
    args.push_back(ConstantInt::get(IntegerType::getInt32Ty(getGlobalContext()), byte_val));

    /*Locate the instruction I in the basic block BB*/
    BasicBlock *BB = I->getParent();
    BasicBlock::iterator BI, BINext;
    for (BI = BB->begin(); BI != BB->end(); BI++)
        if (BI == *I)
            break;

    if (isa<CallInst>(I))
        return inject_Call(I, args, CallI, BI, BB);


    return false;
}

bool DynamicFaults::inject_Store_Data(Instruction* I, std::vector<Value*> args, CallInst* CallI) {
    args.push_back(I->getOperand(0)); // value stored

    /*Integer Data*/
    if (I->getOperand(0)->getType()->isIntegerTy(8)) {
        CallI = CallInst::Create(func_corruptIntData_8bit, args, "call_corruptIntData_8bit", I);
        assert(CallI);
        CallI->setCallingConv(CallingConv::C);
    } else if (I->getOperand(0)->getType()->isIntegerTy(16)) {
        CallI = CallInst::Create(func_corruptIntData_16bit, args, "call_corruptIntData_16bit", I);
        assert(CallI);
        CallI->setCallingConv(CallingConv::C);
    } else if (I->getOperand(0)->getType()->isIntegerTy(32)) {
        CallI = CallInst::Create(func_corruptIntData_32bit, args, "call_corruptIntData_32bit", I);
        assert(CallI);
        CallI->setCallingConv(CallingConv::C);
    } else if (I->getOperand(0)->getType()->isIntegerTy(64)) {
        CallI = CallInst::Create(func_corruptIntData_64bit, args, "call_corruptIntData_64bit", I);
        assert(CallI);
        CallI->setCallingConv(CallingConv::C);
    } else if (I->getOperand(0)->getType()->isFloatTy()) {
    /*Float Data*/
        CallI = CallInst::Create(func_corruptFloatData_32bit, args,
                                 "call_corruptFloatData_32bit", I);
        assert(CallI);
        CallI->setCallingConv(CallingConv::C);
    } else if (I->getOperand(0)->getType()->isDoubleTy()) {
      CallI = CallInst::Create(func_corruptFloatData_64bit, args,
                               "call_corruptFloatData_64bit", I);
        assert(CallI);
        CallI->setCallingConv(CallingConv::C);
    } else {
        return false;
    }
    if (CallI) {
        Value* corruptVal = &(*CallI);
        I->setOperand(0, corruptVal);
        comment = "Value";
    }
    return true;
}

bool DynamicFaults::inject_Compare(Instruction* I, std::vector<Value*> args, CallInst* CallI) {
    /* select a random arg to corrupt because corrupting the result will yeild a
    50% chance of branching incorrectly */
    unsigned int opPos = rand() % 2;
    PtrToIntInst* p2iI = NULL;
    IntToPtrInst* i2pI = NULL;
    Value* corruptVal = NULL;
    Type* i64Ty = Type::getInt64Ty(I->getContext());

    /* LLVM doesn't like attempting to corrupt NULL */
    if (I->getOperand(opPos) == NULL)
    	opPos = (opPos+1) % 2;

    args.push_back(I->getOperand(opPos));


    /*integer data*/
    if (I->getOperand(opPos)->getType()->isIntegerTy(8)) {
        CallI = CallInst::Create(func_corruptIntData_8bit, args, "call_corruptIntData_8bit", I);
        assert(CallI);
        CallI->setCallingConv(CallingConv::C);
    } else if (I->getOperand(opPos)->getType()->isIntegerTy(16)) {
        CallI = CallInst::Create(func_corruptIntData_16bit, args, "call_corruptIntData_16bit", I);
        assert(CallI);
        CallI->setCallingConv(CallingConv::C);
    } else if (I->getOperand(opPos)->getType()->isIntegerTy(32)) {
        CallI = CallInst::Create(func_corruptIntData_32bit, args, "call_corruptIntData_32bit", I);
        assert(CallI);
        CallI->setCallingConv(CallingConv::C);
    } else if (I->getOperand(opPos)->getType()->isIntegerTy(64)) {
        CallI = CallInst::Create(func_corruptIntData_64bit, args, "call_corruptIntData_64bit", I);
        assert(CallI);
        CallI->setCallingConv(CallingConv::C);
    } else if (I->getOperand(opPos)->getType()->isFloatTy()) {
    /*Float Data*/
        CallI = CallInst::Create(func_corruptFloatData_32bit, args,
                                 "call_corruptFloatData_32bit", I);
        assert(CallI);
        CallI->setCallingConv(CallingConv::C);
    } else if (I->getOperand(opPos)->getType()->isDoubleTy()) {
        CallI = CallInst::Create(func_corruptFloatData_64bit, args,
                                 "call_corruptFloatData_64bit", I);
        assert(CallI);
        CallI->setCallingConv(CallingConv::C);
    } else {
       /* We are comparing non-scalar data a.k.a pointers. Let's convert the pointer to a 64-bit
        * integer corrupt that and cast back to the pointer type. */
        /* Value to corrupt is no longer correct */
        args.pop_back();

        /* Convert ptr to int64 */
        p2iI = new PtrToIntInst(I->getOperand(opPos), i64Ty, "convert_ptr2i64", I);
        assert(p2iI);

        /* Corrupt */
        args.push_back(p2iI);
        CallI = CallInst::Create(func_corruptPtr2Int_64bit, args, "call_corruptPtr2Int_64bit", I);
        assert(CallI);
        CallI->setCallingConv(CallingConv::C);
        corruptVal = &(*CallI);

        /* convert int64 to ptr */
        i2pI = new IntToPtrInst(corruptVal, I->getOperand(opPos)->getType(), "convert_i642ptr", I);
        assert(i2pI);
    }

    /* make sure everyone is using corrupt value */
    if (CallI) {
        if (i2pI == NULL)
            corruptVal = &(*CallI);
        else
            corruptVal = &(*i2pI);
        I->setOperand(opPos, corruptVal);

        strStream << "Operand # " << opPos;
        comment = strStream.str();
        strStream.str("");
    }
    return true;
}


bool DynamicFaults::inject_Generic(Instruction* I, std::vector<Value*> args, CallInst* CallI,
                                   BasicBlock* BB) {
    Instruction* INext = NULL;
    BasicBlock::iterator BI = *I;
    args.push_back(&(*I));
    /* Corrupt result of instruction I */
    if (BI == BB->end()) {

        /*Integer Data*/
        if (I->getType()->isIntegerTy(8)) {
            CallI = CallInst::Create(func_corruptIntData_8bit, args,
                                     "call_corruptIntData_8bit", BB);
            assert(CallI);
            CallI->setCallingConv(CallingConv::C);
        } else if (I->getType()->isIntegerTy(16)) {
            CallI = CallInst::Create(func_corruptIntData_16bit, args,
                                     "call_corruptIntData_16bit", BB);
            assert(CallI);
            CallI->setCallingConv(CallingConv::C);
        } else if (I->getType()->isIntegerTy(32)) {
            CallI = CallInst::Create(func_corruptIntData_32bit, args,
                                     "call_corruptIntData_32bit", BB);
            assert(CallI);
            CallI->setCallingConv(CallingConv::C);
        } else if (I->getType()->isIntegerTy(64)) {
            CallI = CallInst::Create(func_corruptIntData_64bit, args,
                                     "call_corruptIntData_64bit", BB);
            assert(CallI);
            CallI->setCallingConv(CallingConv::C);
        } else if (I->getType()->isFloatTy()) {
        /*Float Data*/
            CallI = CallInst::Create(func_corruptFloatData_32bit, args,
                                     "call_corruptFloatData_32bit", BB);
            assert(CallI);
            CallI->setCallingConv(CallingConv::C);
        } else if (I->getType()->isDoubleTy()) {
            CallI = CallInst::Create(func_corruptFloatData_64bit, args,
                                     "call_corruptFloatData_64bit", BB);
            assert(CallI);
            CallI->setCallingConv(CallingConv::C);
        } else {
            return false;
        }
    } else {
        BasicBlock::iterator BINext = BI;
        BINext++;
        INext = &*BINext;
        assert(INext);

        /*Integer Data*/
        if (I->getType()->isIntegerTy(8)) {
            CallI = CallInst::Create(func_corruptIntData_8bit, args,
                                     "call_corruptIntData_8bit", INext);
            assert(CallI);
            CallI->setCallingConv(CallingConv::C);
        } else if (I->getType()->isIntegerTy(16)) {
            CallI = CallInst::Create(func_corruptIntData_16bit, args,
                                      "call_corruptIntData_16bit", INext);
         assert(CallI);
         CallI->setCallingConv(CallingConv::C);
        } else if (I->getType()->isIntegerTy(32)) {
            CallI = CallInst::Create(func_corruptIntData_32bit, args,
                                   "call_corruptIntData_32bit", INext);
            assert(CallI);
            CallI->setCallingConv(CallingConv::C);
        } else if (I->getType()->isIntegerTy(64)) {
            CallI = CallInst::Create(func_corruptIntData_64bit, args,
                                   "call_corruptIntData_64bit", INext);
            assert(CallI);
            CallI->setCallingConv(CallingConv::C);
        } else if (I->getType()->isFloatTy()) {
        /*Float Data*/
            CallI = CallInst::Create(func_corruptFloatData_32bit, args,
                                     "call_corruptFloatData_32bit", INext);
            assert(CallI);
            CallI->setCallingConv(CallingConv::C);
        } else if (I->getType()->isDoubleTy()) {
            CallI = CallInst::Create(func_corruptFloatData_64bit, args,
                                     "call_corruptFloatData_64bit", INext);
            assert(CallI);
            CallI->setCallingConv(CallingConv::C);
        } else {
            return false;
        }
    }

    if (CallI) {
        Value* corruptVal = &(*CallI);
        I->replaceAllUsesWith(corruptVal);

        /* Because of the preceeding method invocation, we messed up last argument in the call instruction.
            We need to manually set this value to the result of Insturction I */
        BasicBlock::iterator BINext = BI;
        BINext++;
        INext = &*BINext;
        INext->setOperand(4, I); // hard coded. If like others, it says we have an extra argument
        comment = "Result";
    }
    return true;
}


bool DynamicFaults::inject_Store_Ptr(Instruction* I, std::vector<Value*> args, CallInst* CallI) {

    PtrToIntInst* p2iI = NULL;
    IntToPtrInst* i2pI = NULL;
    Value* corruptVal = NULL;
    Type* i64Ty = Type::getInt64Ty(I->getContext());
    /* Make sure we corrupt an pointer. First attempt to corupt the value being stored,
    * but if that isn't a pointer let's inject into the address.*/
    int opNum = 0;
    comment  = "Value";
    if (!I->getType()->isPointerTy()) {
        comment = "Address";
        opNum = 1;
    }

    /*Corrupt operand*/
    args.push_back(I->getOperand(opNum));

    if (I->getOperand(opNum)->getType()->isIntegerTy(8)) {
        CallI = CallInst::Create(func_corruptIntAdr_8bit, args, "call_corruptIntAdr_8bit", I);
        assert(CallI);
        CallI->setCallingConv(CallingConv::C);
    } else if (I->getOperand(opNum)->getType()->isIntegerTy(16)) {
        CallI = CallInst::Create(func_corruptIntAdr_16bit, args, "call_corruptIntAdr_16bit", I);
        assert(CallI);
        CallI->setCallingConv(CallingConv::C);
    } else if (I->getOperand(opNum)->getType()->isIntegerTy(32)) {
        CallI = CallInst::Create(func_corruptIntAdr_32bit, args, "call_corruptIntAdr_32bit", I);
        assert(CallI);
        CallI->setCallingConv(CallingConv::C);
    } else if (I->getOperand(opNum)->getType()->isIntegerTy(64)) {
        CallI = CallInst::Create(func_corruptIntAdr_64bit, args, "call_corruptIntAdr_64bit", I);
        assert(CallI);
        CallI->setCallingConv(CallingConv::C);
    } else if (I->getOperand(opNum)->getType()->isFloatTy()) {
        CallI = CallInst::Create(func_corruptFloatAdr_32bit, args, "call_corruptFloatAdr_32bit", I);
        assert(CallI);
        CallI->setCallingConv(CallingConv::C);
    } else if (I->getOperand(opNum)->getType()->isDoubleTy()){
        CallI = CallInst::Create(func_corruptFloatAdr_64bit, args, "call_corruptFloatAdr_64bit", I);
        assert(CallI);
        CallI->setCallingConv(CallingConv::C);
    } else {
        args.pop_back();

        /* Convert ptr to int64 */
        p2iI = new PtrToIntInst(I->getOperand(opNum), i64Ty, "convert_ptr2i64", I);
        assert(p2iI);

        /* Corrupt */
        args.push_back(p2iI);
        CallI = CallInst::Create(func_corruptPtr2Int_64bit, args, "call_corruptPtr2Int_64bit", I);
        assert(CallI);
        CallI->setCallingConv(CallingConv::C);
        corruptVal = &(*CallI);

        /* convert int64 to ptr */
        i2pI = new IntToPtrInst(corruptVal, I->getOperand(opNum)->getType(), "convert_i642ptr", I);
        assert(p2iI);

    }

    /* make sure everyone is using corrupt value */
    if (CallI) {
        if (i2pI == NULL)
            corruptVal = &(*CallI);
        else
            corruptVal = &(*i2pI);
        I->setOperand(opNum, corruptVal);

        strStream << "Operand # " << opNum;
        comment = strStream.str();
        strStream.str("");
    }
    return true;
}

bool DynamicFaults::inject_Load_Ptr(Instruction* I, std::vector<Value*> args, CallInst* CallI, BasicBlock::iterator BI,  BasicBlock* BB) {

    Instruction* INext;
    PtrToIntInst* p2iI = NULL;
    IntToPtrInst* i2pI = NULL;
    Value* corruptVal = NULL;
    Type* i64Ty = Type::getInt64Ty(I->getContext());

    /* Make sure we corrupt an pointer. First attempt to corupt the value being loaded,
    * but if that isn't a pointer let's inject into the address */
    Value* ptr =  &(*I);
    comment  = "Value";
    if (!I->getType()->isPointerTy()) {
        ptr = dyn_cast<LoadInst>(I)->getPointerOperand();
        comment = "Address";
    }
    args.push_back(ptr);


    if (ptr->getType()->getPointerElementType()->isVectorTy() || ptr->getType()->getPointerElementType()->isIntegerTy(1))
        return false;

    /* Depending on if we are injecting into the result of an operand we need to handle things differenlty */
    if (comment == "Value") {
        if (BI == BB->end()) {
            if (ptr->getType()->isIntegerTy(8)) {
                CallI = CallInst::Create(func_corruptIntAdr_8bit, args, "call_corruptIntAdr_8bit", BB);
                assert(CallI);
                CallI->setCallingConv(CallingConv::C);
            } else if (ptr->getType()->isIntegerTy(16)) {
                CallI = CallInst::Create(func_corruptIntAdr_16bit, args, "call_corruptIntAdr_16bit", BB);
                assert(CallI);
                CallI->setCallingConv(CallingConv::C);
            } else if (ptr->getType()->isIntegerTy(32)) {
                CallI = CallInst::Create(func_corruptIntAdr_32bit, args, "call_corruptIntAdr_32bit", BB);
                assert(CallI);
                CallI->setCallingConv(CallingConv::C);
            } else if (ptr->getType()->isIntegerTy(64)) {
                CallI = CallInst::Create(func_corruptIntAdr_64bit, args, "call_corruptIntAdr_64bit", BB);
                assert(CallI);
                CallI->setCallingConv(CallingConv::C);
            } else if (ptr->getType()->isFloatTy()) {
                CallI = CallInst::Create(func_corruptFloatAdr_32bit, args, "call_corruptFloatAdr_32bit", BB);
                assert(CallI);
                CallI->setCallingConv(CallingConv::C);
            } else if (ptr->getType()->isDoubleTy()) {
                CallI = CallInst::Create(func_corruptFloatAdr_64bit, args, "call_corruptFloatAdr_64bit", BB);
                assert(CallI);
                CallI->setCallingConv(CallingConv::C);
            } else {
                args.pop_back();
                /* Convert ptr to int64 */
                p2iI = new PtrToIntInst(ptr, i64Ty, "convert_ptr2i64", BB);
                assert(p2iI);

                /* Corrupt */
                args.push_back(p2iI);
                CallI = CallInst::Create(func_corruptPtr2Int_64bit, args, "call_corruptPtr2Int_64bit", BB);
                assert(CallI);
                CallI->setCallingConv(CallingConv::C);
                corruptVal = &(*CallI);

                /* convert int64 to ptr */
                i2pI = new IntToPtrInst(corruptVal, ptr->getType(), "convert_i642ptr", BB);
                assert(i2pI);
            }
        } else {
            BasicBlock::iterator BINext = BI;
            BINext++;
            INext = &*BINext;
            assert(INext);

            if (ptr->getType()->isIntegerTy(8)) {
                CallI = CallInst::Create(func_corruptIntAdr_8bit, args, "call_corruptIntAdr_8bit", INext);
                assert(CallI);
                CallI->setCallingConv(CallingConv::C);
            } else if (ptr->getType()->isIntegerTy(16)) {
                CallI = CallInst::Create(func_corruptIntAdr_16bit, args, "call_corruptIntAdr_16bit", INext);
                assert(CallI);
                CallI->setCallingConv(CallingConv::C);
            } else if (ptr->getType()->isIntegerTy(32)) {
                CallI = CallInst::Create(func_corruptIntAdr_32bit, args, "call_corruptIntAdr_32bit", INext);
                assert(CallI);
                CallI->setCallingConv(CallingConv::C);
            } else if (ptr->getType()->isIntegerTy(64)) {
                CallI = CallInst::Create(func_corruptIntAdr_64bit, args, "call_corruptIntAdr_64bit", INext);
                assert(CallI);
                CallI->setCallingConv(CallingConv::C);
            } else if (ptr->getType()->isFloatTy()) {
                CallI = CallInst::Create(func_corruptFloatAdr_32bit, args, "call_corruptFloatAdr_32bit", INext);
                assert(CallI);
                CallI->setCallingConv(CallingConv::C);
            } else if (ptr->getType()->isDoubleTy()) {
                CallI = CallInst::Create(func_corruptFloatAdr_64bit, args, "call_corruptFloatAdr_64bit", INext);
                assert(CallI);
                CallI->setCallingConv(CallingConv::C);
            } else {
                args.pop_back();
                /* Convert ptr to int64 */
                p2iI = new PtrToIntInst(ptr, i64Ty, "convert_ptr2i64", INext);
                assert(p2iI);

                /* Corrupt */
                args.push_back(p2iI);
                CallI = CallInst::Create(func_corruptPtr2Int_64bit, args, "call_corruptPtr2Int_64bit", INext);
                assert(CallI);
                CallI->setCallingConv(CallingConv::C);
                corruptVal = &(*CallI);

                /* convert int64 to ptr */
                i2pI = new IntToPtrInst(corruptVal, ptr->getType(), "convert_i642ptr", INext);
                assert(i2pI);
            }
        }
    } else {
      /* if (comment == "Address") */
        if (ptr->getType()->isIntegerTy(8)) {
            CallI = CallInst::Create(func_corruptIntAdr_8bit, args, "call_corruptIntAdr_8bit", I);
            assert(CallI);
            CallI->setCallingConv(CallingConv::C);
        } else if (ptr->getType()->isIntegerTy(16)) {
            CallI = CallInst::Create(func_corruptIntAdr_16bit, args, "call_corruptIntAdr_16bit", I);
            assert(CallI);
            CallI->setCallingConv(CallingConv::C);
        } else if (ptr->getType()->isIntegerTy(32)) {
            CallI = CallInst::Create(func_corruptIntAdr_32bit, args, "call_corruptIntAdr_32bit", I);
            assert(CallI);
            CallI->setCallingConv(CallingConv::C);
        } else if (ptr->getType()->isIntegerTy(64)) {
            CallI = CallInst::Create(func_corruptIntAdr_64bit, args, "call_corruptIntAdr_64bit", I);
            assert(CallI);
            CallI->setCallingConv(CallingConv::C);
        } else if (ptr->getType()->isFloatTy()) {
            CallI = CallInst::Create(func_corruptFloatAdr_32bit, args, "call_corruptFloatAdr_32bit", I);
            assert(CallI);
            CallI->setCallingConv(CallingConv::C);
        } else if (ptr->getType()->isDoubleTy()) {
            CallI = CallInst::Create(func_corruptFloatAdr_64bit, args, "call_corruptFloatAdr_64bit", I);
            assert(CallI);
            CallI->setCallingConv(CallingConv::C);
        } else {
            args.pop_back();

            BasicBlock::iterator BINext = BI;
            BINext++;
            Instruction* INext = &*BINext;

            assert(INext);

            /* Convert ptr to int64 */
            p2iI = new PtrToIntInst(ptr, i64Ty, "convert_ptr2i64", I);
            assert(p2iI);

            /* Corrupt */
            args.push_back(p2iI);
            CallI = CallInst::Create(func_corruptPtr2Int_64bit, args, "call_corruptPtr2Int_64bit", I);
            assert(CallI);
            CallI->setCallingConv(CallingConv::C);
            corruptVal = &(*CallI);

            /* convert int64 to ptr */
            i2pI = new IntToPtrInst(corruptVal, ptr->getType(), "convert_i642ptr", I);
            assert(p2iI);
        }
    }
    if (CallI) {

        if (comment == "Value") {

            if (i2pI == NULL)
                corruptVal = &(*CallI);
            else
                corruptVal = &(*i2pI);
            I->replaceAllUsesWith(corruptVal);

            /* Because of the preceeding method invocation, we messed up last argument in the call instruction.
                We need to manually set this value to the result of Insturction I */
            BasicBlock::iterator BINext = BI;
            BINext++;
            INext = &*BINext;
            INext->setOperand(/*4*/INext->getNumOperands() - 1, I);
        } else {
            /* if (comment == "Address") */
            if (i2pI == NULL)
                corruptVal = &(*CallI);
            else
                corruptVal = &(*i2pI);
            I->setOperand(0, corruptVal);

        }
    }
    return true;
}

bool DynamicFaults::inject_Alloc_Ptr(Instruction* I, std::vector<Value*> args, CallInst* CallI, BasicBlock::iterator BI,  BasicBlock* BB) {

    Instruction* INext = NULL;
    PtrToIntInst* p2iI = NULL;
    IntToPtrInst* i2pI = NULL;
    Value* corruptVal = NULL;
    Type* i64Ty = Type::getInt64Ty(I->getContext());

    /* we are corrupting the pointer returned from the allocation */
    args.push_back(I);
    if (BI == BB->end()) {
        if (I->getType()->isIntegerTy(8)) {
            CallI = CallInst::Create(func_corruptIntAdr_8bit, args, "call_corruptIntAdr_8bit", BB);
            assert(CallI);
            CallI->setCallingConv(CallingConv::C);
        } else if (I->getType()->isIntegerTy(16)) {
            CallI = CallInst::Create(func_corruptIntAdr_16bit, args, "call_corruptIntAdr_16bit", BB);
            assert(CallI);
            CallI->setCallingConv(CallingConv::C);
        } else if (I->getType()->isIntegerTy(32)) {
            CallI = CallInst::Create(func_corruptIntAdr_32bit, args, "call_corruptIntAdr_32bit", BB);
            assert(CallI);
            CallI->setCallingConv(CallingConv::C);
        } else if (I->getType()->isIntegerTy(64)) {
            CallI = CallInst::Create(func_corruptIntAdr_64bit, args, "call_corruptIntAdr_64bit", BB);
            assert(CallI);
            CallI->setCallingConv(CallingConv::C);
        } else if (I->getType()->isFloatTy()) {
            CallI = CallInst::Create(func_corruptFloatAdr_32bit, args, "call_corruptFloatAdr_32bit", BB);
            assert(CallI);
            CallI->setCallingConv(CallingConv::C);
        } else if (I->getType()->isDoubleTy()) {
            CallI = CallInst::Create(func_corruptFloatAdr_64bit, args, "call_corruptFloatAdr_64bit", BB);
            assert(CallI);
            CallI->setCallingConv(CallingConv::C);
        } else {
            args.pop_back();
            /* Convert ptr to int64 */
            p2iI = new PtrToIntInst(I, i64Ty, "convert_ptr2i64", BB);
            assert(p2iI);

            /* Corrupt */
            args.push_back(p2iI);
            CallI = CallInst::Create(func_corruptPtr2Int_64bit, args, "call_corruptPtr2Int_64bit", BB);
            assert(CallI);
            CallI->setCallingConv(CallingConv::C);
            corruptVal = &(*CallI);

            /* convert int64 to ptr */
            i2pI = new IntToPtrInst(corruptVal, I->getType(), "convert_i642ptr", BB);
            assert(i2pI);
        }
    } else {
        BasicBlock::iterator BINext = BI;
        BINext++;
        INext = &*BINext;
        assert(INext);

        if (I->getType()->isIntegerTy(8)) {
            CallI = CallInst::Create(func_corruptIntAdr_8bit, args, "call_corruptIntAdr_8bit", INext);
            assert(CallI);
            CallI->setCallingConv(CallingConv::C);
        } else if (I->getType()->isIntegerTy(16)) {
            CallI = CallInst::Create(func_corruptIntAdr_16bit, args, "call_corruptIntAdr_16bit", INext);
            assert(CallI);
            CallI->setCallingConv(CallingConv::C);
        } else if (I->getType()->isIntegerTy(32)) {
            CallI = CallInst::Create(func_corruptIntAdr_32bit, args, "call_corruptIntAdr_32bit", INext);
            assert(CallI);
            CallI->setCallingConv(CallingConv::C);
        } else if (I->getType()->isIntegerTy(64)) {
            CallI = CallInst::Create(func_corruptIntAdr_64bit, args, "call_corruptIntAdr_64bit", INext);
            assert(CallI);
            CallI->setCallingConv(CallingConv::C);
        } else if (I->getType()->isFloatTy()) {
            CallI = CallInst::Create(func_corruptFloatAdr_32bit, args, "call_corruptFloatAdr_32bit", INext);
            assert(CallI);
            CallI->setCallingConv(CallingConv::C);
        } else if (I->getType()->isDoubleTy()) {
            CallI = CallInst::Create(func_corruptFloatAdr_64bit, args, "call_corruptFloatAdr_64bit", INext);
            assert(CallI);
            CallI->setCallingConv(CallingConv::C);
        } else {
            args.pop_back();
            /* Convert ptr to int64 */
            p2iI = new PtrToIntInst(I, i64Ty, "convert_ptr2i64", INext);
            assert(p2iI);

            /* Corrupt */
            args.push_back(p2iI);
            CallI = CallInst::Create(func_corruptPtr2Int_64bit, args, "call_corruptPtr2Int_64bit", INext);
            assert(CallI);
            CallI->setCallingConv(CallingConv::C);
            corruptVal = &(*CallI);

            /* convert int64 to ptr */
            i2pI = new IntToPtrInst(corruptVal, I->getType(), "convert_i642ptr", INext);
            assert(i2pI);
        }
    }

    /* make sure everyone is using corrupt value */
    if (CallI) {

        if (i2pI == NULL)
            corruptVal = &(*CallI);
        else
            corruptVal = &(*i2pI);
        I->replaceAllUsesWith(corruptVal);

        /* Because of the preceeding method invocation, we messed up last argument in the call instruction.
            We need to manually set this value to the result of Insturction I */
        BasicBlock::iterator BINext = BI;
        BINext++;
        INext = &*BINext;
        INext->setOperand(/*4*/INext->getNumOperands() - 1, I);
        comment = "Result";
    }

    return true;
}

int DynamicFaults::selectArgument(CallInst* callInst) {
    int arg = -1;
    int possArgLen = callInst->getNumArgOperands();
    std::vector<int> argPos;
    if ( callInst->getCalledFunction() == NULL)
        return arg;

    bool argFound = false;
    string funcName = callInst->getCalledFunction()->getName().str();
    if (funcProbs.find(funcName) != funcProbs.end())
        if (funcProbs[funcName] == 0)
            return arg;

    // populate with possible args.
    if (funcName.find("llvm.lifetime") != string::npos) {
        argPos.push_back(1);
    } else if (funcName.find("llvm.dbg") != string::npos) {
        return arg;
    } else if (funcName.find("toggleInjector") != string::npos) {
        return arg;
    } else {
        /* populate with valid arguemnts detection of constant integers should
        fix the LLVM intrinsic problem */
        for (int i = 0; i < possArgLen; i++) {
            ConstantInt* CI = dyn_cast<llvm::ConstantInt>(callInst->getArgOperand(i));
            if (!( CI != NULL && funcName.find("llvm.") != string::npos) )
                argPos.push_back(i);
        }
    }


    /* select possible arg based on "arith", "ctrl", and "ptr" */
    while (argFound == false && argPos.size() > 0)  {
        int a = rand() % argPos.size();
        if (ctrl_err && callInst->getArgOperand(a)->getType()->isIntegerTy()) {
            Value* v = (Value*) callInst->getArgOperand(a);
            if ( v->getName().str().find("indvars") == 0
                || v->getName().str().substr(0, 3) == "inc") {
                arg = a;
                injectionType = "Control";
                argFound = true;
            }
        } else if (arith_err && (callInst->getArgOperand(a)->getType()->isIntegerTy()
                || callInst->getArgOperand(a)->getType()->isFloatTy()
                || callInst->getArgOperand(a)->getType()->isDoubleTy() )
                && !callInst->getArgOperand(a)->getType()->isIntegerTy(1)) {
            arg = a;
            injectionType = "Arithmetic";
            argFound = true;
        } else if (ptr_err) {
            arg = a;
            injectionType = "Pointer";
            argFound = true;
        }
        argPos.erase(argPos.begin() + a);
    }

    return arg;
}

bool DynamicFaults::inject_Call(Instruction* I, std::vector<Value*> args, CallInst* CallI,
                                BasicBlock::iterator BI,  BasicBlock* BB) {
    PtrToIntInst* p2iI = NULL;
    IntToPtrInst* i2pI = NULL;
    Value* corruptVal = NULL;
    Type* i64Ty = Type::getInt64Ty(I->getContext());
    if (dyn_cast<CallInst>(I)->getNumArgOperands() == 0)
        return false;

    int opNum = selectArgument(dyn_cast<CallInst>(I));
    if (opNum == -1)
        return false;

    args.push_back(I->getOperand(opNum));

    /* corrupting scalar values */
    if (I->getOperand(opNum)->getType()->isIntegerTy(8)) {
        CallI = CallInst::Create(func_corruptIntData_8bit, args, "call_corruptIntData_8bit", I);
        assert(CallI);
        CallI->setCallingConv(CallingConv::C);
    } else if (I->getOperand(opNum)->getType()->isIntegerTy(16)) {
        CallI = CallInst::Create(func_corruptIntData_16bit, args, "call_corruptIntData_16bit", I);
        assert(CallI);
        CallI->setCallingConv(CallingConv::C);
    } else if (I->getOperand(opNum)->getType()->isIntegerTy(32)) {
        CallI = CallInst::Create(func_corruptIntData_32bit, args, "call_corruptIntData_32bit", I);
        assert(CallI);
        CallI->setCallingConv(CallingConv::C);
    } else if (I->getOperand(opNum)->getType()->isIntegerTy(64)) {
        CallI = CallInst::Create(func_corruptIntData_64bit, args, "call_corruptIntData_64bit", I);
        assert(CallI);
        CallI->setCallingConv(CallingConv::C);
    } else if (I->getOperand(opNum)->getType()->isFloatTy()) {
        CallI = CallInst::Create(func_corruptFloatData_32bit, args,
                                 "call_corruptFloatData_32bit", I);
        assert(CallI);
        CallI->setCallingConv(CallingConv::C);
    } else if (I->getOperand(opNum)->getType()->isDoubleTy()) {
        CallI = CallInst::Create(func_corruptFloatData_64bit, args,
                                 "call_corruptFloatData_64bit", I);
        assert(CallI);
        CallI->setCallingConv(CallingConv::C);
    } else {
        args.pop_back();

        /* Convert ptr to int64 */
        p2iI = new PtrToIntInst(I->getOperand(opNum), i64Ty, "convert_ptr2i64", I);
        assert(p2iI);

        /* Corrupt */
        args.push_back(p2iI);
        CallI = CallInst::Create(func_corruptPtr2Int_64bit, args, "call_corruptPtr2Int_64bit", I);
        assert(CallI);
        CallI->setCallingConv(CallingConv::C);
        corruptVal = &(*CallI);

        /* convert int64 to ptr */
        i2pI = new IntToPtrInst(corruptVal, I->getOperand(opNum)->getType(), "convert_i642ptr", I);
        assert(p2iI);
    }

    /* make sure everyone is using corrupt value */
    if (CallI != NULL) {
        if (i2pI == NULL)
            corruptVal = &(*CallI);
        else
            corruptVal = &(*i2pI);
        BI->setOperand(opNum, corruptVal);

        strStream << "Arg # " << opNum;
        comment = strStream.str();
        strStream.str("");
    }
    return true;
}

bool DynamicFaults::inject_GetElementPtr_Ptr(Instruction* I, std::vector<Value*> args,
                                             CallInst* CallI, BasicBlock::iterator BI,
                                             BasicBlock* BB) {
    Instruction* INext = NULL;
    PtrToIntInst* p2iI = NULL;
    IntToPtrInst* i2pI = NULL;
    Value* corruptVal = NULL;
    BI = *I;
    Type* i64Ty = Type::getInt64Ty(I->getContext());

    if (BI == BB->end()) {
        /* Convert ptr to int64 */
        p2iI = new PtrToIntInst(I, i64Ty, "convert_ptr2i64", BB);
        assert(p2iI);

        /* Corrupt */
        args.push_back(p2iI);
        CallI = CallInst::Create(func_corruptPtr2Int_64bit, args, "call_corruptPtr2Int_64bit", BB);
        assert(CallI);
        CallI->setCallingConv(CallingConv::C);
        corruptVal = &(*CallI);

        /* convert int64 to ptr */
        i2pI = new IntToPtrInst(corruptVal, I->getType(), "convert_i642ptr", BB);
        assert(i2pI);
    } else {
        BasicBlock::iterator BINext = BI;
        BINext++;
        INext = &*BINext;
        assert(INext);

        /* Convert ptr to int64 */
        p2iI = new PtrToIntInst(I, i64Ty, "convert_ptr2i64", INext);
        assert(p2iI);

        /* Corrupt */
        args.push_back(p2iI);
        assert(func_corruptPtr2Int_64bit);
        CallI = CallInst::Create(func_corruptPtr2Int_64bit, args,
                                 "call_corruptPtr2Int_64bit", INext);
        assert(CallI);
        CallI->setCallingConv(CallingConv::C);
        corruptVal = &(*CallI);

        /* convert int64 to ptr */
        i2pI = new IntToPtrInst(corruptVal, I->getType(), "convert_i642ptr", INext);
        assert(i2pI);
    }

    /* make sure everyone is using corrupt value */
    if (CallI) {
        corruptVal = &(*i2pI);
        I->replaceAllUsesWith(corruptVal);

        /* Because of the preceeding method invocation, we messed up last argument in
         * the call instruction.  We need to manually set this value */
        BasicBlock::iterator BINext = BI;
        BINext++;
        INext = &*BINext;
        INext->setOperand(/*4*/INext->getNumOperands() -1, I);
        comment = "Result";
    }
    return true;
}


void DynamicFaults::cacheFunctions(Module::FunctionListType &functionList) {
    StringRef lstr;
    for (Module::iterator it = functionList.begin(); it != functionList.end(); ++it) {
        lstr = it->getName();
        string cstr = lstr.str();
        if (cstr.find("corruptIntData_8bit") != td::string::npos) {
            func_corruptIntData_8bit =&*it;
        } else if (cstr.find("corruptIntData_16bit") != td::string::npos) {
            func_corruptIntData_16bit =&*it;
        } else if (cstr.find("corruptIntData_32bit") != td::string::npos) {
            func_corruptIntData_32bit =&*it;
        } else if (cstr.find("corruptIntData_64bit") != td::string::npos) {
            func_corruptIntData_64bit =&*it;
        } else if (cstr.find("corruptPtr2Int_64bit") != std::string::npos) {
            func_corruptPtr2Int_64bit =&*it;
        } else if (cstr.find("corruptFloatData_32bit") != std::string::npos) {
            func_corruptFloatData_32bit =&*it;
        } else if (cstr.find("corruptFloatData_64bit") != std::string::npos) {
            func_corruptFloatData_64bit =&*it;
        } else if (cstr.find("corruptIntAdr_8bit") != std::string::npos) {
            func_corruptIntAdr_8bit =&*it;
        } else if (cstr.find("corruptIntAdr_16bit") != std::string::npos) {
            func_corruptIntAdr_16bit =&*it;
        } else if (cstr.find("corruptIntAdr_32bit") != std::string::npos) {
            func_corruptIntAdr_32bit =&*it;
        } else if (cstr.find("corruptIntAdr_64bit") != std::string::npos) {
            func_corruptIntAdr_64bit =&*it;
        } else if (cstr.find("corruptFloatAdr_32bit") != std::string::npos) {
            func_corruptFloatAdr_32bit =&*it;
        } else if (cstr.find("corruptFloatAdr_64bit") != std::string::npos) {
            func_corruptFloatAdr_64bit =&*it;
        }
    }/*end for*/

    assert(func_corruptIntData_8bit != NULL  && func_corruptIntData_16bit != NULL
        && func_corruptIntData_32bit != NULL && func_corruptIntData_64bit != NULL
        && func_corruptPtr2Int_64bit != NULL  && func_corruptFloatData_32bit != NULL
        && func_corruptFloatData_64bit != NULL && func_corruptIntAdr_8bit != NULL
        && func_corruptIntAdr_16bit != NULL  && func_corruptIntAdr_32bit != NULL
        && func_corruptIntAdr_64bit != NULL && func_corruptFloatAdr_32bit != NULL
        && func_corruptFloatAdr_64bit != NULL);
}

void DynamicFaults::enumerateSites(std::vector<Instruction*>& ilist, Function *F,
                                   unsigned& displayIdx) {
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; I++) {
        Value *in = &(*I);
        if (in == NULL)
            continue;
        errs()  << *I;
        if ( (isa<StoreInst>(in) || isa<LoadInst>(in) || isa<BinaryOperator>(in) || isa<CmpInst>(in)
            || isa<CallInst>(in) || isa<AllocaInst>(in) || isa<GetElementPtrInst>(in)) ) {
            ilist.push_back(&*I);
            errs()  << "; Fault Index: " << displayIdx++;
        }
        errs() <<'\n';
    }
}

void DynamicFaults::injectFaults(std::vector<Instruction*>& ilist, unsigned& faultIdx) {
    bool ret;

    Instruction* inst;
    for (std::vector<Instruction*>::iterator its = ilist.begin();
         its != ilist.end(); its++, faultIdx++) {
        inst = *its;
        comment = "";
        ret = false;
        injectionType = "";

        if (ctrl_err && injectControl(inst, faultIdx)) {
            ret = true;
            injectionType = "Control";
        } else if (arith_err && injectArithmetic(inst, faultIdx)) {
            ret = true;
            injectionType = "Arithmetic"
        } else if (ptr_err && injectPointer(inst, faultIdx)) {
            ret = true;
            injectionType = "Pointer";
        } else if ( (ctrl_err || arith_err || ptr_err) && injectCall(inst, faultIdx) ) {
            ret = true;
        }
        /*
        else {
            errs() << "Warning: Didn't injection into \"" << *inst << "\"\n";
        }
        */
        if (ret) {
            // Site #,   injection type, comment, inst
            errs() << '#' << faultIdx << '\t' << injectionType << '\t' << comment  << "\t";
            if (MDNode *N = inst->getMetadata("dbg")) {
                DILocation Loc(N);
                unsigned Line = Loc.getLineNumber();
                StringRef File = Loc.getFilename();
                // StringRef Dir = Loc.getDirectory();
                errs() << File << ":" << Line << "\n";
            } else {
                errs() << *inst << '\n';
        }
    }/*end for*/
}
/****************************************************************************************/

