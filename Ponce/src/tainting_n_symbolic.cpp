//Ponce
#include "callbacks.hpp"
#include "globals.hpp"
#include "utils.hpp"

//IDA
#include <ida.hpp>
#include <dbg.hpp>

//Triton


void taint_or_symbolize_main_callback(ea_t main_address)
{
	//We need to invalidate the memory to forze IDA to reaload all the segments and new allocs
	invalidate_dbgmem_config();

	//Iterate through argc, argv[argc] and taint every byte and argc
	triton::__uint argc = get_args(0, true);
	triton::__uint argv = get_args(1, true);
	//msg("argc: %d argv: "HEX_FORMAT"\n", argc, argv);
	if (TAINT_ARGC)
	{
		//First we taint the argc
#ifdef X86_32
		//In x86 we taint the memory of the first arg, argc
		msg("%s argc at memory: "HEX_FORMAT"\n", MODE == TAINT? "Tainting": "Symbolizing", get_args_pointer(0, true));
		if (MODE == TAINT)
			triton::api.taintMemory(triton::arch::MemoryAccess(get_args_pointer(0, true), 4, argc));
		else
			triton::api.convertMemoryToSymbolicVariable(triton::arch::MemoryAccess(get_args_pointer(0, true), 4, argc), "argc");
		if (DEBUG)
			msg("[!] argc %s\n", MODE == TAINT ? "Tainted" : "Symbolized");
#elif 
		//Inx64 we taint the register rcx
		auto reg = str_to_register("RCX");
		reg.setConcreteValue(argc);
		triton::api.taintRegister(reg);
#endif
		start_tainting_or_symbolic_analysis();
	}
	//Second we taint all the arguments values
	//We are tainting the argv[0], this is the program path, and it is something that the 
	//user controls and sometimes is used to do somechecks
	for (unsigned int i = SKIP_ARGV0; i < argc; i++)
	{
		triton::__uint current_argv = read_uint_from_ida(argv + i * REG_SIZE);
		if (current_argv == 0xffffffff)
		{
			msg("[!] Error reading mem~ "HEX_FORMAT"\n", argv + i * REG_SIZE);
			break;
		}
		//We iterate through all the bytes of the current argument
		int j = 0;
		char current_char;
		do
		{
			current_char = read_char_from_ida(current_argv + j);
			if (current_char == '\0' && !TAINT_END_OF_STRING)
				break;
			if (EXTRADEBUG)
				msg("[!] %s argv[%d][%d]: %c\n", MODE == TAINT ? "Tainting" : "Symbolizing", i, j, current_char);
			if (MODE == TAINT)
				triton::api.taintMemory(triton::arch::MemoryAccess(current_argv + j, 1, current_char));
			else
			{
				char comment[256];
				sprintf_s(comment, 256, "argv[%d][%d]", i, j);
				triton::api.convertMemoryToSymbolicVariable(triton::arch::MemoryAccess(current_argv + j, 1, current_char), comment);
			}
			j++;
		} while (current_char != '\0');
		if (j > 1)
		{
			//Enable trigger, something is tainted...
			start_tainting_or_symbolic_analysis();
			if (DEBUG)
				msg("[!] argv[%d] %s (%d bytes)\n", i, MODE == TAINT ? "Tainted" : "Symbolized", j);
		}
	}
}

/*This function set all the breakpoints to automatically taint all the user inputs: argc, argv, recv, fread, etc..*/
void set_automatic_taint_n_simbolic()
{
	if (TAINT_ARGV)
	{
		//We should transparentelly hook the main so we could taint the argv when the execution is there
		//First we need to find the main function
		ea_t main_function = find_function("main");
		if (main_function == -1)
		{
			//Maybe we should look for more? _tmain?
			main_function = find_function("_main");
			if (main_function == -1)
			{
				msg("[!] main function not found, we cannot taint the args :S\n");
				return;
			}
		}
		if (DEBUG)
			msg("[+] main function found at "HEX_FORMAT"\n", main_function);
		//Then we should check if there is already a breakpoint
		bpt_t breakpoint;
		bool bp_exists = get_bpt(main_function, &breakpoint);
		//Then we should add a callback to do at that breakpoint, we should indicate if there was a previous bp there	
		//if not we should continue the execution without stop
		breakpoint_pending_action bpa;
		bpa.address = main_function;
		bpa.ignore_breakpoint = bp_exists;
		bpa.callback = taint_or_symbolize_main_callback;
		//We add the action to the list
		breakpoint_pending_actions.push_back(bpa);
		if (!bp_exists)
		{
			//We need to add a breakpoint
			add_bpt(main_function, 0, BPT_SOFT);
		}
	}
}

/*This function gets the tainted operands for an instruction and add a comment to that instruction with this info*/
void get_controlled_operands_and_add_comment(triton::arch::Instruction* tritonInst, ea_t pc)//, std::list<triton::arch::OperandWrapper> &tainted_reg_operands)
{
	std::stringstream comment;
	std::stringstream regs_controlled;
	std::stringstream mems_controlled;

	/*Here we check all the registers and memory read to know which are tainted*/
	auto regs = tritonInst->getReadRegisters();
	for (auto it = regs.begin(); it != regs.end(); it++)
	{
		auto reg = it->first;
		if ((MODE == TAINT && triton::api.isRegisterTainted(reg)) ||
			(MODE == SYMBOLIC && triton::api.getSymbolicRegisterId(reg) != triton::engines::symbolic::UNSET && triton::api.getSymbolicExpressionFromId(triton::api.getSymbolicRegisterId(reg))->isSymbolized()))
			regs_controlled << reg.getName() << " ";
	}
	if (regs_controlled.str().size() > 0)
	{
		if (MODE == TAINT)
			comment << "Tainted regs: " << regs_controlled.str();
		else
			comment << "Symbolized regs: " << regs_controlled.str();
	}
	auto accesses = tritonInst->getLoadAccess();
	for (auto it = accesses.begin(); it != accesses.end(); it++)
	{
		auto mem = it->first;
		//For the memory we can't use the operand because they don't have yet the real value of the address
		if ((MODE == TAINT && triton::api.isMemoryTainted(mem)) ||
			(MODE == SYMBOLIC && triton::api.getSymbolicMemoryId(mem.getAddress()) != triton::engines::symbolic::UNSET && triton::api.getSymbolicExpressionFromId(triton::api.getSymbolicMemoryId(mem.getAddress()))->isSymbolized()))
			mems_controlled << "0x" << std::hex << mem.getAddress() << " ";
	}
	if (mems_controlled.str().size() > 0)
	{
		if (MODE == TAINT)
			comment << "Tainted memory: " << mems_controlled.str();
		else
			comment << "Symbolized memory: " << mems_controlled.str();
	}
		
	//We set the comment
	if (comment.str().size() > 0)
	{
		set_cmt(pc, comment.str().c_str(), false);
	}
}

/*This function get the */
//std::list<triton::arch::OperandWrapper> get_tainted_regs_operands(triton::arch::Instruction* tritonInst)
//{
//	std::list<triton::arch::OperandWrapper> tainted_reg_operands;
//	for (auto it = tritonInst->operands.begin(); it != tritonInst->operands.end(); it++) 
//	{
//		auto op = *it;
//		if (op.getType() == triton::arch::OP_REG)
//		{
//			msg("op reg: %s\n", op.reg.getName().c_str());
//			if (triton::api.isTainted(*it))
//				tainted_reg_operands.push_back(*it);
//		}
//	}
//	return tainted_reg_operands;
//}
